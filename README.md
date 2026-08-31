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

the exact state of the keyboard work, including the currently expected build
failure and the next changes, is recorded in [`NEXT.md`](NEXT.md).

the successful run uses a boot-volume-discovering file layer. runlevel 0 does
not provide the normal per-process `C:` dos-device mapping, so the platform
layer tries `\\GLOBAL??\\C:` and the native `\\Device\\HarddiskVolume*`
names, then caches the volume which contains the wad.

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
