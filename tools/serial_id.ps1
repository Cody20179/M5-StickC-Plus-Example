$rates = 115200,74880,9600,57600,38400
foreach ($b in $rates) {
    $sp = New-Object System.IO.Ports.SerialPort 'COM3',$b,'None',8,'One'
    $sp.ReadTimeout = 200
    $sp.DtrEnable = $false; $sp.RtsEnable = $false
    try { $sp.Open() } catch { Write-Output "[$b] open failed: $($_.Exception.Message)"; continue }
    Start-Sleep -Milliseconds 150
    # EN-reset pulse
    $sp.RtsEnable = $true
    Start-Sleep -Milliseconds 150
    $sp.RtsEnable = $false
    $sp.DiscardInBuffer()
    $all = New-Object System.Collections.Generic.List[byte]
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 8) {
        $n = $sp.BytesToRead
        if ($n -gt 0) { $buf = New-Object byte[] $n; $r = $sp.Read($buf,0,$n); for($i=0;$i -lt $r;$i++){$all.Add($buf[$i])} }
        Start-Sleep -Milliseconds 50
    }
    $arr = $all.ToArray()
    if ($arr.Length -eq 0) { Write-Output "[$b] bytes=0"; $sp.Close(); $sp.Dispose(); continue }
    $printable = ($arr | Where-Object { ($_ -ge 32 -and $_ -lt 127) -or $_ -eq 10 -or $_ -eq 13 -or $_ -eq 9 }).Count
    $pct = [math]::Round(100.0*$printable/$arr.Length,1)
    $txt = -join ($arr | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } elseif ($_ -eq 10) { "`n" } elseif ($_ -eq 13) { '' } else { '.' } })
    Write-Output "[$b] bytes=$($arr.Length) printable=$pct%"
    if ($pct -gt 85) {
        Write-Output "----- TEXT -----"
        Write-Output $txt
        Write-Output "----------------"
        $sp.Close(); $sp.Dispose()
        break
    } else {
        Write-Output "  (garbled, first 120 chars): $($txt.Substring(0,[math]::Min(120,$txt.Length)) -replace "`n",'\n')"
    }
    $sp.Close(); $sp.Dispose()
}
