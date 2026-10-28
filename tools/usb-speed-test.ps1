# Measures real write and read throughput of a USB drive, bypassing the Windows cache.
# Usage:  .\usb-speed-test.ps1 -Drive E [-SizeMB 100]
param(
    [Parameter(Mandatory = $true)][string]$Drive,
    [int]$SizeMB = 100
)
$ErrorActionPreference = 'Stop'
$Drive = $Drive.TrimEnd(':', '\')
$root = "${Drive}:\"
if (-not (Test-Path $root)) { throw "Drive $root is not mounted" }

$chunk = 1MB
$src = Join-Path $env:TEMP "usbtest-src-$SizeMB.bin"
if (-not (Test-Path $src) -or (Get-Item $src).Length -ne $SizeMB * 1MB) {
    Write-Host "Generating $SizeMB MB of random data..."
    $rng = [System.Random]::new(42)
    $buf = New-Object byte[] $chunk
    $fs = [System.IO.File]::Create($src)
    for ($i = 0; $i -lt $SizeMB; $i++) { $rng.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }
    $fs.Close()
}
$srcHash = (Get-FileHash $src -Algorithm SHA256).Hash

# ---- write test: write-through so bytes are on the device when the timer stops ----
$dst = Join-Path $root "usbtest.bin"
if (Test-Path $dst) { Remove-Item $dst -Force }
$WriteThrough = [System.IO.FileOptions]0x80000000
$in = [System.IO.File]::OpenRead($src)
$out = [System.IO.FileStream]::new($dst, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None, $chunk, $WriteThrough)
$buf = New-Object byte[] $chunk
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) { $out.Write($buf, 0, $n) }
$out.Flush($true); $out.Close(); $in.Close()
$sw.Stop()
$writeMBps = $SizeMB / $sw.Elapsed.TotalSeconds

# ---- read test: unbuffered so the data really comes from the device ----
$NoBuffering = [System.IO.FileOptions]0x20000000
$back = Join-Path $env:TEMP "usbtest-back.bin"
$in = [System.IO.FileStream]::new($dst, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read, $chunk, $NoBuffering)
$out = [System.IO.File]::Create($back)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) { $out.Write($buf, 0, $n) }
$sw.Stop()
$in.Close(); $out.Close()
$readMBps = $SizeMB / $sw.Elapsed.TotalSeconds
$backHash = (Get-FileHash $back -Algorithm SHA256).Hash

Remove-Item $dst -Force
[pscustomobject]@{
    Drive      = $root
    SizeMB     = $SizeMB
    WriteMBps  = [math]::Round($writeMBps, 2)
    ReadMBps   = [math]::Round($readMBps, 2)
    HashMatch  = ($srcHash -eq $backHash)
} | Format-List
