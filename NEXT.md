# keyboard development record

doom is now playable in validationos runlevel 0. this file preserves the
failed paths, the working design, and the exact proof so the same dead ends do
not have to be rediscovered.

## observations

- the native process receives null console, stdin, stdout, and stderr handles
- `NtReadFile` on the null standard-input handle returns
  `STATUS_INVALID_HANDLE` (`0xc0000008`)
- opening `\Device\KeyboardClass0` or `KeyboardClass1` from user mode returns
  `STATUS_ACCESS_DENIED` (`0xc0000022`)
- an asynchronous kernel `ZwReadFile` returned `STATUS_INVALID_PARAMETER`
  (`0xc000000d`)
- synchronous kernel reader threads could open keyboard-class devices, but
  competed with ccs for their queues and did not reliably deliver input
- terminating ccs from user mode was denied; a narrowly scoped experimental
  kernel path which verified the target basename and terminated only ccs
  immediately bugchecked with `CRITICAL_PROCESS_DIED` (`0xef`)

the ccs termination experiment was removed. it is not part of the source or
the installed driver.

## working architecture

the display driver now also implements the standard wdm keyboard upper-filter
pattern used by microsoft's `kbfiltr` sample:

1. the keyboard setup class loads `Win0DoomDisplay` before `kbdclass`
2. pnp calls the driver's `AddDevice`, which attaches a filter device to each
   keyboard stack
3. the filter intercepts `IOCTL_INTERNAL_KEYBOARD_CONNECT`, saves the original
   `CONNECT_DATA`, and substitutes `Win0DoomKeyboardServiceCallback`
4. the callback runs at dispatch level, copies each `KEYBOARD_INPUT_DATA`
   record into the driver's spinlock-protected ring, and calls the original
   class callback unchanged
5. ccs keeps receiving keyboard input, while doom drains its copy through
   `IOCTL_WIN0DOOM_GET_KEYBOARD`
6. opening the control device resets the ring read position so launch-command
   keystrokes are not replayed into doom

the registry entry is in `driver/install-win0doom-display.reg`. it preserves
`kbdclass` and inserts `Win0DoomDisplay` before it in the multi-string value.

## proven test

the final test used a clean validationos runlevel-0 boot and qemu keyboard
injection:

1. ccs remained alive and accepted the native path used to launch doom
2. doom entered its attract demo without consuming the launch command
3. escape opened the main menu
4. enter selected new game, episode one, and the default skill
5. holding w moved the player from the e1m1 spawn into the blue-floor room
6. control fired the pistol; the muzzle flash rendered and ammo decreased

the vm survived two cold boots with the filter installed and showed no
bugcheck.

## recovery points used during development

- the original validationos qcow2 was never the filter test target
- the disposable clone has a complete pre-driver qcow2 backup
- the vm contains a pre-filter system hive and signed driver beside their live
  files
- a second driver backup preserves the first working filter before the
  launch-input ring reset

## remaining work

- add audio only if a small native/kernel bridge is worth the complexity
- replace the hard-coded stdvga physical aperture with discovered resources
- package test signing and deployment into a reproducible disposable-vm flow
- test device removal and driver-unload paths beyond the normal boot-only use
