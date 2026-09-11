# you already have a validationos vm or disk

for an existing bootable validationos runlevel-0 disk that lacks the
win0-doom payload, or for swapping payloads on an already-generated disk.
if you have nothing yet, start at [from-scratch.md](from-scratch.md).

## the payload is four things

- `Windows\System32\win0doom.exe` — native-subsystem pe, imports
  `ntdll.dll` only
- `Win0DoomDisplay.sys` plus its boot-start service registration
  (`driver/install-win0doom-display.reg`, which also inserts the driver
  into the keyboard setup class `UpperFilters` ahead of `kbdclass`)
- the doom wad
- test signing enabled for the boot chain (the image builders do this
  for you; manual deployments need the bcdedit equivalent)

## linux host: deploy-vm.sh

`tools/deploy-vm.sh` creates a compressed, checksummed rollback of the
target disk, then replaces only the payload files in place.
`tools/rollback-vm.sh` restores that backup while preserving the
displaced image. this is the supported path for qcow2 disks on linux.

## manual offline deployment

with the vm fully shut down, mount the disk (guestfish, or qemu-nbd +
ntfs-3g on linux; native vhd mount on windows) and place the payload
items. hive edits need an offline registry tool — hivexregedit on
linux, `reg.exe` against the mounted hive on windows. never mount a
disk that a running qemu still holds open.

## booting the right disk

this is the classic failure, hit in anger: rollback and pre-payload
disks boot to the exact same runlevel-0 banner and the exact same
prompt as a payload disk. the screen cannot tell you what is missing.
if the launch command answers "not recognized" with the full correct
path, the disk you booted does not contain `win0doom.exe`. compare
filenames, timestamps, sizes, and sha256 against your known-good
release before booting, and remember that a disk with a name like
`*.before-*.qcow2` in its ancestry is exactly what it says on the tin.

keep pristine release images untouched: verify the checksum, copy, and
boot the copy.

## launching

at the ccs prompt, full native path required:

```
\SystemRoot\system32\win0doom.exe
```

a bare `win0doom.exe` fails — ccs's working directory is not system32.
alternatively set the session manager's `CcsCommand` value to the same
path and doom auto-launches after session init (README, "automated vm
image build"). headless launch through the qemu monitor: see the host
guides; the launch keystrokes do not leak into the game because opening
the driver's control device resets the keyboard ring.
