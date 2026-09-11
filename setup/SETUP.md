# setup

guides for actually getting win0-doom running. the project README explains
what the software is; this folder explains how to reach a running doom
session, per starting point and per host os. the flow is complicated enough
that it gets its own directory.

## pick a path

| where you are                                   | read                                       |
|-------------------------------------------------|--------------------------------------------|
| nothing; starting from microsoft inputs         | [from-scratch.md](from-scratch.md)         |
| a bootable validationos runlevel-0 disk or vm   | [have-vm.md](have-vm.md)                   |
| linux host                                      | [linux.md](linux.md)                       |
| windows host                                    | [windows.md](windows.md)                   |
| macos host                                      | [macos.md](macos.md)                       |

the host guides cover launching qemu and driving it without touching the
window: monitor scripting, keystroke injection, screenshots, and headless
verification.

## quick reference

everything here was verified against a live run (kvm host, qemu 10.2.3,
the v0.1-playable release image).

- the ccs launch command is the full native path:

  ```
  \SystemRoot\system32\win0doom.exe
  ```

  a bare `win0doom.exe` fails with "is not recognized as an internal or
  external command" because ccs's working directory is not system32.
  nothing else at the prompt does path magic for you.

- the ccs banner advertises `TLIST` (running processes) and `MEMSTAT`
  (memory usage). it does not know `cls`, `clear`, or any other comfort.
  there is no way to clean the screen; scroll up in your soul.

- if the full path is "not recognized", the booted disk does not contain
  the payload. rollback and pre-payload images boot to an identical
  banner and identical prompt — the only difference is what is on the
  disk. check names, timestamps, and sha256 before booting. this exact
  confusion cost a full debugging session once already.

- images differ in whether they auto-launch. some builds set the session
  manager's `CcsCommand` value so doom starts by itself after session
  init (see README, "automated vm image build"). the v0.1-playable
  release presented a bare `>` prompt and required the manual command.

- the qemu hmp command is `sendkey` (singular), one key per invocation.
  `sendkeys` with a key list does not exist and answers "unknown
  command". key names are case-sensitive; uppercase is `shift-x`.
  mappings that matter here: `\` = `backslash`, `.` = `dot`,
  enter = `ret`, space = `spc`, `:` = `shift-semicolon`.

- inject per key with ~300ms between commands. one socat burst of all
  `sendkey` lines can outrun the ccs raw-input path under load and the
  command arrives garbled — `docs/demo-boot-to-doom.mp4` contains both
  the garbled burst and the clean slow retyping. the reliable loop
  lives in [linux.md](linux.md).

- headless verification without looking at the window: count distinct
  colors in a `screendump`. the runlevel-0 text console measured 19
  distinct colors; a doom attract-demo frame measured 114. ~19 means you
  are still staring at the console.

- boot to the ccs prompt takes tens of seconds under kvm/ovmf. after
  sending the launch command, allow ~10s for doom init before capturing.

- at runlevel 0, `system_powerdown` produced no visible effect within
  seconds — there is no comfortable usermode around to handle acpi in a
  hurry. the verified flow ends the vm with `quit`, which is acceptable
  only because the booted disk is a disposable copy. never run the vm
  directly on a pristine release image; copy it first and keep the
  original checksummed.

## scripts

windows host helpers live in [scripts/](scripts/). they speak to a qemu
tcp monitor:

- `start-win0-vm.ps1` — parameterized example launch with a tcp monitor
- `win0-monitor.ps1` — send one monitor command, print the response
- `win0-send-launch.ps1` — type the launch command into the guest, press enter
- `win0-screendump.ps1` — capture the framebuffer to png through the monitor
- `win0-verify.ps1` — console-vs-doom verdict from the distinct-color count

linux and macos hosts use `socat` one-liners instead; see the host guides.
