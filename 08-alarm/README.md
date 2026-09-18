# 08 alarm

時鐘畫面 + 每隔固定分鐘數讓蜂鳴器響、紅色 LED 閃。

預設 **每 5 分鐘**（整點、05 分、10 分…），間隔可改，設定存在 NVS，重開機不會忘記。

## 畫面

```
 21:49                    13
 2026-09-18  FRI
 ──────────────────────────────
 EVERY                       ON
 5                 next in 0m 47s
  min
                          75%
 A=on/off  B=test
```

響的時候時間會轉成紅色，右上顯示 `RINGING`。

## 按鍵

| 按鍵 | 動作 |
|---|---|
| BtnA | 提醒開 / 關 |
| BtnB | 試響 |
| 響的時候按任一鍵 | 停止（**不會**順便把提醒關掉）|

## Serial 指令

| 指令 | 動作 |
|---|---|
| `s` | 狀態 |
| `t` | 試響 |
| `e` | 開 / 關 |
| `I5` | 設定間隔為 5 分鐘（`I` 後接 1~2 位數，1~60）|
| `T20260918214914` | 對時 |

也可以用 [`../tools/settime.ps1`](../tools/settime.ps1) 對時。

## 幾個實作上的取捨

### 蜂鳴器不能用 `M5.Beep`

函式庫的 `SPEAKER::tone()` 把「通道」傳給 `ledcWriteTone()`，但 arduino-esp32 3.x
的那個參數已經改成「腳位」，結果是對 GPIO0 寫入，而蜂鳴器接在 GPIO2 ——
**永遠不會有聲音，而且沒有任何錯誤訊息**。直接驅動腳位：

```cpp
ledcAttach(PIN_BUZZER, freq, 10);
ledcWriteTone(PIN_BUZZER, freq);   // 傳腳位
```

用 **4 kHz** 是因為那接近這顆被動式蜂鳴器的共振點，同樣的驅動電壓下最大聲。

詳見根目錄 README 的 ISSUE #1。

### 響鈴是非阻塞的

用 `millis()` 推進的狀態機，不是 `delay()` 迴圈：

```cpp
if ((ringStep % 2) == 0) { buzzerOn(4000); digitalWrite(PIN_LED, LOW);  ringNextMs = now + 150; }
else                     { buzzerOff();    digitalWrite(PIN_LED, HIGH); ringNextMs = now + 180; }
```

這樣響的時候**時鐘還會繼續走、按鍵還能停它**。用 `delay()` 的話畫面會凍住約一秒，
而且那段時間按鍵完全沒反應。

### 同一分鐘只響一次

用「日期 + 當天第幾分鐘」當鍵值去重：

```cpp
long key = minuteKey();          // ymd * 1440 + hour * 60 + minute
if (key == lastFiredKey) return;
```

主迴圈每秒跑幾十次，不去重的話那一分鐘內會連續觸發。用日期而不只是分鐘，
是為了讓隔天的同一分鐘還會正常響。

開機時和對時後，如果正好落在該響的那一分鐘，會先把鍵值設成當下值 ——
否則插上電或對完時就會莫名其妙炸一次。

### 數字參數要用 `peek()` 判斷結束

```cpp
int c = Serial.peek();
if (c >= '0' && c <= '9')          buf[n++] = (char)Serial.read();
else if (c == '\r' || c == '\n') { Serial.read(); if (n > 0) break; }
else                               break;   // 別人的指令，留著
```

用 `Serial.read()` 去判斷結束會**把下一個指令的第一個字元吃掉**。
實測踩過：連續送 `I5` 再送 `T2026...`，`I` 的讀取器在等第二位數字時吃掉了那個 `T`，
後面整串日期被主迴圈當成一堆無效指令丟棄，**對時靜默失敗、沒有任何錯誤訊息**。

## 實機驗證

把時鐘設到邊界前 5 秒，等它自己響：

```
>>> set clock to 10:04:55 (5 s before the boundary)
# RTC set to 2026-09-18 10:04:55 FRI

#   interval every 5 min  ON
#   next in  0m 04s

>>> waiting for it to fire by itself at 10:05:00
# ALARM at 10:05
# ring
```

## 建置

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus alarm
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" alarm
```

或用 [`../tools/flash.ps1`](../tools/flash.ps1)：

```powershell
.\tools\flash.ps1 -Sketch 8
```

需要的函式庫：`M5StickCPlus`（`Preferences` 隨 core 提供）

## 注意

**板子關機時不會響。** 這是 RTC 鬧鐘而不是硬體喚醒 —— BM8563 本身有中斷輸出，
但這塊板子沒有把它接到 ESP32 的喚醒腳位，所以無法做「睡到時間到再醒來」。
要長時間待機提醒的話，只能讓 ESP32 保持運作，靠電池約撐 1~1.5 小時。
