// color_memory_game_wifi.ino
//
// 色記憶ゲームのESP32側。ゲームロジックはPC側 (pc_game/game_server.py) に移し、
// ESP32はIRリモコンのボタンを受信してWiFi経由でPCに通知する「ブリッジ」に徹する。
// D1(LED)の配線がまだ未解決なため、実機での色表示は一旦PC画面に任せる構成。
//
// 必要なライブラリ:
//   - IRremoteESP8266  (作者: crankyoldgit)
//   - Adafruit SSD1306 / Adafruit GFX (OLEDでステータス表示用)
//
// 配線: IR受信部のみ (VS838のデータシート確認済み: ドーム正面から見て左から OUT/GND/VCC)
//   OUT -> ESP32 GPIO27
//   GND -> ESP32 GND
//   VCC -> ESP32 3V3
//   OLEDはボードにオンボード実装済み (SDA=21 SCL=22 確認済み)
//
// セットアップ:
//   1. wifi_config.example.h を wifi_config.h にコピーしてSSID/パスワード、
//      PC_SERVER_HOST (pc_game/game_server.py を動かすPCのIP) を書き込む
//   2. pc_game/game_server.py を起動しておく
//   3. このスケッチを書き込む。OLEDにWiFi接続状況とIPアドレスが出る

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPClient.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <WiFi.h>
#include <Wire.h>
#include "wifi_config.h"

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

ColorButton COLORS[12] = {
    {"Red", 0x1FE20DF},      {"Green", 0x1FEA05F},     {"Blue1", 0x1FE609F},
    {"Orange", 0x1FEE01F},   {"Lime", 0x1FE10EF},      {"Purple1", 0x1FEF807},
    {"Yellow", 0x1FE50AF},   {"SkyBlue", 0x1FED827},   {"Pink", 0x1FE708F},
    {"LightPink", 0x1FE30CF}, {"Blue2", 0x1FEB04F},    {"Purple2", 0x1FE906F},
};
constexpr uint8_t NUM_COLORS = 12;

IRrecv irrecv(IR_RECV_PIN);
decode_results results;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

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

void sendButton(const char* name) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String("http://") + PC_SERVER_HOST + ":" + PC_SERVER_PORT + "/button";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"button\":\"") + name + "\"}";
  int code = http.POST(body);
  Serial.printf("POST %s -> %d\n", name, code);
  http.end();
}

void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
  }
  drawStatus("Color Memory Game", "Connecting WiFi...");

  irrecv.enableIRIn();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  drawStatus("WiFi OK", WiFi.localIP().toString());
}

void loop() {
  if (irrecv.decode(&results)) {
    uint32_t code = (uint32_t)results.value;
    if (code != IR_REPEAT_CODE) {
      const char* name = nameForCode(code);
      if (name != nullptr) {
        Serial.printf("Button: %s (0x%lX)\n", name, code);
        sendButton(name);
        drawStatus("Sent:", name);
      } else {
        Serial.printf("Unknown code: 0x%lX\n", code);
      }
    }
    irrecv.resume();
  }
}
