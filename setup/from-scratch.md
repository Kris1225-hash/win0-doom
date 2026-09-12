# from scratch

end to end: microsoft inputs and source tree in, running doom in
validationos runlevel 0 out. assumes nothing prebuilt. if you already have
a bootable runlevel-0 disk and only need the payload on it, read
[have-vm.md](have-vm.md) instead.

## 0. what you need

budget an afternoon for a first run. the two slow parts are generating
the runlevel-0 wim on a windows host (the adk `GenImage` step) and the
first image build; none of it is interactive. once the toolchains are
installed, steps 1-5 take well under an hour on a warm machine.

install the whole linux host toolchain in one go, then read on for what
each piece is for:

```bash
# gentoo (atoms as of the llvm-core-era tree; qsearch if one has moved)
emerge -av app-emulation/qemu sys-firmware/edk2-bin app-misc/hivex \
  sys-fs/ntfs3g app-arch/wimlib net-misc/socat app-crypt/osslsigncode \
  llvm-core/clang llvm-core/lld

# debian/ubuntu
sudo apt install qemu-system-x86 ovmf ntfs-3g wimtools libhivex-bin \
  socat osslsigncode clang lld
```

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

you should see: the repo tree with `native/`, `driver/`, `tools/`, and
`setup/` present. the tightened `toolchain/` layout arrives with your
sdk/wdk extraction, not from the clone.

## 2. build the payload

```bash
./native/build-win0-doom.sh   # win0doom.exe: native-subsystem pe, imports ntdll only
./driver/build-driver.sh      # win0doom-display.sys: framebuffer bridge + keyboard filter
```

the windows image builder rebuilds both itself; step 3 is enough on a
windows host unless you want standalone artifacts.

you should see: `native/build/win0doom.exe` and
`driver/build/win0doom-display.sys`, each followed by an `llvm-readobj`
dump. the import list for `win0doom.exe` should name `ntdll` and nothing
else — if a crt or kernel32 shows up, the link line was edited and the
whole "no host runtime" premise is broken.

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

you should see: staging noise from the wim apply and ntfs-3g, a
test-signing pass, and finally the output path plus its sha-256 printed
to the terminal, e.g. `dist/win0-doom.qcow2` and
`dist/win0-doom.qcow2.sha256`. the script refusing to overwrite is a
safety check, not a failure — delete or rename the old output, or pick a
new path.

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

you should see: ovmf splash, then windows boot manager text, then the
validationos banner ("hello! the os is booted to runlevel 0 - only")
with the `>` prompt underneath. doom itself then either starts on its
own or waits for the command above.

host-specific launch commands, monitor automation, and screenshots:
[linux.md](linux.md), [windows.md](windows.md), [macos.md](macos.md).

## 5. verify

- runlevel-0 banner plus `>` prompt: boot succeeded
- doom frame (colorful game image, hud along the bottom): done
- headless: screendump and count distinct colors — ~19 means still the
  console, ~100+ means doom is rendering (SETUP.md, quick reference)

for a visual reference of every stage above, watch
[../docs/demo-boot-to-doom.mp4](../docs/demo-boot-to-doom.mp4): 69
seconds of firmware, banner, prompt, the injected launch, and the
attract demo, recorded from qemu's window. it also shows what a failed
injection looks like.

## 6. when it goes wrong

the failure modes that actually happened here, in roughly the order
people hit them:

| symptom | cause | fix |
| --- | --- | --- |
| `clang-cl` / `lld-link` not found, or sdk headers missing in step 2/3 | llvm or the sdk/wdk tree not where the scripts expect | install llvm (one-liner above); confirm `toolchain/wdk-26100/c` exists; the scripts honor `WINDOWS_SDK_ROOT` where they can |
| step 3 stops early saying the output exists | the builder refuses to overwrite by design | move or delete the old output, or pass a fresh path |
| firmware drops to a uefi shell or "no bootable device" | the built disk lost boot order, or you're booting the wrong file | select the built disk first in firmware; verify sha-256 and boot a disposable copy, never the pristine image |
| vm sits at a bare `>` prompt and no doom appears | `CcsCommand` wasn't set, or the injected launch never landed | type `\SystemRoot\system32\win0doom.exe` by hand; if injecting via the monitor, send one `sendkey` at a time with ~300ms between them — a batch arrives garbled ([linux.md](linux.md)) |
| doom launches but the frame is black, scrambled, or absent | wrong vga model | this port renders only on qemu std-vga: pass `-vga std` |
| screendump color count parks at ~19 | that's the console; doom either never started or already exited (wad not found on the discovered volume) | check the wad made it into the image and read the file-layer notes in the README |
