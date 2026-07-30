# sensor_infrared — 100円ショップのLEDライトで作るリズムゲーム

100円ショップで売っているリモコン式LEDライト(300円)を分解し、中の赤外線受信部品とリモコンを
**リズムゲームのコントローラー**に変身させるプロジェクトです。

- 📖 **制作記事(なぜ作ったか・どう作ったか): https://sensor-infrared.vercel.app/**
- 🎮 遊び方はこのREADMEの下の方にあります

## 仕組み

```mermaid
flowchart LR
    R["リモコン<br>(12色 + ON/OFF ボタン)"] -- 赤外線 --> E["ESP32<br>VS838で受信して<br>ボタン名をPCへ転送"]
    E -- "Bluetooth (BLE)" --> P["PC<br>Flaskサーバー :8000<br>ゲームロジック・判定"]
    P --> B["ブラウザ<br>16拍のリズムレーン表示"]
```

ESP32は「どのボタンが押されたか」をBluetooth (BLE) でPCに送るだけのブリッジ役。
ペアリング不要で、サーバーがESP32を自動で見つけて接続します。
ゲーム本体(パターン生成・タイミング判定・スコア・ライフ管理)はPC側のPythonで動き、
画面はブラウザに表示されます。WiFi経由で送る旧構成(`color_memory_game_wifi`)も残してあり、
サーバーはどちらの経路からの入力も受け付けます。

## 必要なもの

| 品目 | 補足 |
|---|---|
| リモコン式LEDライト | 100円ショップで購入(300円商品)。中の赤外線受信部品(VS838)とリモコンを使う |
| ESP32開発ボード | ESP-WROOM-32。今回はOLED(SSD1306 128x64)一体型の ideaspark 製を使用 |
| ブレッドボード・ジャンパー線 | 受信部品とESP32の接続用 |
| PC | Python 3 が動き、Bluetoothが使えればOK。WiFi版を使う場合のみWiFi環境(モバイルルーター可)が必要 |

## 配線

LEDライトを分解し、基板上の赤外線受信部品(VS838)から3本の線を取り出してESP32につなぎます。

| VS838(ドーム正面から見て左から) | ESP32 |
|---|---|
| OUT(信号) | GPIO27 |
| GND | GND |
| VCC(電源) | 3V3 |

OLED一体型ボードの場合、画面の配線は不要です(I2C: SDA=21 / SCL=22 をコード内で設定済み)。

> ⚠️ 分解・配線は自己責任で。電源電圧(VS838は3.3Vで動作)とプラス/マイナスの向きは
> 接続前に必ず確認してください。

## セットアップ手順

### 1. ESP32側(ファームウェア)

1. Arduino IDE に ESP32 ボードサポート(Espressif の `esp32`)を追加
2. ライブラリマネージャーから以下をインストール
   - `IRremoteESP8266`(crankyoldgit)
   - `Adafruit SSD1306` / `Adafruit GFX Library`
   - `NimBLE-Arduino`(h2zero)
3. `firmware/color_memory_game_ble/color_memory_game_ble.ino` をESP32に書き込み

起動するとOLEDに `BLE: ColorMemoryGame / Waiting for PC...` と表示されます。

### 2. PC側(ゲームサーバー)

```bash
pip install flask "bleak<3"   # bleak 3.x はmacOSでスキャンが固まる不具合あり
python3 pc_game/game_server.py
```

ペアリングは不要です。サーバーがBLEで「ColorMemoryGame」を自動で見つけて接続します
(macOS初回はBluetoothの使用許可を求められるので「許可」してください)。
ログに「リンク正常、リモコン操作OKです」と出て、ESP32のOLEDが `PC connected!` に
変われば準備完了。ブラウザで `http://localhost:8000/` を開くとゲーム画面が表示されます。
ESP32の電源を入れ直しても自動で再接続します。

<details>
<summary>旧構成: WiFi経由で使う場合(color_memory_game_wifi)</summary>

1. WiFi設定ファイルを作成:

   ```bash
   cp firmware/color_memory_game_wifi/wifi_config.example.h \
      firmware/color_memory_game_wifi/wifi_config.h
   ```

2. `wifi_config.h` にWiFiのSSID/パスワードと、ゲームサーバーを動かすPCのIPアドレス
   (Macなら `ipconfig getifaddr en0` で確認)を書き込みます。
   このファイルは `.gitignore` 済みなのでコミットされません。
3. `firmware/color_memory_game_wifi/color_memory_game_wifi.ino` をESP32に書き込み。
   起動するとOLEDにWiFi接続状況が表示され、接続に成功するとESP32のIPアドレスが出ます。

</details>

### 3. リモコンのIRコードが違う場合

リモコンの個体・製品が違うとIRコードも異なります。その場合は:

1. スケッチを書き込んだ状態でシリアルモニタ(115200bps)を開く
2. リモコンのボタンを押すと `Unknown code: 0x......` と表示される
3. その値を `color_memory_game_ble.ino` の `COLORS[]` / `BTN_ON_CODE` / `BTN_OFF_CODE` /
   `BTN_MODE_CODE` に書き写す

## 遊び方

| ボタン | 動作 |
|---|---|
| ON | ゲーム開始 / 次のステージへ |
| 色ボタン(12色) | 流れてくるビートと同じ色を、判定ラインに重なる瞬間に押す |
| Mode | 練習モード(待機中のみ。好きな色を押して反応を確認できる) |
| OFF | 中断してタイトルに戻る |

- 全5ステージ。ステージが進むと16拍中の色付きビートが 2 → 4 → 6 → 8 → 10 個に増えます
- 判定: ピッタリ(±0.09秒)= **EXCELLENT** / 少しズレ(±0.22秒)= **OK** / それ以上・押しそびれ = ライフ−1
- ライフは3つ。なくなるとゲームオーバー、5ステージ抜ければ ALL CLEAR

## ガチャロック連携(任意)

友人([MaedaReno/gacha-machine](https://github.com/MaedaReno/gacha-machine))が作った、ESP32+サーボ+赤外線センサーで
ガチャガチャのロックを解錠する装置と連携できる。ゲーム終了時(Game Over / ALL CLEAR問わず)の
最終スコアが500点以上なら、`firmware/gacha_lock_ble` を書き込んだガチャ機ESP32にBLEで解錠コマンドを送る。

- ガチャ機側: `firmware/gacha_lock_ble/gacha_lock_ble.ino` を書き込む(サーボ=GPIO26、赤外線センサー=GPIO25)。
  BLEで「GachaLock」としてアドバタイズし、PCからの解錠コマンドを受けるとサーボが解錠、
  赤外線センサーがカプセルの落下を検知すると自動で再施錠する
- PC側: `pc_game/game_server.py` が起動時に自動で「GachaLock」を探して接続する(色記憶ゲームの
  リモコンブリッジとは別のBLE接続として共存)。閾値は環境変数 `GACHA_UNLOCK_SCORE`(既定500)で変更可能
- ガチャ機のESP32個体によってはBluetooth/WiFiの無線ハードウェア自体が故障している場合がある。
  その場合は書き込み自体は成功し起動ログも正常に見えるが、電波が一切飛ばないため気づきにくい。
  疑わしいときは最小構成のBLE広告テスト・WiFi APテストスケッチで電波が飛んでいるか単体確認するとよい

## リポジトリ構成

```
firmware/
├── color_memory_game_ble/    # ★ 現行版: IR受信→BLEブリッジ(これを書き込む)
├── color_memory_game_bt/     # 旧版: Bluetooth Classic (SPP)版(現行macOSでは接続が不安定)
├── color_memory_game_wifi/   # 旧構成: IR受信→WiFiブリッジ(WiFi環境がある場合の代替)
├── color_memory_game/        # 旧版: ESP32単体で完結する色記憶ゲーム(要WS2812配線・未解決)
├── gacha_lock_ble/           # ガチャ機ロック(友人プロジェクト連携)のBLEファームウェア
├── oled_i2c_scan/            # OLEDのI2Cピンを特定する診断ツール
└── presence_light/           # 実験: 人感ライト構想のスケッチ
pc_game/
├── game_server.py            # ★ ゲーム本体(Flask)
└── templates/index.html      # ブラウザのゲーム画面
site/                         # 制作記事の公開ページ(Vercel用静的サイト)
SESSION_LOG.md                # 開発ログ(AIとのセッション記録)
```

## クレジット

作: [KeitoKuramochi](https://github.com/KeitoKuramochi) — AIアシスタント(Claude)と一緒に制作しました。
