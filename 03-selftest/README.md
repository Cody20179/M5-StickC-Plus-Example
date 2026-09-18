# 03 selftest

硬體自檢。把板子上每個元件都實際跑一遍，確認沒有壞掉。

拿到新板子、或懷疑某個感測器有問題時先燒這支。

## 五個頁面

`BtnA` 翻頁 · `BtnB` 執行當頁動作

| 頁 | 內容 |
|---|---|
| 1 I2C SCAN | 掃描內部匯流排（SDA21/SCL22）與 Grove 埠（SDA32/SCL33），列出所有回應的位址並標註已知型號 |
| 2 SYSTEM | 晶片型號、核心數、頻率、Flash、堆積、MAC、重置原因、SDK 版本 |
| 3 WIFI | 掃描周遭 AP，顯示數量與訊號最強的幾個 |
| 4 MIC | SPM1423 PDM 麥克風即時音量條 |
| 5 OUTPUTS | 蜂鳴器、紅色 LED、紅外線 LED 依序測試 |

## Serial 指令

不用碰裝置就能跑完整套：

| 指令 | 動作 |
|---|---|
| `n` | 下一頁 |
| `i` | 重掃 I2C |
| `w` | 掃描 WiFi |
| `o` | 輸出測試（蜂鳴器 + LED + IR）|
| `b` | 只測蜂鳴器 |
| `m` | 讀麥克風 |
| `s` | 系統報告 |
| `?` | 說明 |

`tools/run_selftest.ps1` 會自動依序送出並收集結果。

## 實機驗證結果

```
[I2C] internal (SDA21/SCL22): 3 device(s)
      0x34  AXP192 PMU
      0x51  BM8563 RTC
      0x68  MPU6886 IMU
[I2C] Grove    (SDA32/SCL33): 0 device(s)

[SYS]
      chip        : ESP32-PICO-D4 rev 100, 2 core(s) @ 240 MHz
      flash       : 4194304 bytes @ 80000000 Hz
      heap        : 212948 free / 327420 total
      psram       : 0 bytes
      sdk         : v5.5.4

[MIC] SPM1423 PDM init OK

[IMU]
      accel 0.00 0.04 1.09 g       <- Z 軸約 1g，平放姿態正確
      gyro  -4.9 -12.5 -9.4 dps    <- 零偏，見 ISSUE #8
      temp  55.1 C (die temp, not ambient)

[AXP192]
      bat   4.19 V  22 mA
      vbus  5.07 V  130 mA
```

WiFi 實測掃到 17 個 AP，紅外線、LED、蜂鳴器皆正常。

## 蜂鳴器的驅動方式

**這支不用 `M5.Beep`** —— 那個在 arduino-esp32 3.x 上完全沒聲音（函式庫 bug，詳見根目錄
README 的 ISSUE #1）。改成直接驅動 GPIO2：

```cpp
void buzzerTone(uint32_t freq, uint32_t ms) {
  ledcAttach(PIN_BUZZER, freq, 10);
  ledcWriteTone(PIN_BUZZER, freq);   // 傳腳位，不是通道
  delay(ms);
  ledcWriteTone(PIN_BUZZER, 0);
  ledcDetach(PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);     // 閒置拉低，不要浮接
}
```

輸出測試會掃 2k / 3k / 4k / 5k Hz 四個頻率 —— 被動式蜂鳴器有共振頻率，
偏離時音量會小很多，掃頻才聽得出哪個最大聲。

## 紅外線怎麼確認

IR LED 肉眼看不見。**拿手機相機對著板子頂端**，送 `o` 指令時會看到紫白色閃爍。

## MAC 位址的讀法

`WiFi.macAddress()` 在 WiFi 驅動啟動前會回傳全 0。這支改讀 efuse：

```cpp
uint64_t m = ESP.getEfuseMac();   // 位元組順序是反的
```

## 建置

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus selftest
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" selftest
```

需要的函式庫：`M5StickCPlus`（WiFi、ESP_I2S 隨 core 提供）
