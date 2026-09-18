$sp = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$sp.ReadTimeout = 200
$sp.DtrEnable = $false; $sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 3000
$sp.DiscardInBuffer()
function Drain([int]$ms) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalMilliseconds -lt $ms) {
        $s = $sp.ReadExisting(); if ($s) { Write-Host -NoNewline $s }
        Start-Sleep -Milliseconds 40
    }
}
# sit on the sound page a moment so the FFT has real data before the report
$sp.Write('4'); Drain 3000
$sp.Write('s'); Drain 2000
$sp.Write('1'); Drain 500
$sp.Close(); $sp.Dispose()
