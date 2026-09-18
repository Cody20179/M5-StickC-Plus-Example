# 02 dashboard

單頁儀表：時間、溫度、電池、振動一次顯示。
這是 [`07-multiclock`](../07-multiclock) 的前身，想要單純一點的可以用這支。

## 畫面

240×135 橫向，`TFT_eSprite` 雙緩衝，10 fps。

| 區塊 | 內容 |
|---|---|
| 時間 | BM8563 RTC，7-seg 字型 `HH:MM` + 秒數 + 日期 |
| 溫度 | MPU6886 晶片溫度（**不是室溫**）|
| 電池 | AXP192 電壓、電流、百分比、USB 充電偵測 |
| 振動 | 即時 RMS（mg）+ 峰值 + 長條圖 |

電量顏色：`>50%` 綠 · `20~50%` 黃 · `<20%` 紅

## 按鍵

| 按鍵 | 動作 |
|---|---|
| BtnA | 歸零振動峰值 |
| BtnB | 背光 100 → 75 → 50 → 25 % |

## Serial 輸出

每秒一行 CSV，可直接導到檔案做記錄：

```
time,tempC,batV,batPct,vibRms_mg,vibPeak_mg
14:25:07,50.5,4.19,100,8,13
14:25:08,50.0,4.19,100,6,13
```

## 振動怎麼算的

1. 加速度計 **200 Hz** 取樣
2. 取三軸向量長度 `|a|`
3. **單極點高通**濾掉重力：基準 `gravityBaseline` 以 0.02 的係數慢速追隨 `|a|`，差值就是動態分量
4. 250 ms 時間窗內做 **RMS**，轉成 mg

靜置噪訊底實測約 **6~11 mg**，拿起來晃很容易破 100 mg。

高通是必要的：不濾掉的話 1 g 的重力會完全淹沒訊號，而且裝置一換姿勢讀值就跳。

## 建置

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus dashboard
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" dashboard
```

需要的函式庫：`M5StickCPlus`

## 注意

**溫度是晶片溫度，不是環境溫度。** 實測 49~52°C，ESP32 自身發熱會拉高 20~25°C。
要量室溫請從 Grove 孔外接感測器（例如 ENV III Unit）。詳見根目錄 README 的 ISSUE #7。
