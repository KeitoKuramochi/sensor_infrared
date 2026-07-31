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

// ---------------- 表示画像 (任意) ----------------
// PROMPTS.md を参考にAIで画像を作り、make_images.py を実行すると images.h が生成される。
// 画像がある場合は全画面イラストで表示し、無い場合は従来の図形(○/⊘)描画になる。
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

// ---------------- BLE設定 ----------------
// ★注意: 色記憶ゲーム側(firmware/color_memory_game_ble)とは別デバイスとして同時にスキャン
// されるため、Service UUIDは意図的に重複しない専用の値にすること(Nordic UART Serviceの
// 標準UUIDをそのまま使うと、PC側がサービスUUIDだけで見分けようとした際に誤接続する)。
constexpr char BLE_DEVICE_NAME[] = "GachaLock";
constexpr char SERVICE_UUID[] = "3F316DB0-CE69-462E-8C66-13D130EEB732";
constexpr char RX_CHAR_UUID[] = "3F316DB1-CE69-462E-8C66-13D130EEB732";  // 書込: 解除コマンド
constexpr char TX_CHAR_UUID[] = "3F316DB2-CE69-462E-8C66-13D130EEB732";  // 通知: 状態

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
volatile bool bleLockReq = false;    // 管理画面からの強制施錠
volatile bool bleStatusReq = false;  // 管理画面からの状態問い合わせ

// ---------------- 状態 ----------------
// ST_THANKS: カプセルを検知して「ありがとう」を見せている状態。
// この状態に入った直後はまだ解錠のままで、LOCK_DELAY_MS 経過してから施錠する
// (回転盤が良い位置まで進んでから爪を落とすため)。
enum State { ST_LOCKED, ST_UNLOCKED, ST_THANKS };
State state = ST_LOCKED;
bool  armed = false;    // 解錠後、センサーがクリアになってから検知を有効化するフラグ
uint32_t thanksSince = 0;
bool  servoLockedInThanks = false;  // ST_THANKS中に施錠まで済んだか
const uint32_t THANKS_MS = 3000;   // 「ありがとう」を表示する時間

// ---------------- BLE通知 ----------------
void notifyStatus(const char* s) {
  if (txChar == nullptr) return;
  txChar->setValue((const uint8_t*)s, strlen(s));
  if (pcConnected) txChar->notify();
}

// 今の状態を表す文字列。PC側はこれを見てロック状況を把握する。
const char* stateName() {
  switch (state) {
    case ST_UNLOCKED: return "UNLOCKED";
    case ST_THANKS:   return "DISPENSED";   // サーボは施錠済み、お礼を表示中
    default:          return "LOCKED";
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    pcConnected = true;
    Serial.println("[BLE] PC connected");
    notifyStatus(stateName());
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
    // UNLOCK: 解錠(ゲームクリア時 / 管理画面の手動解錠)
    // LOCK  : 強制施錠(管理画面の手動施錠。状態に関係なく即ロックする)
    // STATUS: 今の状態を通知で返す(管理画面の状態確認)
    if (cmd == "UNLOCK") {
      Serial.println("[BLE] UNLOCK command received");
      bleUnlockReq = true;
    } else if (cmd == "LOCK") {
      Serial.println("[BLE] LOCK command received");
      bleLockReq = true;
    } else if (cmd == "STATUS") {
      Serial.println("[BLE] STATUS command received");
      bleStatusReq = true;
    }
  }
};

// ---------------- 画面描画 ----------------
// images.h があれば全画面イラスト、無ければ従来の図形描画にフォールバックする。

void drawReady() {                 // 引ける
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

void drawLocked() {                // 引けない
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

void drawThanks() {                // カプセルが出た直後のお礼
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

// カプセル検知時。すぐには施錠せず、回転盤が良い位置まで進むのを LOCK_DELAY_MS だけ待つ
// (落ちた瞬間に爪を落とすと、ポケットが開いた位置で止まって機構が引っかかる)。
// 実際の施錠は loop の ST_THANKS 処理で行う。
void thanksThenLock() {
  armed = false;
  drawThanks();
  state = ST_THANKS;
  thanksSince = millis();
  servoLockedInThanks = false;
  Serial.printf("[STATE] THANKS (%lums後に施錠します)\n", LOCK_DELAY_MS);
  notifyStatus("DISPENSED");
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
  NimBLEDevice::setPower(9);  // 送信出力を最大に(電波が弱い環境でも届きやすくする)
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  service->createCharacteristic(RX_CHAR_UUID, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new RxCallbacks());
  txChar = service->createCharacteristic(
      TX_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  service->start();

  // ★電波が弱い環境でも見つけてもらえるよう、名前を「広告本体」に直接入れる。
  // 128bit UUID(18B)と名前(11B)は31バイト枠に同居できないため、UUIDはスキャン応答に回す。
  // (逆の構成=UUIDを広告・名前をスキャン応答にすると、名前の取得に往復通信が必要になり、
  //  電波が弱いとデバイスとして全く見つけられなくなる。実機で -93dBm の環境下で確認済み)
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setName(BLE_DEVICE_NAME);
  adv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setCompleteServices(NimBLEUUID(SERVICE_UUID));
  adv->setScanResponseData(scanData);
  bool advOk = adv->start();
  Serial.printf("BLE advertising as: %s (start=%s)\n", BLE_DEVICE_NAME,
                advOk ? "OK" : "FAILED");

  lockNow();                        // 起動時は施錠状態から
}

// ---------------- loop ----------------
void loop() {
  // --- 管理画面からの割り込み(状態に関係なく先に処理する) ---
  if (bleStatusReq) {
    bleStatusReq = false;
    notifyStatus(stateName());
  }
  if (bleLockReq) {
    bleLockReq = false;
    Serial.println("[CMD] 強制施錠");
    lockNow();
    delay(200);
  }

  if (state == ST_LOCKED) {
    // 解除指示を待つ
    if (unlockRequested()) {
      Serial.println("[CMD] 解除指示を受信");
      unlockNow();
      delay(300);                   // ボタン連打・チャタリング対策
    }

  } else if (state == ST_THANKS) {
    const uint32_t elapsed = millis() - thanksSince;
    // まず、回転盤が良い位置まで進むのを待ってから施錠する
    if (!servoLockedInThanks && elapsed >= LOCK_DELAY_MS) {
      lockServo.write(ANGLE_LOCK);
      servoLockedInThanks = true;
      Serial.println("[SERVO] 施錠しました");
    }
    // お礼を見せ終わったらロック中の表示に戻す
    if (elapsed > THANKS_MS) {
      lockNow();
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
      // 有効化後、物体を検知したらカプセルが落ちたとみなす
      // (施錠は LOCK_DELAY_MS 待ってから ST_THANKS 側で行う。ここで delay を入れると
      //  その分だけ施錠が遅れてしまうので待たない)
      if (irDetected()) {
        Serial.println("[IR] カプセル検知!");
        thanksThenLock();
      }
    }
  }

  delay(10);
}
