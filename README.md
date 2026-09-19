# win0-doom

doom, running as a native application in windows validation os runlevel 0.

runlevel 0 is not shutdown here. it is validationos's smallest execution
environment: no win32 subsystem, no services, and normally only `ccs.exe`,
`system`, `registry`, and the system process. ordinary windows executables do
not load there.

this project is an experimental native-subsystem port built against the
windows 11 26100 wdk. the game imports only `ntdll.dll`; a tiny boot-start
kernel driver presents its 320x200 rgba framebuffer directly through qemu's
standard-vga aperture.

## current status

- native hello-world runs successfully in validationos runlevel 0
- the signed display driver loads at boot
- a native test client fills the complete 1280x800 qemu display
- the puredoom-based executable builds as a native-subsystem pe importing only
  `ntdll.dll`
- doom finds the wad, initializes successfully, enters its demo loop, and
  renders live game frames in validationos runlevel 0
- the driver is also a keyboard-class upper filter: it mirrors raw scan-code
  records into a spinlock-protected ring while forwarding every record to ccs
- menus, key presses, key releases, movement, and firing have been proven in a
  clean runlevel-0 boot; doom is playable

the keyboard development record — failed paths, the working filter design,
proven tests, and remaining work — is in [`NEXT.md`](NEXT.md).

the successful run uses a boot-volume-discovering file layer. runlevel 0 does
not provide the normal per-process `C:` dos-device mapping, so the platform
layer tries `\\GLOBAL??\\C:` and the native `\\Device\\HarddiskVolume*`
names, then caches the volume which contains the wad.

## demo

[docs/demo-boot-to-doom.mp4](docs/demo-boot-to-doom.mp4) — one continuous
host-side capture (wf-recorder on the qemu window region) of the full arc:
ovmf firmware, windows boot manager, the validationos runlevel-0 banner,
the launch command injected one scancode at a time through the qemu
monitor, and doom rising into its attract demo. the rejected first
attempt is left in on purpose: a monitor-fed burst of `sendkey` commands
outran the ccs raw-input path, the command arrived garbled
(`\SystemRoot\ssdoom.exe`), and the void answered "is not recognized".
the slow per-key retyping that follows is the reliable method documented
in [setup/linux.md](setup/linux.md). recorded, reviewed, and cut with
the help of two vision models; no desktop content survives the edit.

## keyboard input

runlevel 0 gives native applications no console or standard handles. directly
opening `\\Device\\KeyboardClass*` from user mode is denied, while reading
those devices from a kernel thread competes with ccs for the same input queue.

the working design follows microsoft's
[`kbfiltr`](https://github.com/microsoft/Windows-driver-samples/tree/main/input/kbfiltr)
model. `Win0DoomDisplay` is registered before `kbdclass` in the keyboard setup
class's `UpperFilters` value. it intercepts
`IOCTL_INTERNAL_KEYBOARD_CONNECT`, substitutes a small service callback, copies
each `KEYBOARD_INPUT_DATA` record into doom's ring, and immediately calls the
original class callback. ccs therefore continues to receive normal input.

opening the driver's control device discards older ring contents. this keeps
the command used to launch `win0doom.exe` from becoming doom input.

sound remains disabled. runlevel 0 does not provide the ordinary windows audio
stack, so audio will require another native/kernel bridge if implemented.

## layout

- `native/win0-doom.c` — puredoom platform layer using native nt apis
- `native/build-win0-doom.sh` — clang/lld native executable build
- `driver/win0doom-display.c` — qemu stdvga framebuffer bridge and keyboard
  class upper filter
- `driver/build-driver.sh` — wdk kernel-driver build
- `driver/install-win0doom-display.reg` — boot-start service definition
- `native/driver-display-test.c` — full-screen gradient test client
- `native/input-probe.c` — confirms win0 native processes have no usable
  standard handles
- `PureDOOM/` — pinned upstream puredoom submodule

the other native programs are small probes which established what runlevel 0
does and does not permit. in particular, user-mode access to
`\\Device\\PhysicalMemory` is denied and `\\Device\\Video0` does not exist, so
the framebuffer bridge has to live in kernel mode.

## build prerequisites

the current scripts expect:

- x86-64 linux with clang-cl and lld-link in `/usr/lib/llvm/22/bin`
- microsoft windows sdk/wdk version `10.0.26100.0`
- the extracted sdk package at `toolchain/wdk-26100/c`
- `osslsigncode` if a test-signed driver is required
- a legally obtained doom shareware or retail wad for runtime

the toolchain, certificates, binaries, game data, and virtual-machine disks are
deliberately ignored. they are not redistributable project source.

```bash
git clone --recurse-submodules git@github.com:Kris1225-hash/win0-doom.git
cd win0-doom

./native/build-win0-doom.sh
./driver/build-driver.sh
```

the build scripts accept `WINDOWS_SDK_ROOT` when the extracted sdk lives
somewhere else:

```bash
WINDOWS_SDK_ROOT=/path/to/sdk/c ./native/build-win0-doom.sh
```

## automated vm image build

`tools/build-vm-image.sh` builds a fresh qcow2 without modifying an existing
windows disk. it requires two microsoft-sourced inputs:

- the official validationos iso, which supplies the bootable win4 vhdx
- a `GenImage`-generated **runlevel-0** wim from the same validationos build

the second input is mandatory. microsoft's stock vhdx does not contain the
Win0-N packages or even `ccs.exe`, `CloudCoreInit.exe`, and the win0 api-set
schema. changing registry values on that stock image produces an unbootable or
empty pseudo-win0 environment; it cannot manufacture the omitted packages.

```bash
./tools/build-vm-image.sh \
  /path/to/official-validationos.iso \
  /path/to/generated-runlevel-0.wim \
  ./dist/win0-doom.qcow2 \
  /path/to/doom1.wad
```

the builder compiles and test-signs the current artifacts, converts the
official vhdx, asks pristine win4 to construct a target-specific bcd using
windows-native tools, overlays the generated wim, installs the driver/game/wad,
checks the qcow2, and writes a sha-256 file. it refuses to overwrite an existing
output and preserves a failed partial image for diagnosis.

runlevel 0 still starts microsoft's `ccs.exe` as its required initial process.
the builder sets the session manager's `CcsCommand` value to
`\SystemRoot\system32\win0doom.exe`; ccs completes session initialization and
then launches doom automatically. replacing `NtInitialUserProcess` or the ccs
binary itself causes `SESSION1_INITIALIZATION_FAILED (0x6d)`. deleting
`CcsCommand` from win4 or an offline registry editor restores the normal ccs
prompt.

host-side dependencies are qemu with kvm and nbd support, ovmf, ntfs-3g,
wimlib, hivex, `socat`, and the build dependencies above. the helper vm uses
2 gib of ram; the resulting image can be launched with 8 gib or more.

neither the generated wim, validationos binaries, vm image, nor doom wad belongs
in this repository. whether microsoft permits redistribution of a finished
validationos disk must be established separately before publishing one on
google drive, proton drive, or a release page. the safe public workflow is
bring-your-own microsoft inputs plus a legally obtained wad.

`tools/deploy-vm.sh` creates a compressed, checked rollback image before
replacing only the win0 doom payload files in an already-generated runlevel-0
disk. `tools/rollback-vm.sh` restores that backup while preserving the displaced
image.

### native windows builder

windows can build the same image without qemu-nbd, ntfs-3g, hivex, or the
temporary win4 helper vm. from an elevated powershell prompt:

```powershell
.\tools\build-vm-image.ps1 `
  -GeneratedRunLevel0Wim C:\path\ValidationOS.wim `
  -OutputVhdx C:\VMs\win0-doom.vhdx `
  -DoomWad C:\path\doom1.wad
```

the script uses windows-native vhd mounting, diskpart, dism, `reg.exe`, and
`bcdedit.exe`. unlike the linux builder, it does not need the stock iso/vhdx or
a temporary win4 boot because the host windows installation can construct the
disk, registry, and bcd directly. it emits a 64 gib vhdx, which qemu can boot
directly. it requires an elevated windows 10/11 environment with the storage
cmdlets, llvm's `clang-cl`/`lld-link`, the 26100 sdk/wdk files, and the
validationos test-signing certificate chain. paths for non-default llvm, sdk,
signtool, and certificate locations can be passed as parameters. use
`-SkipBuild` only when the current `native\build\win0doom.exe` and signed driver
already exist.

the windows builder obtains the mounted disk number from the exact vhdx it just
created and passes that number to every partitioning cmdlet. it never runs
diskpart `clean` or selects a physical disk. failed partial images are preserved
with a `.failed` suffix.

## giant warning label

this is a qemu-only research prototype, not a general windows display driver.
the current driver assumes a 1280x800, 32-bpp qemu stdvga framebuffer at
physical address `0x80000000`. mapping the wrong physical address from kernel
mode is an excellent way to crash or corrupt a machine. use a disposable vm,
keep a verified disk backup, and do not install this on real hardware.

the driver is intentionally unsigned in source form. test signing and the test
certificate chain are local deployment concerns and are not stored here.

## third-party software

the doom engine integration uses [PureDOOM](https://github.com/Daivuk/PureDOOM),
which is included as a pinned git submodule and retains its own license. doom
game data is not included.
