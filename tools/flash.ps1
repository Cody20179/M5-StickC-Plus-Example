<#
.SYNOPSIS
  M5StickC Plus 燒錄工具。選一個範例或自己的 .ino，選 COM 埠，編譯並燒進去。

.EXAMPLE
  .\flash.ps1
      全互動：列出範例和 COM 埠讓你挑。

.EXAMPLE
  .\flash.ps1 -Sketch 7 -Port COM3
      直接燒第 7 個範例。

.EXAMPLE
  .\flash.ps1 -Sketch C:\my\blink.ino -Monitor
      燒自己的檔案，燒完直接開序列埠監看。

.EXAMPLE
  .\flash.ps1 -List
      只列出可用的範例和 COM 埠，不燒錄。
#>
[CmdletBinding()]
param(
    # 範例編號 (1-7)、範例名稱 (multiclock)、資料夾路徑、或單一 .ino 檔路徑
    [string]$Sketch,

    # 目標序列埠，例如 COM3。省略則互動選擇；只有一個埠時自動採用
    [string]$Port,

    # 上傳速率。這塊板子的 FTDI 線在更高速率會斷線，預設 115200
    [int]$Baud = 115200,

    # 燒完直接開序列埠監看
    [switch]$Monitor,

    # 只編譯不燒錄
    [switch]$CompileOnly,

    # 只列出範例與可用埠
    [switch]$List
)

# 不能用 'Stop'：arduino-cli 把進度寫到 stderr，PowerShell 5.1 會把原生程式的
# stderr 包成 ErrorRecord，在 Stop 模式下即使 exit code 是 0 也會中斷腳本。
# 原生指令一律看 $LASTEXITCODE，cmdlet 需要中斷的地方用 throw。
$ErrorActionPreference = 'Continue'
$FQBN = 'm5stack:esp32:m5stack_stickc_plus'
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Write-Head($t) { Write-Host ""; Write-Host $t -ForegroundColor Cyan }
function Write-Ok($t)   { Write-Host $t -ForegroundColor Green }
function Write-Warn2($t){ Write-Host $t -ForegroundColor Yellow }
function Write-Err($t)  { Write-Host $t -ForegroundColor Red }

# ---------------------------------------------------------------- arduino-cli
function Find-ArduinoCli {
    $c = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @(
        (Join-Path $PSScriptRoot 'arduino-cli.exe'),
        (Join-Path $RepoRoot 'arduino-cli.exe'),
        "$env:LOCALAPPDATA\Programs\arduino-cli\arduino-cli.exe",
        "$env:ProgramFiles\arduino-cli\arduino-cli.exe"
    )) { if (Test-Path $p) { return $p } }
    return $null
}

function Install-ArduinoCli {
    $dest = $PSScriptRoot
    $zip  = Join-Path $env:TEMP 'arduino-cli.zip'
    Write-Warn2 "找不到 arduino-cli，正在下載到 $dest ..."
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing `
        -Uri 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip' `
        -OutFile $zip
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    Remove-Item $zip -Force
    $exe = Join-Path $dest 'arduino-cli.exe'
    if (-not (Test-Path $exe)) { throw "下載後仍找不到 arduino-cli.exe" }
    Write-Ok "已安裝：$exe"
    return $exe
}

# --------------------------------------------------------------------- 序列埠
function Get-Ports {
    $names = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    $info  = @{}
    try {
        Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.Name -match '\((COM\d+)\)' } |
            ForEach-Object {
                if ($_.Name -match '\((COM\d+)\)') {
                    $info[$Matches[1]] = ($_.Name -replace '\s*\(COM\d+\)', '')
                }
            }
    } catch { }

    $out = @()
    foreach ($n in $names) {
        $desc = '未知裝置'
        if ($info.ContainsKey($n)) { $desc = $info[$n] }
        $out += [pscustomobject]@{ Port = $n; Device = $desc }
    }
    return $out
}

# --------------------------------------------------------------------- 範例表
function Get-Examples {
    $out = @()
    Get-ChildItem $RepoRoot -Directory |
        Where-Object { $_.Name -match '^\d\d-' } |
        Sort-Object Name |
        ForEach-Object {
            $ino = Get-ChildItem $_.FullName -Filter *.ino -Recurse -File |
                   Select-Object -First 1
            if ($ino) {
                $out += [pscustomobject]@{
                    Index  = $out.Count + 1
                    Name   = $ino.BaseName
                    Folder = $ino.Directory.FullName
                    Group  = $_.Name
                }
            }
        }
    return $out
}

# ------------------------------------------------------ sketch 路徑正規化
# arduino-cli 要求「主檔名 == 資料夾名」。使用者指定的 .ino 若不符合，
# 就複製到暫存區湊成合法的 sketch 目錄，而不是直接失敗。
function Resolve-SketchFolder([string]$spec, $examples) {
    if ([string]::IsNullOrWhiteSpace($spec)) { return $null }

    # 純數字 -> 範例編號
    if ($spec -match '^\d+$') {
        $i = [int]$spec
        if ($i -ge 1 -and $i -le $examples.Count) { return $examples[$i - 1].Folder }
        throw "範例編號 $i 超出範圍 (1-$($examples.Count))"
    }

    # 名稱比對
    $m = $examples | Where-Object { $_.Name -eq $spec -or $_.Group -eq $spec }
    if ($m) { return ($m | Select-Object -First 1).Folder }

    if (-not (Test-Path $spec)) { throw "找不到：$spec" }
    $item = Get-Item $spec

    if ($item.PSIsContainer) {
        $ino = Get-ChildItem $item.FullName -Filter *.ino -File | Select-Object -First 1
        if (-not $ino) { throw "資料夾裡沒有 .ino：$spec" }
        if ($ino.BaseName -eq $item.Name) { return $item.FullName }
        $item = $ino   # 名稱不符，往下走複製流程
    }

    if ($item.Extension -ne '.ino') { throw "不是 .ino 檔：$spec" }

    if ($item.Directory.Name -eq $item.BaseName) { return $item.Directory.FullName }

    $tmp = Join-Path $env:TEMP ("m5flash\" + $item.BaseName)
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    Copy-Item $item.FullName (Join-Path $tmp ($item.BaseName + '.ino')) -Force
    # 同資料夾的其他 sketch 檔案一起帶過去
    Get-ChildItem $item.Directory -File |
        Where-Object { $_.Extension -in '.h', '.hpp', '.c', '.cpp' } |
        ForEach-Object { Copy-Item $_.FullName $tmp -Force }
    Write-Warn2 "主檔名與資料夾名不符，已複製到暫存 sketch：$tmp"
    return $tmp
}

# ------------------------------------------------------------------ 序列監看
function Start-Monitor([string]$port, [int]$baud) {
    Write-Head "監看 $port @ $baud   (Ctrl+C 離開)"
    $sp = New-Object System.IO.Ports.SerialPort $port, $baud, 'None', 8, 'One'
    $sp.ReadTimeout = 200
    $sp.Open()
    try {
        while ($true) {
            try { $s = $sp.ReadExisting() } catch { $s = '' }
            if ($s) { Write-Host -NoNewline $s }
            Start-Sleep -Milliseconds 40
        }
    } finally {
        if ($sp.IsOpen) { $sp.Close() }
        $sp.Dispose()
    }
}

# =============================================================== 主流程

# @() 是必要的：PowerShell 回傳單一元素的陣列時會自動解包成純量，
# 那樣 $ports.Count 會是 $null，"只有一個埠就自動選用" 的判斷會整個失效。
$examples = @(Get-Examples)
$ports    = @(Get-Ports)

if ($List) {
    Write-Head "範例"
    $examples | ForEach-Object { "  [{0}] {1,-12} {2}" -f $_.Index, $_.Name, $_.Group }
    Write-Head "序列埠"
    if ($ports.Count -eq 0) { Write-Warn2 "  (沒有偵測到序列埠)" }
    else { $ports | ForEach-Object { "  {0,-6} {1}" -f $_.Port, $_.Device } }
    return
}

$cli = Find-ArduinoCli
if (-not $cli) { $cli = Install-ArduinoCli }

# ---- 選 sketch ----
$folder = $null
if ($Sketch) {
    $folder = Resolve-SketchFolder $Sketch $examples
} else {
    Write-Head "要燒哪一個？"
    $examples | ForEach-Object { "  [{0}] {1,-12} {2}" -f $_.Index, $_.Name, $_.Group }
    Write-Host "  [c] 自訂 .ino 檔路徑"
    $sel = Read-Host "選擇"
    if ($sel -eq 'c') {
        $p = Read-Host "  .ino 檔完整路徑"
        $folder = Resolve-SketchFolder $p.Trim('"') $examples
    } else {
        $folder = Resolve-SketchFolder $sel $examples
    }
}
if (-not $folder) { Write-Err "沒有選到 sketch"; return }
Write-Ok "sketch : $folder"

# ---- 選埠 ----
if (-not $CompileOnly) {
    if (-not $Port) {
        if ($ports.Count -eq 0) {
            Write-Err "沒有偵測到任何序列埠。板子插上了嗎？驅動裝了嗎？"
            return
        } elseif ($ports.Count -eq 1) {
            $Port = $ports[0].Port
            Write-Ok "port   : $Port ($($ports[0].Device))  <- 唯一的埠，自動選用"
        } else {
            Write-Head "要用哪個序列埠？"
            for ($i = 0; $i -lt $ports.Count; $i++) {
                "  [{0}] {1,-6} {2}" -f ($i + 1), $ports[$i].Port, $ports[$i].Device
            }
            $sel = Read-Host "選擇"
            $idx = [int]$sel - 1
            if ($idx -lt 0 -or $idx -ge $ports.Count) { Write-Err "選擇無效"; return }
            $Port = $ports[$idx].Port
        }
    }
    Write-Ok "port   : $Port @ $Baud"
}

# ---- 編譯 ----
Write-Head "編譯中..."
& $cli compile --fqbn $FQBN $folder
if ($LASTEXITCODE -ne 0) {
    Write-Err "編譯失敗。缺函式庫的話試試："
    Write-Host "  arduino-cli lib install M5StickCPlus ESP32BLECombo IRremoteESP8266 arduinoFFT"
    return
}
Write-Ok "編譯成功"

if ($CompileOnly) { return }

# ---- 燒錄 ----
Write-Head "燒錄到 $Port ..."
& $cli upload -p $Port --fqbn "${FQBN}:UploadSpeed=$Baud" $folder
if ($LASTEXITCODE -ne 0) {
    Write-Err "燒錄失敗。"
    Write-Host "  - 序列埠被別的程式佔用了嗎？（監看視窗、Arduino IDE）"
    Write-Host "  - 這塊板子的 FTDI 線在 460800 以上會斷線，$Baud 以外的速率不保證可用"
    return
}
Write-Ok "燒錄完成"

if ($Monitor) { Start-Monitor $Port $Baud }
