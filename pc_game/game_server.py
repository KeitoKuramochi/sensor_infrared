"""
リズムゲーム(16拍パターン方式) PCサーバー。

ESP32はIRリモコンのボタンを受信してこのサーバーに POST /button で通知するだけの
ブリッジに徹する。ゲームのロジック(パターン生成・タイミング判定・スコア・ライフ管理)は
すべてこちら側(Python)で行う。判定はサーバー側の時刻を基準に行うので、
ブラウザの表示がどれだけ揺れても採点自体はブレない。

起動方法:
    pip install flask pyserial
    python3 game_server.py
    ブラウザで http://localhost:8000/ を開く

ESP32からのボタン入力は2経路どちらでも受けられる:
  - BLE版 (firmware/color_memory_game_ble): 「ColorMemoryGame」を自動で見つけて
    接続する(推奨。ペアリングもWiFi設定も不要。pip install bleak が必要)
  - WiFi版 (firmware/color_memory_game_wifi): このPCのIPの :8000/button に
    ボタン名がPOSTされてくる

ゲーム仕様:
    - 全5ステージ、各ステージは16拍の固定パターン
    - ステージごとに「色付きビート」の数が 2,4,6,8,10 と増える(残りは空ビート)
    - 色・位置は毎回ランダム
    - 各色ビートの到着タイミングに対して押したタイミングを判定:
      早すぎ/遅すぎ(ズレが大きい)= ライフを失う、多少のズレ = OK、ピッタリ = EXCELLENT
      押さずに素通りさせても同様にライフを失う
    - ライフが尽きたらゲームオーバー、5ステージ全部クリアしたらALL CLEAR
"""

import json
import os
import random
import sys
import threading
import time
import urllib.request

from flask import Flask, jsonify, render_template, request

app = Flask(__name__)

# name/hex はリモコンの実物ボタン配置の並び順(左上から読み順)。
# row/col はテンプレート側でリモコンの見た目を再現するためのグリッド位置
# (0-indexed。実機は縦持ちでON/OFF/Modeが上段、色ボタンは3列×4行、row 2-5・col 0-2)。
COLORS = [
    {"name": "Red", "hex": "#d33b30", "row": 2, "col": 0},
    {"name": "Green", "hex": "#2fa85a", "row": 2, "col": 1},
    {"name": "Blue1", "hex": "#3a5fe0", "row": 2, "col": 2},
    {"name": "Orange", "hex": "#d98a1f", "row": 3, "col": 0},
    {"name": "Lime", "hex": "#9ecc1f", "row": 3, "col": 1},
    {"name": "Purple1", "hex": "#5b3aa6", "row": 3, "col": 2},
    {"name": "Yellow", "hex": "#cbc72e", "row": 4, "col": 0},
    {"name": "SkyBlue", "hex": "#6fb8e8", "row": 4, "col": 1},
    {"name": "Pink", "hex": "#9c3fa0", "row": 4, "col": 2},
    {"name": "LightPink", "hex": "#c9a8cf", "row": 5, "col": 0},
    {"name": "Blue2", "hex": "#3a5cc4", "row": 5, "col": 1},
    {"name": "Purple2", "hex": "#4a2570", "row": 5, "col": 2},
]
COLOR_NAMES = [c["name"] for c in COLORS]

STARTING_LIVES = 3
TOTAL_STAGES = 5
BEATS_PER_STAGE = 16
ACTIVE_BEATS_BY_STAGE = {1: 2, 2: 4, 3: 6, 4: 8, 5: 10}
BEAT_INTERVAL_SEC = 0.7  # 1拍の長さ。テンポを変えたいときはここをいじる(小さいほど速い)
EXCELLENT_WINDOW_SEC = 0.09
OK_WINDOW_SEC = 0.22
END_BUFFER_BEATS = 1  # 最後のアクティブビートの後、少し待ってからステージ終了とみなす
COUNT_IN_BEATS = 4  # ビート0が来るまでのカウントイン拍数(実際の拍と同じテンポでメトロノームを鳴らす)
# ステージクリア後、ONを押させずに自動で次のステージへ進むまでの時間。
# 画面側はこの間に「3・2・1」のカウントダウンと次のステージの予告を出す。
STAGE_CLEAR_COUNTDOWN_SEC = 4.0
INTRO_SHOW_SEC = 1.5  # 「Stage N」の黒幕を表示する時間。幕が上がってからカウントインが始まる
INTRO_DELAY_SEC = INTRO_SHOW_SEC + COUNT_IN_BEATS * BEAT_INTERVAL_SEC

EXCELLENT_SCORE = 30
EXCELLENT_COMBO_BONUS = 3
OK_SCORE = 15
OK_COMBO_BONUS = 1

# --- 練習モード (エンドレス) ---
# 一定間隔でノーツが流れ続けるだけのモード。ライフは減らず、ゲームオーバーにもならない。
# ステージ制と違い「絶対ビート番号」で管理し、必要になった分だけ遅延生成していく。
PRACTICE_NOTE_EVERY = 3      # 何拍ごとにノーツを出すか
PRACTICE_LOOKAHEAD_BEATS = 24  # 先読みして送るビート数(画面外から流入させるのに十分な量)

# --- 目標点数のプリセット ---
# 挑戦者がリモコンの色ボタンで選ぶ。到達したらガチャのロックを解除する。
# 目安は「各ステージを全部EXCELLENTで抜けたときの累計点」から算出:
#   ステージ1=69 / 2=243 / 3=594 / 4=1230 / 全クリア=2295
TARGET_PRESETS = [
    {"key": "Red", "score": 100, "label": "かんたん", "hint": "ステージ2くらい"},
    {"key": "Green", "score": 250, "label": "ふつう", "hint": "ステージ3くらい"},
    {"key": "Blue1", "score": 500, "label": "むずかしい", "hint": "ステージ3を完走"},
    {"key": "Orange", "score": 1000, "label": "げきむず", "hint": "ステージ4を完走"},
    {"key": "Lime", "score": 2000, "label": "神", "hint": "ほぼ完璧"},
]
# 各ステージ終了時の累計点の目安(全部EXCELLENT時)。画面に出して目標選びの参考にする。
STAGE_BENCHMARKS = [69, 243, 594, 1230, 2295]
DEFAULT_TARGET_SCORE = 500

# 結果発表の演出が終わってから解錠したい(先に開いてしまうと盛り上がらない)。
# 画面側の演出タイムライン(スコアのカウントアップ→判定中の溜め→達成の発表)の
# 尺に合わせてある。演出を変えるときは templates/index.html 側と一緒に調整する。
GACHA_UNLOCK_DELAY_SEC = 4.2

# --- ガチャロック解除 (友人(MaedaReno)制作の gacha-machine プロジェクトとの連携) ---
# ガチャ機側のESP32には firmware/gacha_lock_wifi を書き込む。同じLANに繋がり、
# HTTPで /status /unlock /lock を受け付ける。mDNSで gacha.local を名乗るのでIPは不要。
#
# 以前はBLE版(firmware/gacha_lock_ble)を使っていたが、電波が弱いと接続が頻繁に切れ、
# 「排出した」の通知を取りこぼして次の人に進めなくなる問題が実イベントで出たため
# WiFiに移行した。ポーリング方式なので一時的に通信が途切れても次のポーリングで
# 追いつける。さらに排出は通算カウンタで数えるので、取りこぼしても検知できる。
# (リモコン側 ColorMemoryGame はBLEのまま。そちらは安定しているため)
GACHA_HOST = os.environ.get("GACHA_HOST", "gacha.local")
GACHA_TIMEOUT_SEC = 3
GACHA_POLL_SEC = 0.4       # 状態を見に行く間隔
# 解除に必要な点数は挑戦者がプレイ開始時に選ぶ (TARGET_PRESETS)。

lock = threading.Lock()
state = {
    # idle | target_select | practice | playing | stage_clear | game_over | all_clear
    "phase": "idle",
    "stage": 0,
    "pattern": [],            # 16要素、Noneまたは色名
    "beat_interval": BEAT_INTERVAL_SEC,
    "stage_start_time": 0,
    "next_active_index": None,
    "lives": STARTING_LIVES,
    "score": 0,
    "combo": 0,
    "best_stage": 0,
    "best_score": 0,
    "judgment_seq": 0,        # 判定が出るたびに増える通し番号(クライアントの検知用)
    "last_judgment": "",      # "EXCELLENT" | "OK" | "早すぎ!" | "遅すぎ!" | "MISS"
    "message": "",
    "last_action": time.time(),
    # 挑戦者が選んだ目標点数。この点数以上でゲームが終わるとガチャが解錠される。
    "target_score": DEFAULT_TARGET_SCORE,
    # ステージクリア中、次のステージが自動で始まる時刻(画面のカウントダウン用)
    "next_stage_at": 0,
    # 練習モード(エンドレス)用。絶対ビート番号で管理する。
    #   start_time: ビート0が判定ゾーンに到着する時刻
    #   notes: {ビート番号: 色名} を必要な分だけ生成して保持
    #   next_index: 次に判定すべきノーツのビート番号
    "practice": None,
    # ガチャ連携の状況。
    #   eligible : このゲームが目標点数に到達したか
    #   status   : 解錠コマンドの送信結果。None(対象外) | "pending" | "ok" | "error"
    #   connected: ガチャ機とBLE接続できているか
    #   device   : ガチャ機が通知してきた実際の状態。None | "LOCKED" | "UNLOCKED" | "DISPENSED"
    #   awaiting : 解錠したがまだ再施錠されていない(=次の人を待たせる)
    #   reconnects: 通信が切れて復帰した回数。増え続けるなら置き場所か電波が悪い
    #   connected_since: 今の接続が始まった時刻
    #   rssi: ガチャ機から見たWiFiの電波強度(dBm)。設置位置の判断に使う
    #   dispense_count: ガチャ機が数えている排出の通算数(差分で新しい排出を検知する)
    "gacha": {
        "eligible": False,
        "status": None,
        "detail": "",
        "connected": False,
        "device": None,
        "awaiting": False,
        "reconnects": 0,
        "connected_since": None,
        "rssi": None,
        "dispense_count": None,
    },
}


def reset_gacha_progress():
    """ゲーム1回ぶんのガチャ進行(達成判定・解錠結果)をクリアする。
    connected/device はガチャ機の実状態なので保持する。"""
    g = state["gacha"]
    g.update({"eligible": False, "status": None, "detail": "", "awaiting": False})


# --- その日のランキング (ranking.json に永続化、gitignore対象) ---
RANKING_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ranking.json")
RANKING_KEEP_PER_DAY = 200
RANKING_TOP_N = 10


def _load_rankings():
    try:
        with open(RANKING_FILE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


rankings = _load_rankings()
ranking_seq = max(
    (e.get("id", 0) for day in rankings.values() for e in day), default=0)
last_entry_id = None


def record_result(cleared: bool):
    """ゲーム終了時のスコアを今日のランキングに記録する(lockを保持した状態で呼ぶ)。"""
    global ranking_seq, last_entry_id
    ranking_seq += 1
    last_entry_id = ranking_seq
    entries = rankings.setdefault(time.strftime("%Y-%m-%d"), [])
    entries.append({
        "id": ranking_seq,
        "score": state["score"],
        "stage": state["stage"],
        "cleared": cleared,
        "time": time.strftime("%H:%M"),
    })
    del entries[:-RANKING_KEEP_PER_DAY]
    try:
        with open(RANKING_FILE, "w", encoding="utf-8") as f:
            json.dump(rankings, f, ensure_ascii=False)
    except OSError as e:
        print(f"ランキングの保存に失敗しました: {e}")


def today_top():
    entries = rankings.get(time.strftime("%Y-%m-%d"), [])
    return sorted(entries, key=lambda e: (-e["score"], e["id"]))[:RANKING_TOP_N]


def generate_pattern(stage_num: int):
    active_count = ACTIVE_BEATS_BY_STAGE[stage_num]
    positions = sorted(random.sample(range(BEATS_PER_STAGE), active_count))
    pattern = [None] * BEATS_PER_STAGE
    prev_color = None
    for pos in positions:
        choices = [c for c in COLOR_NAMES if c != prev_color]
        color = random.choice(choices)
        pattern[pos] = color
        prev_color = color
    return pattern


def first_active_index(pattern, start=0):
    for i in range(start, BEATS_PER_STAGE):
        if pattern[i] is not None:
            return i
    return None


def last_active_index(pattern):
    for i in range(BEATS_PER_STAGE - 1, -1, -1):
        if pattern[i] is not None:
            return i
    return BEATS_PER_STAGE - 1


def start_stage(stage_num: int):
    state["stage"] = stage_num
    state["pattern"] = generate_pattern(stage_num)
    state["stage_start_time"] = time.time() + INTRO_DELAY_SEC
    state["next_active_index"] = first_active_index(state["pattern"])
    state["phase"] = "playing"
    state["message"] = ""
    state["last_action"] = time.time()


def start_game(target_score: int):
    state["lives"] = STARTING_LIVES
    state["score"] = 0
    state["combo"] = 0
    state["target_score"] = target_score
    state["practice"] = None
    reset_gacha_progress()
    start_stage(1)


def go_target_select():
    """プレイ開始前に、挑戦者が目標点数を選ぶ画面へ。"""
    state["phase"] = "target_select"
    state["stage"] = 0
    state["pattern"] = []
    state["next_active_index"] = None
    state["practice"] = None
    state["message"] = ""
    state["last_action"] = time.time()


# --- 練習モード (エンドレス) ---

def practice_color_for(beat_index: int, notes: dict):
    """直前のノーツと違う色をランダムに選ぶ(同じ色が連続しないように)。"""
    prev_index = beat_index - PRACTICE_NOTE_EVERY
    prev_color = notes.get(prev_index)
    choices = [c for c in COLOR_NAMES if c != prev_color]
    return random.choice(choices)


def start_practice():
    state["phase"] = "practice"
    state["stage"] = 0
    state["pattern"] = []
    state["next_active_index"] = None
    state["lives"] = STARTING_LIVES
    state["score"] = 0
    state["combo"] = 0
    state["practice"] = {
        "start_time": time.time() + INTRO_DELAY_SEC,
        "notes": {},
        "next_index": 0,
    }
    extend_practice_notes()
    state["message"] = ""
    state["last_action"] = time.time()


def extend_practice_notes():
    """今の時刻から先読み分のノーツを生成しておく(lockを保持した状態で呼ぶ)。"""
    p = state["practice"]
    if p is None:
        return
    elapsed = time.time() - p["start_time"]
    current_beat = max(0, int(elapsed / BEAT_INTERVAL_SEC))
    horizon = current_beat + PRACTICE_LOOKAHEAD_BEATS
    for beat in range(0, horizon + 1, PRACTICE_NOTE_EVERY):
        if beat not in p["notes"]:
            p["notes"][beat] = practice_color_for(beat, p["notes"])
    # 通り過ぎて不要になった古いノーツは捨ててメモリを抑える
    cutoff = p["next_index"] - PRACTICE_NOTE_EVERY * 4
    for beat in [b for b in p["notes"] if b < cutoff]:
        del p["notes"][beat]


def practice_window():
    """クライアントに送る、これから流れてくるノーツの一覧。

    color が入っているビートは色ノーツ、間のビートは「空ビート」として輪だけ流す
    (本編と同じく、リズムの手がかりとメトロノーム音になる)。そのためノーツだけでなく
    描画すべきビート範囲 window も一緒に渡す。
    """
    p = state["practice"]
    if p is None:
        return None
    beats = p["notes"].keys()
    return {
        "start_time": p["start_time"],
        "note_every": PRACTICE_NOTE_EVERY,
        "next_index": p["next_index"],
        "notes": [{"beat": b, "color": c} for b, c in sorted(p["notes"].items())],
        "window": [min(beats), max(beats)] if beats else None,
    }


def apply_gacha_status(data: dict):
    """ガチャ機から返ってきた状態を state に反映する。

    排出は「通算カウンタ(dispensed)が増えたか」で判定する。BLEの通知方式では
    切断中の1回を取りこぼすと永久に次へ進めなくなったが、カウンタなら
    通信が一時的に途切れても、復帰後の差分で確実に気づける。
    """
    with lock:
        g = state["gacha"]
        dev = str(data.get("state", "")).upper()
        if dev in ("LOCKED", "UNLOCKED", "DISPENSED"):
            g["device"] = dev
        g["rssi"] = data.get("rssi")

        count = data.get("dispensed")
        if isinstance(count, int):
            prev = g.get("dispense_count")
            if prev is not None and count > prev:
                print(f"[GACHA] カプセルの排出を確認しました (通算{count}個目)")
            g["dispense_count"] = count

        # 施錠まで戻ったら、次の人が始められる
        if dev == "LOCKED" and g["awaiting"]:
            g["awaiting"] = False
            print("[GACHA] 再施錠を確認しました — 次の人どうぞ")


def gacha_http(path: str):
    """ガチャ機にHTTPでアクセスして、返ってきたJSONを返す。
    (成功したか, データまたはエラー文) を返す。ネットワーク待ちが入るので
    lock を保持したまま呼ばないこと。"""
    url = f"http://{GACHA_HOST}/{path}"
    try:
        with urllib.request.urlopen(url, timeout=GACHA_TIMEOUT_SEC) as resp:
            return True, json.loads(resp.read().decode())
    except Exception as e:
        return False, str(e)


def send_gacha_command(cmd: str):
    """ガチャ機にコマンドを送る。(成功したか, 詳細) を返す。
    受け付けるコマンドは firmware/gacha_lock_wifi 側の実装に対応: UNLOCK / LOCK / STATUS"""
    path = {"UNLOCK": "unlock", "LOCK": "lock", "STATUS": "status"}.get(cmd)
    if path is None:
        return False, f"不明なコマンド: {cmd}"
    ok, data = gacha_http(path)
    if ok:
        apply_gacha_status(data)
        return True, ""
    return False, f"ガチャ機({GACHA_HOST})に接続できません: {data}"


def _request_gacha_unlock():
    """ゲームクリア時の解錠(別スレッドで実行、失敗してもゲームは止めない)。"""
    # 画面の結果発表の演出が終わるのを待ってから解錠する(先に開くと盛り上がらない)
    time.sleep(GACHA_UNLOCK_DELAY_SEC)
    ok, detail = send_gacha_command("UNLOCK")
    with lock:
        g = state["gacha"]
        g["status"] = "ok" if ok else "error"
        if detail:
            g["detail"] = detail
        # 解錠が通ったら、カプセルが出て再施錠されるまで次の人を待たせる
        g["awaiting"] = ok
    print(f"[GACHA] 解錠コマンド{'送信成功' if ok else '送信失敗'}: {detail}")


def maybe_unlock_gacha():
    """ゲーム終了時に呼ぶ。挑戦者が選んだ目標点数に到達していればガチャの解錠を試みる。
    (呼び出し元がすでに lock を保持している前提。ネットワーク待ちで長引かないよう別スレッドに逃がす)"""
    reset_gacha_progress()
    if state["score"] >= state["target_score"]:
        state["gacha"].update({"eligible": True, "status": "pending"})
        threading.Thread(target=_request_gacha_unlock, daemon=True).start()


def go_idle():
    """OFFボタンで待機状態へ。ガチャが詰まって再施錠されない等で進めなくなったときの
    運営側の手動リセットも兼ねるので、awaiting(次の人を待たせるフラグ)も解除する。"""
    state["phase"] = "idle"
    state["stage"] = 0
    state["pattern"] = []
    state["next_active_index"] = None
    state["message"] = ""
    state["practice"] = None
    state["gacha"]["awaiting"] = False
    state["last_action"] = time.time()


def go_game_over():
    state["phase"] = "game_over"
    state["best_stage"] = max(state["best_stage"], state["stage"] - 1)
    state["best_score"] = max(state["best_score"], state["score"])
    state["message"] = f"Game Over! Score: {state['score']}"
    state["last_action"] = time.time()
    record_result(cleared=False)
    maybe_unlock_gacha()


def register_judgment(judgment: str, kind: str):
    """判定を記録する。練習モードではライフを減らさない(エンドレスで延々練習できるように)。"""
    practicing = state["phase"] == "practice"
    state["judgment_seq"] += 1
    state["last_judgment"] = judgment
    if kind == "excellent":
        state["combo"] += 1
        state["score"] += EXCELLENT_SCORE + state["combo"] * EXCELLENT_COMBO_BONUS
    elif kind == "ok":
        state["combo"] += 1
        state["score"] += OK_SCORE + state["combo"] * OK_COMBO_BONUS
    else:
        state["combo"] = 0
        if not practicing:
            state["lives"] -= 1


def advance_active():
    idx = state["next_active_index"]
    if idx is not None:
        state["next_active_index"] = first_active_index(state["pattern"], idx + 1)


def finish_stage():
    if state["stage"] >= TOTAL_STAGES:
        state["phase"] = "all_clear"
        state["best_stage"] = TOTAL_STAGES
        state["best_score"] = max(state["best_score"], state["score"])
        state["message"] = f"ALL CLEAR! Score: {state['score']}"
        record_result(cleared=True)
        maybe_unlock_gacha()
    else:
        state["best_stage"] = max(state["best_stage"], state["stage"])
        state["phase"] = "stage_clear"
        state["message"] = f"Stage {state['stage']} Clear!"
        # ONを押させずに自動で次のステージへ進む。画面はこの時刻までカウントダウンを出す
        state["next_stage_at"] = time.time() + STAGE_CLEAR_COUNTDOWN_SEC
    state["last_action"] = time.time()


def judge_press(name: str, press_time: float):
    idx = state["next_active_index"]
    if idx is None:
        return
    expected = state["pattern"][idx]
    beat_time = state["stage_start_time"] + idx * state["beat_interval"]
    diff = press_time - beat_time  # 正なら遅い、負なら早い

    if name != expected:
        register_judgment("MISS", "fail")
    else:
        absdiff = abs(diff)
        if absdiff <= EXCELLENT_WINDOW_SEC:
            register_judgment("EXCELLENT", "excellent")
        elif absdiff <= OK_WINDOW_SEC:
            register_judgment("OK", "ok")
        else:
            register_judgment("遅すぎ!" if diff > 0 else "早すぎ!", "fail")

    advance_active()
    if state["lives"] <= 0:
        go_game_over()


def judge_practice_press(name: str, press_time: float):
    """練習モードの判定。ライフは減らないので、判定表示とコンボだけが動く。"""
    p = state["practice"]
    if p is None:
        return
    idx = p["next_index"]
    expected = p["notes"].get(idx)
    if expected is None:
        return
    beat_time = p["start_time"] + idx * BEAT_INTERVAL_SEC
    diff = press_time - beat_time

    if name != expected:
        register_judgment("MISS", "fail")
    else:
        absdiff = abs(diff)
        if absdiff <= EXCELLENT_WINDOW_SEC:
            register_judgment("EXCELLENT", "excellent")
        elif absdiff <= OK_WINDOW_SEC:
            register_judgment("OK", "ok")
        else:
            register_judgment("遅すぎ!" if diff > 0 else "早すぎ!", "fail")

    p["next_index"] = idx + PRACTICE_NOTE_EVERY
    extend_practice_notes()


def handle_button(name: str):
    press_time = time.time()
    with lock:
        state["last_action"] = press_time

        if name == "OFF":
            go_idle()
            return

        if name == "ON":
            # ガチャを解錠したまま次の人に進ませない。カプセルが出て再施錠されるまで待つ
            # (ガチャ機がBLEで "LOCKED" を通知してきた時点で awaiting が解除される)。
            # 解錠コマンドの送信中(pending)も、awaiting が立つ前に始められてしまうので弾く。
            # 詰まって進めない場合は OFF で手動リセットできる。
            if state["gacha"]["awaiting"] or state["gacha"]["status"] == "pending":
                return
            # 結果画面からは目標選択に直行せず、いったんトップ(待機)に戻す。
            # 「もう一度」で目標を選び直すか、次の人が最初から始めるための入口にする。
            if state["phase"] in ("game_over", "all_clear"):
                go_idle()
            elif state["phase"] in ("idle", "practice"):
                go_target_select()
            elif state["phase"] == "stage_clear":
                # カウントダウンを待たずに次へ進めたいとき用
                start_stage(state["stage"] + 1)
            return

        if name == "Mode":
            if state["phase"] in ("idle", "target_select"):
                start_practice()
            return

        if name not in COLOR_NAMES:
            return

        # 目標点数の選択: プリセットが割り当てられた色ボタンで即スタート
        if state["phase"] == "target_select":
            for preset in TARGET_PRESETS:
                if preset["key"] == name:
                    start_game(preset["score"])
                    return
            return

        if state["phase"] == "practice":
            judge_practice_press(name, press_time)
            return

        if state["phase"] != "playing":
            return

        judge_press(name, press_time)


def stage_ticker():
    """押さなかったビートを自動でMISS判定にし、16拍を終えたらステージを終了させる。
    練習モードでは、通り過ぎたノーツをMISSにしつつ次のノーツを生成し続ける。"""
    while True:
        time.sleep(0.02)
        with lock:
            # ステージクリアのカウントダウンが終わったら自動で次のステージへ
            if state["phase"] == "stage_clear":
                if time.time() >= state["next_stage_at"]:
                    start_stage(state["stage"] + 1)
                continue

            if state["phase"] == "practice":
                p = state["practice"]
                if p is not None:
                    now = time.time()
                    # 判定時間を過ぎたノーツは MISS にして次へ進める(ライフは減らない)
                    while True:
                        idx = p["next_index"]
                        if idx not in p["notes"]:
                            break
                        beat_time = p["start_time"] + idx * BEAT_INTERVAL_SEC
                        if now - beat_time > OK_WINDOW_SEC:
                            register_judgment("MISS", "fail")
                            p["next_index"] = idx + PRACTICE_NOTE_EVERY
                        else:
                            break
                    extend_practice_notes()
                continue

            if state["phase"] != "playing":
                continue
            now = time.time()

            idx = state["next_active_index"]
            while idx is not None:
                beat_time = state["stage_start_time"] + idx * state["beat_interval"]
                if now - beat_time > OK_WINDOW_SEC:
                    register_judgment("MISS", "fail")
                    advance_active()
                    idx = state["next_active_index"]
                    if state["lives"] <= 0:
                        go_game_over()
                        break
                else:
                    break

            if state["phase"] == "playing":
                # 最後のアクティブビートの後は残りの空ビートを待たずに切り上げる
                last_idx = last_active_index(state["pattern"])
                total_duration = (last_idx + 1 + END_BUFFER_BEATS) * state["beat_interval"]
                if now - state["stage_start_time"] >= total_duration:
                    finish_stage()


# 生成AIで作った画像を pc_game/static/img/ に置くと、ページ再読み込みだけで反映される。
# ファイル名(拡張子は png/jpg/jpeg/webp のどれでも可)と用途は
# pc_game/static/img/PROMPTS.md を参照。
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")
ART_KEYS = [
    "bg",         # ページ全体の背景
    "logo",       # タイトルロゴ (h1テキストの代わり)
    "gameover",   # ゲームオーバー画面のイラスト
    "allclear",   # 全ステージクリア画面のイラスト
    "stageclear",  # ステージクリア画面のスタンプ
    "excellent",  # EXCELLENT判定の瞬間のバースト演出
    "mascot",     # 遊び方パネルのマスコット (透過PNG推奨)
    "heart",      # ライフ表示のハート (透過PNG推奨)
]


def find_art():
    art = {}
    for key in ART_KEYS:
        art[key] = None
        for ext in ("png", "jpg", "jpeg", "webp"):
            rel = f"img/{key}.{ext}"
            if os.path.exists(os.path.join(STATIC_DIR, rel)):
                art[key] = f"/static/{rel}"
                break
    return art


@app.route("/")
def index():
    # プリセットには画面表示用に色コードを添えて渡す
    hex_by_name = {c["name"]: c["hex"] for c in COLORS}
    presets = [{**p, "hex": hex_by_name.get(p["key"], "#888")} for p in TARGET_PRESETS]
    return render_template(
        "index.html",
        colors=COLORS,
        starting_lives=STARTING_LIVES,
        beats_per_stage=BEATS_PER_STAGE,
        beat_interval_ms=int(BEAT_INTERVAL_SEC * 1000),
        total_stages=TOTAL_STAGES,
        count_in_beats=COUNT_IN_BEATS,
        target_presets=presets,
        stage_benchmarks=STAGE_BENCHMARKS,
        # ステージごとのノーツ数。クリア後の予告(「次は○個流れてくるよ」)に使う
        active_beats_by_stage=ACTIVE_BEATS_BY_STAGE,
        art=find_art(),
    )


@app.route("/state")
def get_state():
    with lock:
        return jsonify({**state,
                        "practice": practice_window(),
                        "ranking": today_top(),
                        "ranking_last_id": last_entry_id})


@app.route("/button", methods=["POST"])
def post_button():
    data = request.get_json(silent=True) or {}
    name = data.get("button", "")
    handle_button(name)
    return jsonify({"ok": True})


# --- 管理者用の手動操作 ---
# イベント中に「ガチャが詰まった」「解錠信号が届かなかった」等が起きたときに、
# 運営が画面から直接介入できるようにする。合言葉等は付けない(手元運用のため)。

@app.route("/admin/action", methods=["POST"])
def admin_action():
    action = (request.get_json(silent=True) or {}).get("action", "")

    # --- ガチャ機に直接コマンドを送る ---
    if action in ("gacha_unlock", "gacha_lock", "gacha_status"):
        cmd = {"gacha_unlock": "UNLOCK",
               "gacha_lock": "LOCK",
               "gacha_status": "STATUS"}[action]
        ok, detail = send_gacha_command(cmd)
        if ok and action == "gacha_lock":
            # 手動で施錠したなら、次の人を待たせる必要はもう無い
            with lock:
                state["gacha"]["awaiting"] = False
        print(f"[ADMIN] {cmd} {'成功' if ok else '失敗: ' + detail}")
        return jsonify({"ok": ok,
                        "message": f"{cmd} を送信しました" if ok else f"送信できません: {detail}"})

    # --- ゲーム側の状態を戻す ---
    if action == "game_reset":
        with lock:
            go_idle()
        print("[ADMIN] ゲームを待機状態に戻しました")
        return jsonify({"ok": True, "message": "待機状態に戻しました"})

    if action == "clear_awaiting":
        with lock:
            state["gacha"]["awaiting"] = False
            state["gacha"]["status"] = None
        print("[ADMIN] 再施錠待ちを解除しました")
        return jsonify({"ok": True, "message": "次の人が始められるようにしました"})

    # --- 今日のランキングを消す(イベント前のテスト記録の掃除用) ---
    if action == "ranking_clear":
        global last_entry_id
        today = time.strftime("%Y-%m-%d")
        with lock:
            removed = len(rankings.pop(today, []))
            # 消した記録を指したままだと「NEW」バッジが幽霊表示になるのでクリアする
            last_entry_id = None
            try:
                with open(RANKING_FILE, "w", encoding="utf-8") as f:
                    json.dump(rankings, f, ensure_ascii=False)
            except OSError as e:
                return jsonify({"ok": False, "message": f"保存に失敗: {e}"})
        print(f"[ADMIN] 今日のランキングを{removed}件削除しました")
        return jsonify({"ok": True, "message": f"今日の記録 {removed}件を削除しました"})

    return jsonify({"ok": False, "message": f"不明な操作: {action}"}), 400


# --- BLE受信 (firmware/color_memory_game_ble 用) ---
# ESP32はBLEで「ColorMemoryGame」としてアドバタイズし、ボタン名を通知で送ってくる。
# ペアリング不要。切断検知はBLEスタック内蔵なので、見つけて接続し直すだけでよい。

BLE_DEVICE_NAME = "ColorMemoryGame"
BLE_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
BLE_BUTTON_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"


def ble_reader():
    """BLE通知でESP32のボタン名を受け取り、HTTPと同じ handle_button に流す。
    未発見・切断時は再試行し続ける(ESP32の電源ON/OFFに自動で追従)。"""
    try:
        import asyncio
        import bleak
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("[BLE] bleak が無いためBluetooth受信は無効です (pip install 'bleak<3')")
        return

    if getattr(bleak, "__version__", "").startswith("3."):
        # bleak 3.0.x はこのMacでスキャンが永久に固まる不具合を実機確認済み
        print("[BLE] 警告: bleak 3.x はmacOSでスキャンが固まることがあります。"
              "動かない場合は pip install 'bleak<3' を実行してください")

    print("[BLE] スキャンを開始します。初回はmacOSがBluetoothの使用許可を求めるので"
          "「許可」してください (システム設定 > プライバシーとセキュリティ > Bluetooth)")

    def matches(device, adv):
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        if BLE_SERVICE_UUID.lower() in uuids:
            return True
        return (device.name or adv.local_name) == BLE_DEVICE_NAME

    async def run():
        waiting_logged = False
        while True:
            try:
                device = await BleakScanner.find_device_by_filter(
                    matches, timeout=5.0)
                if device is None:
                    if not waiting_logged:
                        print(f"[BLE] ESP32「{BLE_DEVICE_NAME}」を探しています... "
                              "(電源が入っていれば自動で見つかります)")
                        waiting_logged = True
                    await asyncio.sleep(2)
                    continue

                disconnected = asyncio.Event()
                loop = asyncio.get_running_loop()

                def on_disconnect(_client):
                    loop.call_soon_threadsafe(disconnected.set)

                def on_notify(_char, data):
                    name = bytes(data).decode(errors="ignore").strip()
                    if name:
                        print(f"[BLE] Button: {name}")
                        handle_button(name)

                async with BleakClient(
                        device, disconnected_callback=on_disconnect) as client:
                    print("[BLE] 接続しました — リンク正常、リモコン操作OKです")
                    waiting_logged = False
                    await client.start_notify(BLE_BUTTON_CHAR_UUID, on_notify)
                    await disconnected.wait()
                print("[BLE] 切断されました — 再接続します")
            except Exception as e:
                print(f"[BLE] エラー: {e} — 3秒後に再接続します")
                await asyncio.sleep(3)

    asyncio.run(run())


# --- ガチャ機との通信 (firmware/gacha_lock_wifi 用) ---
# ガチャ機のESP32は同じLAN上でHTTPサーバーとして待ち受け、mDNSで gacha.local を名乗る。
# BLEの通知方式は電波が弱いと切断で取りこぼしが起きたため、こちらから定期的に
# 状態を見に行くポーリング方式にした。一時的に通信が切れても次のポーリングで追いつく。


def gacha_poller():
    """定期的にガチャ機の状態を見に行き、接続状況と排出状況を追いかける。"""
    print(f"[GACHA] {GACHA_HOST} を監視します (mDNS。IPを直接指定したいときは "
          f"環境変数 GACHA_HOST で上書きできます)")
    was_connected = None
    first = True
    while True:
        ok, data = gacha_http("status")
        if ok:
            apply_gacha_status(data)
            with lock:
                g = state["gacha"]
                if not g["connected"]:
                    g["connected"] = True
                    g["connected_since"] = time.time()
                    if not first:
                        g["reconnects"] += 1
            if was_connected is not True:
                print(f"[GACHA] つながりました ({GACHA_HOST}) — "
                      f"電波 {data.get('rssi')} dBm")
            was_connected = True
            first = False
        else:
            with lock:
                g = state["gacha"]
                g["connected"] = False
                g["connected_since"] = None
            if was_connected is not False:
                print(f"[GACHA] つながりません: {data}")
            was_connected = False
        time.sleep(GACHA_POLL_SEC)


if __name__ == "__main__":
    # ログをファイルにリダイレクトしても [BLE] メッセージが即座に出るようにする
    sys.stdout.reconfigure(line_buffering=True)
    threading.Thread(target=stage_ticker, daemon=True).start()
    threading.Thread(target=ble_reader, daemon=True).start()
    threading.Thread(target=gacha_poller, daemon=True).start()
    app.run(host="0.0.0.0", port=8000, debug=False)
