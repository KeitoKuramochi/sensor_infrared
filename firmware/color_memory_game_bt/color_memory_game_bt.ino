// color_memory_game_bt.ino
//
// 色記憶ゲームのESP32側・Bluetooth版。ゲームロジックはPC側 (pc_game/game_server.py) が持ち、
// ESP32はIRリモコンのボタンを受信して Bluetooth Classic (SPP) でPCに通知する「ブリッジ」に徹する。
// WiFi版 (color_memory_game_wifi) と違い、WiFiルーターもIPアドレス設定 (wifi_config.h) も不要。
//
// 必要なライブラリ:
//   - IRremoteESP8266  (作者: crankyoldgit)
//   - Adafruit SSD1306 / Adafruit GFX (OLEDでステータス表示用)
//   - BluetoothSerial (ESP32ボードパッケージに同梱)
//
// 配線: IR受信部のみ (VS838のデータシート確認済み: ドーム正面から見て左から OUT/GND/VCC)
//   OUT -> ESP32 GPIO27
//   GND -> ESP32 GND
//   VCC -> ESP32 3V3
//   OLEDはボードにオンボード実装済み (SDA=21 SCL=22 確認済み)
//
// セットアップ:
//   1. このスケッチを書き込む (標準のパーティション設定で収まる。フラッシュ使用率89%)
//   2. Macの システム設定 > Bluetooth で「ColorMemoryGame」をペアリング(接続)する。
//      ペアリングすると /dev/cu.ColorMemoryGame というシリアルポートが生える
//   3. pc_game/game_server.py を起動する (pyserial導入済みなら自動でポートを見つけて接続する)
//
// 動作: 起動するとBluetoothで待ち受けし、PC側の game_server.py がポートを開いた時点で接続完了。
// 以後、リモコンのボタン名を1行ずつ ("Red\n" など) 送信する。

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BluetoothSerial.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <Wire.h>

constexpr char BT_DEVICE_NAME[] = "ColorMemoryGame";

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
BluetoothSerial SerialBT;
bool lastConnected = false;

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

void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
  }
  drawStatus("Color Memory Game", "Starting Bluetooth...");

  irrecv.enableIRIn();

  if (!SerialBT.begin(BT_DEVICE_NAME)) {
    Serial.println("Bluetooth init failed");
    drawStatus("BT init failed", "Reset the board");
    return;
  }
  Serial.printf("Bluetooth ready: %s\n", BT_DEVICE_NAME);
  drawStatus(String("BT: ") + BT_DEVICE_NAME, "Waiting for PC...");
}

void loop() {
  bool connected = SerialBT.hasClient();
  if (connected != lastConnected) {
    lastConnected = connected;
    if (connected) {
      Serial.println("PC connected");
      drawStatus("PC connected!", "Remote ready");
    } else {
      Serial.println("PC disconnected");
      drawStatus(String("BT: ") + BT_DEVICE_NAME, "Waiting for PC...");
    }
  }

  if (irrecv.decode(&results)) {
    uint32_t code = (uint32_t)results.value;
    if (code != IR_REPEAT_CODE) {
      const char* name = nameForCode(code);
      if (name != nullptr) {
        Serial.printf("Button: %s (0x%lX)\n", name, code);
        if (connected) {
          SerialBT.println(name);
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
