#Requires -Version 5.1
#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GeneratedRunLevel0Wim,
    [Parameter(Mandatory = $true)][string]$OutputVhdx,
    [Parameter(Mandatory = $true)][string]$DoomWad,
    [switch]$SkipBuild,
    [string]$SdkRoot,
    [string]$LlvmBin,
    [string]$SignTool,
    [string]$SigningPfx,
    [string]$IntermediateCertificate,
    [string]$SigningPassword = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$WimPath = (Resolve-Path -LiteralPath $GeneratedRunLevel0Wim).Path
$WadPath = (Resolve-Path -LiteralPath $DoomWad).Path
$OutputPath = [System.IO.Path]::GetFullPath($OutputVhdx)
$OutputDirectory = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
    throw "output directory does not exist: $OutputDirectory"
}
if ([System.IO.Path]::GetExtension($OutputPath) -ne '.vhdx') {
    throw 'the native Windows builder outputs a .vhdx; qemu can boot it directly.'
}
if (Test-Path -LiteralPath $OutputPath) {
    throw "output already exists; refusing to overwrite it: $OutputPath"
}

$PartialPath = "$OutputPath.partial.$PID.vhdx"
$FailedPath = "$PartialPath.failed"
$VhdMounted = $false
$LoadedSystemHive = $false
$LoadedBcdHive = $false
$RootDrive = $null
$EfiDrive = $null
$TemporaryFiles = [System.Collections.Generic.List[string]]::new()

foreach ($RequiredCommand in @(
    'Mount-DiskImage', 'Dismount-DiskImage', 'Get-DiskImage', 'Get-Disk',
    'Initialize-Disk', 'New-Partition', 'Set-Partition', 'Format-Volume',
    'Get-Volume', 'bcdboot.exe', 'bcdedit.exe', 'dism.exe', 'diskpart.exe', 'reg.exe'
)) {
    if (-not (Get-Command $RequiredCommand -ErrorAction SilentlyContinue)) {
        throw "required Windows command is missing: $RequiredCommand"
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host ('> {0} {1}' -f $FilePath, ($Arguments -join ' '))
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

function Get-FreeDriveLetter {
    $Used = @(Get-Volume | Where-Object DriveLetter | ForEach-Object { $_.DriveLetter })
    foreach ($Letter in [char[]]'WVUTSRQPONMLKJIHGFED') {
        $Upper = [char]::ToUpperInvariant($Letter)
        if ($Used -notcontains $Upper) {
            return $Upper
        }
    }
    throw 'no free drive letter is available.'
}

function Get-ImageDisk {
    param([Parameter(Mandatory = $true)][string]$ImagePath)
    $Image = Get-DiskImage -ImagePath $ImagePath
    $Disk = $Image | Get-Disk
    if ($null -eq $Disk -or @($Disk).Count -ne 1) {
        throw "expected exactly one disk for mounted image: $ImagePath"
    }
    return $Disk
}

function Ensure-DriveLetter {
    param([Parameter(Mandatory = $true)]$Partition)
    if ($Partition.DriveLetter) {
        return [char]$Partition.DriveLetter
    }
    $Letter = Get-FreeDriveLetter
    Set-Partition -DiskNumber $Partition.DiskNumber -PartitionNumber $Partition.PartitionNumber `
        -NewDriveLetter $Letter | Out-Null
    return $Letter
}

function Invoke-BcdEdit {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    Invoke-Native 'bcdedit.exe' $Arguments
}

function Configure-Bcd {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsRoot,
        [Parameter(Mandatory = $true)][string]$EfiRoot
    )

    $Store = Join-Path $env:TEMP "BCD.win0doom.$PID"
    $script:TemporaryFiles.Add($Store)
    $Template = Join-Path $WindowsRoot 'Windows\System32\Config\BCD-Template'
    $Loader = '{5c438883-a49b-11f1-90f8-525400123456}'
    $Resume = '{f0e79e13-a4e6-11f1-a0a0-806e6f6e6963}'
    $WindowsPartition = "partition=$($WindowsRoot.Substring(0, 2))"

    Copy-Item -LiteralPath $Template -Destination $Store -Force
    Invoke-BcdEdit @('/store', $Store, '/create', $Loader, '/d', 'Windows OneCore', '/application', 'osloader')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'device', $WindowsPartition)
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'osdevice', $WindowsPartition)
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'path', '\Windows\System32\Boot\winload.efi')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'systemroot', '\Windows')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'locale', 'en-US')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'inherit', '{bootloadersettings}')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'isolatedcontext', 'Yes')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'nx', 'OptIn')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'bootmenupolicy', 'Standard')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'bootstatuspolicy', 'IgnoreAllFailures')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'recoveryenabled', 'No')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'ems', 'Yes')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'allowedinmemorysettings', '0x15000075')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'nointegritychecks', 'On')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'testsigning', 'On')
    Invoke-BcdEdit @('/store', $Store, '/create', $Resume, '/d', 'Windows OneCore', '/application', 'resume')
    Invoke-BcdEdit @('/store', $Store, '/set', $Resume, 'device', $WindowsPartition)
    Invoke-BcdEdit @('/store', $Store, '/set', $Resume, 'path', '\Windows\System32\Boot\winresume.efi')
    Invoke-BcdEdit @('/store', $Store, '/set', $Resume, 'locale', 'en-US')
    Invoke-BcdEdit @('/store', $Store, '/set', $Resume, 'inherit', '{resumeloadersettings}')
    Invoke-BcdEdit @('/store', $Store, '/set', $Resume, 'filedevice', $WindowsPartition)
    Invoke-BcdEdit @('/store', $Store, '/set', $Resume, 'filepath', '\hiberfil.sys')
    Invoke-BcdEdit @('/store', $Store, '/set', $Resume, 'allowedinmemorysettings', '0x15000075')
    Invoke-BcdEdit @('/store', $Store, '/set', $Loader, 'resumeobject', $Resume)
    Invoke-BcdEdit @('/store', $Store, '/set', '{bootmgr}', 'default', $Loader)
    Invoke-BcdEdit @('/store', $Store, '/displayorder', $Loader, '/addfirst')
    Invoke-BcdEdit @('/store', $Store, '/timeout', '0')

    Invoke-Native 'reg.exe' @('load', 'HKLM\WIN0DOOM_BCD', $Store)
    $script:LoadedBcdHive = $true
    try {
        $ElementRoot = "Registry::HKEY_LOCAL_MACHINE\WIN0DOOM_BCD\Objects\$Resume\Elements"
        $Source = Join-Path $ElementRoot '21000001'
        $OneCoreDevice = (Get-ItemProperty -LiteralPath $Source -Name Element).Element
        $Target = Join-Path $ElementRoot '21000026'
        $ResumeFlag = Join-Path $ElementRoot '26000006'
        $null = New-Item -Path $Target -Force
        $null = New-ItemProperty -LiteralPath $Target -Name Element -PropertyType Binary `
            -Value $OneCoreDevice -Force
        $null = New-Item -Path $ResumeFlag -Force
        $null = New-ItemProperty -LiteralPath $ResumeFlag -Name Element -PropertyType Binary `
            -Value ([byte[]](0)) -Force
    }
    finally {
        Invoke-Native 'reg.exe' @('unload', 'HKLM\WIN0DOOM_BCD')
        $script:LoadedBcdHive = $false
    }

    $InstalledStore = Join-Path $EfiRoot 'EFI\Microsoft\Boot\BCD'
    Copy-Item -LiteralPath $InstalledStore -Destination "$InstalledStore.pre-win0doom" -Force
    Copy-Item -LiteralPath $Store -Destination $InstalledStore -Force
}

try {
    if (-not $SkipBuild) {
        $BuildArguments = @{}
        foreach ($Pair in @{
            SdkRoot = $SdkRoot
            LlvmBin = $LlvmBin
            SignTool = $SignTool
            SigningPfx = $SigningPfx
            IntermediateCertificate = $IntermediateCertificate
        }.GetEnumerator()) {
            if ($Pair.Value) {
                $BuildArguments[$Pair.Key] = $Pair.Value
            }
        }
        if ($SigningPassword) {
            $BuildArguments.SigningPassword = $SigningPassword
        }
        & (Join-Path $PSScriptRoot 'build-release.ps1') @BuildArguments
        if (-not $?) {
            throw 'the Windows release build failed.'
        }
    }

    $DoomExe = Join-Path $RepoRoot 'native\build\win0doom.exe'
    $DriverSys = Join-Path $RepoRoot 'driver\build\win0doom-display-signed.sys'
    foreach ($Artifact in @($DoomExe, $DriverSys)) {
        if (-not (Test-Path -LiteralPath $Artifact -PathType Leaf)) {
            throw "required release artifact is missing: $Artifact"
        }
    }

    $DiskPartScript = Join-Path $env:TEMP "win0doom-create-$PID.txt"
    $TemporaryFiles.Add($DiskPartScript)
    @(
        "create vdisk file=`"$PartialPath`" maximum=65536 type=expandable",
        'exit'
    ) | Set-Content -LiteralPath $DiskPartScript -Encoding ASCII
    Invoke-Native 'diskpart.exe' @('/s', $DiskPartScript)

    Mount-DiskImage -ImagePath $PartialPath -StorageType VHDX -Access ReadWrite | Out-Null
    $VhdMounted = $true
    $Disk = Get-ImageDisk $PartialPath
    if ($Disk.IsOffline) {
        Set-Disk -Number $Disk.Number -IsOffline $false
    }
    if ($Disk.IsReadOnly) {
        Set-Disk -Number $Disk.Number -IsReadOnly $false
    }

    Initialize-Disk -Number $Disk.Number -PartitionStyle GPT
    $EfiPartition = New-Partition -DiskNumber $Disk.Number -Size 246MB `
        -GptType '{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}'
    $null = $EfiPartition | Format-Volume -FileSystem FAT32 -NewFileSystemLabel 'SYSTEM' `
        -Force -Confirm:$false
    $null = New-Partition -DiskNumber $Disk.Number -Size 16MB `
        -GptType '{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}'
    $RootPartition = New-Partition -DiskNumber $Disk.Number -UseMaximumSize `
        -GptType '{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}'
    $null = $RootPartition | Format-Volume -FileSystem NTFS -NewFileSystemLabel 'ValidationOS' `
        -Force -Confirm:$false

    $RootDrive = Ensure-DriveLetter $RootPartition
    $EfiDrive = Ensure-DriveLetter $EfiPartition
    $WindowsRoot = "$RootDrive`:\"
    $EfiRoot = "$EfiDrive`:\"

    Invoke-Native 'dism.exe' @(
        '/Apply-Image', "/ImageFile:$WimPath", '/Index:1', "/ApplyDir:$WindowsRoot"
    )
    foreach ($RequiredRelativePath in @(
        'Windows\System32\ccs.exe',
        'Windows\System32\CloudCoreInit.exe',
        'Windows\System32\apisetschema_win0.dll',
        'Windows\System32\f911f154-081b-49de-bbbd-a0dd908085bb_win0-n.dll'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $WindowsRoot $RequiredRelativePath))) {
            throw "generated WIM is missing required runlevel-0 payload: $RequiredRelativePath"
        }
    }

    Invoke-Native 'bcdboot.exe' @(
        (Join-Path $WindowsRoot 'Windows'), '/s', "$EfiDrive`:", '/f', 'UEFI'
    )

    $SystemHive = Join-Path $WindowsRoot 'Windows\System32\config\SYSTEM'
    Invoke-Native 'reg.exe' @('load', 'HKLM\WIN0DOOM_SYSTEM', $SystemHive)
    $LoadedSystemHive = $true
    try {
        $RunLevelReg = Join-Path $RepoRoot 'driver\install-win0-runlevel.reg'
        $DriverReg = Join-Path $RepoRoot 'driver\install-win0doom-display.reg'
        $RunLevelTarget = Join-Path $env:TEMP "win0doom-runlevel-$PID.reg"
        $DriverTarget = Join-Path $env:TEMP "win0doom-driver-$PID.reg"
        $TemporaryFiles.Add($RunLevelTarget)
        $TemporaryFiles.Add($DriverTarget)
        (Get-Content -LiteralPath $RunLevelReg -Raw).Replace(
            'HKEY_LOCAL_MACHINE\SYSTEM', 'HKEY_LOCAL_MACHINE\WIN0DOOM_SYSTEM'
        ) | Set-Content -LiteralPath $RunLevelTarget -Encoding Unicode
        (Get-Content -LiteralPath $DriverReg -Raw).Replace(
            'HKEY_LOCAL_MACHINE\SYSTEM', 'HKEY_LOCAL_MACHINE\WIN0DOOM_SYSTEM'
        ) | Set-Content -LiteralPath $DriverTarget -Encoding Unicode
        Invoke-Native 'reg.exe' @('import', $RunLevelTarget)
        Invoke-Native 'reg.exe' @('import', $DriverTarget)

        $RunLevels = 'HKLM\WIN0DOOM_SYSTEM\ControlSet001\Control\RunLevels'
        $DefaultWindows = '%SystemRoot%\system32\csrss.exe ObjectDirectory=\Windows SharedSection=1024,3072,512 Windows=On SubSystemType=Windows ServerDll=basesrv,1 ServerDll=winsrv:UserServerDllInitialization,3 ProfileControl=Off MaxRequestThreads=16'
        $Windows = '%SystemRoot%\system32\csrss.exe ObjectDirectory=\Windows SharedSection=1024,3072,512 Windows=On SubSystemType=Windows ServerDll=basesrv,1 ServerDll=winsrv:UserServerDllInitializationWin1,3 ServerDll=coniosrv:ServerDllInitialization,2 ProfileControl=Off MaxRequestThreads=16'
        Invoke-Native 'reg.exe' @('add', $RunLevels, '/v', 'DefaultWindows', '/t', 'REG_EXPAND_SZ', '/d', $DefaultWindows, '/f')
        Invoke-Native 'reg.exe' @('add', $RunLevels, '/v', 'Windows', '/t', 'REG_EXPAND_SZ', '/d', $Windows, '/f')
    }
    finally {
        Invoke-Native 'reg.exe' @('unload', 'HKLM\WIN0DOOM_SYSTEM')
        $LoadedSystemHive = $false
    }

    Configure-Bcd $WindowsRoot $EfiRoot
    Copy-Item -LiteralPath $DriverSys `
        -Destination (Join-Path $WindowsRoot 'Windows\System32\drivers\win0doom-display.sys') -Force
    Copy-Item -LiteralPath $DoomExe `
        -Destination (Join-Path $WindowsRoot 'Windows\System32\win0doom.exe') -Force
    Copy-Item -LiteralPath $WadPath `
        -Destination (Join-Path $WindowsRoot 'Windows\System32\doom1.wad') -Force

    Dismount-DiskImage -ImagePath $PartialPath
    $VhdMounted = $false
    Move-Item -LiteralPath $PartialPath -Destination $OutputPath
    $Hash = Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath
    '{0}  {1}' -f $Hash.Hash.ToLowerInvariant(), $OutputPath |
        Set-Content -LiteralPath "$OutputPath.sha256" -Encoding ASCII

    Write-Host ''
    Write-Host "ready-to-boot image: $OutputPath"
    Write-Host "sha-256: $($Hash.Hash.ToLowerInvariant())"
}
finally {
    if ($LoadedBcdHive) {
        & reg.exe unload 'HKLM\WIN0DOOM_BCD' | Out-Null
    }
    if ($LoadedSystemHive) {
        & reg.exe unload 'HKLM\WIN0DOOM_SYSTEM' | Out-Null
    }
    if ($VhdMounted) {
        Dismount-DiskImage -ImagePath $PartialPath -ErrorAction SilentlyContinue
    }
    if ((Test-Path -LiteralPath $PartialPath) -and -not (Test-Path -LiteralPath $FailedPath)) {
        Move-Item -LiteralPath $PartialPath -Destination $FailedPath
        Write-Warning "the incomplete image was preserved at $FailedPath"
    }
    foreach ($TemporaryFile in $TemporaryFiles) {
        if (Test-Path -LiteralPath $TemporaryFile -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryFile -Force -ErrorAction SilentlyContinue
        }
    }
}
