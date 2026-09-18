$sp = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$sp.ReadTimeout = 200
$sp.DtrEnable = $false; $sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 2500     # let it finish booting
$sp.DiscardInBuffer()
Write-Output ">>> sending 'b' (buzzer only)"
$sp.Write('b')
$t = Get-Date
while (((Get-Date) - $t).TotalSeconds -lt 6) {
    $s = $sp.ReadExisting()
    if ($s) { Write-Host -NoNewline $s }
    Start-Sleep -Milliseconds 40
}
$sp.Close(); $sp.Dispose()
