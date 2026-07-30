// =====================================================================
//  gacha_lock_ble.ino  --  ガチャ機ロック ESP32 BLE版
//
//  元ネタ: https://github.com/MaedaReno/gacha-machine
//    (servo_mod/firmware/gacha_main/gacha_main.ino のロジックがベース。
//     解除トリガーをボタン/シリアルから BLE 経由に差し替えたもの)
//
//  このMac側のプロジェクト(色記憶リズムゲーム)とBluetoothだけで完結させたいという
//  要望から、先方リポジトリにあった WiFi版 (gacha_ap.ino / gacha_servo.ino) の代わりに
//  新規作成した。色記憶ゲームのリモコンブリッジ(color_memory_game_ble)と同じ
//  NimBLE-Arduinoで実装しており、同時に2台のESP32(リモコンブリッジ+このガチャ錠)と
//  PCがそれぞれ独立にBLE接続する構成になる。
//
//  動作の流れ:
//    [施錠中 LOCKED]  画面=赤⊘ / サーボ=施錠
//         │  PCから "UNLOCK" コマンドを受信 (または物理ボタン/シリアル'u': 手動フォールバック)
//         ▼
//    [解錠中 UNLOCKED] 画面=緑○(READY) / サーボ=解錠
//         │  赤外線センサーがカプセルの落下を検知
//         ▼
//    再び [施錠中 LOCKED] に戻る (サーボ施錠 + 画面切替)
//
//  PC側はBLEキャラクタリスティックに書き込むだけなので、ペアリング不要・
//  切断してもESP32側は自動で再アドバタイズする(色記憶ゲーム側と同じ設計)。
//
//  ボード: ideaspark ESP32 + 1.9" ST7789 (170x320) (実機確認済みのピン割り当てを流用)
//  必要ライブラリ (Arduino IDE / arduino-cli):
//    - "GFX Library for Arduino" (moononournation)
//    - "ESP32Servo"              (Kevin Harrington)
//    - "NimBLE-Arduino"          (h2zero)
//
//  配線 (実機確認済み):
//    サーボ信号   -> GPIO26
//    赤外線センサー D0 -> GPIO25 (★3.3Vで駆動すること)
//    手動解除ボタン(任意, もう片足GND) -> GPIO33
//    画面バックライト -> GPIO32 (ideaspark基板固有、オンボード配線)
// =====================================================================
#include <Arduino_GFX_Library.h>
#include <ESP32Servo.h>
#include <NimBLEDevice.h>

// ---------------- ピン割り当て (実機確認済み) ----------------
#define PIN_SERVO   26    // サーボ信号
#define PIN_IR      25    // 赤外線センサー D0 (★3.3Vで駆動すること)
#define PIN_BUTTON  33    // 手動解除ボタン(任意, もう片足GND)
#define GFX_BL      32    // 画面バックライト (このボード固有)

// ---------------- サーボの角度 (★実機で要調整) ----------------
const int ANGLE_LOCK = 180;   // 施錠位置
const int ANGLE_FREE = 0;     // 解錠位置

// ---------------- 赤外線センサーの極性 ----------------
//  多くの LM393 障害物センサーは「物体を検知すると D0 が LOW」になる。
//  もし逆(検知でHIGH)なら false にする。
const bool IR_ACTIVE_LOW = true;

// ---------------- BLE設定 ----------------
// 色記憶ゲーム側(firmware/color_memory_game_ble)と同じNordic UART Service互換UUID体系。
// RXは「PCから受信」、TXは「PCへ通知」の向き(標準的なNUSの割り当てに合わせている)。
constexpr char BLE_DEVICE_NAME[] = "GachaLock";
constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char RX_CHAR_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // 書込: 解除コマンド
constexpr char TX_CHAR_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // 通知: 状態

// ---------------- 画面の色 (RGB565) ----------------
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED   0xF800
#define C_GREEN 0x07E0

// ---------------- 画面の初期化 (先方リポジトリの display_test と同じ配線) ----------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(2 /*DC*/, 15 /*CS*/, 18 /*SCK*/, 23 /*MOSI*/, GFX_NOT_DEFINED /*MISO*/);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 4 /*RST*/, 1 /*rotation*/, true /*IPS*/, 170, 320, 35, 0, 35, 0);
const int CX = 160;   // 画面中心X (横向き 320x170)
const int CY = 70;    // マーク中心Y
const int R  = 50;    // マーク半径

Servo lockServo;
NimBLECharacteristic* txChar = nullptr;
volatile bool pcConnected = false;
volatile bool bleUnlockReq = false;

// ---------------- 状態 ----------------
enum State { ST_LOCKED, ST_UNLOCKED };
State state = ST_LOCKED;
bool  armed = false;    // 解錠後、センサーがクリアになってから検知を有効化するフラグ

// ---------------- BLE通知 ----------------
void notifyStatus(const char* s) {
  if (txChar == nullptr) return;
  txChar->setValue((const uint8_t*)s, strlen(s));
  if (pcConnected) txChar->notify();
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    pcConnected = true;
    Serial.println("[BLE] PC connected");
    notifyStatus(state == ST_LOCKED ? "LOCKED" : "UNLOCKED");
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    pcConnected = false;
    Serial.printf("[BLE] PC disconnected (reason %d)\n", reason);
    NimBLEDevice::startAdvertising();  // すぐに次の接続を受け付ける
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    std::string v = characteristic->getValue();
    String cmd(v.c_str());
    cmd.trim();
    cmd.toUpperCase();
    if (cmd == "UNLOCK") {
      Serial.println("[BLE] UNLOCK command received");
      bleUnlockReq = true;
    }
  }
};

// ---------------- 画面描画 ----------------
void drawReady() {                 // 引ける = 緑の ○
  gfx->fillScreen(C_BLACK);
  gfx->fillCircle(CX, CY, R,    C_GREEN);
  gfx->fillCircle(CX, CY, R-16, C_BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(C_GREEN);
  gfx->setCursor(CX - 45, 130);
  gfx->print("READY");
}

void drawLocked() {                // 引けない = 赤の ⊘
  gfx->fillScreen(C_BLACK);
  gfx->fillCircle(CX, CY, R,    C_RED);
  gfx->fillCircle(CX, CY, R-16, C_BLACK);
  for (int o = -4; o <= 4; o++)
    gfx->drawLine(CX-35+o, CY-35, CX+35+o, CY+35, C_RED);
  gfx->setTextSize(3);
  gfx->setTextColor(C_RED);
  gfx->setCursor(CX - 54, 130);
  gfx->print("LOCKED");
}

// ---------------- センサー読み取り ----------------
bool irDetected() {                // カプセル(物体)がセンサー前にある = true
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
  notifyStatus("LOCKED");
}

void unlockNow() {
  lockServo.write(ANGLE_FREE);
  drawReady();
  state = ST_UNLOCKED;
  armed = false;                   // まだ検知は有効化しない(下でクリア確認後に有効化)
  Serial.println("[STATE] UNLOCKED (待機: カプセル落下)");
  notifyStatus("UNLOCKED");
}

// ---------------- 解除トリガー ----------------
bool unlockRequested() {
  // (1) PCからBLEで "UNLOCK" が届いた
  if (bleUnlockReq) { bleUnlockReq = false; return true; }
  // (2) ボタンが押された (GND接続, プルアップ) → LOW (手動フォールバック)
  if (digitalRead(PIN_BUTTON) == LOW) return true;
  // (3) シリアルモニタから 'u' を送った (デバッグ用フォールバック)
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'u' || c == 'U') return true;
  }
  return false;
}

// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== gacha_lock_ble start ===");
  Serial.println("解除するには: PCからBLE 'UNLOCK' / ボタン / シリアル 'u'");

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);       // バックライトON
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT);

  lockServo.setPeriodHertz(50);
  lockServo.attach(PIN_SERVO, 500, 2400);

  gfx->begin();
  gfx->fillScreen(C_BLACK);

  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  service->createCharacteristic(RX_CHAR_UUID, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new RxCallbacks());
  txChar = service->createCharacteristic(
      TX_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  service->start();

  // 広告パケット(31バイト)に128bit UUID(18B)と名前(17B)は同居できないため、
  // 広告にはUUIDのみ、名前はスキャン応答に分ける(color_memory_game_ble と同じ対策)
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(SERVICE_UUID));
  adv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName(BLE_DEVICE_NAME);
  adv->setScanResponseData(scanData);
  bool advOk = adv->start();
  Serial.printf("BLE advertising as: %s (start=%s)\n", BLE_DEVICE_NAME,
                advOk ? "OK" : "FAILED");

  lockNow();                        // 起動時は施錠状態から
}

// ---------------- loop ----------------
void loop() {
  if (state == ST_LOCKED) {
    // 解除指示を待つ
    if (unlockRequested()) {
      Serial.println("[CMD] 解除指示を受信");
      unlockNow();
      delay(300);                   // ボタン連打・チャタリング対策
    }

  } else {  // ST_UNLOCKED
    // まずセンサーが「クリア(何もない)」になるまで待ってから検知を有効化する。
    // (解錠した瞬間に機構などがセンサー前にあっても誤検知しないため)
    if (!armed) {
      if (!irDetected()) {
        armed = true;
        Serial.println("[IR] 監視開始 (カプセル落下を待機)");
      }
    } else {
      // 有効化後、物体を検知したらカプセルが落ちたとみなして再施錠
      if (irDetected()) {
        Serial.println("[IR] カプセル検知! -> 再施錠");
        notifyStatus("DISPENSED");
        lockNow();
        delay(300);
      }
    }
  }

  delay(10);
}
