# 06 acremote

日立冷氣紅外線遙控，只做開和關。

**實機驗證通過。**

## 用法

| 操作 | 動作 |
|---|---|
| BtnA | **開**（冷氣模式 25°C 自動風）|
| BtnB | **關** |
| 長按 BtnB 1 秒 | 換下一個日立協定變體 |

## 冷氣遙控器和電視遙控器不一樣

**冷氣遙控器不送「電源碼」。** 每按一次鍵，它送出的是**整台機器的完整狀態**
—— 電源、模式、溫度、風速、擺葉全部打包成一個 100~300 bit 的長訊框。

所以沒有單獨的「開關碼」可以複製，必須用正確的品牌協定產生完整訊框。

好處是**不會有狀態不同步的問題**：BtnA 一定是開、BtnB 一定是關，
不像電視那樣按同一顆切換。

## 五種訊框變體

日立在 IRremoteESP8266 裡有 5 種訊框長度，對應不同年代機種：

| # | 協定 | 長度 |
|---|---|---|
| 1 | `HITACHI_AC` | 224 bit（經典款）|
| 2 | `HITACHI_AC1` | 104 bit |
| 3 | `HITACHI_AC424` | 424 bit（預設）|
| 4 | `HITACHI_AC344` | 344 bit |
| 5 | `HITACHI_AC264` | 264 bit |

## 沒有紅外線接收器，怎麼找出是哪一種

**M5StickC Plus 只有 IR 發射，沒有接收。** 所以無法錄下原廠遙控器的訊號來比對，
只能逐一試。

送 `A` 指令會**自動把 5 種各發一次**（間隔 2 秒），螢幕顯示當下在送第幾個：

```powershell
$sp = New-Object System.IO.Ports.SerialPort COM3,115200,'None',8,'One'
$sp.Open(); $sp.Write('A'); Start-Sleep 12; $sp.ReadExisting(); $sp.Close()
```

對著冷氣跑一次，**注意它在第幾個「嗶」一聲**，然後長按 BtnB 切到那個。

## Serial 指令

| 指令 | 動作 |
|---|---|
| `1` / `0` | 開 / 關 |
| `v` | 下一個變體 |
| `A` | 掃描全部變體（送開）|
| `O` | 掃描全部變體（送關）|
| `s` | 狀態 |

## 發射距離

**約 1~3 公尺，而且要對得相當準。** IR LED 由 GPIO9 直接驅動，電流小。

測試時先靠近一點（1 公尺內），對準冷氣的接收窗（通常在顯示面板附近）。
確定協定對了之後再測實際距離。

## 建置

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_stickc_plus acremote
arduino-cli upload -p COM3 \
  --fqbn "m5stack:esp32:m5stack_stickc_plus:UploadSpeed=115200" acremote
```

需要的函式庫：`M5StickCPlus`、`IRremoteESP8266`

## 換成別的品牌

IRremoteESP8266 支援近百種冷氣協定。把 `sendPower()` 裡的 `IRHitachiAc*`
換成對應的類別即可（`IRDaikinESP`、`IRPanasonicAc`、`IRCoolixAC`…）。

注意 **`convertMode()` / `convertFan()` 在某些類別是虛擬成員函式**，
不能用類別名呼叫，要用物件（見根目錄 README 的 ISSUE #6）。

## 五種都沒反應怎麼辦

那就需要加一顆**紅外線接收器**才能往下走（M5 的 IR Unit 接 Grove 孔，同時有收發）。
有接收器就能錄下原廠遙控器的原始波形直接重播，那是一定會成功的做法。
