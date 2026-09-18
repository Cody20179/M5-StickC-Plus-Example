# 07 multiclock

五頁儀表，本專案的主要成品。

**完整說明與實機截圖在[根目錄的 README](../README.md)。**

## 快速參考

`BtnA` 翻頁 · `BtnB` 執行當頁動作 · 長按 `BtnB` 切換背光

| 頁 | 內容 |
|---|---|
| 1 CLOCK | 時分秒、年月日、星期、電池 |
| 2 ENV | 晶片溫度、電池明細、振動 + 累計量 |
| 3 IMU | 六軸原始值、姿態角、水平儀氣泡 |
| 4 SOUND | dBFS 音量表、主頻率、9 段八度頻譜 |
| 5 INFO | 系統資訊 |

Serial：`n` 翻頁 · `1`~`5` 跳頁 · `b` 亮度 · `r` 歸零 · `s` 狀態 ·
**`C` 螢幕擷取** · **`T` 對時**

## 對時

`T` 後面接 `YYYYMMDDHHMMSS`，例如 `T20260918212018`。
會印出修改前後的時間與差值，方便量測兩塊板子的漂移。

實務上用 [`../tools/settime.ps1`](../tools/settime.ps1) 就好，它會自動帶入電腦時間。

韌體開機時的 `seedRtcFromBuildTime()` **只在年份不合理時才設定 RTC** ——
否則每次燒錄都會把時鐘覆蓋成編譯時間。代價是時間偏掉後不會自己修正，
所以對時只能靠這個指令。

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus multiclock
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" multiclock
python ../tools/screenshot.py --all
```

需要的函式庫：`M5StickCPlus`、`arduinoFFT`
