// =====================================================================
//  gacha_lock_wifi.ino  --  ガチャ機ロック ESP32 WiFi版
//
//  BLE版 (firmware/gacha_lock_ble) から通信方式だけをWiFiに置き換えたもの。
//  BLEは電波が弱いと接続が頻繁に切れ(supervision timeout)、排出の通知を
//  取りこぼす問題が実イベントで発生したため、ローカルのWiFiに乗り換えた。
//  ロック機構・センサー・画面まわりのロジックはBLE版と同一。
//
//  ■ 通信方式
//    ESP32はWiFiに接続し、HTTPサーバーとして待ち受ける。
//    PC側 (pc_game/game_server.py) は http://gacha.local/ に対して:
//      GET /status  → 現在の状態をJSONで返す (PCはこれを定期的に見て状態を追う)
//      GET /unlock  → 解錠
//      GET /lock    → 強制施錠(管理画面用)
//    mDNSで "gacha.local" を名乗るので、IPが変わっても設定変更は不要。
//    (macOSは標準で .local を名前解決できる)
//
//  ■ 動作の流れ (BLE版と同じ)
//    [施錠中]  画面「ロック中」
//         │  /unlock を受信 (または物理ボタン/シリアル'u')
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
//    (WiFi / WebServer / ESPmDNS はESP32ボードパッケージに標準で入っている)
//
//  配線 (BLE版と同じ):
//    サーボ信号   -> GPIO26
//    赤外線センサー D0 -> GPIO25 (★3.3Vで駆動すること)
//    手動解除ボタン(任意, もう片足GND) -> GPIO33
//    画面バックライト -> GPIO32 (ideaspark基板固有)
//
//  ■ 接続情報
//    wifi_config.example.h を wifi_config.h にコピーして書き込むこと。
//    wifi_config.h は .gitignore 済み(認証情報はコミットしない)。
// =====================================================================
#include <Arduino_GFX_Library.h>
#include <ESP32Servo.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "wifi_config.h"

// ---------------- 表示画像 (任意) ----------------
// BLE版と同じ images.h をそのまま使える。無ければ従来の図形描画にフォールバックする。
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
const int CX = 160;   // 画面中心X (横向き 320x170)
const int CY = 70;
const int R  = 50;

Servo lockServo;
WebServer server(80);

// ---------------- 状態 ----------------
enum State { ST_LOCKED, ST_UNLOCKED, ST_THANKS };
State state = ST_LOCKED;
bool  armed = false;    // 解錠後、センサーがクリアになってから検知を有効化するフラグ
uint32_t thanksSince = 0;
bool  servoLockedInThanks = false;
const uint32_t THANKS_MS = 3000;

bool webUnlockReq = false;   // HTTPからの解錠要求
bool webLockReq   = false;   // HTTPからの強制施錠要求

// 排出のたびに増える通し番号。PCはこれを見て「新しく1個出た」を判定するので、
// 通信が一瞬切れても取りこぼさない(BLE版で通知を落とした反省点)。
uint32_t dispenseCount = 0;

// 今の状態を表す文字列。PC側はこれを見てロック状況を把握する。
const char* stateName() {
  switch (state) {
    case ST_UNLOCKED: return "UNLOCKED";
    case ST_THANKS:   return "DISPENSED";   // サーボは施錠済み、お礼を表示中
    default:          return "LOCKED";
  }
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

// WiFi接続待ちの画面(画像が無い環境でも状況が分かるように)
void drawConnecting(const char* line) {
  gfx->fillScreen(C_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(10, 60);
  gfx->print("WiFi...");
  gfx->setTextSize(1);
  gfx->setCursor(10, 100);
  gfx->print(line);
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
}

void unlockNow() {
  lockServo.write(ANGLE_FREE);
  drawReady();
  state = ST_UNLOCKED;
  armed = false;
  Serial.println("[STATE] UNLOCKED (待機: カプセル落下)");
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
}

// ---------------- HTTPハンドラ ----------------
void sendStatus() {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"state\":\"%s\",\"dispensed\":%lu,\"rssi\":%d}",
           stateName(), dispenseCount, WiFi.RSSI());
  server.send(200, "application/json", buf);
}

void handleStatus() { sendStatus(); }

void handleUnlock() {
  webUnlockReq = true;
  Serial.println("[HTTP] /unlock");
  sendStatus();
}

void handleLock() {
  webLockReq = true;
  Serial.println("[HTTP] /lock");
  sendStatus();
}

void handleRoot() {
  // ブラウザから直接開いたとき用の簡単な確認ページ
  String h = "<!doctype html><meta charset=utf-8>"
             "<meta name=viewport content='width=device-width,initial-scale=1'>"
             "<style>body{font-family:sans-serif;text-align:center;margin-top:12vh;background:#111;color:#eee}"
             "a{display:inline-block;margin:8px;padding:14px 26px;border-radius:10px;"
             "background:#2b6cb0;color:#fff;text-decoration:none;font-size:18px}</style>"
             "<h2>ガチャロック</h2><p>状態: ";
  h += stateName();
  h += "<br>排出通算: " + String(dispenseCount);
  h += "<br>電波: " + String(WiFi.RSSI()) + " dBm</p>";
  h += "<a href=/unlock>解錠する</a><a href=/lock>施錠する</a>";
  server.send(200, "text/html; charset=utf-8", h);
}

// ---------------- 解除トリガー ----------------
bool unlockRequested() {
  if (webUnlockReq) { webUnlockReq = false; return true; }   // HTTPから
  if (digitalRead(PIN_BUTTON) == LOW) return true;           // 物理ボタン
  if (Serial.available()) {                                  // シリアル 'u'
    char c = Serial.read();
    if (c == 'u' || c == 'U') return true;
  }
  return false;
}

// ---------------- WiFi接続 ----------------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // 応答が遅れないよう省電力を切る
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi接続中: %s\n", WIFI_SSID);
  drawConnecting(WIFI_SSID);

  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("接続しました IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("http://%s.local/ でアクセスできます\n", MDNS_NAME);
    } else {
      Serial.println("mDNSの開始に失敗(IP直指定でアクセスしてください)");
    }
  } else {
    Serial.println("WiFiに接続できませんでした。以降も自動で再試行します");
  }
}

// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== gacha_lock_wifi start ===");
  Serial.println("解除するには: PCから /unlock / 物理ボタン / シリアル 'u'");

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT);

  lockServo.setPeriodHertz(50);
  lockServo.attach(PIN_SERVO, 500, 2400);

  gfx->begin();
  gfx->fillScreen(C_BLACK);

  connectWiFi();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/unlock", handleUnlock);
  server.on("/lock", handleLock);
  server.begin();
  Serial.println("HTTPサーバーを開始しました");

  lockNow();   // 起動時は施錠から
}

// ---------------- loop ----------------
void loop() {
  server.handleClient();

  // WiFiが切れたら繋ぎ直す(ルーター再起動などに追従)。
  // ただし接続を試している最中に reconnect を重ねて呼ぶと
  // "sta is connecting, return error" を延々と出すだけなので、
  // 「切断が確定した状態」のときだけ呼び直す。
  static uint32_t lastWifiCheck = 0;
  static bool wasConnected = false;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      if (!wasConnected) {
        wasConnected = true;
        Serial.printf("[WiFi] 接続しました IP=%s RSSI=%d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
      }
    } else {
      if (wasConnected) {
        wasConnected = false;
        Serial.println("[WiFi] 切断されました — 再接続します");
      }
      // WL_IDLE_STATUS(接続処理中)のときは邪魔しない
      if (st == WL_CONNECTION_LOST || st == WL_DISCONNECTED || st == WL_CONNECT_FAILED) {
        WiFi.reconnect();
      }
    }
  }

  // 管理画面からの強制施錠(状態に関係なく先に処理する)
  if (webLockReq) {
    webLockReq = false;
    Serial.println("[CMD] 強制施錠");
    lockNow();
    delay(200);
  }

  if (state == ST_LOCKED) {
    if (unlockRequested()) {
      Serial.println("[CMD] 解除指示を受信");
      unlockNow();
      delay(300);                   // ボタン連打・チャタリング対策
    }

  } else if (state == ST_THANKS) {
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

  } else {  // ST_UNLOCKED
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
