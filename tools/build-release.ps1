#Requires -Version 5.1

[CmdletBinding()]
param(
    [string]$SdkRoot,
    [string]$LlvmBin,
    [string]$SignTool,
    [string]$SigningPfx,
    [string]$IntermediateCertificate,
    [string]$SigningPassword = '',
    [string]$SdkVersion = '10.0.26100.0'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SdkRoot) {
    $RepositorySdk = Join-Path $RepoRoot 'toolchain\wdk-26100\c'
    if (Test-Path -LiteralPath $RepositorySdk -PathType Container) {
        $SdkRoot = $RepositorySdk
    }
    else {
        $SdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    }
}
if (-not $LlvmBin) {
    $clangCommand = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    if (-not $clangCommand) {
        $clangCommand = Get-Command clang-cl -ErrorAction SilentlyContinue
    }
    if (-not $clangCommand) {
        throw 'clang-cl was not found. install LLVM or pass -LlvmBin.'
    }
    $LlvmBin = Split-Path -Parent $clangCommand.Source
}
if (-not $SignTool) {
    $SignTool = Join-Path $SdkRoot "bin\$SdkVersion\x64\signtool.exe"
}
if (-not $SigningPfx) {
    $SigningPfx = Join-Path $SdkRoot 'tools\certificates\OEM_Test_Cert_2017.pfx'
}
if (-not $IntermediateCertificate) {
    $IntermediateCertificate = Join-Path $SdkRoot 'tools\certificates\OEM_Intermediate_Cert_2017.cer'
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

$ClangCl = Join-Path $LlvmBin 'clang-cl.exe'
$LldLink = Join-Path $LlvmBin 'lld-link.exe'
$ReadObj = Join-Path $LlvmBin 'llvm-readobj.exe'
foreach ($RequiredFile in @($ClangCl, $LldLink, $ReadObj, $SignTool, $SigningPfx, $IntermediateCertificate)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "required build file is missing: $RequiredFile"
    }
}

$NativeDir = Join-Path $RepoRoot 'native'
$DriverDir = Join-Path $RepoRoot 'driver'
$NativeBuild = Join-Path $NativeDir 'build'
$DriverBuild = Join-Path $DriverDir 'build'
$null = New-Item -ItemType Directory -Force -Path $NativeBuild, $DriverBuild

$DoomObject = Join-Path $NativeBuild 'win0-doom.obj'
$DoomExe = Join-Path $NativeBuild 'win0doom.exe'
$DriverObject = Join-Path $DriverBuild 'win0doom-display.obj'
$UnsignedDriver = Join-Path $DriverBuild 'win0doom-display.sys'
$SignedDriver = Join-Path $DriverBuild 'win0doom-display-signed.sys'
$NtdllCandidates = @(
    (Join-Path $SdkRoot 'um\x64\ntdll.lib'),
    (Join-Path $SdkRoot "Lib\$SdkVersion\um\x64\ntdll.lib")
)
$NtdllLib = $NtdllCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $NtdllLib) {
    throw "ntdll.lib was not found under the supplied SDK root: $SdkRoot"
}

$CommonDefines = @('/D_AMD64_', '/D_WIN64')
$CommonWarnings = @(
    '/clang:-Wno-nonportable-include-path',
    '/clang:-Wno-unknown-pragmas',
    '/clang:-Wno-ignored-pragma-intrinsic',
    '/clang:-Wno-pragma-pack'
)

$DoomCompileArguments = @(
    '/c', '/nologo', '/O2', '/GS-', '/GR-', '/Zl', '/W1',
    $CommonWarnings[0], $CommonWarnings[1], $CommonWarnings[2], $CommonWarnings[3],
    '/clang:-fno-builtin-memset', '/clang:-fno-builtin-memcpy', '/clang:-fno-builtin-strlen',
    $CommonDefines[0], $CommonDefines[1], '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $SdkRoot "Include\$SdkVersion\km\crt")",
    "/I$(Join-Path $SdkRoot "Include\$SdkVersion\shared")",
    "/I$(Join-Path $SdkRoot "Include\$SdkVersion\um")",
    "/I$(Join-Path $SdkRoot "Include\$SdkVersion\ucrt")",
    "/I$RepoRoot", "/Fo$DoomObject", (Join-Path $NativeDir 'win0-doom.c')
)
Invoke-Native $ClangCl $DoomCompileArguments
Invoke-Native $LldLink @(
    '/nologo', '/machine:x64', '/subsystem:native', '/entry:NtProcessStartup',
    '/nodefaultlib', '/stack:4194304,65536', "/out:$DoomExe", $DoomObject,
    $NtdllLib
)

$DriverCompileArguments = @(
    '/c', '/nologo', '/kernel', '/GS-', '/GR-', '/Zl', '/W4',
    $CommonWarnings[0], $CommonWarnings[1], $CommonWarnings[2], $CommonWarnings[3],
    $CommonDefines[0], $CommonDefines[1], '/D_KERNEL_MODE',
    "/I$(Join-Path $SdkRoot "Include\$SdkVersion\km\crt")",
    "/I$(Join-Path $SdkRoot "Include\$SdkVersion\shared")",
    "/I$(Join-Path $SdkRoot "Include\$SdkVersion\km")",
    "/Fo$DriverObject", (Join-Path $DriverDir 'win0doom-display.c')
)
Invoke-Native $ClangCl $DriverCompileArguments
Invoke-Native $LldLink @(
    '/nologo', '/driver', '/release', '/machine:x64', '/subsystem:native',
    '/entry:DriverEntry', '/nodefaultlib', "/out:$UnsignedDriver", $DriverObject,
    (Join-Path $SdkRoot "Lib\$SdkVersion\km\x64\ntoskrnl.lib"),
    (Join-Path $SdkRoot "Lib\$SdkVersion\km\x64\hal.lib")
)

Copy-Item -LiteralPath $UnsignedDriver -Destination $SignedDriver -Force
$SignArguments = @('sign', '/f', $SigningPfx)
if ($SigningPassword) {
    $SignArguments += @('/p', $SigningPassword)
}
$SignArguments += @(
    '/ac', $IntermediateCertificate, '/fd', 'SHA256',
    '/d', 'Win0 Doom display and keyboard bridge', '/v', $SignedDriver
)
Invoke-Native $SignTool $SignArguments

Invoke-Native $ReadObj @('--file-headers', '--coff-imports', $DoomExe)
Invoke-Native $ReadObj @('--file-headers', '--coff-imports', $SignedDriver)

$Hashes = Get-FileHash -Algorithm SHA256 -LiteralPath $DoomExe, $SignedDriver
$HashPath = Join-Path $DriverBuild 'release-windows.sha256'
$Hashes | ForEach-Object { '{0}  {1}' -f $_.Hash.ToLowerInvariant(), $_.Path } |
    Set-Content -LiteralPath $HashPath -Encoding ASCII

Write-Host ''
Write-Host 'release artifacts:'
Get-Content -LiteralPath $HashPath
