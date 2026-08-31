@echo off
setlocal
set STORE=C:\Windows\Temp\BCD.win0doom
set LOADER={5c438883-a49b-11f1-90f8-525400123456}
set RESUME={f0e79e13-a4e6-11f1-a0a0-806e6f6e6963}
set MARKER=C:\Windows\System32\win0doom-bcd-ready.txt
set LOG=C:\Windows\System32\win0doom-bcdboot.log

>"%LOG%" echo rebuilding the ValidationOS boot store from BCD-Template

set STAGE=copy-template
copy /y C:\Windows\System32\Config\BCD-Template "%STORE%" >>"%LOG%" 2>&1 || goto failed
set STAGE=create-loader
bcdedit.exe /store "%STORE%" /create %LOADER% /d "Windows OneCore" /application osloader >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-device
bcdedit.exe /store "%STORE%" /set %LOADER% device partition=C: >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-osdevice
bcdedit.exe /store "%STORE%" /set %LOADER% osdevice partition=C: >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-path
bcdedit.exe /store "%STORE%" /set %LOADER% path \Windows\System32\Boot\winload.efi >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-systemroot
bcdedit.exe /store "%STORE%" /set %LOADER% systemroot \Windows >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-locale
bcdedit.exe /store "%STORE%" /set %LOADER% locale en-US >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-inherit
bcdedit.exe /store "%STORE%" /set %LOADER% inherit {bootloadersettings} >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-isolated-context
bcdedit.exe /store "%STORE%" /set %LOADER% isolatedcontext Yes >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-nx
bcdedit.exe /store "%STORE%" /set %LOADER% nx OptIn >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-boot-menu-policy
bcdedit.exe /store "%STORE%" /set %LOADER% bootmenupolicy Standard >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-boot-status-policy
bcdedit.exe /store "%STORE%" /set %LOADER% bootstatuspolicy IgnoreAllFailures >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-recovery
bcdedit.exe /store "%STORE%" /set %LOADER% recoveryenabled No >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-ems
bcdedit.exe /store "%STORE%" /set %LOADER% ems Yes >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-allowed-memory-settings
bcdedit.exe /store "%STORE%" /set %LOADER% allowedinmemorysettings 0x15000075 >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-no-integrity-checks
bcdedit.exe /store "%STORE%" /set %LOADER% nointegritychecks On >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-test-signing
bcdedit.exe /store "%STORE%" /set %LOADER% testsigning On >>"%LOG%" 2>&1 || goto failed
set STAGE=create-resume
bcdedit.exe /store "%STORE%" /create %RESUME% /d "Windows OneCore" /application resume >>"%LOG%" 2>&1 || goto failed
set STAGE=resume-device
bcdedit.exe /store "%STORE%" /set %RESUME% device partition=C: >>"%LOG%" 2>&1 || goto failed
set STAGE=resume-path
bcdedit.exe /store "%STORE%" /set %RESUME% path \Windows\System32\Boot\winresume.efi >>"%LOG%" 2>&1 || goto failed
set STAGE=resume-locale
bcdedit.exe /store "%STORE%" /set %RESUME% locale en-US >>"%LOG%" 2>&1 || goto failed
set STAGE=resume-inherit
bcdedit.exe /store "%STORE%" /set %RESUME% inherit {resumeloadersettings} >>"%LOG%" 2>&1 || goto failed
set STAGE=resume-file-device
bcdedit.exe /store "%STORE%" /set %RESUME% filedevice partition=C: >>"%LOG%" 2>&1 || goto failed
set STAGE=resume-file-path
bcdedit.exe /store "%STORE%" /set %RESUME% filepath \hiberfil.sys >>"%LOG%" 2>&1 || goto failed
set STAGE=resume-allowed-memory-settings
bcdedit.exe /store "%STORE%" /set %RESUME% allowedinmemorysettings 0x15000075 >>"%LOG%" 2>&1 || goto failed
set STAGE=loader-resume-object
bcdedit.exe /store "%STORE%" /set %LOADER% resumeobject %RESUME% >>"%LOG%" 2>&1 || goto failed
set STAGE=bootmgr-default
bcdedit.exe /store "%STORE%" /set {bootmgr} default %LOADER% >>"%LOG%" 2>&1 || goto failed
set STAGE=bootmgr-display-order
bcdedit.exe /store "%STORE%" /displayorder %LOADER% /addfirst >>"%LOG%" 2>&1 || goto failed
set STAGE=bootmgr-timeout
bcdedit.exe /store "%STORE%" /timeout 0 >>"%LOG%" 2>&1 || goto failed
set REGROOT=HKLM\SYSTEM
set RUNLEVELREG=C:\Windows\System32\win0doom-runlevel.reg
set DRIVERREG=C:\Windows\System32\win0doom-driver.reg
set OFFLINE=0
if exist C:\Windows\System32\config\SYSTEM.win0doom-target goto offline_registry
goto apply_registry

:offline_registry
set STAGE=load-target-system
reg.exe load HKLM\VOS_SYSTEM C:\Windows\System32\config\SYSTEM.win0doom-target >>"%LOG%" 2>&1 || goto failed
set REGROOT=HKLM\VOS_SYSTEM
set RUNLEVELREG=C:\Windows\System32\win0doom-runlevel-target.reg
set DRIVERREG=C:\Windows\System32\win0doom-driver-target.reg
set OFFLINE=1

:apply_registry
set STAGE=runlevel-registry
reg.exe import "%RUNLEVELREG%" >>"%LOG%" 2>&1 || goto failed_registry
set STAGE=runlevel-default-windows
reg.exe add "%REGROOT%\ControlSet001\Control\RunLevels" /v DefaultWindows /t REG_EXPAND_SZ /d "%%SystemRoot%%\system32\csrss.exe ObjectDirectory=\Windows SharedSection=1024,3072,512 Windows=On SubSystemType=Windows ServerDll=basesrv,1 ServerDll=winsrv:UserServerDllInitialization,3 ProfileControl=Off MaxRequestThreads=16" /f >>"%LOG%" 2>&1 || goto failed_registry
set STAGE=runlevel-windows
reg.exe add "%REGROOT%\ControlSet001\Control\RunLevels" /v Windows /t REG_EXPAND_SZ /d "%%SystemRoot%%\system32\csrss.exe ObjectDirectory=\Windows SharedSection=1024,3072,512 Windows=On SubSystemType=Windows ServerDll=basesrv,1 ServerDll=winsrv:UserServerDllInitializationWin1,3 ServerDll=coniosrv:ServerDllInitialization,2 ProfileControl=Off MaxRequestThreads=16" /f >>"%LOG%" 2>&1 || goto failed_registry
set STAGE=driver-registry
reg.exe import "%DRIVERREG%" >>"%LOG%" 2>&1 || goto failed_registry
if "%OFFLINE%"=="1" goto unload_target_system
goto registry_done

:unload_target_system
set STAGE=unload-target-system
reg.exe unload HKLM\VOS_SYSTEM >>"%LOG%" 2>&1 || goto failed

:registry_done
>"%MARKER%" echo ready
shutdown.exe /s /t 0
exit /b 0

:failed
>"%MARKER%" echo failed-%STAGE%
shutdown.exe /s /t 0
exit /b 1

:failed_registry
if "%OFFLINE%"=="1" reg.exe unload HKLM\VOS_SYSTEM >>"%LOG%" 2>&1
goto failed
