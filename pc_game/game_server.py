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

import random
import sys
import threading
import time

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
BEAT_INTERVAL_SEC = 0.55
EXCELLENT_WINDOW_SEC = 0.09
OK_WINDOW_SEC = 0.22
END_BUFFER_BEATS = 1  # 最後のアクティブビートの後、少し待ってからステージ終了とみなす
COUNT_IN_BEATS = 4  # ビート0が来るまでのカウントイン拍数(実際の拍と同じテンポでメトロノームを鳴らす)
INTRO_SHOW_SEC = 1.5  # 「Stage N」の黒幕を表示する時間。幕が上がってからカウントインが始まる
INTRO_DELAY_SEC = INTRO_SHOW_SEC + COUNT_IN_BEATS * BEAT_INTERVAL_SEC

EXCELLENT_SCORE = 30
EXCELLENT_COMBO_BONUS = 3
OK_SCORE = 15
OK_COMBO_BONUS = 1

lock = threading.Lock()
state = {
    "phase": "idle",  # idle | practice | playing | stage_clear | game_over | all_clear
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
    "practice_last": None,    # 練習モード中に最後に押された色
    "message": "",
    "last_action": time.time(),
}


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


def start_game():
    state["lives"] = STARTING_LIVES
    state["score"] = 0
    state["combo"] = 0
    start_stage(1)


def go_idle():
    state["phase"] = "idle"
    state["stage"] = 0
    state["pattern"] = []
    state["next_active_index"] = None
    state["message"] = ""
    state["practice_last"] = None
    state["last_action"] = time.time()


def go_game_over():
    state["phase"] = "game_over"
    state["best_stage"] = max(state["best_stage"], state["stage"] - 1)
    state["best_score"] = max(state["best_score"], state["score"])
    state["message"] = f"Game Over! Score: {state['score']}"
    state["last_action"] = time.time()


def register_judgment(judgment: str, kind: str):
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
    else:
        state["best_stage"] = max(state["best_stage"], state["stage"])
        state["phase"] = "stage_clear"
        state["message"] = f"Stage {state['stage']} Clear!"
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


def handle_button(name: str):
    press_time = time.time()
    with lock:
        state["last_action"] = press_time

        if name == "OFF":
            go_idle()
            return

        if name == "ON":
            if state["phase"] in ("idle", "game_over", "all_clear"):
                start_game()
            elif state["phase"] == "stage_clear":
                start_stage(state["stage"] + 1)
            return

        if name == "Mode":
            if state["phase"] == "idle":
                state["phase"] = "practice"
                state["practice_last"] = None
                state["message"] = "練習モード: 好きな色ボタンを押してみよう(OFFで戻る)"
            return

        if name not in COLOR_NAMES:
            return

        if state["phase"] == "practice":
            state["practice_last"] = name
            return

        if state["phase"] != "playing":
            return

        judge_press(name, press_time)


def stage_ticker():
    """押さなかったビートを自動でMISS判定にし、16拍を終えたらステージを終了させる。"""
    while True:
        time.sleep(0.02)
        with lock:
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


@app.route("/")
def index():
    return render_template(
        "index.html",
        colors=COLORS,
        starting_lives=STARTING_LIVES,
        beats_per_stage=BEATS_PER_STAGE,
        beat_interval_ms=int(BEAT_INTERVAL_SEC * 1000),
        total_stages=TOTAL_STAGES,
        count_in_beats=COUNT_IN_BEATS,
    )


@app.route("/state")
def get_state():
    with lock:
        return jsonify(state)


@app.route("/button", methods=["POST"])
def post_button():
    data = request.get_json(silent=True) or {}
    name = data.get("button", "")
    handle_button(name)
    return jsonify({"ok": True})


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


if __name__ == "__main__":
    # ログをファイルにリダイレクトしても [BLE] メッセージが即座に出るようにする
    sys.stdout.reconfigure(line_buffering=True)
    threading.Thread(target=stage_ticker, daemon=True).start()
    threading.Thread(target=ble_reader, daemon=True).start()
    app.run(host="0.0.0.0", port=8000, debug=False)
