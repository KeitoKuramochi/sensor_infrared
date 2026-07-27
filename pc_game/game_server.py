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
  - Bluetooth版 (firmware/color_memory_game_bt): Macとペアリング済みなら
    /dev/cu.ColorMemoryGame を自動検出してシリアル受信する(推奨。WiFi設定不要)
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

import glob
import random
import threading
import time

from flask import Flask, jsonify, render_template, request

try:
    import serial  # pyserial (Bluetooth受信用。無くてもWiFi/ブラウザ経由は動く)
except ImportError:
    serial = None

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
INTRO_DELAY_SEC = COUNT_IN_BEATS * BEAT_INTERVAL_SEC

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


# --- Bluetooth受信 (firmware/color_memory_game_bt 用) ---
# ESP32をMacとペアリングすると /dev/cu.ColorMemoryGame というポートが生える。
# それを開いた時点でBluetooth接続が確立し、ボタン名が1行ずつ流れてくる。

BT_PORT_PATTERNS = ["/dev/cu.ColorMemoryGame*", "/dev/tty.ColorMemoryGame*"]


def find_bt_port():
    for pattern in BT_PORT_PATTERNS:
        matches = glob.glob(pattern)
        if matches:
            return matches[0]
    return None


def bt_reader():
    """BluetoothシリアルからESP32のボタン名を読み、HTTPと同じ handle_button に流す。
    ポート未検出・切断時は数秒おきに再試行し続ける(ESP32の電源ON/OFFに追従)。"""
    if serial is None:
        print("[BT] pyserial が無いためBluetooth受信は無効です (pip install pyserial)")
        return
    waiting_logged = False
    while True:
        port = find_bt_port()
        if port is None:
            if not waiting_logged:
                print("[BT] Bluetoothポート待機中... "
                      "(Macの設定でESP32「ColorMemoryGame」をペアリングしてください)")
                waiting_logged = True
            time.sleep(3)
            continue
        try:
            with serial.Serial(port, 115200, timeout=1) as ser:
                print(f"[BT] 接続しました: {port}")
                waiting_logged = False
                while True:
                    name = ser.readline().decode(errors="ignore").strip()
                    if name:
                        print(f"[BT] Button: {name}")
                        handle_button(name)
        except (serial.SerialException, OSError) as e:
            print(f"[BT] 切断されました ({e}) — 3秒後に再接続を試みます")
            time.sleep(3)


if __name__ == "__main__":
    threading.Thread(target=stage_ticker, daemon=True).start()
    threading.Thread(target=bt_reader, daemon=True).start()
    app.run(host="0.0.0.0", port=8000, debug=False)
