# 01 hello_world

最小範例。用來確認工具鏈、燒錄流程、序列埠都通了，再往下做別的。

## 行為

- Serial 每秒印一行 `hello world <計數>`
- LCD 顯示綠色大字 `hello world`

```
=== M5StickC Plus boot ok ===
hello world 0
hello world 1
hello world 2
```

## 程式

```cpp
#include <M5StickCPlus.h>

uint32_t counter = 0;

void setup() {
  M5.begin();                 // 初始化 LCD、電源、Serial
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
```

## 建置

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus hello_world
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" hello_world
```

需要的函式庫：`M5StickCPlus`

## 備註

開機會出現兩行 GPIO 錯誤：

```
E gpio: gpio_pullup_en(85): GPIO number error (input-only pad has no internal PU)
```

這是函式庫對 GPIO39（BtnB）開內部上拉造成的，該腳位是 input-only。
不影響功能，詳見根目錄 README 的 ISSUE #3。
