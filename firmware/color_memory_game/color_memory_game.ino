// color_memory_game.ino
//
// 手持ちの部品で作る色記憶ゲーム (Simon Says方式):
//   - ESP32 (ideaspark WROOM-32 + OLED 開発ボード)
//   - 分解した100均RGBパック基板 (CHS005RGB-02) から流用する IR受信部 と WS2812系LED (D1)
//   - 18ボタンのIRリモコン(うち12個の色ボタンを使用)
//
// ルール:
//   ONボタンでスタート。LEDが色のシーケンスを1つずつ再生するので、
//   同じ順番でリモコンの色ボタンを押して再現する。正解するたびに
//   シーケンスが1色ずつ伸びていく。間違えるかタイムアウトするとゲームオーバー。
//   OLEDに現在のステージ数/ゲームオーバー時の到達ステージを表示する。
//   OFFボタンでいつでも中断してタイトル画面に戻る。
//   Wi-Fi/ネットワークは使わない、完全オフライン動作。
//
// 必要なライブラリ (Arduino IDE の Library Manager からインストール):
//   - IRremoteESP8266  (作者: crankyoldgit)
//   - Adafruit NeoPixel
//   - Adafruit SSD1306
//   - Adafruit GFX Library
//
// 配線 (パックの基板から3+3本を引き出してESP32のブレッドボードに接続):
//   IR受信部 (パック基板の黒い3本足の部品)  VCC -> ESP32 3V3
//   IR受信部                                GND -> ESP32 GND
//   IR受信部                                OUT -> ESP32 IR_RECV_PIN (下記で指定)
//   D1 (WS2812 LED)                         VCC -> ESP32 3V3 (実測電圧に応じて5V/VINでも可)
//   D1 (WS2812 LED)                         GND -> ESP32 GND
//   D1 (WS2812 LED)                         DIN -> ESP32 LED_PIN (下記で指定)
//   OLED はボードにオンボード実装済み(I2C)。実機のI2Cスキャンで SDA=21 SCL=22 (アドレス0x3C)
//   と確認済み(firmware/oled_i2c_scan/oled_i2c_scan.ino で検証)。
//
// セットアップ手順:
//   1. このスケッチを書き込んだ後、シリアルモニタ(115200bps)を開く
//   2. リモコンの12個の色ボタンとON/OFFボタンを1つずつ押し、表示される
//      IRコード(0x...)を COLORS[] テーブルと BTN_ON_CODE / BTN_OFF_CODE に反映する
//   3. 各ボタンの印字色に合わせて COLORS[] の r,g,b を微調整する

#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_SSD1306.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <Wire.h>

// ---- ピン設定 (実機に合わせて要確認・要調整) ----
constexpr uint8_t IR_RECV_PIN = 27;
constexpr uint8_t LED_PIN = 26;
constexpr uint16_t NUM_LEDS = 1;
constexpr int OLED_SDA_PIN = 21;  // 実機のI2Cスキャンで確認済み (SDA=21 SCL=22 -> 0x3C)
constexpr int OLED_SCL_PIN = 22;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr uint8_t OLED_ADDR = 0x3C;

// ---- リモコンのボタンコード (TODO: シリアルモニタで確認した実際の値に置き換える) ----
// 実機のシリアルモニタで確認済みの実コード
constexpr uint32_t BTN_ON_CODE = 0x1FE48B7;
constexpr uint32_t BTN_OFF_CODE = 0x1FE58A7;
constexpr uint32_t IR_REPEAT_CODE = 0xFFFFFFFF;

// 未使用だが参考: Mode=0x1FE7887, 4H=0x1FE807F, 8H=0x1FE40BF, MultiColor=0x1FEC03F

struct ColorButton {
  const char* name;
  uint32_t irCode;
  uint8_t r, g, b;
};

// 実機のシリアルモニタで確認済みの実コード。RGB値は各ボタンの印字色に近い仮値なので
// 実際の見た目に合わせて微調整してよい。
ColorButton COLORS[12] = {
    {"Red", 0x1FE20DF, 255, 0, 0},
    {"Green", 0x1FEA05F, 0, 180, 0},
    {"Blue1", 0x1FE609F, 0, 0, 255},
    {"Orange", 0x1FEE01F, 230, 120, 0},
    {"Lime", 0x1FE10EF, 150, 200, 0},
    {"Purple1", 0x1FE708F, 128, 0, 200},
    {"Yellow", 0x1FE50AF, 220, 200, 0},
    {"SkyBlue", 0x1FED827, 100, 180, 255},
    {"Pink", 0x1FE906F, 230, 0, 150},
    {"LightPink", 0x1FE30CF, 200, 150, 200},
    {"Blue2", 0x1FEB04F, 0, 80, 220},
    {"Purple2", 0x1FEF807, 90, 0, 160},
};
constexpr uint8_t NUM_COLORS = 12;

constexpr uint8_t MAX_SEQUENCE = 64;
constexpr unsigned long SHOW_ON_MS = 500;
constexpr unsigned long SHOW_GAP_MS = 250;
constexpr unsigned long FEEDBACK_MS = 200;
constexpr unsigned long INPUT_TIMEOUT_MS = 5000;

enum GameState { IDLE, SHOWING, WAITING_INPUT, GAME_OVER };

IRrecv irrecv(IR_RECV_PIN);
decode_results results;
Adafruit_NeoPixel pixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

GameState state = IDLE;
uint8_t sequence[MAX_SEQUENCE];
uint8_t sequenceLength = 0;
uint8_t showIndex = 0;
uint8_t inputIndex = 0;
unsigned long lastActionMillis = 0;
bool ledIsOn = false;

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void ledOff() { setLed(0, 0, 0); }

void drawCentered(const String& line1, const String& line2) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(line1);
  display.setCursor(0, 36);
  display.println(line2);
  display.display();
}

void showIdleScreen() { drawCentered("Color Memory Game", "Press ON to start"); }

void showStageScreen() {
  drawCentered("Stage: " + String(sequenceLength), "Watch, then repeat");
}

void showGameOverScreen() {
  drawCentered("Game Over!", "Stage reached: " + String(sequenceLength));
}

void startGame() {
  sequenceLength = 0;
  addRandomColor();
  beginShowing();
}

void addRandomColor() {
  sequence[sequenceLength] = random(0, NUM_COLORS);
  sequenceLength++;
}

void beginShowing() {
  state = SHOWING;
  showIndex = 0;
  ledOff();
  showStageScreen();
  lastActionMillis = millis();
  ledIsOn = false;
}

void beginInput() {
  state = WAITING_INPUT;
  inputIndex = 0;
  ledOff();
  lastActionMillis = millis();
}

void goGameOver() {
  state = GAME_OVER;
  ledOff();
  showGameOverScreen();
  setLed(255, 0, 0);
  delay(150);
  ledOff();
  delay(100);
  setLed(255, 0, 0);
  delay(150);
  ledOff();
  delay(100);
  setLed(255, 0, 0);
  delay(150);
  ledOff();
  lastActionMillis = millis();
}

void goIdle() {
  state = IDLE;
  ledOff();
  showIdleScreen();
}

int8_t colorIndexForCode(uint32_t code) {
  for (uint8_t i = 0; i < NUM_COLORS; i++) {
    if (COLORS[i].irCode == code) return i;
  }
  return -1;
}

void handleShowing() {
  unsigned long elapsed = millis() - lastActionMillis;
  if (ledIsOn) {
    if (elapsed >= SHOW_ON_MS) {
      ledOff();
      ledIsOn = false;
      lastActionMillis = millis();
      showIndex++;
      if (showIndex >= sequenceLength) {
        beginInput();
      }
    }
  } else {
    if (elapsed >= SHOW_GAP_MS) {
      ColorButton& c = COLORS[sequence[showIndex]];
      setLed(c.r, c.g, c.b);
      ledIsOn = true;
      lastActionMillis = millis();
    }
  }
}

void handleInputCode(uint32_t code) {
  int8_t idx = colorIndexForCode(code);
  if (idx < 0) return;  // not a color button, ignore

  lastActionMillis = millis();
  if (idx == sequence[inputIndex]) {
    ColorButton& c = COLORS[idx];
    setLed(c.r, c.g, c.b);
    delay(FEEDBACK_MS);
    ledOff();
    inputIndex++;
    if (inputIndex >= sequenceLength) {
      // round clear: brief success flash
      for (uint8_t i = 0; i < 3; i++) {
        setLed(0, 255, 0);
        delay(100);
        ledOff();
        delay(80);
      }
      addRandomColor();
      beginShowing();
    }
  } else {
    goGameOver();
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));

  pixel.begin();
  ledOff();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
  }
  display.setRotation(0);

  irrecv.enableIRIn();

  goIdle();
}

void loop() {
  if (irrecv.decode(&results)) {
    uint32_t code = (uint32_t)results.value;
    if (code != IR_REPEAT_CODE) {
      Serial.print("IR code: 0x");
      Serial.println(code, HEX);

      if (code == BTN_OFF_CODE) {
        goIdle();
      } else if (state == IDLE && code == BTN_ON_CODE) {
        startGame();
      } else if (state == WAITING_INPUT) {
        handleInputCode(code);
      } else if (state == GAME_OVER && code == BTN_ON_CODE) {
        startGame();
      }
    }
    irrecv.resume();
  }

  if (state == SHOWING) {
    handleShowing();
  } else if (state == WAITING_INPUT) {
    if (millis() - lastActionMillis >= INPUT_TIMEOUT_MS) {
      goGameOver();
    }
  }
}
