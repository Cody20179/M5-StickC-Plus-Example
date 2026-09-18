# M5StickC Plus - air mouse axis finder
#
# Works out which gyro axis your "up/down" gesture actually drives,
# instead of guessing it from the IMU datasheet orientation.

$sp = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$sp.ReadTimeout = 200
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 500
$sp.DiscardInBuffer()

function Drain([int]$ms) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalMilliseconds -lt $ms) {
        $s = $sp.ReadExisting()
        if ($s) { Write-Host -NoNewline $s }
        Start-Sleep -Milliseconds 40
    }
}

function Countdown([string]$msg, [int]$secs) {
    Write-Host ""
    Write-Host $msg -ForegroundColor Yellow
    for ($i = $secs; $i -gt 0; $i--) {
        Write-Host "  $i..." -ForegroundColor DarkGray
        Start-Sleep -Seconds 1
    }
}

Write-Host "=== air mouse axis finder ===" -ForegroundColor Cyan

# make sure we are in air mouse mode
$sp.Write('M'); Drain 2500

Countdown "STEP 1 - put the device FLAT on the desk and do not touch it." 4
$sp.Write('z'); Drain 2500

Countdown "STEP 2 - pick it up and TILT IT UP AND DOWN, nothing else, for 6 s." 4
Write-Host "  GO - tilt up/down now!" -ForegroundColor Green
$sp.Write('T'); Drain 8000

Countdown "STEP 3 - now turn it LEFT AND RIGHT only, for 6 s." 4
Write-Host "  GO - turn left/right now!" -ForegroundColor Green
$sp.Write('T'); Drain 8000

Write-Host ""
Write-Host "Done. Report the two 'dominant axis' lines back." -ForegroundColor Cyan
$sp.Close()
$sp.Dispose()
