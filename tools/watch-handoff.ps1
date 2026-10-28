# Watches the board's USB drive from Windows and logs every change between "mounted" and
# "no media", then prints HANDOFF.TXT. Used to observe the storage ownership handoff.
# Usage:  .\watch-handoff.ps1 [-Seconds 90]
param([int]$Seconds = 90)
$ErrorActionPreference = 'SilentlyContinue'
function Get-State {
    $disk = Get-Disk | Where-Object { $_.BusType -eq 'USB' } | Select-Object -First 1
    if (-not $disk) { return @{ s = 'no USB disk'; d = $null } }
    $vol = $disk | Get-Partition | Get-Volume | Where-Object { $_.DriveLetter } | Select-Object -First 1
    if ($vol -and $disk.Size -gt 0) { return @{ s = "mounted as $($vol.DriveLetter): ($($vol.FileSystemLabel))"; d = $vol.DriveLetter } }
    return @{ s = "present, $($disk.OperationalStatus) (no media)"; d = $null }
}
$t0 = Get-Date; $last = $null; $letter = $null
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $st = Get-State
    if ($st.d) { $letter = $st.d }
    if ($st.s -ne $last) { "{0,6:N1}s  {1}" -f ((Get-Date) - $t0).TotalSeconds, $st.s; $last = $st.s }
    Start-Sleep -Milliseconds 500
}
if ($letter) {
    $f = "${letter}:\HANDOFF.TXT"
    if (Test-Path $f) { "--- $f ---"; Get-Content $f } else { "HANDOFF.TXT not found on ${letter}:" }
}
