// =====================================================================
//  gacha_lock_serial.ino  --  ガチャ機ロック ESP32 USBシリアル版 ★現行版
//
//  通信方式の変遷:
//    BLE版  (gacha_lock_ble)  … 電波が弱いと切断が頻発し、排出の通知を取りこぼして
//                               次の人に進めなくなる問題が実イベントで発生
//    WiFi版 (gacha_lock_wifi) … ルーター経由。PCとガチャ機を同じLANに置く必要があり、
//                               会場のネットワーク事情に左右される
//    シリアル版(これ)        … USBケーブル1本で直結。電波の問題が原理的に無くなる
//
//  ■ 通信のしかた
//    PC ← USBケーブル → ESP32 のシリアル(115200bps)で行をやり取りする。
//
//    PCから送るコマンド(1行ずつ):
//      UNLOCK  … 解錠する
//      LOCK    … 強制的に施錠する(管理画面用)
//      STATUS  … 今の状態を返させる
//
//    ESP32が返す状態行(状態が変わるたびに自動でも送る):
//      #GACHA state=LOCKED dispensed=3
//        state     : LOCKED / UNLOCKED / DISPENSED
//        dispensed : 電源を入れてからの排出の通算数
//
//    PC側は「dispensed が増えたか」で排出を判定する。1行取りこぼしても
//    次の行の数字で気づけるので、通信が乱れても詰まらない。
//    人が読むためのログは日本語で出すが、機械が読む行は必ず #GACHA で始める。
//
//  ■ 動作の流れ (他バージョンと同じ)
//    [施錠中]  画面「ロック中」
//         │  UNLOCK を受信 (または物理ボタン / シリアルに 'u')
//         ▼
//    [解錠中]  画面「かいじょ!」/ サーボ=解錠
//         │  赤外線センサーがカプセルの落下を検知
//         ▼
//    [お礼表示] LOCK_DELAY_MS 待ってから施錠 → 3秒後に施錠中へ戻る
//
//  ボード: ideaspark ESP32 + 1.9" ST7789 (170x320)
//  必要ライブラリ:
//    - "GFX Library for Arduino" (moononournation)
//    - "ESP32Servo"              (Kevin Harrington)
//
//  配線 (他バージョンと同じ):
//    サーボ信号   -> GPIO26
//    赤外線センサー D0 -> GPIO25 (★3.3Vで駆動すること)
//    手動解除ボタン(任意, もう片足GND) -> GPIO33
//    画面バックライト -> GPIO32 (ideaspark基板固有)
// =====================================================================
#include <Arduino_GFX_Library.h>
#include <ESP32Servo.h>

// ---------------- 表示画像 (任意) ----------------
// make_images.py が生成する images.h があれば全画面イラスト、
// 無ければ従来の図形描画にフォールバックする。
#if defined(__has_include)
#  if __has_include("images.h")
#    include "images.h"
#    define HAS_IMAGES 1
#  endif
#endif

// ---------------- ピン割り当て (実機確認済み) ----------------
#define PIN_SERVO   26    // サーボ信号
#define PIN_IR      25    // 赤外線センサー D0 (★3.3Vで駆動すること)
#define PIN_BUTTON  33    // 手動解除ボタン(任意, もう片足GND)
#define GFX_BL      32    // 画面バックライト (このボード固有)

// ---------------- サーボの角度 (★実機で要調整) ----------------
const int ANGLE_LOCK = 180;   // 施錠位置
const int ANGLE_FREE = 0;     // 解錠位置

// ---------------- 排出検知から施錠までの遅延 (★実機で要調整) ----------------
//  カプセルが落ちた瞬間に施錠すると、回転盤のポケットがまだ開いた位置で止まってしまい、
//  機構が中途半端なところで引っかかる。少し待って回転盤が良い位置まで進んでから
//  爪を落とすことで、きれいに止まる。長すぎると2個目が出るおそれがあるので注意。
const uint32_t LOCK_DELAY_MS = 200;

// ---------------- 赤外線センサーの極性 ----------------
//  多くの LM393 障害物センサーは「物体を検知すると D0 が LOW」になる。
//  もし逆(検知でHIGH)なら false にする。
const bool IR_ACTIVE_LOW = true;

// ---------------- 画面の色 (RGB565) ----------------
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED   0xF800
#define C_GREEN 0x07E0

// ---------------- 画面の初期化 ----------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(2 /*DC*/, 15 /*CS*/, 18 /*SCK*/, 23 /*MOSI*/, GFX_NOT_DEFINED /*MISO*/);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 4 /*RST*/, 1 /*rotation*/, true /*IPS*/, 170, 320, 35, 0, 35, 0);
const int CX = 160;
const int CY = 70;
const int R  = 50;

Servo lockServo;

// ---------------- 状態 ----------------
enum State { ST_LOCKED, ST_UNLOCKED, ST_THANKS };
State state = ST_LOCKED;
bool  armed = false;    // 解錠後、センサーがクリアになってから検知を有効化するフラグ
uint32_t thanksSince = 0;
bool  servoLockedInThanks = false;
const uint32_t THANKS_MS = 3000;

// 排出のたびに増える通し番号。PC側はこの差分で「新しく1個出た」を判定する
uint32_t dispenseCount = 0;

// シリアルから1行受け取るためのバッファ
String rxLine;

// 今の状態を表す文字列
const char* stateName() {
  switch (state) {
    case ST_UNLOCKED: return "UNLOCKED";
    case ST_THANKS:   return "DISPENSED";   // サーボは施錠済み、お礼を表示中
    default:          return "LOCKED";
  }
}

// PCが読む状態行。状態が変わるたび・問い合わせのたびに送る
void sendStatus() {
  Serial.printf("#GACHA state=%s dispensed=%lu\n", stateName(), dispenseCount);
}

// ---------------- 画面描画 ----------------
void drawReady() {
#ifdef HAS_IMG_UNLOCKED
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)IMG_UNLOCKED, IMG_W, IMG_H);
#else
  gfx->fillScreen(C_BLACK);
  gfx->fillCircle(CX, CY, R,    C_GREEN);
  gfx->fillCircle(CX, CY, R-16, C_BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(C_GREEN);
  gfx->setCursor(CX - 45, 130);
  gfx->print("READY");
#endif
}

void drawLocked() {
#ifdef HAS_IMG_LOCKED
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)IMG_LOCKED, IMG_W, IMG_H);
#else
  gfx->fillScreen(C_BLACK);
  gfx->fillCircle(CX, CY, R,    C_RED);
  gfx->fillCircle(CX, CY, R-16, C_BLACK);
  for (int o = -4; o <= 4; o++)
    gfx->drawLine(CX-35+o, CY-35, CX+35+o, CY+35, C_RED);
  gfx->setTextSize(3);
  gfx->setTextColor(C_RED);
  gfx->setCursor(CX - 54, 130);
  gfx->print("LOCKED");
#endif
}

void drawThanks() {
#ifdef HAS_IMG_THANKS
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)IMG_THANKS, IMG_W, IMG_H);
#else
  gfx->fillScreen(C_BLACK);
  gfx->setTextSize(4);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(CX - 130, CY - 16);
  gfx->print("THANK YOU!");
#endif
}

// ---------------- センサー読み取り ----------------
bool irDetected() {
  int v = digitalRead(PIN_IR);
  return IR_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

// ---------------- 施錠/解錠 ----------------
void lockNow() {
  lockServo.write(ANGLE_LOCK);
  drawLocked();
  state = ST_LOCKED;
  armed = false;
  Serial.println("[STATE] LOCKED");
  sendStatus();
}

void unlockNow() {
  lockServo.write(ANGLE_FREE);
  drawReady();
  state = ST_UNLOCKED;
  armed = false;
  Serial.println("[STATE] UNLOCKED (待機: カプセル落下)");
  sendStatus();
}

// カプセル検知時。すぐには施錠せず、回転盤が良い位置まで進むのを LOCK_DELAY_MS だけ待つ。
// 実際の施錠は loop の ST_THANKS 処理で行う。
void thanksThenLock() {
  armed = false;
  drawThanks();
  state = ST_THANKS;
  thanksSince = millis();
  servoLockedInThanks = false;
  dispenseCount++;
  Serial.printf("[STATE] THANKS (%lums後に施錠します) 排出通算=%lu\n",
                LOCK_DELAY_MS, dispenseCount);
  sendStatus();
}

// ---------------- シリアルのコマンド処理 ----------------
void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  cmd.toUpperCase();

  if (cmd == "UNLOCK" || cmd == "U") {
    Serial.println("[CMD] 解除指示を受信");
    if (state == ST_LOCKED) unlockNow();
    else sendStatus();            // すでに解錠中などは現状を返すだけ
  } else if (cmd == "LOCK" || cmd == "L") {
    Serial.println("[CMD] 強制施錠");
    lockNow();
  } else if (cmd == "STATUS" || cmd == "S") {
    sendStatus();
  } else {
    Serial.printf("[CMD] 不明なコマンド: %s\n", cmd.c_str());
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLine.length() > 0) {
        handleCommand(rxLine);
        rxLine = "";
      }
    } else if (rxLine.length() < 32) {
      rxLine += c;
    }
  }
}

// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== gacha_lock_serial start ===");
  Serial.println("コマンド: UNLOCK / LOCK / STATUS (物理ボタンでも解錠できます)");

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT);

  lockServo.setPeriodHertz(50);
  lockServo.attach(PIN_SERVO, 500, 2400);

  gfx->begin();
  gfx->fillScreen(C_BLACK);

  lockNow();   // 起動時は施錠から(ここで状態行も1回送られる)
}

// ---------------- loop ----------------
void loop() {
  pollSerial();

  // 物理ボタン(もしもの時の手動解錠)
  if (state == ST_LOCKED && digitalRead(PIN_BUTTON) == LOW) {
    Serial.println("[CMD] ボタンで解除");
    unlockNow();
    delay(300);                   // チャタリング対策
  }

  if (state == ST_THANKS) {
    const uint32_t elapsed = millis() - thanksSince;
    // 回転盤が良い位置まで進むのを待ってから施錠する
    if (!servoLockedInThanks && elapsed >= LOCK_DELAY_MS) {
      lockServo.write(ANGLE_LOCK);
      servoLockedInThanks = true;
      Serial.println("[SERVO] 施錠しました");
    }
    if (elapsed > THANKS_MS) {
      lockNow();
    }

  } else if (state == ST_UNLOCKED) {
    // まずセンサーが「クリア(何もない)」になるまで待ってから検知を有効化する
    if (!armed) {
      if (!irDetected()) {
        armed = true;
        Serial.println("[IR] 監視開始 (カプセル落下を待機)");
      }
    } else {
      if (irDetected()) {
        Serial.println("[IR] カプセル検知!");
        thanksThenLock();
      }
    }
  }

  delay(5);
}
