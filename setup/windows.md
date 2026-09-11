# windows host

building images natively and running the vm with a fully scripted
monitor. the helper scripts in [scripts/](scripts/) need powershell
(with `System.Drawing`, i.e. windows powershell 5.1 or pwsh on windows)
and a qemu that exposes a tcp monitor.

## building

`tools\build-vm-image.ps1` (elevated) builds the full disk without
qemu-nbd, ntfs-3g, hivex, or a helper vm — see README, "native windows
builder". use `-SkipBuild` only when current `native\build\win0doom.exe`
and a signed driver already exist.

## running the vm

the scripts talk to:

```
-monitor tcp:127.0.0.1:4444,server,nowait
```

`scripts\start-win0-vm.ps1` is a parameterized example launch (q35,
whpx or tcg, bundled edk2 firmware with a writable vars copy, tcp
monitor, disk format inferred from the file extension). its defaults
are placeholders — pass your real paths:

```powershell
.\scripts\start-win0-vm.ps1 -ValidationOsDisk C:\VMs\win0-doom-run.vhdx
```

acceleration notes:

- whpx needs the "windows hypervisor platform" optional feature enabled
- without it use `-Accel tcg`: everything works, everything is slower
- the linux `hv_*` cpu flags are kvm-specific; the script omits them
- `-vga std` is mandatory: the driver targets the qemu standard-vga
  aperture

as everywhere: verify the release image's sha256, copy it, boot the
copy.

## driving the monitor

```powershell
.\scripts\win0-monitor.ps1 "info version"          # single command transact
.\scripts\win0-send-launch.ps1                     # type the launch command + enter
.\scripts\win0-screendump.ps1 -OutFile .\win0.png  # framebuffer capture -> png
.\scripts\win0-verify.ps1 -ImageFile .\win0.png    # console or doom?
```

`win0-send-launch.ps1` sends `\SystemRoot\system32\win0doom.exe` as
one `sendkey` per character — the hmp command is singular, there is no
`sendkeys` — and surfaces monitor errors instead of failing silently.
override `-ExecutablePath` for payloads installed elsewhere.

`win0-screendump.ps1` asks the monitor for a ppm and converts it to
png with `System.Drawing` (no imagemagick needed); the conversion pass
takes a second or two at 1280x800.

`win0-verify.ps1` counts distinct colors: the text console measured
~19, a doom frame ~114. below the threshold it tells you doom is not
running and where to look (launch path, payload disk).

## typical headless flow

```powershell
.\scripts\start-win0-vm.ps1 -ValidationOsDisk C:\VMs\win0-doom-run.vhdx
start-sleep 45                      # boot to the ccs prompt
.\scripts\win0-send-launch.ps1
start-sleep 10                      # doom init
.\scripts\win0-screendump.ps1
.\scripts\win0-verify.ps1
```

if the image has `CcsCommand` auto-launch configured, skip
`win0-send-launch.ps1` and capture straight after boot.

## shutdown

`system_powerdown` is not reliably serviced at runlevel 0 (observed on
the linux host; expect the same here). end the qemu process after
confirming you booted a disposable copy:

```powershell
.\scripts\win0-monitor.ps1 "quit"
```
