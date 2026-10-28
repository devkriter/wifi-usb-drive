# Finds the board's IPv4 address on the local /24 by its WiFi MAC (ESP32-S3 station MAC = base MAC).
# Pings the whole subnet in parallel so the ARP table gets populated, then looks the MAC up.
# Usage:  .\find-board-ip.ps1 [-Mac 94-a9-90-15-da-98] [-Subnet 10.10.0]
param(
    [string]$Mac = '94-a9-90-15-da-98',
    [string]$Subnet
)
$ErrorActionPreference = 'SilentlyContinue'
$Mac = $Mac.ToLower().Replace(':', '-')
if (-not $Subnet) {
    $ip = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.PrefixOrigin -eq 'Dhcp' -or ($_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.PrefixLength -le 24) } | Sort-Object PrefixLength -Descending | Select-Object -First 1
    if (-not $ip) { throw "Could not determine the local subnet; pass -Subnet" }
    $Subnet = ($ip.IPAddress -split '\.')[0..2] -join '.'
}
# fast path: already in the ARP table?
$hit = arp -a | Select-String -Pattern "^\s*(\S+)\s+$Mac" | Select-Object -First 1
if (-not $hit) {
    1..254 | ForEach-Object -Parallel { Test-Connection -TargetName "$using:Subnet.$_" -Count 1 -TimeoutSeconds 1 -Quiet | Out-Null } -ThrottleLimit 64
    Start-Sleep -Milliseconds 500
    $hit = arp -a | Select-String -Pattern "^\s*(\S+)\s+$Mac" | Select-Object -First 1
}
if ($hit) { $hit.Matches[0].Groups[1].Value } else { Write-Error "MAC $Mac not found on $Subnet.0/24" }
