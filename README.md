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
- keyboard scan-code translation and the user/kernel ioctl are implemented,
  but the in-progress kernel reader is not complete yet, so the last proven vm
  build remains watchable rather than playable

the exact state of the keyboard work, including the currently expected build
failure and the next changes, is recorded in [`NEXT.md`](NEXT.md).

the successful run uses a boot-volume-discovering file layer. runlevel 0 does
not provide the normal per-process `C:` dos-device mapping, so the platform
layer tries `\\GLOBAL??\\C:` and the native `\\Device\\HarddiskVolume*`
names, then caches the volume which contains the wad.

## next milestone: make it playable

the next change will connect qemu keyboard input without depending on win32:

1. finish the synchronous system-thread readers for
   `\\Device\\KeyboardClass1` and `\\Device\\KeyboardClass0`
2. shut those threads down safely when the driver unloads
3. build, sign, and install the driver in the disposable win0 vm while keeping
   the known-good framebuffer-only driver and qcow2 backup intact
4. verify escape opens doom's menu, then verify movement, firing, menus, and
   key releases

sound remains disabled for now. it will be considered only after keyboard
input is reliable; runlevel 0 does not provide the ordinary windows audio
stack either.

## layout

- `native/win0-doom.c` — puredoom platform layer using native nt apis
- `native/build-win0-doom.sh` — clang/lld native executable build
- `driver/win0doom-display.c` — qemu stdvga framebuffer bridge
- `driver/build-driver.sh` — wdk kernel-driver build
- `driver/install-win0doom-display.reg` — boot-start service definition
- `native/driver-display-test.c` — full-screen gradient test client
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
