<#
example qemu launch for win0-doom on a windows host, with a tcp monitor
for the other helper scripts. defaults are placeholders - pass real paths.

acceleration: whpx needs the "windows hypervisor platform" optional
feature enabled; without it pass -Accel tcg (slow but functional). the
linux hv_* cpu enlightenments are kvm-specific and omitted here.

vga must stay "std": the driver targets the qemu standard-vga aperture
(see README, giant warning label).

boot a disposable copy of a checksum-verified release image, never the
pristine image itself.

examples:
  .\start-win0-vm.ps1 -ValidationOsDisk C:\VMs\win0-doom-run.vhdx
  .\start-win0-vm.ps1 -ValidationOsDisk C:\VMs\run.qcow2 -Accel tcg -MonitorPort 5555
#>
param(
    [Parameter(Mandatory = $true)][string]$ValidationOsDisk,
    [string]$WindowsSystemDisk,    # optional second disk (e.g. full windows)
    [string]$Qemu = 'qemu-system-x86_64',
    [ValidateSet('whpx', 'tcg')][string]$Accel = 'whpx',
    [string]$FirmwareCode = 'C:\Program Files\qemu\share\edk2-x86_64-code.fd',
    [string]$FirmwareVarsTemplate = 'C:\Program Files\qemu\share\edk2-i386-vars.fd',
    [string]$VarsFile = (Join-Path $env:LOCALAPPDATA 'win0-doom\OVMF_VARS.fd'),
    [int]$MemoryMb = 8192,
    [int]$Smp = 4,
    [int]$MonitorPort = 4444
)

$ErrorActionPreference = 'Stop'

function Get-DiskFormat {
    param([Parameter(Mandatory = $true)][string]$Path)

    switch ([System.IO.Path]::GetExtension($Path).ToLowerInvariant()) {
        '.qcow2' { 'qcow2' }
        '.vhdx'  { 'vhdx' }
        '.vhd'   { 'vpc' }
        '.img'   { 'raw' }
        '.raw'   { 'raw' }
        default  { throw "unknown disk format for extension: $Path" }
    }
}

if (-not (Test-Path $ValidationOsDisk)) { throw "validationos disk not found: $ValidationOsDisk" }
if (-not (Test-Path $FirmwareCode)) { throw "firmware not found: $FirmwareCode (adjust -FirmwareCode to your qemu installation)" }

# writable per-vm firmware vars
$varsDir = Split-Path $VarsFile
if ($varsDir -and -not (Test-Path $varsDir)) { New-Item -ItemType Directory -Path $varsDir | Out-Null }
if (-not (Test-Path $VarsFile)) { Copy-Item $FirmwareVarsTemplate $VarsFile }

$cpuModel = if ($Accel -eq 'tcg') { 'max' } else { 'host' }

$qemuArgs = @(
    '-name', 'win0-doom',
    '-monitor', "tcp:127.0.0.1:$MonitorPort,server,nowait",
    '-machine', "q35,accel=$Accel",
    '-cpu', $cpuModel,
    '-smp', "$Smp",
    '-m', "$MemoryMb",
    '-drive', "if=pflash,format=raw,readonly=on,file=$FirmwareCode",
    '-drive', "if=pflash,format=raw,file=$VarsFile"
)

$validationFormat = Get-DiskFormat $ValidationOsDisk
$qemuArgs += @('-drive', "if=none,id=validationos_disk,format=$validationFormat,cache=writeback,file=$ValidationOsDisk")
$qemuArgs += @('-device', 'ide-hd,drive=validationos_disk,bus=ide.0,bootindex=1')

if ($WindowsSystemDisk) {
    if (-not (Test-Path $WindowsSystemDisk)) { throw "system disk not found: $WindowsSystemDisk" }
    $systemFormat = Get-DiskFormat $WindowsSystemDisk
    $qemuArgs += @('-drive', "if=none,id=system_disk,format=$systemFormat,cache=writeback,file=$WindowsSystemDisk")
    $qemuArgs += @('-device', 'ide-hd,drive=system_disk,bus=ide.1,bootindex=2')
}

$qemuArgs += @(
    '-boot', 'menu=on,strict=on',
    '-device', 'qemu-xhci',
    '-device', 'usb-tablet',
    '-vga', 'std',
    '-rtc', 'base=localtime,clock=host,driftfix=slew'
)

Write-Output "starting: $Qemu"
Write-Output ($qemuArgs -join ' ')
Start-Process -FilePath $Qemu -ArgumentList $qemuArgs
Write-Output ""
Write-Output "monitor: tcp:127.0.0.1:$MonitorPort"
Write-Output "next:    .\win0-send-launch.ps1 -MonitorPort $MonitorPort   (after the ccs prompt appears)"
Write-Output "         .\win0-screendump.ps1 ; .\win0-verify.ps1"
