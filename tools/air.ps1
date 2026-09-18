$sp = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$sp.ReadTimeout = 200
$sp.DtrEnable = $false; $sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 2500
$sp.DiscardInBuffer()
function Drain([int]$ms) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalMilliseconds -lt $ms) {
        $s = $sp.ReadExisting(); if ($s) { Write-Host -NoNewline $s }
        Start-Sleep -Milliseconds 40
    }
}
Write-Output ">>> M (switch to AIRMOUSE, calibrates gyro)"
$sp.Write('M'); Drain 3000
Write-Output "`n>>> g (gyro reading)"
$sp.Write('g'); Drain 800
Write-Output "`n>>> s (status)"
$sp.Write('s'); Drain 1200
$sp.Close(); $sp.Dispose()
