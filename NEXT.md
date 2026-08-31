# current checkpoint and next changes

this is an intentional work-in-progress checkpoint. commit `e511106` is the
last fully proven state: doom boots and renders inside validationos runlevel 0,
but it cannot receive keyboard input.

## what is implemented in this checkpoint

- the native doom client polls a new nonblocking keyboard ioctl on the existing
  display-driver handle
- raw `KEYBOARD_INPUT_DATA` set-1 make/break records are translated into
  puredoom keys, including escape, enter, arrows, wasd, control, alt, shift,
  space, tab, backspace, function keys, and number keys
- the display driver exposes that keyboard ioctl
- the driver has a spinlock-protected 128-record keyboard ring buffer
- the driver opens both qemu keyboard class devices from kernel mode and starts
  one synchronous reader system thread per successfully opened device

## why the design changed

opening `\\Device\\KeyboardClass0` or `\\Device\\KeyboardClass1` directly from
the runlevel-0 native process returned `STATUS_ACCESS_DENIED` (`0xc0000022`).
moving the open into the kernel driver succeeded, but the first asynchronous
`ZwReadFile` design returned `STATUS_INVALID_PARAMETER` (`0xc000000d`).

the replacement design therefore performs a synchronous blocking `ZwReadFile`
on a dedicated kernel system thread for each keyboard class device. those
threads copy official `KEYBOARD_INPUT_DATA` records into the ring buffer. the
game thread only drains the ring through the ioctl, so puredoom's event queue
still has a single producer in user mode.

## known broken edge at this exact checkpoint

the source is halfway through the async-reader-to-system-thread conversion.
`Win0DoomUnload` still references the removed `EventHandle` field, so
`./driver/build-driver.sh` currently fails with two errors at that reference.
this is documented deliberately instead of hiding a mid-surgery commit behind
a misleading green status.

## next changes, in order

1. replace the stale unload loop with orderly thread shutdown:
   - set `StopKeyboardThreads`
   - close each keyboard file handle so its blocking read returns
   - wait for each system thread with `ZwWaitForSingleObject`
   - close each thread handle
2. initialize the spinlock, stop flag, and ring indexes in `DriverEntry`
3. rebuild both the driver and native executable and inspect their imports
4. preserve the last signed framebuffer-only driver, sign the new driver, and
   install it only in the disposable qcow2 clone
5. boot runlevel 0 and use qemu `sendkey esc` as the first proof: during doom's
   demo loop, any received key should open the menu
6. test arrows, wasd, control/fire, space/use, enter, escape, and make/break
   behavior; only then call the port playable

## recovery points which must remain untouched

- the original validationos qcow2 is not the test target
- the disposable clone has a complete pre-driver qcow2 backup
- the vm contains `win0doom-display.sys.pre-keyboard`, the known-good signed
  framebuffer-only driver

if the input driver crashes the vm, restore only the disposable clone or its
known-good driver. do not modify the original validationos disk.
