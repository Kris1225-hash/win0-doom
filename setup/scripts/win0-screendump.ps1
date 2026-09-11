<#
capture the guest framebuffer through the qemu hmp monitor and convert the
ppm dump to png. no imagemagick needed: the P6 conversion is pure
System.Drawing. takes a second or two at 1280x800.

requires windows powershell or pwsh on windows (System.Drawing/GDI+).
on linux/macos hosts use "screendump" + imagemagick per the host guides.

examples:
  .\win0-screendump.ps1
  .\win0-screendump.ps1 -OutFile C:\shots\win0.png -KeepPpm
#>
param(
    [string]$OutFile = (Join-Path $env:TEMP 'win0-screen.png'),
    [string]$MonitorHost = '127.0.0.1',
    [int]$MonitorPort = 4444,
    [int]$WaitMs = 8000,
    [switch]$KeepPpm
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$ppmFile = [System.IO.Path]::ChangeExtension($OutFile, '.ppm')
if (Test-Path $ppmFile) { Remove-Item $ppmFile -Force }

function Read-MonitorAvailable {
    param([System.Net.Sockets.NetworkStream]$Stream, [int]$WindowMs)

    $text = New-Object System.Text.StringBuilder
    $buffer = New-Object byte[] 8192
    $deadline = (Get-Date).AddMilliseconds($WindowMs)
    while ((Get-Date) -lt $deadline) {
        if ($Stream.DataAvailable) {
            $read = $Stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) { break }
            $null = $text.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
        } else {
            Start-Sleep -Milliseconds 100
        }
    }
    $text.ToString()
}

# --- ask the monitor for the dump --------------------------------------
$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect($MonitorHost, $MonitorPort)
    $stream = $client.GetStream()
    $null = Read-MonitorAvailable $stream 400    # drain the banner

    $command = "screendump `"$ppmFile`""
    $bytes = [System.Text.Encoding]::ASCII.GetBytes("$command`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()

    $deadline = (Get-Date).AddMilliseconds($WaitMs)
    while ((Get-Date) -lt $deadline) {
        $response = Read-MonitorAvailable $stream 300
        if ($response -match 'unknown command|error') { throw "monitor: $($response.Trim())" }
        if (Test-Path $ppmFile) { break }
    }
}
finally {
    $client.Dispose()
}

if (-not (Test-Path $ppmFile)) { throw "screendump did not produce $ppmFile" }
Start-Sleep -Milliseconds 300    # let the dump finish flushing to disk

# --- convert ppm (binary P6, maxval 255) to png -------------------------
$bytes = [System.IO.File]::ReadAllBytes($ppmFile)
$script:pos = 0
$script:data = $bytes

function Read-PpmToken {
    while ($true) {
        while ($script:pos -lt $script:data.Length -and $script:data[$script:pos] -le 32) { $script:pos++ }
        if ($script:pos -lt $script:data.Length -and $script:data[$script:pos] -eq 35) {
            while ($script:pos -lt $script:data.Length -and $script:data[$script:pos] -ne 10) { $script:pos++ }
            continue
        }
        break
    }
    if ($script:pos -ge $script:data.Length) { throw 'unexpected end of ppm header' }
    $start = $script:pos
    while ($script:pos -lt $script:data.Length -and $script:data[$script:pos] -gt 32) { $script:pos++ }
    [System.Text.Encoding]::ASCII.GetString($script:data, $start, $script:pos - $start)
}

$magic = Read-PpmToken
if ($magic -ne 'P6') { throw "unsupported ppm magic '$magic' (only binary P6)" }
$width = [int](Read-PpmToken)
$height = [int](Read-PpmToken)
$maxval = [int](Read-PpmToken)
if ($maxval -ne 255) { throw "unsupported ppm maxval $maxval" }
$script:pos++    # exactly one whitespace char separates header from data

$pixelCount = $width * $height
$expected = $pixelCount * 3
if ($bytes.Length -lt $script:pos + $expected) { throw 'truncated ppm pixel data' }

# pack rgb triples into argb ints (32bppArgb wants little-endian b,g,r,a in
# memory, so the int form is 0xAARRGGBB)
$pixels = New-Object int[] $pixelCount
for ($i = 0; $i -lt $pixelCount; $i++) {
    $o = $script:pos + $i * 3
    $pixels[$i] = -16777216 -bor ($bytes[$o] -shl 16) -bor ($bytes[$o + 1] -shl 8) -bor $bytes[$o + 2]
}

$bitmap = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
$locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bitmap.PixelFormat)
try {
    [System.Runtime.InteropServices.Marshal]::Copy($pixels, 0, $locked.Scan0, $pixelCount)
}
finally {
    $bitmap.UnlockBits($locked)
}

$outDir = Split-Path $OutFile
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$bitmap.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()

if ($KeepPpm) {
    Write-Output "saved $OutFile (${width}x${height}); kept $ppmFile"
} else {
    Remove-Item $ppmFile -Force
    Write-Output "saved $OutFile (${width}x${height})"
}
