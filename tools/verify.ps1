$sp = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$sp.ReadTimeout = 200
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.DtrEnable = $false; $sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 150
$sp.RtsEnable = $true; Start-Sleep -Milliseconds 120; $sp.RtsEnable = $false
$sp.DiscardInBuffer()
Write-Output "=== reset, monitoring 12s ==="
$t0 = Get-Date; $pending = ''
while (((Get-Date) - $t0).TotalSeconds -lt 12) {
    $s = $sp.ReadExisting()
    if ($s) {
        $pending += $s
        while ($pending -match "`n") {
            $i = $pending.IndexOf("`n"); $line = $pending.Substring(0,$i).TrimEnd("`r"); $pending = $pending.Substring($i+1)
            Write-Output ("[{0:mm\:ss\.fff}] {1}" -f ((Get-Date)-$t0), $line)
        }
    }
    Start-Sleep -Milliseconds 50
}
Write-Output "=== end ==="
$sp.Close(); $sp.Dispose()
