// oled_i2c_scan.ino
//
// 診断用スケッチ: ideaspark ESP32+OLED ボードの実際のI2C(SDA/SCL)ピンを
// 特定するために、複数の候補ピアで順番にI2Cスキャンを行いシリアルに出力する。
// color_memory_game.ino の OLED_SDA_PIN / OLED_SCL_PIN を確定させるためのもの。
//
// 各ステップでこまめに Serial.flush() しているので、途中でフリーズしても
// どのピアで止まったかがシリアルモニタに残る。I2Cバスがスタックした場合に
// 無限ハングしないよう Wire.setTimeOut() でタイムアウトを設定している。

#include <Wire.h>

struct PinPair {
  int sda;
  int scl;
};

PinPair candidates[] = {
    {5, 4},  {4, 5},   {4, 15}, {15, 4}, {21, 22}, {22, 21},
    {14, 2}, {2, 14},  {23, 19}, {19, 23}, {13, 14}, {14, 13},
};
constexpr int NUM_CANDIDATES = sizeof(candidates) / sizeof(candidates[0]);

void scanPins(int sda, int scl) {
  Serial.printf("[trying SDA=%d SCL=%d] ", sda, scl);
  Serial.flush();

  Wire.begin(sda, scl);
  Wire.setTimeOut(200);  // I2Cバスが固まっても200msで諦める
  delay(50);

  bool found = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("FOUND 0x%02X  ", addr);
      Serial.flush();
      found = true;
    }
  }
  if (!found) {
    Serial.println("-- no device");
  } else {
    Serial.println();
  }
  Serial.flush();
  Wire.end();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("BOOT OK - oled_i2c_scan starting");
  Serial.flush();

  Serial.println("=== I2C pin scan start ===");
  Serial.flush();
  for (int i = 0; i < NUM_CANDIDATES; i++) {
    scanPins(candidates[i].sda, candidates[i].scl);
    delay(100);
  }
  Serial.println("=== I2C pin scan done ===");
  Serial.flush();
}

void loop() {
  delay(1000);
  Serial.println("(idle, reset board to scan again)");
}
