// presence_light.ino
//
// 手持ちの部品だけで作る第一歩:
//   - ESP32 (ideaspark WROOM-32 + OLED 開発ボード)
//   - 分解した100均RGBパック基板 (CHS005RGB-02) から流用する IR受信部 と WS2812系LED (D1)
//   - 18ボタンのIRリモコン
// を組み合わせて、「リモコンのボタンでESP32が直接LEDを光らせつつ、
// Wi-Fi経由でブラウザから最後に受信したボタン/現在の色を確認できる」装置を作る。
//
// 必要なライブラリ (Arduino IDE の Library Manager からインストール):
//   - IRremoteESP8266  (作者: crankyoldgit)
//   - Adafruit NeoPixel
//
// 配線 (パックの基板から3+3本を引き出してESP32のブレッドボードに接続):
//   IR受信部 (パック基板の黒い3本足の部品)  VCC -> ESP32 3V3
//   IR受信部                                GND -> ESP32 GND
//   IR受信部                                OUT -> ESP32 IR_RECV_PIN (下記で指定)
//   D1 (WS2812 LED)                         VCC -> ESP32 3V3 (実測電圧に応じて5V/VINでも可)
//   D1 (WS2812 LED)                         GND -> ESP32 GND
//   D1 (WS2812 LED)                         DIN -> ESP32 LED_PIN (下記で指定)
//   ※ どのピンがVCC/GND/OUT(DIN)かはテスターで基板を当たって確認してください。
//   ※ IR_RECV_PIN / LED_PIN は OLED が使っていない空きGPIOに合わせて変更してください。
//
// セットアップ:
//   1. wifi_config.example.h を wifi_config.h にコピーしてSSID/パスワードを書き込む
//      (wifi_config.h は .gitignore 済みなのでコミットされません)
//   2. このスケッチを書き込んだ後、シリアルモニタ(115200bps)を開きながらリモコンの
//      各ボタンを押すと実際のIRコードが表示されるので、handleButton() の case に
//      自分のリモコンのコードを書き足していく。

#include <WiFi.h>
#include <WebServer.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <Adafruit_NeoPixel.h>
#include "wifi_config.h"

constexpr uint8_t IR_RECV_PIN = 27;
constexpr uint8_t LED_PIN = 26;
constexpr uint16_t NUM_LEDS = 1;

IRrecv irrecv(IR_RECV_PIN);
decode_results results;
Adafruit_NeoPixel pixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

String lastCodeHex = "-";
uint32_t currentColor = 0x000000;

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  currentColor = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// シリアルモニタで確認した実際のボタンコードに置き換えてください。
void handleButton(uint32_t code) {
  switch (code) {
    case 0xFFFFFFFF:  // NECのリピートコードは無視
      break;
    default:
      // 未登録のボタン。とりあえず白にして「押されたこと」だけ分かるようにする。
      setColor(255, 255, 255);
      break;
  }
}

void handleRoot() {
  String html = "<html><body><h1>Presence Light</h1>";
  html += "<p>Last IR code: 0x" + lastCodeHex + "</p>";
  html += "<p>Current color: #" + String(currentColor, HEX) + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  pixel.begin();
  setColor(0, 0, 0);

  irrecv.enableIRIn();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  if (irrecv.decode(&results)) {
    lastCodeHex = String((uint32_t)results.value, HEX);
    Serial.print("IR code: 0x");
    Serial.println(lastCodeHex);
    handleButton((uint32_t)results.value);
    irrecv.resume();
  }
  server.handleClient();
}
