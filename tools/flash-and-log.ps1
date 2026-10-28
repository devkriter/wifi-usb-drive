# Flashes a build directory to the board over its USB-Serial-JTAG port and captures the boot log.
# Only works while the firmware has not taken over the USB port with TinyUSB (the ROM's
# USB-JTAG CDC port must be present). Otherwise enter download mode by hand first.
# Usage:  .\flash-and-log.ps1 -BuildDir ..\a-usb-stick\build-dbg4 [-Port COM7] [-Seconds 20] [-NoFlash]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$Port,
    [int]$Seconds = 20,
    [switch]$NoFlash
)
$ErrorActionPreference = 'Continue'
. "C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1" 2>&1 | Out-Null

if (-not $Port) {
    $name = (Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -match 'VID_303A' -and $_.Name -match '\((COM\d+)\)' } | Select-Object -First 1).Name
    if ($name -match '\((COM\d+)\)') { $Port = $Matches[1] } else { throw "No Espressif USB-JTAG COM port found (board running TinyUSB, or unplugged?)" }
}
$BuildDir = (Resolve-Path $BuildDir).Path
Write-Host "Port: $Port   Build: $BuildDir"

if (-not $NoFlash) {
    $flashArgs = ((Get-Content (Join-Path $BuildDir 'flash_args')) -join ' ') -replace '\s+', ' '
    # flash_args paths are relative to the build dir
    Push-Location $BuildDir
    try {
        $cmd = "esptool --chip esp32s3 --port $Port --baud 460800 --before default_reset --after hard_reset write_flash $flashArgs"
        Write-Host $cmd
        Invoke-Expression $cmd 2>&1 | Select-Object -Last 6
    } finally { Pop-Location }
}

Write-Host "Listening on $Port for $Seconds s..."
$p = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$p.ReadTimeout = 300; $p.DtrEnable = $false; $p.RtsEnable = $false
$deadline = (Get-Date).AddSeconds($Seconds)
# the port vanishes/reappears across the reset; keep trying to open until the deadline
$sb = [System.Text.StringBuilder]::new()
while ((Get-Date) -lt $deadline) {
    try {
        if (-not $p.IsOpen) { $p.Open() }
        [void]$sb.Append($p.ReadExisting())
    } catch {
        try { $p.Close() } catch {}
        Start-Sleep -Milliseconds 300
        continue
    }
    Start-Sleep -Milliseconds 100
}
try { $p.Close() } catch {}
$sb.ToString() -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { "  | $_" }
