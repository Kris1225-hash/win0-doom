# from scratch

end to end: microsoft inputs and source tree in, running doom in
validationos runlevel 0 out. assumes nothing prebuilt. if you already have
a bootable runlevel-0 disk and only need the payload on it, read
[have-vm.md](have-vm.md) instead.

## 0. what you need

microsoft inputs (bring your own; none of it is redistributable here):

- the official validationos iso, which supplies the bootable win4 vhdx
- a `GenImage`-generated **runlevel-0** wim from the same validationos
  build. this is mandatory: the stock vhdx contains neither the win0
  packages nor `ccs.exe`, and no registry edit can conjure them (see
  README, "automated vm image build")

game data:

- a legally obtained doom wad (`doom1.wad` shareware is fine)

toolchain:

- llvm `clang-cl` + `lld-link` (linux hosts: expected in
  `/usr/lib/llvm/22/bin` by default; override with `WINDOWS_SDK_ROOT`
  and friends where the scripts allow it)
- microsoft windows sdk/wdk `10.0.26100.0`, extracted at
  `toolchain/wdk-26100/c`
- `osslsigncode` for test-signing the driver on linux hosts
- linux builder path additionally needs: qemu with kvm and nbd support,
  ovmf, ntfs-3g, wimlib, hivex, socat
- windows builder path additionally needs: an elevated windows 10/11
  host with the storage cmdlets and the validationos test-signing
  certificate chain

## 1. get the source

```bash
git clone --recurse-submodules git@github.com:Kris1225-hash/win0-doom.git
cd win0-doom
```

## 2. build the payload

```bash
./native/build-win0-doom.sh   # win0doom.exe: native-subsystem pe, imports ntdll only
./driver/build-driver.sh      # Win0DoomDisplay.sys: framebuffer bridge + keyboard filter
```

the windows image builder rebuilds both itself; step 3 is enough on a
windows host unless you want standalone artifacts.

## 3. build the vm image

linux:

```bash
./tools/build-vm-image.sh \
  /path/to/official-validationos.iso \
  /path/to/generated-runlevel-0.wim \
  ./dist/win0-doom.qcow2 \
  /path/to/doom1.wad
```

windows (elevated powershell):

```powershell
.\tools\build-vm-image.ps1 `
  -GeneratedRunLevel0Wim C:\path\ValidationOS.wim `
  -OutputVhdx C:\VMs\win0-doom.vhdx `
  -DoomWad C:\path\doom1.wad
```

the builder test-signs artifacts, constructs the bcd, overlays the wim,
installs driver + game + wad, checks the result, and writes a sha-256
file next to it. it refuses to overwrite an existing output.

## 4. boot

- give the vm 8 gib or more
- uefi/ovmf firmware; the generated disk must win the boot order
- `-vga std` is required: the driver targets the qemu standard-vga
  aperture and nothing else (README, "giant warning label")
- boot a disposable copy of the built image, never the pristine one:
  verify its sha256, then copy, then boot the copy

whether doom starts by itself depends on the image's `CcsCommand`
setting. if you land on a bare `>` prompt instead, launch by hand:

```
\SystemRoot\system32\win0doom.exe
```

host-specific launch commands, monitor automation, and screenshots:
[linux.md](linux.md), [windows.md](windows.md), [macos.md](macos.md).

## 5. verify

- runlevel-0 banner plus `>` prompt: boot succeeded
- doom frame (colorful game image, hud along the bottom): done
- headless: screendump and count distinct colors — ~19 means still the
  console, ~100+ means doom is rendering (SETUP.md, quick reference)
