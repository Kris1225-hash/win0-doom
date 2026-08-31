@echo off
setlocal EnableExtensions
title Install native Win0 smoke test

fltmc >nul 2>&1
if errorlevel 1 (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SOURCE=%~dp0hello-win0.exe"
set "TARGET=W:\Windows\System32\hello.exe"

if not exist "%SOURCE%" (
    echo hello-win0.exe must be in the same folder as this installer.
    pause
    exit /b 2
)

if not exist "W:\Windows\System32\Config\SYSTEM" (
    echo the validationos installation was not found on W:.
    pause
    exit /b 3
)

powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$p = Get-Partition -DriveLetter W -ErrorAction Stop; $d = Get-Disk -Number $p.DiskNumber -ErrorAction Stop; if ($d.Size -lt 60GB -or $d.Size -gt 68GB -or $d.IsBoot -or $d.IsSystem) { exit 1 }"
if errorlevel 1 (
    echo W: is not the expected non-system 64 GB validationos disk.
    pause
    exit /b 4
)

copy /y "%SOURCE%" "%TARGET%"
if errorlevel 1 (
    echo failed to copy the native executable.
    pause
    exit /b 5
)

echo.
echo installed %TARGET%
echo shut down win4, boot validationos rl0, and type: hello
echo.
pause
