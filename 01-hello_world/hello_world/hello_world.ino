#include <M5StickCPlus.h>

uint32_t counter = 0;

void setup() {
  M5.begin();                 // init LCD, power, Serial(115200)
  Serial.begin(115200);

  M5.Lcd.setRotation(3);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(GREEN, BLACK);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(10, 40);
  M5.Lcd.print("hello world");

  Serial.printf("\n=== M5StickC Plus boot ok ===\n");
}

void loop() {
  Serial.printf("hello world %lu\n", counter++);
  delay(1000);
}
