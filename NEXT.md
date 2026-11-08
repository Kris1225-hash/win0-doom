# keyboard and audio development record

doom is now playable, with sound effects, in validationos runlevel 0. this file
preserves the failed paths, the working designs, and the exact proof so the
same dead ends do not have to be rediscovered.

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

## audio development record

### observations

- runlevel 0 has no audio stack: the image contains no `portcls`, `ks`,
  `hdaudbus`, `sysaudio`, `wdmaud`, no audio service and no `audiosrv`/
  `mmdevapi`/`audiodg`. `winmm.dll` exists only as a forwarder stub. the HAL
  is a thin forwarder into `ntoskrnl`, and `isapnp.sys` is present, so a driver
  has to own the hardware.
- PureDOOM's mixer is pull-based. `doom_get_sound_buffer()` mixes exactly one
  block of 512 interleaved stereo frames (2048 bytes, S16LE, 11025 Hz) per
  call and advances the channel sample pointers. the `I_UpdateSound()` call in
  `D_DoomLoop` is inside `#if 0`, and `doom_update()` does not mix, so the
  platform must drain one block every 46.44 ms.
- music is not implemented. `doom_tick_midi()` only returns MIDI events at
  140 Hz, and the image has no OPL/FM synth.

### working architecture

1. the client drains one block per 46.44 ms (three-block catch-up cap) and
   submits it with `IOCTL_WIN0DOOM_SOUND_WRITE` (method buffered).
2. the driver keeps a 64 KiB drop-oldest ring and a 32 KiB physically
   contiguous DMA buffer below 16 MiB, allocated so it does not cross a 64 KiB
   boundary.
3. a PASSIVE pump thread resets the DSP (write 1 to `0x226`, poll `0x22e`,
   read `0xaa` from `0x22a`), reads the DSP version, sets 16-bit stereo output
   at 11025 Hz (`0x41` followed by `0x2b`, `0x11` — QEMU's argument stack pops
   low last), programs 8237 channel 5 (unmask `0xd4=0x01`, mode `0xd6=0x59`,
   page `0x8b`, word address `0xc4`, word count `0xc6`) and starts 16-bit
   auto-init playback (`0xb6`, format `0x30`, count low/high).
4. refills are anchored to the real DMA cursor: clear the flip-flop with
   `0xd8`, read the channel-5 count register `0xc6` twice. QEMU's 16-bit
   controller reports that value in **words**, so the byte position is
   `(base_count - count_now) * 2`; the code multiplies by two and keeps every
   write 4-byte aligned. the write cursor stays one lead (16 KiB, 371 ms)
   ahead of the play cursor and is re-locked if it falls behind.
5. no ISR is installed. each pump pass reads `0x22f`, which acknowledges the
   emulated 16-bit DMA interrupt and keeps the ISA line low.
6. the driver appends DSP/DMA state and per-second ring counters to
   `\SystemRoot\win0doom-audio.log`. creating `win0-audio-selftest` in
   `C:\Windows\System32` makes the client play a 1 s 440 Hz tone before doom;
   `win0-audio-dump` tees the exact submitted PCM to `win0doom-client.pcm`.

### failed paths

- estimating the play cursor from `KeQueryPerformanceCounter() * 44100 B/s`
  drifts by a non-multiple-of-4 byte count. the DAC then reads a byte-rotated
  16-bit stream: episodes of full-scale broadband noise (peak 1.0, RMS
  −6 dBFS) while the client's own PCM peaked at 0.2. anchoring to the 8237
  count removed it.
- treating the 16-bit count register as bytes made the position advance at
  half rate, so the fill window covered only half the buffer and left a stale
  ~30 ms fragment looping.
- qemu's `-audiodev wav` never finalizes the RIFF/data sizes on shutdown. the
  analyzer derives the length from the file size and writes correct headers
  for its slices.
- current x86-64 linux kernels cannot drive the emulated SB16 (`CONFIG_ISA`
  is unselectable), so the host transport proof used a custom kernel build;
  the AC97 control produced bit-identical PCM.

### proven test

two clean runlevel-0 cold boots of a scratch copy with `-device sb16` and
`-audiodev wav`:

- DSP reset ack `0xaa`, version `0x0405`, rate 11025, mode `0x59`, ready=1
- 2276 blocks played, 0 dropped, about 1 s of startup silence
- capture: 117.7 s, 44100 Hz stereo, peak −6.02 dBFS, RMS −28.2 dBFS,
  verdict `audio-present`; top FFT peaks 437/439 Hz (the 440 Hz self-test)
- the submitted PCM (`win0doom-client.pcm`) and the capture track within a few
  dB second by second, and no second exceeds 0.6 peak
- the same image booted without `-device sb16`: doom renders and the keyboard
  works; the driver logs `reset-ack=0xff` and `status=0xc0000483`
  (`STATUS_DEVICE_HARDWARE_ERROR`) and stays non-fatal

## recovery points used during development

- the original validationos qcow2 was never the filter test target
- the disposable clone has a complete pre-driver qcow2 backup
- the vm contains a pre-filter system hive and signed driver beside their live
  files
- a second driver backup preserves the first working filter before the
  launch-input ring reset

## remaining work

- music: PureDOOM only emits MIDI events, so the soundtrack needs an OPL/FM
  synth or a host-side MIDI bridge; sound effects are complete
- exercise device removal and driver-unload at runtime. runlevel 0 exposes no
  SCM or PnP removal trigger, so the teardown paths are implemented and were
  independently audited against the WDM filter rules but cannot be induced in
  this image
- an interrupt-driven SB16 path, if a future HAL exposes a usable ISA IRQ;
  the current sink deliberately avoids `IoConnectInterrupt` and polls the DMA
  count and the `0x22f` acknowledge port
- the collected audio logs are missing their first `audio init start` line
  (the DMA physical address); the DSP line and the per-second counters are
  present. worth fixing only if the DMA address is needed for debugging
- real-hardware support is out of scope: both the display (`1234:1111`
  stdvga) and the audio device (emulated SB16 at `0x220`) are qemu-specific
