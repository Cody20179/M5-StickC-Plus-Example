<#
.SYNOPSIS
  把電腦的時間寫進 M5StickC Plus 的 BM8563 RTC。

.DESCRIPTION
  韌體的 seedRtcFromBuildTime() 只在時間明顯不合理（年份不在 2024-2099）時才
  設定 RTC —— 不然每次燒錄都會把走得好好的時鐘覆蓋成編譯當下的時間，燒完就
  已經慢了。代價是：一旦時間偏掉，韌體不會自己修正。

  而每塊板子有自己的 BM8563 和自己的備援電源，彼此之間沒有任何關聯，
  所以兩塊板子的時間本來就沒有理由一致。這支腳本是唯一會真正對時的東西。

  需要韌體支援 'T' 指令（07-multiclock 有）。

.EXAMPLE
  .\settime.ps1
.EXAMPLE
  .\settime.ps1 -Port COM5
.EXAMPLE
  .\settime.ps1 -Check          # 只比對誤差，不修改
#>
[CmdletBinding()]
param(
    [string]$Port,
    [int]$Baud = 115200,
    [switch]$Check
)

$ErrorActionPreference = 'Continue'

if (-not $Port) {
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    if ($ports.Count -eq 0) { Write-Host "沒有偵測到序列埠" -ForegroundColor Red; return }
    $Port = $ports[0]
    if ($ports.Count -gt 1) {
        Write-Host "偵測到多個埠：$($ports -join ', ')，使用 $Port（可用 -Port 指定）" -ForegroundColor Yellow
    }
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 200
# 不要動 DTR/RTS，否則會重置板子
$sp.DtrEnable = $false
$sp.RtsEnable = $false

try {
    $sp.Open()
} catch {
    Write-Host "無法開啟 $Port：$($_.Exception.Message)" -ForegroundColor Red
    return
}

function Drain([int]$ms) {
    $acc = ''
    $t = Get-Date
    while (((Get-Date) - $t).TotalMilliseconds -lt $ms) {
        $s = $sp.ReadExisting()
        if ($s) { $acc += $s }
        Start-Sleep -Milliseconds 40
    }
    return $acc
}

try {
    Start-Sleep -Milliseconds 800
    $sp.DiscardInBuffer()

    # 先問現在幾點。電腦時間必須在「rtc 那行剛抵達」的瞬間記下，
    # 不能等整個緩衝讀完 —— 那會固定多算掉一整個 drain 的時間，
    # 讓一個其實很準的時鐘看起來慢了一秒多。
    $sp.Write('s')
    $before = ''
    $pcAtRead = $null
    $t = Get-Date
    while (((Get-Date) - $t).TotalMilliseconds -lt 2000) {
        $s = $sp.ReadExisting()
        if ($s) {
            $before += $s
            if (-not $pcAtRead -and $before -match '#\s+rtc\s') { $pcAtRead = Get-Date }
        }
        if ($pcAtRead -and $before -match "`n.*`n") { break }
        Start-Sleep -Milliseconds 20
    }
    if (-not $pcAtRead) { $pcAtRead = Get-Date }

    $rtcLine = ($before -split "`n" | Where-Object { $_ -match '#\s+rtc\s' } | Select-Object -First 1)
    if ($rtcLine -and $rtcLine -match '(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})') {
        $rtc = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss', $null)
        $delta = ($rtc - $pcAtRead).TotalSeconds
        Write-Host ("板子 : {0:yyyy-MM-dd HH:mm:ss}" -f $rtc)
        Write-Host ("電腦 : {0:yyyy-MM-dd HH:mm:ss}" -f $pcAtRead)
        $col = 'Green'
        if ([math]::Abs($delta) -gt 2) { $col = 'Yellow' }
        if ([math]::Abs($delta) -gt 60) { $col = 'Red' }
        # RTC 只回報到秒，所以 ±1 秒是量測本身的解析度極限，不是真的偏差
        Write-Host ("誤差 : {0:+0.0;-0.0;0} 秒  (±1 秒是 RTC 的解析度)" -f $delta) `
            -ForegroundColor $col
    } else {
        Write-Host "讀不到 RTC（韌體有支援 's' 指令嗎？）" -ForegroundColor Yellow
    }

    if ($Check) { return }

    # 卡在整秒邊界再送，把 PowerShell 的排程抖動降到最低
    $now = Get-Date
    $target = $now.AddSeconds(1)
    $target = $target.AddMilliseconds(-$target.Millisecond)
    $wait = ($target - (Get-Date)).TotalMilliseconds - 15
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }

    $stamp = $target.ToString('yyyyMMddHHmmss')
    $sp.Write("T$stamp`n")
    Write-Host ""
    Write-Host (Drain 1500).Trim()
    Write-Host ""
    Write-Host "已對時" -ForegroundColor Green
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}
