# 05 blectrl

BLE HID 控制器。以鍵盤 + 滑鼠複合裝置配對到電腦，含陀螺儀空中滑鼠。

## 配對

裝置名 **`M5Stick Ctrl`**，免 PIN。

Windows：設定 → 藍牙與其他裝置 → 新增裝置 → 藍牙 → 選 `M5Stick Ctrl`

配對後螢幕右上角從黃色 `PAIRING` 變成綠色 `LINKED`，之後會自動重連。
電池電量也會透過 BLE 回報，所以藍牙裝置清單裡看得到。

## 四種設定檔

**長按 BtnA 一秒**切換。

| 設定檔 | BtnA | BtnB |
|---|---|---|
| CLICK | ENTER | 滑鼠左鍵 |
| SLIDES | 方向鍵 → | 方向鍵 ← |
| SCROLL | 滑鼠左鍵 | 滾輪下捲 |
| AIRMOUSE | 左鍵 | 右鍵（長按 1 秒 = 凍結游標）|

## 空中滑鼠

**水平**用陀螺儀 yaw 角速度，**垂直**用加速度計傾角（搖桿式）。

### 為什麼垂直不用陀螺儀

原本兩軸都用陀螺儀，結果上下完全不能控制。用內建的手勢擷取（`T` 指令）量測後發現：

| | gx / gz | gy / gz |
|---|---|---|
| 上下手勢 | 0.148 | 0.099 |
| 左右手勢 | 0.155 | **0.239** |

`gx` 在兩個手勢裡的比例幾乎相同（0.148 vs 0.155）—— **完全沒有分辨能力**。
`gy` 反而更偏向左右。也就是說，**上下手勢沒有產生任何獨立的陀螺儀訊號**。

原因很簡單：**陀螺儀只感測旋轉，對平移完全無感**。把裝置整個平移上下，
陀螺儀什麼都看不到，接哪個軸都一樣。

改用加速度計量重力方向後，變成**絕對姿態量測**：傾斜多少度就是多少度，
不漂移、不需大動作、也不會被左右動作污染。傾著不動游標就持續移動，像搖桿。

### 陀螺儀零偏必須校正

實測靜止時 **−5 / −12 / −9 dps**。不扣掉的話游標會自己往一個方向爬走。

開機（或送 `z`）時取 300 筆平均當基準，扣除後殘差只剩 0.5 dps 以內，
再加 1.8 dps 死區，放著完全不動。

## Serial 指令

| 指令 | 動作 |
|---|---|
| `a` / `b` | 觸發 BtnA / BtnB 的動作 |
| `p` | 下一個設定檔 |
| `M` | 直接跳到 AIRMOUSE |
| `s` | 狀態 |
| `z` | 重新校正陀螺儀（放平不要動）|
| `n` | 重設傾角基準（用現在的姿勢當「不動」）|
| `V` | 切換垂直來源：`tiltX` → `tiltY` → `gyro` |
| `x` / `y` | 水平 / 垂直方向反轉 |
| `1` / `2` | 水平靈敏度 −/+ |
| `3` / `4` | 垂直增益 −/+ |
| `d` | 循環傾角死區（2→4→6→8→10°）|
| `T` | 6 秒手勢擷取，報告主導軸 |
| `f` | 凍結 / 解凍 |
| `g` | 即時陀螺儀數值 |

`tools/aircal.ps1` 是互動式的軸向量測，有倒數提示，會帶你做完整套。

## 按鍵編碼

`ESP32BLECombo::press()` 用的規則是 **`code >= 136` 時 HID usage = `code - 136`**
（和 ESP32-BLE-Keyboard 相同），ASCII 走自己的對照表，所以 `'\n'` 本身就是 Enter。

```cpp
#define KEY_RETURN      0xB0   // 176 - 136 = 0x28 Enter
#define KEY_RIGHT_ARROW 0xD7   // 215 - 136 = 0x4F
#define KEY_LEFT_ARROW  0xD8
#define KEY_PAGE_UP     0xD3
#define KEY_PAGE_DOWN   0xD6
```

要加其他鍵（Esc、F5、音量鍵）照這個規則擴充 `PROFILES` 表即可。

## 建置

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus blectrl
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" blectrl
```

需要的函式庫：`M5StickCPlus`、`ESP32BLECombo`（會一併帶入 `NimBLE-Arduino`）
