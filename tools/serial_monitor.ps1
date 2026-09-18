$sp = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$sp.ReadTimeout = 200
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.DtrEnable = $false; $sp.RtsEnable = $false
$sp.Open()
Start-Sleep -Milliseconds 200

# --- hard reset via EN: DTR/RTS auto-reset circuit ---
$sp.DtrEnable = $false; $sp.RtsEnable = $true   # EN low  -> hold in reset
Start-Sleep -Milliseconds 150
$sp.RtsEnable = $false                           # EN high -> release, boot from flash
$sp.DiscardInBuffer()
Write-Output "=== RESET issued, monitoring 60s ==="

$t0 = Get-Date
$pending = ''
while (((Get-Date) - $t0).TotalSeconds -lt 60) {
    $s = $sp.ReadExisting()
    if ($s) {
        $pending += $s
        while ($pending -match "`n") {
            $idx = $pending.IndexOf("`n")
            $line = $pending.Substring(0, $idx).TrimEnd("`r")
            $pending = $pending.Substring($idx + 1)
            $ts = '{0:mm\:ss\.fff}' -f ((Get-Date) - $t0)
            Write-Output "[$ts] $line"
        }
    }
    Start-Sleep -Milliseconds 50
}
if ($pending) {
    $ts = '{0:mm\:ss\.fff}' -f ((Get-Date) - $t0)
    Write-Output "[$ts] $pending   <partial line, no newline>"
}
Write-Output "=== monitor ended ==="
$sp.Close(); $sp.Dispose()
