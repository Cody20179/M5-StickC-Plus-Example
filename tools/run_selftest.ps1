$sp = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$sp.ReadTimeout = 200
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.DtrEnable = $false; $sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 150
$sp.RtsEnable = $true; Start-Sleep -Milliseconds 120; $sp.RtsEnable = $false
$sp.DiscardInBuffer()

function Drain([int]$ms) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalMilliseconds -lt $ms) {
        $s = $sp.ReadExisting()
        if ($s) { Write-Host -NoNewline $s }
        Start-Sleep -Milliseconds 40
    }
}

Drain 3500                              # boot report
foreach ($cmd in @(@('m',2500), @('w',9000), @('o',5000), @('s',1500))) {
    Write-Output "`n>>> sending '$($cmd[0])'"
    $sp.Write($cmd[0])
    Drain $cmd[1]
}
$sp.Close(); $sp.Dispose()
