#ifndef WIN0DOOM_AUDIO_H
#define WIN0DOOM_AUDIO_H

/*
 * PCM ABI between the native Doom client (win0doom.exe) and the
 * Win0DoomDisplay control device.
 *
 * PureDOOM's doom_get_sound_buffer() mixes exactly one block per call:
 * 512 interleaved stereo frames, signed 16-bit little-endian, at 11025 Hz.
 * That is 2048 bytes and 46.4399 ms of playback per call, so the client
 * must drain at 2048 bytes every ~46.44 ms (44100 bytes per second).
 */

#define WIN0DOOM_PCM_SAMPLE_RATE 11025UL
#define WIN0DOOM_PCM_CHANNELS 2UL
#define WIN0DOOM_PCM_BITS_PER_SAMPLE 16UL
#define WIN0DOOM_PCM_BLOCK_FRAMES 512UL
#define WIN0DOOM_PCM_BLOCK_BYTES 2048UL
#define WIN0DOOM_PCM_BYTES_PER_SECOND 44100UL

/*
 * CTL_CODE(FILE_DEVICE_UNKNOWN, function, METHOD_BUFFERED, access) values.
 * They match the existing display/keyboard codes in win0doom-display.c:
 *   PRESENT      0x800 FILE_WRITE_ACCESS -> 0x0022a000
 *   GET_KEYBOARD 0x801 FILE_ANY_ACCESS   -> 0x00222004
 */
#define WIN0DOOM_IOCTL_SOUND_WRITE 0x0022a008UL
#define WIN0DOOM_IOCTL_SOUND_STATUS 0x0022200cUL
#define WIN0DOOM_IOCTL_SOUND_RESET 0x0022a010UL

/*
 * SOUND_WRITE: METHOD_BUFFERED, FILE_WRITE_ACCESS.
 *   input:  4-byte aligned PCM bytes, at most WIN0DOOM_AUDIO_MAX_WRITE bytes.
 *   output: none.
 *   Returns STATUS_SUCCESS with Information = bytes accepted. Playback
 *   never blocks; when the ring is full the oldest bytes are discarded and
 *   counted in RingDroppedBytes.
 *
 * SOUND_STATUS: METHOD_BUFFERED, FILE_ANY_ACCESS.
 *   output: WIN0DOOM_AUDIO_STATUS.
 *   Returns STATUS_SUCCESS with Information = sizeof(WIN0DOOM_AUDIO_STATUS).
 *
 * SOUND_RESET: METHOD_BUFFERED, FILE_WRITE_ACCESS.
 *   Discards queued PCM and restarts the device. Returns STATUS_SUCCESS.
 */
#define WIN0DOOM_AUDIO_MAX_WRITE 8192UL

typedef struct _WIN0DOOM_AUDIO_STATUS {
    unsigned long RingCapacityBytes;
    unsigned long RingQueuedBytes;
    unsigned long RingDroppedBytes;
    unsigned long Submissions;
    unsigned long BlocksPlayed;
    unsigned long SilenceBytes;
    unsigned long SampleRate;
    unsigned long Channels;
    unsigned long BitsPerSample;
    unsigned long DeviceReady;
    unsigned long PlaybackActive;
    unsigned long DeviceStatus;
} WIN0DOOM_AUDIO_STATUS;

#ifdef _KERNEL_MODE

/*
 * Implemented by win0doom-audio.c, called by the display driver.
 *
 * Win0DoomAudioInitialize starts the PCM sink. It must not fail the
 * driver load when the audio device is absent: display and keyboard must
 * keep working, and SOUND_STATUS reports DeviceReady = 0.
 *
 * Win0DoomAudioShutdown stops playback, terminates the pump thread and
 * releases the DMA buffer. Safe to call when initialize failed.
 *
 * Win0DoomAudioDeviceControl handles the three SOUND ioctls. It returns
 * TRUE when Code belongs to the audio ABI, FALSE for any other code.
 */
NTSTATUS Win0DoomAudioInitialize(VOID);
VOID Win0DoomAudioShutdown(VOID);

/*
 * Discards queued PCM immediately and asks the pump thread to restart the
 * DSP from a clean auto-init DMA loop. The ring is cleared synchronously;
 * the device restart happens on the pump thread within a few milliseconds.
 * Safe at IRQL <= DISPATCH_LEVEL, so IRP_MJ_CREATE/IRP_MJ_CLOSE may call it.
 */
VOID Win0DoomAudioReset(VOID);

BOOLEAN Win0DoomAudioDeviceControl(
    unsigned long Code,
    void *Input,
    unsigned long InputLength,
    void *Output,
    unsigned long OutputLength,
    unsigned long *Information
);

/*
 * Same dispatch with the exact NTSTATUS. Returns STATUS_SUCCESS,
 * STATUS_INVALID_PARAMETER for a malformed SOUND_WRITE, STATUS_BUFFER_TOO_SMALL
 * for a short SOUND_STATUS buffer, and STATUS_INVALID_DEVICE_REQUEST for any
 * code outside the audio ABI. The display driver should prefer this entry
 * point so the client sees the real status; complete the IRP with it and
 * *Information.
 */
NTSTATUS Win0DoomAudioDeviceControlEx(
    unsigned long Code,
    void *Input,
    unsigned long InputLength,
    void *Output,
    unsigned long OutputLength,
    unsigned long *Information
);

#endif

#endif
