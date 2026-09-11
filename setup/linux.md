# linux host

kvm host, qemu with a unix-socket monitor. every command below ran
end-to-end during a verified session (gentoo, qemu 10.2.3,
v0.1-playable image): boot, payload launch by injected keystrokes,
framebuffer capture, and headless verification.

## launching the vm

example real-world invocation (two-disk layout: full windows on ide.0,
validationos on ide.1 winning the boot order). adjust paths; keep
`-vga std` — the driver targets the qemu standard-vga aperture.

```bash
qemu-system-x86_64 -name windows-11-two-disk-validationos \
 -monitor unix:/run/user/1000/windows-11-two-disk.monitor.sock,server=on,wait=off \
 -machine q35,accel=kvm -cpu host,hv_relaxed=on,hv_vapic=on,hv_spinlocks=0x1fff \
 -smp 8,sockets=1,cores=4,threads=2 -m 8G \
 -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/OvmfX64/OVMF_CODE.fd \
 -drive if=pflash,format=raw,file=$HOME/VMs/windows-11-two-disk/OVMF_VARS.fd \
 -drive if=none,id=system_disk,format=qcow2,cache=writeback,discard=unmap,file=/mnt/Data/VMs/windows-11-two-disk/windows-system-128gb.qcow2 \
 -device ide-hd,drive=system_disk,bus=ide.0,bootindex=2 \
 -drive if=none,id=validationos_disk,format=qcow2,cache=writeback,discard=unmap,file=/mnt/Data/VMs/windows-11-two-disk/validationos-playable-run.qcow2 \
 -device ide-hd,drive=validationos_disk,bus=ide.1,bootindex=1 \
 -boot menu=on,strict=on -device qemu-xhci -device usb-tablet \
 -device ich9-intel-hda -device hda-duplex,audiodev=desktop_audio \
 -audiodev pipewire,id=desktop_audio \
 -nic user,model=e1000e,hostfwd=tcp:127.0.0.1:33890-:3389 \
-vga std -display gtk,gl=on -rtc base=localtime,clock=host,driftfix=slew \
 -global ICH9-LPC.disable_s3=1
```

(s3 sleep is disabled because nothing at runlevel 0 would ever resume it
gracefully.)

before booting, make the run disk disposable:

```bash
cd /mnt/Data/VMs/windows-11-two-disk
sha256sum -c validationos-64gb-v0.1-playable.qcow2.sha256
cp --reflink=auto validationos-64gb-v0.1-playable.qcow2 validationos-playable-run.qcow2
```

boot the copy. the pristine image never sees a write.

## monitor automation

the hmp monitor on the unix socket works fine with socat. the banner
and readline echo noise (`[K`, `[D` sequences) are cosmetic; ignore
them.

single command:

```bash
printf 'screendump /tmp/win0.ppm\n' | socat -t 3 - \
  UNIX-CONNECT:/run/user/1000/windows-11-two-disk.monitor.sock
```

keystroke injection (many commands from a file — this exact sequence
typed `\SystemRoot\system32\win0doom.exe` + enter and launched doom):

```bash
for k in backslash shift-s y s t e m shift-r o o t backslash \
         s y s t e m 3 2 backslash w i n 0 d o o m dot e x e ret; do
  echo "sendkey $k"
done > /tmp/win0-keys.txt
socat -t 20 OPEN:/tmp/win0-keys.txt \
  UNIX-CONNECT:/run/user/1000/windows-11-two-disk.monitor.sock
```

gotchas, all hit in anger:

- the command is `sendkey`, singular. `sendkeys <key> <key> ...` does
  not exist; the monitor answers "unknown command: 'sendkeys'" and
  nothing reaches the guest.
- feeding every `sendkey` line in one socat burst can outrun the ccs
  raw-input path under load: keys drop and the command arrives garbled
  (observed live: `\SystemRoot\ssdoom.exe`, rejected as "not
  recognized"; the identical burst typed cleanly on an idle host).
  the reliable method sends one `sendkey` per socat invocation with
  ~300ms between keys:

  ```bash
  for k in backslash shift-s y s t e m shift-r o o t backslash \
           s y s t e m 3 2 backslash w i n 0 d o o m dot e x e ret; do
    printf "sendkey $k\n" | socat -t 2 - UNIX-CONNECT:$MONITOR_SOCK > /dev/null 2>&1
    sleep 0.3
  done
  ```

  this slow path is what `docs/demo-boot-to-doom.mp4` shows being
  accepted after the garbled burst was rejected.
- key names are case-sensitive: `shift-s` works, `shift-S` does not.
- `\` is `backslash`, `.` is `dot`, enter is `ret`.
- the v0.1-playable image boots to a bare prompt (no `CcsCommand`
  auto-launch), so injection or typing is required. an image with
  `CcsCommand` set launches doom by itself; skip the injection.
- allow ~10s between the final `ret` and a screendump for doom init.

## screenshots and headless verification

`screendump` writes a ppm on the host:

```bash
magick /tmp/win0.ppm /tmp/win0.png
```

verdict without eyes or vision models — count distinct colors:

```bash
python3 - << 'EOF'
from PIL import Image
im = Image.open('/tmp/win0.png').convert('RGB')
print('distinct colors:', len(im.getcolors(1000000)))
EOF
```

measured on the verified run: runlevel-0 text console = 19 distinct
colors, doom attract-demo frame = 114. ~19 means doom is not running:
wrong launch path, or a payload-less disk (see have-vm.md).

## the full verified flow, condensed

```bash
# 0. confirm the release image, make a disposable copy
sha256sum -c release.qcow2.sha256 && cp --reflink=auto release.qcow2 run.qcow2
# 1. start qemu (see above), wait ~40s for the ccs prompt
# 2. inject the launch command (see above), wait ~10s
# 3. screendump, convert, count colors
# 4. 114-ish distinct colors: doom is running at runlevel 0. done.
```

## recording a demo video

`docs/demo-boot-to-doom.mp4` was captured host-side. the workflow, with
the lessons learned baked in:

- `wf-recorder -g x,y,w,h` on the exact qemu window region; geometry
  comes from `hyprctl clients -j` (needs
  `HYPRLAND_INSTANCE_SIGNATURE` in the environment). on wayland the
  capture records the screen region, not the window: the recording
  workspace must stay empty and focused for the whole take, because
  every overlap and workspace switch lands in the footage
- fixed duration with `timeout -s INT <secs> wf-recorder ...` —
  SIGINT finalizes the mp4 properly; a hard-killed recorder leaves a
  "moov atom not found" corpse
- the distinct-color doom-init check works on raw `screendump`
  framebuffers only. window captures include the gtk scaler, which
  interpolates even the text console into thousands of colors, so
  color counts cannot tell console from doom there — probe frames
  with a vision model or eyes instead
- review pass: downscale (`scale=944:490,fps=8,crf 32`) to fit hosted
  vision-model video-input limits, ask for bad segments plus a
  timestamped timeline, then probe individual frames on both sides of
  every proposed cut before trusting it — small models confidently
  mislabel both directions (doom gameplay as "other window", console
  as "discord")
- splice with `trim`/`setpts`/`concat` in one filter_complex and
  re-encode `-crf 24 -pix_fmt yuv420p -movflags +faststart`: the
  2:48 final came out at 13.5 mb, comfortable for git

## shutdown

`system_powerdown` produced no visible effect within seconds at
runlevel 0 — there is no usermode stack around to service acpi promptly.
the verified flow sent `quit`. that is fine for a disposable run copy
and would not be fine for anything you care about; keep verified
backups (NEXT.md, "recovery points").
