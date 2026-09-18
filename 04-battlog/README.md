# 04 battlog

電池續航記錄器。[`02-dashboard`](../02-dashboard) 加上寫入內部 flash 的資料記錄。

## 為什麼要寫 flash

**插著 USB 就測不出續航** —— USB 會一直充電，電量永遠是 100%。

所以資料不走序列埠，每筆寫進內部 flash（LittleFS，`spiffs` 分割區 896 KB）。
拔掉線讓它自己跑到沒電，再插回來把記錄倒出來。

## 測試流程

1. **充飽** —— 等 `USB CHG` 的電流掉到接近 0
2. **拔掉 USB** —— 板子偵測到 VBUS 消失，自動記一筆 `# UNPLUGGED`、歸零庫侖計，
   螢幕左下角開始跑 `DISCHG 00:00:00`
3. **放著跑到自動關機**
4. **插回 USB**，送 `d` 倒出 CSV

全程不用操作，拔插就是開始和結束。

## CSV 格式

```
rtc,elapsed_s,batV,batI_mA,batPct,vbusV,usb,tempC,coulomb_mAh,brightness
2026-09-18 18:00:11,0,3.828,85.5,67.4,5.05,1,54.7,0.36,100
```

| 欄位 | 意義 |
|---|---|
| `rtc` | RTC 時間戳 |
| `elapsed_s` | 放電開始後經過秒數 |
| `batV` / `batI_mA` / `batPct` | 電池電壓、電流、估計百分比 |
| `vbusV` / `usb` | USB 電壓、是否接著 |
| `tempC` | 晶片溫度 |
| `coulomb_mAh` | **庫侖計累計放電量** |
| `brightness` | 當下背光百分比 |

每 10 秒一筆。狀態變化（開機、拔插、調亮度）會插入 `#` 開頭的註記行。

## 庫侖計

`M5.Axp.EnableCoulombcounter()` 在 setup 開啟，拔 USB 的瞬間歸零。
AXP192 會累計實際流出的電荷，所以除了「撐了多久」，還能得到
**「這顆電池實際放出多少 mAh」**。

M5StickC Plus 標稱 120 mAh，實測值明顯偏低就代表電池老化了。

## 寫檔策略

每筆**開檔、寫入、關檔**，不是保持開啟：

```cpp
File f = LittleFS.open(LOG_PATH, "a");
f.println(line);
f.close();
```

比較慢，但電池耗盡瞬間斷電時，**最多只掉最後一筆，不會整個檔案損毀**。
這是這支韌體的重點 —— 測到一半資料全毀就白測了。

## Serial 指令

| 指令 | 動作 |
|---|---|
| `d` | 倒出完整記錄 |
| `c` | 清空記錄 |
| `s` | 目前狀態 |
| `+` / `-` | 調亮 / 調暗 |

## 背光主宰續航

`BtnB` 可在測試中途調整，每次調整都會記一筆 `# BRIGHTNESS`，事後分析看得出來。

**要比較不同亮度的續航，請分幾次測、每次固定一個亮度**，結果才乾淨。

粗估：120 mAh 電池、LCD 全亮 + ESP32 不休眠，大約 **1~1.5 小時**。
要測「撐一整天」那種情境需要另外做深度睡眠版本。

## 建置

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus battlog
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" battlog
```

需要的函式庫：`M5StickCPlus`（LittleFS 隨 core 提供）

## 注意

**記錄不會被燒錄洗掉。** 資料在 `spiffs` 分割區（`0x310000`），
燒錄只覆蓋 `app0`（`0x10000`），兩者位址不重疊。
換韌體再換回來，`/batt.csv` 還在。
