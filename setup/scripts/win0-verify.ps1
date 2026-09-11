<#
decide whether a win0 screenshot shows the text console or graphics
(i.e. doom rendering), by distinct-color count.

measured references from a verified run: the runlevel-0 text console had
19 distinct colors; a doom attract-demo frame had 114. the default
threshold sits between them.

requires windows powershell or pwsh on windows (System.Drawing/GDI+).
on linux/macos hosts use the python or imagemagick one-liners from the
host guides instead.

examples:
  .\win0-verify.ps1
  .\win0-verify.ps1 -ImageFile .\win0-screen.png -Threshold 48

exit code 0 when doom is rendering, 1 when it is not.
#>
param(
    [string]$ImageFile = (Join-Path $env:TEMP 'win0-screen.png'),
    [int]$Threshold = 48,
    [int]$SampleStep = 2
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $ImageFile)) { throw "no such image: $ImageFile" }

$bitmap = [System.Drawing.Bitmap]::FromFile($ImageFile)
try {
    $colors = New-Object 'System.Collections.Generic.HashSet[int]'
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bitmap.Width, $bitmap.Height)
    $locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $bitmap.PixelFormat)
    try {
        $bytesPerPixel = [System.Drawing.Image]::GetPixelFormatSize($bitmap.PixelFormat) / 8
        $buffer = New-Object byte[] ($locked.Stride * $bitmap.Height)
        [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $buffer, 0, $buffer.Length)
        for ($y = 0; $y -lt $bitmap.Height; $y += $SampleStep) {
            $row = $y * $locked.Stride
            for ($x = 0; $x -lt $bitmap.Width; $x += $SampleStep) {
                $o = $row + $x * $bytesPerPixel
                $null = $colors.Add(($buffer[$o] -shl 16) -bor ($buffer[$o + 1] -shl 8) -bor $buffer[$o + 2])
            }
        }
    }
    finally {
        $bitmap.UnlockBits($locked)
    }
}
finally {
    $bitmap.Dispose()
}

$count = $colors.Count
Write-Output "image: $ImageFile"
Write-Output "distinct colors (sampled every ${SampleStep}px): $count (threshold $Threshold)"

if ($count -ge $Threshold) {
    Write-Output "verdict: graphics mode - doom is rendering"
    exit 0
}

Write-Output "verdict: text console - doom is NOT running"
Write-Output "check: full native launch path (\SystemRoot\system32\win0doom.exe),"
Write-Output "       and that the booted disk actually contains the payload (have-vm.md)"
exit 1
