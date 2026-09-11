# macos host

running works; building the payload does not. the native exe and the
wdk driver build expect linux or windows toolchains, so build there
(or use a release image) and bring the finished qcow2/vhdx to the mac.

## qemu

```bash
brew install qemu socat imagemagick
```

acceleration is hvf, and only for guests matching the host
architecture:

- x86_64 mac: `-machine q35,accel=hvf -cpu host` works
- apple silicon: hvf cannot run x86_64 guests. qemu-system-x86_64
  falls back to tcg (`-machine q35 -cpu max`, no `accel=hvf`):
  functional, slow, fine for a 320x200 doom
- omit the linux `hv_*` cpu enlightenments; they are kvm-specific
- `-vga std` is mandatory: the driver targets the qemu standard-vga
  aperture

firmware: use qemu's bundled edk2 —

```bash
QEMU_SHARE="$(brew --prefix qemu)/share/qemu"
-drive if=pflash,format=raw,readonly=on,file=$QEMU_SHARE/edk2-x86_64-code.fd \
-drive if=pflash,format=raw,file=$HOME/VMs/win0-doom/OVMF_VARS.fd
```

with `OVMF_VARS.fd` copied once from the bundled
`edk2-i386-vars.fd` (writable, per-vm).

otherwise the launch mirrors linux.md: validationos disk first in the
boot order, 8g+ ram, disposable copy of a checksum-verified image.

## monitor automation

identical to linux: socat against the unix-socket monitor, one
`sendkey` per key (there is no `sendkeys`), `screendump` to ppm. the
linux.md injection sequence and gotchas apply verbatim — swap paths.

```bash
printf 'screendump /tmp/win0.ppm\n' | socat -t 3 - \
  UNIX-CONNECT:/tmp/win0.monitor.sock
magick /tmp/win0.ppm /tmp/win0.png
```

verification is the distinct-color count (~19 = console, ~114 = doom);
see SETUP.md. any python3 with pillow does, or use imagemagick:

```bash
magick /tmp/win0.png -format %k info:
```

(`%k` is the number of unique colors.)

## shutdown

same as everywhere: `system_powerdown` is not promptly serviced at
runlevel 0; `quit` through the monitor, on a disposable copy.
