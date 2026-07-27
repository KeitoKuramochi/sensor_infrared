// color_memory_game_ble.ino
//
// 色記憶ゲームのESP32側・BLE (Bluetooth Low Energy) 版。
// ゲームロジックはPC側 (pc_game/game_server.py) が持ち、ESP32はIRリモコンの
// ボタンを受信してBLE通知でPCに送る「ブリッジ」に徹する。
//
// Bluetooth Classic (SPP) 版 (color_memory_game_bt) は現行macOSでRFCOMM接続の
// 確立が極めて不安定だったため、macOSが第一級でサポートするBLEに切り替えた。
// BLEはペアリング不要・切断検知はBLEスタック内蔵(supervision timeout)なので、
// アプリ側のハートビートも不要になる。
//
// 必要なライブラリ:
//   - IRremoteESP8266  (作者: crankyoldgit)
//   - Adafruit SSD1306 / Adafruit GFX (OLEDでステータス表示用)
//   - NimBLE-Arduino  (作者: h2zero)
//
// 配線: IR受信部のみ (VS838のデータシート確認済み: ドーム正面から見て左から OUT/GND/VCC)
//   OUT -> ESP32 GPIO27
//   GND -> ESP32 GND
//   VCC -> ESP32 3V3
//   OLEDはボードにオンボード実装済み (SDA=21 SCL=22 確認済み)
//
// セットアップ:
//   1. このスケッチを書き込む
//   2. pc_game/game_server.py を起動する (pip install bleak が必要)。
//      ペアリング不要 — サーバーが「ColorMemoryGame」を自動で見つけて接続する
//
// 動作: 起動するとBLEアドバタイズを開始し、PCが接続すると通知でボタン名を送る。
// 切断されたら自動でアドバタイズを再開する。

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <NimBLEDevice.h>
#include <Wire.h>

constexpr char BLE_DEVICE_NAME[] = "ColorMemoryGame";
// Nordic UART Service 互換のUUID (シリアル代わりの通知によく使われる)
constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BUTTON_CHAR_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

constexpr uint8_t IR_RECV_PIN = 27;
constexpr int OLED_SDA_PIN = 21;
constexpr int OLED_SCL_PIN = 22;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr uint8_t OLED_ADDR = 0x3C;

constexpr uint32_t BTN_ON_CODE = 0x1FE48B7;
constexpr uint32_t BTN_OFF_CODE = 0x1FE58A7;
constexpr uint32_t BTN_MODE_CODE = 0x1FE7887;
constexpr uint32_t IR_REPEAT_CODE = 0xFFFFFFFF;

struct ColorButton {
  const char* name;
  uint32_t irCode;
};

// 実機確認済みの確定版 (SESSION_LOG.md 2026-07-27「★確定」参照。変更しないこと)
ColorButton COLORS[12] = {
    {"Red", 0x1FE20DF},      {"Green", 0x1FEA05F},     {"Blue1", 0x1FE609F},
    {"Orange", 0x1FEE01F},   {"Lime", 0x1FE10EF},      {"Purple1", 0x1FE906F},
    {"Yellow", 0x1FE50AF},   {"SkyBlue", 0x1FED827},   {"Pink", 0x1FEF807},
    {"LightPink", 0x1FE30CF}, {"Blue2", 0x1FEB04F},    {"Purple2", 0x1FE708F},
};
constexpr uint8_t NUM_COLORS = 12;

IRrecv irrecv(IR_RECV_PIN);
decode_results results;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

NimBLECharacteristic* buttonChar = nullptr;
volatile bool pcConnected = false;

void drawStatus(const String& line1, const String& line2) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(line1);
  display.setCursor(0, 36);
  display.println(line2);
  display.display();
}

const char* nameForCode(uint32_t code) {
  if (code == BTN_ON_CODE) return "ON";
  if (code == BTN_OFF_CODE) return "OFF";
  if (code == BTN_MODE_CODE) return "Mode";
  for (uint8_t i = 0; i < NUM_COLORS; i++) {
    if (COLORS[i].irCode == code) return COLORS[i].name;
  }
  return nullptr;
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    pcConnected = true;
    Serial.println("PC connected");
    drawStatus("PC connected!", "Remote ready");
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    pcConnected = false;
    Serial.printf("PC disconnected (reason %d)\n", reason);
    drawStatus(String("BLE: ") + BLE_DEVICE_NAME, "Waiting for PC...");
    NimBLEDevice::startAdvertising();  // すぐに次の接続を受け付ける
  }
};

void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
  }
  drawStatus("Color Memory Game", "Starting BLE...");

  irrecv.enableIRIn();

  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  buttonChar = service->createCharacteristic(
      BUTTON_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  service->start();

  // 広告パケット(31バイト)に128bit UUID(18B)と名前(17B)は同居できないため、
  // 広告にはUUIDのみ、名前はスキャン応答に分けて入れる
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
  drawStatus(String("BLE: ") + BLE_DEVICE_NAME, "Waiting for PC...");
}

void loop() {
  if (irrecv.decode(&results)) {
    uint32_t code = (uint32_t)results.value;
    if (code != IR_REPEAT_CODE) {
      const char* name = nameForCode(code);
      if (name != nullptr) {
        Serial.printf("Button: %s (0x%lX)\n", name, code);
        if (pcConnected && buttonChar != nullptr) {
          buttonChar->setValue((const uint8_t*)name, strlen(name));
          buttonChar->notify();
          drawStatus("Sent:", name);
        } else {
          drawStatus("Not connected", name);
        }
      } else {
        Serial.printf("Unknown code: 0x%lX\n", code);
      }
    }
    irrecv.resume();
  }
}
