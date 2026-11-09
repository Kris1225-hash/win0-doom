/*
 * win0doom-audio.c - kernel PCM sink for QEMU's emulated Sound Blaster 16.
 *
 * The target is QEMU on q35 with
 *     -device sb16,audiodev=...
 * which the parent verified realizes with iobase 0x220, 8-bit DMA channel 1,
 * 16-bit DMA channel 5 and IRQ 5.  This driver never installs an interrupt
 * handler: the SB16's 16-bit auto-init DMA loops over one physically
 * contiguous buffer by itself, and a PASSIVE_LEVEL pump thread refills the
 * region ahead of the playback cursor based on KeQueryPerformanceCounter
 * time and the known 44100 bytes/second.  QEMU raises the ISA IRQ at every
 * block boundary even in auto-init (sb16.c:1257-1259); with no ISR
 * installed the pump thread simply reads the 16-bit acknowledge port 0x22F
 * (sb16.c:1044-1049) so the line is deasserted.
 *
 * The port numbers, command bytes and parameter orders below were checked
 * against QEMU hw/audio/sb16.c and hw/dma/i8257.c (master, matching the
 * 11.1.x code on the target host):
 *
 *  - DSP write data/command is 0x22C, read status is 0x22E (bit 7 = data
 *    available), read data is 0x22A, reset is 0x226.  sb16.c dsp_write()
 *    handles iport 0x0c as command/data and iport 0x06 as reset; writing 1
 *    then 0 to 0x226 calls reset(), which pushes 0xAA into the output FIFO.
 *    dsp_read() iport 0x0e returns 0x80 when the FIFO is non-empty, and
 *    iport 0x0a pops one byte.
 *
 *  - Command 0x41 (set output sample rate) takes two parameter bytes.  The
 *    guest writes the HIGH byte first: complete() reads them with
 *    dsp_get_hilo(), which pops the LIFO in2_data buffer as
 *    lo = last written, hi = first written, and returns hi << 8 | lo.  So
 *    11025 = 0x2B11 is sent as 0x2B then 0x11.  (DOSBox-X sblaster.cpp
 *    documents the same order as (data[0] << 8) | data[1].)
 *
 *  - Command 0xB6 is 16-bit auto-init output.  complete() takes three
 *    parameter bytes, d0/d1/d2, which the guest writes as format, count low,
 *    count high.  dma_cmd() reads d0 bit 5 as stereo and bit 4 as signed, so
 *    0x30 is signed stereo.  It sets
 *         block_size = (dma_len + 1) << 1        (16-bit)
 *    and deliberately does NOT shift by stereo again for auto-init, so the
 *    count argument is (bytes / 2) - 1 and one loop is exactly our buffer.
 *    sb16.c control() then holds DREQ for channel 5.
 *
 *  - The second 8237 (base 0xC0, dshift = 1) gives system channel 5:
 *        0xC4 address   (low then high byte of physical >> 1, a word address)
 *        0xC6 count     (low then high byte of bytes / 2 - 1)
 *        0x8B page      ((physical >> 16) & 0xFF)
 *        0xD4 single-mask (0x04 | 1 sets mask, 0x01 clears it)
 *        0xD6 mode      0x59 = single mode (bits 7:6 = 01), auto-init
 *                       (bit 4), memory-to-device read (bits 3:2 = 10),
 *                       increment (bit 5 = 0), channel 1 (= system 5)
 *        0xD8 clear byte-pointer flip-flop
 *    i8257_write_chan() computes iport = (port >> dshift) & 0xF, so 0xC4 is
 *    iport 2 (channel 5 address) and 0xC6 is iport 3 (channel 5 count).
 *    i8257_write_cont() cases 0x02/0x03 are the single-mask and mode
 *    registers; with the slave continuation region at 0xD0 and dshift = 1,
 *    (0xD4 >> 1) & 0xF = 2 and (0xD6 >> 1) & 0xF = 3.  QEMU reloads
 *    now[ADDR] from base on the high-byte write and reconstructs
 *    address = (page << 16) | (base << 1) in i8257_dma_read_memory().
 *
 * The ring is 64 KiB and protected by one spinlock.  SOUND_WRITE is
 * non-blocking: when the ring cannot take the whole write, the oldest bytes
 * are discarded and counted.  A malformed write is rejected with
 * STATUS_INVALID_PARAMETER.  Everything about the device is best effort:
 * failure to bring the DSP up leaves DeviceReady = 0, keeps the ring
 * accepting data, and never fails the driver load or bugchecks.
 *
 * The pump thread appends a plain-text diagnostic log to
 * \SystemRoot\win0doom-audio.log, lazily opened with a kernel handle and
 * bounded to WIN0DOOM_AUDIO_LOG_MAX bytes per boot.  Only the pump thread
 * ever touches the log (at PASSIVE_LEVEL).
 */

/*
 * clang-cl has no lowering for the MSVC __inbyte/__outbyte intrinsics, and
 * its own <intrin.h> is shadowed by the WDK's kernel CRT header, so the
 * inline READ_PORT_UCHAR/WRITE_PORT_UCHAR from ioaccess.h would emit
 * unresolved calls to those names.  Define the two byte port primitives
 * with inline asm before ntddk.h so the WDK wrappers compile to real
 * inb/outb instructions.  The build is x64-only (/D_AMD64_).
 */
static __inline unsigned char __inbyte(unsigned short port)
{
    unsigned char value;
    __asm__ __volatile__("inb %w1, %b0" : "=a"(value) : "Nd"(port));
    return value;
}

static __inline void __outbyte(unsigned short port, unsigned char value)
{
    __asm__ __volatile__("outb %b0, %w1" : : "a"(value), "Nd"(port));
}

#include <ntddk.h>
#include "win0doom-audio.h"

/*
 * Declared in ntifs.h, which ntddk.h does not pull in, but exported by
 * ntoskrnl.exe.  Waiting on the thread handle keeps the thread object
 * referenced until termination completes, unlike an event set just before
 * PsTerminateSystemThread.
 */
NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(
    HANDLE Handle,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout
);

#define WIN0DOOM_AUDIO_RING_BYTES 65536UL
#define WIN0DOOM_AUDIO_DMA_BYTES 32768UL
#define WIN0DOOM_AUDIO_DMA_LIMIT 0x1000000ULL /* keep below the 16 MiB page limit */
#define WIN0DOOM_AUDIO_DMA_BOUNDARY 0x10000ULL
#define WIN0DOOM_AUDIO_DMA_LEAD (WIN0DOOM_AUDIO_DMA_BYTES / 2UL)
/* Tolerated backward cursor jitter: twice the worst torn count read. */
#define WIN0DOOM_AUDIO_RESYNC_SLACK 1024UL
#define WIN0DOOM_AUDIO_CHUNK 1024UL
#define WIN0DOOM_AUDIO_LOG_MAX 32768UL
#define WIN0DOOM_AUDIO_LOG_LINE_BYTES 256UL

#define WIN0DOOM_AUDIO_PUMP_DELAY_100NS (-20000LL) /* 2 ms */
#define WIN0DOOM_AUDIO_PORT_TIMEOUT 100000UL        /* 1 us stalls */

#define SB16_BASE 0x220
#define SB16_MIXER_INDEX (SB16_BASE + 0x04)
#define SB16_MIXER_DATA (SB16_BASE + 0x05)
#define SB16_DSP_RESET (SB16_BASE + 0x06)
#define SB16_DSP_READ_DATA (SB16_BASE + 0x0a)
#define SB16_DSP_WRITE_DATA (SB16_BASE + 0x0c)
#define SB16_DSP_READ_STATUS (SB16_BASE + 0x0e)
#define SB16_DSP_IRQ_ACK16 (SB16_BASE + 0x0f)

#define SB16_DSP_CMD_SET_RATE_OUT 0x41
#define SB16_DSP_CMD_PLAY_16BIT_AUTO 0xb6
#define SB16_DSP_CMD_SPEAKER_ON 0xd1
#define SB16_DSP_CMD_SPEAKER_OFF 0xd3
#define SB16_DSP_CMD_HALT_16BIT_DMA 0xd5
#define SB16_DSP_CMD_EXIT_16BIT_AUTO 0xd9
#define SB16_DSP_CMD_VERSION 0xe1
#define SB16_DSP_FORMAT_S16_STEREO 0x30
#define SB16_MIXER_IRQ_SELECT 0x80 /* sb16.c: magic_of_irq(irq), 0x02 = IRQ 5 */
#define SB16_MIXER_DMA_SELECT 0x81 /* sb16.c: (1 << dma) | (1 << hdma), 0x22 = 1 and 5 */

#define WIN0DOOM_AUDIO_DMA_CHANNEL 5
#define DMA_MASK_PORT 0xd4
#define DMA_MODE_PORT 0xd6
#define DMA_CLEAR_FLIPFLOP_PORT 0xd8
#define DMA_CHANNEL5_ADDRESS_PORT 0xc4
#define DMA_CHANNEL5_COUNT_PORT 0xc6
#define DMA_CHANNEL5_PAGE_PORT 0x8b
#define DMA_MODE_16BIT_AUTO_SINGLE 0x59

typedef struct _WIN0DOOM_AUDIO_LOG_LINE {
    char Buffer[WIN0DOOM_AUDIO_LOG_LINE_BYTES];
    ULONG Length;
} WIN0DOOM_AUDIO_LOG_LINE;

typedef struct _WIN0DOOM_AUDIO_STATE {
    BOOLEAN StateReady;

    KSPIN_LOCK Lock;

    UCHAR Ring[WIN0DOOM_AUDIO_RING_BYTES];
    ULONG RingTail;
    ULONG RingCount;
    ULONG RingDroppedBytes;
    ULONG Submissions;

    PUCHAR DmaBuffer;
    ULONG DmaBytes;
    PHYSICAL_ADDRESS DmaPhysical;
    ULONG DmaAnchorCount;
    ULONG DmaAnchorPosition;
    BOOLEAN DmaAnchorValid;
    ULONG FillOffset;
    ULONG DmaResyncs;

    HANDLE ThreadHandle;
    BOOLEAN ThreadCreated;
    volatile LONG StopRequested;
    volatile LONG RestartRequested;

    ULONG DeviceReady;
    NTSTATUS DeviceStatus;
    ULONG64 PlayedBytes;
    ULONG BlocksPlayed;
    ULONG SilenceBytes;

    UCHAR DspResetAck;
    ULONG DspVersion;

    /* What the last start actually programmed and read back, for the log. */
    ULONG ProgrammedRate;  /* rate sent with 0x41, 0 if that step failed */
    UCHAR ProgrammedMode;  /* 8237 mode byte written, 0 if not reached */
    ULONG StartCount;      /* channel-5 count read after 0xB6, ~0 if not reached */
    UCHAR MixerIrqSelect;  /* SB16 mixer 0x80 */
    UCHAR MixerDmaSelect;  /* SB16 mixer 0x81 */

    HANDLE LogHandle;
    ULONG LogBytes;
    LARGE_INTEGER LogFileOffset;
} WIN0DOOM_AUDIO_STATE;

static WIN0DOOM_AUDIO_STATE g_Audio;

static NTSTATUS Win0DoomAudioStartDsp(VOID);
static VOID Win0DoomAudioStopDsp(VOID);
static ULONG Win0DoomAudioFillAhead(ULONG PlayOffset, ULONG *BufferedAhead);
static VOID Win0DoomAudioPumpThread(PVOID Context);

/* ------------------------------------------------------------------ */
/* Diagnostic log (pump thread only, PASSIVE_LEVEL only)               */
/* ------------------------------------------------------------------ */

static VOID Win0DoomAudioLogAppend(PVOID Data, ULONG Length)
{
    IO_STATUS_BLOCK statusBlock;
    NTSTATUS status;
    LARGE_INTEGER offset;

    if (Length == 0) {
        return;
    }
    if (g_Audio.LogBytes >= WIN0DOOM_AUDIO_LOG_MAX) {
        if (g_Audio.LogHandle != NULL) {
            ZwClose(g_Audio.LogHandle);
            g_Audio.LogHandle = NULL;
        }
        return;
    }
    if (g_Audio.LogHandle == NULL) {
        UNICODE_STRING name;
        OBJECT_ATTRIBUTES attributes;

        RtlInitUnicodeString(&name, L"\\SystemRoot\\win0doom-audio.log");
        InitializeObjectAttributes(
            &attributes,
            &name,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
            NULL,
            NULL
        );
        status = ZwCreateFile(
            &g_Audio.LogHandle,
            FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            &attributes,
            &statusBlock,
            NULL,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN_IF,
            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
            NULL,
            0
        );
        if (!NT_SUCCESS(status)) {
            g_Audio.LogHandle = NULL;
            return;
        }
        {
            FILE_STANDARD_INFORMATION information;
            status = ZwQueryInformationFile(
                g_Audio.LogHandle,
                &statusBlock,
                &information,
                sizeof(information),
                FileStandardInformation
            );
            if (NT_SUCCESS(status)) {
                g_Audio.LogFileOffset = information.EndOfFile;
            } else {
                g_Audio.LogFileOffset.QuadPart = 0;
            }
        }
    }
    if (Length > WIN0DOOM_AUDIO_LOG_MAX - g_Audio.LogBytes) {
        Length = WIN0DOOM_AUDIO_LOG_MAX - g_Audio.LogBytes;
    }
    offset = g_Audio.LogFileOffset;
    status = ZwWriteFile(
        g_Audio.LogHandle,
        NULL,
        NULL,
        NULL,
        &statusBlock,
        Data,
        Length,
        &offset,
        NULL
    );
    if (!NT_SUCCESS(status)) {
        ZwClose(g_Audio.LogHandle);
        g_Audio.LogHandle = NULL;
        return;
    }
    g_Audio.LogBytes += Length;
    g_Audio.LogFileOffset.QuadPart += (LONGLONG)statusBlock.Information;
}

static VOID Win0DoomAudioLogText(WIN0DOOM_AUDIO_LOG_LINE *Line, const char *Text)
{
    while (*Text != '\0' && Line->Length + 1 < WIN0DOOM_AUDIO_LOG_LINE_BYTES) {
        Line->Buffer[Line->Length++] = *Text++;
    }
}

static VOID Win0DoomAudioLogDecimal(WIN0DOOM_AUDIO_LOG_LINE *Line, ULONG64 Value)
{
    char digits[20];
    ULONG count = 0;

    if (Value == 0) {
        Win0DoomAudioLogText(Line, "0");
        return;
    }
    while (Value != 0 && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (int)(Value % 10));
        Value /= 10;
    }
    while (count > 0 && Line->Length + 1 < WIN0DOOM_AUDIO_LOG_LINE_BYTES) {
        Line->Buffer[Line->Length++] = digits[--count];
    }
}

static VOID Win0DoomAudioLogHex(WIN0DOOM_AUDIO_LOG_LINE *Line, ULONG64 Value, ULONG Digits)
{
    static const char hex[] = "0123456789abcdef";

    if (Digits > 16) {
        Digits = 16;
    }
    while (Digits > 0 && Line->Length + 1 < WIN0DOOM_AUDIO_LOG_LINE_BYTES) {
        --Digits;
        Line->Buffer[Line->Length++] = hex[(Value >> (Digits * 4)) & 0xf];
    }
}

static VOID Win0DoomAudioLogFlush(WIN0DOOM_AUDIO_LOG_LINE *Line)
{
    if (Line->Length + 2 > WIN0DOOM_AUDIO_LOG_LINE_BYTES) {
        Line->Length = WIN0DOOM_AUDIO_LOG_LINE_BYTES - 2;
    }
    Line->Buffer[Line->Length++] = '\r';
    Line->Buffer[Line->Length++] = '\n';
    Win0DoomAudioLogAppend(Line->Buffer, Line->Length);
    Line->Length = 0;
}

static VOID Win0DoomAudioLogInit(VOID)
{
    WIN0DOOM_AUDIO_LOG_LINE line;
    ULONG64 physical = (ULONG64)g_Audio.DmaPhysical.QuadPart;

    line.Length = 0;
    Win0DoomAudioLogText(&line, "audio init start: bytes=");
    Win0DoomAudioLogDecimal(&line, g_Audio.DmaBytes);
    Win0DoomAudioLogText(&line, " phys=0x");
    Win0DoomAudioLogHex(&line, physical, 8);
    Win0DoomAudioLogText(&line, " page=0x");
    Win0DoomAudioLogHex(&line, (physical >> 16) & 0xff, 2);
    Win0DoomAudioLogText(&line, " word=0x");
    Win0DoomAudioLogHex(&line, (physical >> 1) & 0xffff, 4);
    Win0DoomAudioLogFlush(&line);
}

/*
 * Everything after version= is what this start actually programmed (0 when
 * the step was not reached) or read back from the hardware: the channel-5
 * count right after 0xB6 and the SB16 mixer's IRQ/DMA select registers. A
 * failed start therefore no longer prints the intended rate and mode as if
 * they had been applied.
 */
static VOID Win0DoomAudioLogDspStart(NTSTATUS Status)
{
    WIN0DOOM_AUDIO_LOG_LINE line;

    line.Length = 0;
    Win0DoomAudioLogText(&line, "audio dsp: reset-ack=0x");
    Win0DoomAudioLogHex(&line, g_Audio.DspResetAck, 2);
    Win0DoomAudioLogText(&line, " version=0x");
    Win0DoomAudioLogHex(&line, g_Audio.DspVersion, 4);
    Win0DoomAudioLogText(&line, " rate=");
    Win0DoomAudioLogDecimal(&line, g_Audio.ProgrammedRate);
    Win0DoomAudioLogText(&line, " mode=0x");
    Win0DoomAudioLogHex(&line, g_Audio.ProgrammedMode, 2);
    if (g_Audio.StartCount == (ULONG)-1) {
        Win0DoomAudioLogText(&line, " count0=none");
    } else {
        Win0DoomAudioLogText(&line, " count0=0x");
        Win0DoomAudioLogHex(&line, g_Audio.StartCount, 4);
    }
    Win0DoomAudioLogText(&line, " irqsel=0x");
    Win0DoomAudioLogHex(&line, g_Audio.MixerIrqSelect, 2);
    Win0DoomAudioLogText(&line, " dmasel=0x");
    Win0DoomAudioLogHex(&line, g_Audio.MixerDmaSelect, 2);
    Win0DoomAudioLogText(&line, " status=0x");
    Win0DoomAudioLogHex(&line, (ULONG)Status, 8);
    Win0DoomAudioLogFlush(&line);
}

static VOID Win0DoomAudioLogRun(VOID)
{
    WIN0DOOM_AUDIO_LOG_LINE line;
    KIRQL irql;
    ULONG queued;
    ULONG dropped;
    ULONG blocks;
    ULONG silence;
    ULONG ready;
    ULONG64 played;

    KeAcquireSpinLock(&g_Audio.Lock, &irql);
    queued = g_Audio.RingCount;
    dropped = g_Audio.RingDroppedBytes;
    blocks = g_Audio.BlocksPlayed;
    silence = g_Audio.SilenceBytes;
    ready = g_Audio.DeviceReady;
    played = g_Audio.PlayedBytes;
    KeReleaseSpinLock(&g_Audio.Lock, irql);

    line.Length = 0;
    Win0DoomAudioLogText(&line, "audio run: queued=");
    Win0DoomAudioLogDecimal(&line, queued);
    Win0DoomAudioLogText(&line, " dropped=");
    Win0DoomAudioLogDecimal(&line, dropped);
    Win0DoomAudioLogText(&line, " played=");
    Win0DoomAudioLogDecimal(&line, played);
    Win0DoomAudioLogText(&line, " blocks=");
    Win0DoomAudioLogDecimal(&line, blocks);
    Win0DoomAudioLogText(&line, " silence=");
    Win0DoomAudioLogDecimal(&line, silence);
    /* Pump-thread state; this runs on the pump thread. */
    Win0DoomAudioLogText(&line, " resyncs=");
    Win0DoomAudioLogDecimal(&line, g_Audio.DmaResyncs);
    Win0DoomAudioLogText(&line, " ready=");
    Win0DoomAudioLogDecimal(&line, ready);
    Win0DoomAudioLogFlush(&line);
}

/* ------------------------------------------------------------------ */
/* SB16 DSP port helpers                                              */
/* ------------------------------------------------------------------ */

static BOOLEAN Win0DoomAudioDspWrite(UCHAR Value)
{
    ULONG spin;

    for (spin = 0; spin < WIN0DOOM_AUDIO_PORT_TIMEOUT; ++spin) {
        if ((READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_WRITE_DATA) & 0x80) == 0) {
            WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_WRITE_DATA, Value);
            return TRUE;
        }
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static BOOLEAN Win0DoomAudioDspRead(PUCHAR Value)
{
    ULONG spin;

    for (spin = 0; spin < WIN0DOOM_AUDIO_PORT_TIMEOUT; ++spin) {
        if ((READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_READ_STATUS) & 0x80) != 0) {
            *Value = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_READ_DATA);
            return TRUE;
        }
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static NTSTATUS Win0DoomAudioResetDsp(VOID)
{
    UCHAR ack = 0xff;

    g_Audio.DspResetAck = 0xff;
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_RESET, 1);
    KeStallExecutionProcessor(10);
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_RESET, 0);
    if (!Win0DoomAudioDspRead(&ack)) {
        return STATUS_IO_TIMEOUT;
    }
    g_Audio.DspResetAck = ack;
    if (ack != 0xaa) {
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    return STATUS_SUCCESS;
}

static ULONG Win0DoomAudioDspVersion(VOID)
{
    UCHAR major = 0;
    UCHAR minor = 0;

    if (!Win0DoomAudioDspWrite(SB16_DSP_CMD_VERSION)) {
        return 0;
    }
    if (!Win0DoomAudioDspRead(&major)) {
        return 0;
    }
    if (!Win0DoomAudioDspRead(&minor)) {
        return 0;
    }
    return ((ULONG)major << 8) | minor;
}

/* Mixer index at base+4, data at base+5 (sb16.c sb16_ioport_list). */
static UCHAR Win0DoomAudioMixerRead(UCHAR Index)
{
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_MIXER_INDEX, Index);
    return READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_MIXER_DATA);
}

static VOID Win0DoomAudioProgramDma(VOID)
{
    ULONG64 physical = (ULONG64)g_Audio.DmaPhysical.QuadPart;
    ULONG wordAddress = (ULONG)((physical >> 1) & 0xffffULL);
    ULONG count = g_Audio.DmaBytes / 2UL - 1UL;

    /* Mask channel 5 (bit 2 = set mask, bits 1:0 = channel 1 = system 5). */
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_MASK_PORT, (UCHAR)(0x04 | (WIN0DOOM_AUDIO_DMA_CHANNEL & 3)));
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CLEAR_FLIPFLOP_PORT, 0);
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_MODE_PORT, DMA_MODE_16BIT_AUTO_SINGLE);
    g_Audio.ProgrammedMode = DMA_MODE_16BIT_AUTO_SINGLE;
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CHANNEL5_ADDRESS_PORT, (UCHAR)(wordAddress & 0xff));
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CHANNEL5_ADDRESS_PORT, (UCHAR)((wordAddress >> 8) & 0xff));
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CHANNEL5_COUNT_PORT, (UCHAR)(count & 0xff));
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CHANNEL5_COUNT_PORT, (UCHAR)((count >> 8) & 0xff));
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CHANNEL5_PAGE_PORT, (UCHAR)((physical >> 16) & 0xff));
    /* Unmask channel 5. */
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_MASK_PORT, (UCHAR)(WIN0DOOM_AUDIO_DMA_CHANNEL & 3));
}

/*
 * Current channel-5 count register as two bytes.  i8257_read_chan returns
 * the byte value (base[COUNT] << dshift) - now[COUNT] shifted by
 * (dshift + ff*8) before masking, so for the 16-bit slave (dshift=1) the
 * assembled 16-bit value is val >> 1: a WORD count, 16383 - fetched_bytes/2.
 * Clear the byte-pointer flip-flop first so the two reads are low then high.
 */
static ULONG Win0DoomAudioReadDmaCountOnce(VOID)
{
    UCHAR low;
    UCHAR high;

    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CLEAR_FLIPFLOP_PORT, 0);
    low = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CHANNEL5_COUNT_PORT);
    high = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)DMA_CHANNEL5_COUNT_PORT);
    return ((ULONG)high << 8) | (ULONG)low;
}

/*
 * The low and high bytes are two separate port reads, and qemu's DMA engine
 * (an i8257 bottom half on the main loop) can advance between them: a borrow
 * out of the low byte then assembles a count up to 255 words too low, a
 * cursor ~0.5 KiB ahead of the truth. The engine moves in bursts, so repeat
 * the pair until two consecutive values agree (bounded; the last value is
 * used if the controller never settles).
 */
static ULONG Win0DoomAudioReadDmaCount(VOID)
{
    ULONG previous = Win0DoomAudioReadDmaCountOnce();
    ULONG attempt;

    for (attempt = 0; attempt < 4; ++attempt) {
        ULONG current = Win0DoomAudioReadDmaCountOnce();

        if (current == previous) {
            return current;
        }
        previous = current;
    }
    return previous;
}

static NTSTATUS Win0DoomAudioStartDsp(VOID)
{
    NTSTATUS status;
    ULONG count;

    g_Audio.DspVersion = 0;
    g_Audio.ProgrammedRate = 0;
    g_Audio.ProgrammedMode = 0;
    g_Audio.StartCount = (ULONG)-1;
    /* Read before the reset so an absent card is visible as 0xff here. */
    g_Audio.MixerIrqSelect = Win0DoomAudioMixerRead(SB16_MIXER_IRQ_SELECT);
    g_Audio.MixerDmaSelect = Win0DoomAudioMixerRead(SB16_MIXER_DMA_SELECT);
    status = Win0DoomAudioResetDsp();
    if (!NT_SUCCESS(status)) {
        return status;
    }
    g_Audio.DspVersion = Win0DoomAudioDspVersion();

    if (!Win0DoomAudioDspWrite(SB16_DSP_CMD_SPEAKER_ON)) {
        return STATUS_IO_TIMEOUT;
    }
    /* 11025 = 0x2B11, high byte first: sb16.c dsp_get_hilo() pops lo last. */
    if (!Win0DoomAudioDspWrite(SB16_DSP_CMD_SET_RATE_OUT) ||
        !Win0DoomAudioDspWrite(0x2b) ||
        !Win0DoomAudioDspWrite(0x11)) {
        return STATUS_IO_TIMEOUT;
    }
    g_Audio.ProgrammedRate = WIN0DOOM_PCM_SAMPLE_RATE;

    Win0DoomAudioProgramDma();
    count = g_Audio.DmaBytes / 2UL - 1UL;
    if (!Win0DoomAudioDspWrite(SB16_DSP_CMD_PLAY_16BIT_AUTO) ||
        !Win0DoomAudioDspWrite(SB16_DSP_FORMAT_S16_STEREO) ||
        !Win0DoomAudioDspWrite((UCHAR)(count & 0xff)) ||
        !Win0DoomAudioDspWrite((UCHAR)((count >> 8) & 0xff))) {
        return STATUS_IO_TIMEOUT;
    }

    /*
     * Anchor every later refill to the real 8237 channel-5 position.  The
     * count register is in 16-bit words: count = DmaBytes/2 - 1 - bytes/2,
     * so bytes = DmaBytes - 2 - count*2.  The DSP may have consumed part of
     * the block inside the 0xB6 command, so record the absolute buffer
     * position this anchor corresponds to and add (anchorCount - countNow)
     * * 2 bytes later.  A value above DmaBytes/2 - 1 is the 0xFFFF wrap read
     * or a floating bus and is not usable as an anchor.
     */
    g_Audio.DmaAnchorCount = Win0DoomAudioReadDmaCount();
    g_Audio.StartCount = g_Audio.DmaAnchorCount;
    if (g_Audio.DmaAnchorCount <= g_Audio.DmaBytes / 2UL - 1UL) {
        g_Audio.DmaAnchorPosition =
            (g_Audio.DmaBytes - 2UL - g_Audio.DmaAnchorCount * 2UL) &
            (g_Audio.DmaBytes - 1UL);
        g_Audio.DmaAnchorValid = TRUE;
    } else {
        g_Audio.DmaAnchorPosition = 0;
        g_Audio.DmaAnchorValid = FALSE;
    }
    /* The buffer was prefilled from offset 0 up to one lead. */
    g_Audio.FillOffset = WIN0DOOM_AUDIO_DMA_LEAD;
    return STATUS_SUCCESS;
}

static VOID Win0DoomAudioStopDsp(VOID)
{
    Win0DoomAudioDspWrite(SB16_DSP_CMD_EXIT_16BIT_AUTO);
    Win0DoomAudioDspWrite(SB16_DSP_CMD_HALT_16BIT_DMA);
    Win0DoomAudioDspWrite(SB16_DSP_CMD_SPEAKER_OFF);
    WRITE_PORT_UCHAR(
        (PUCHAR)(ULONG_PTR)DMA_MASK_PORT,
        (UCHAR)(0x04 | (WIN0DOOM_AUDIO_DMA_CHANNEL & 3))
    );
    (VOID)READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_IRQ_ACK16);
}

/* ------------------------------------------------------------------ */
/* Ring buffer                                                        */
/* ------------------------------------------------------------------ */

static VOID Win0DoomAudioRingWrite(const UCHAR *Data, ULONG Length)
{
    KIRQL irql;
    ULONG tail;
    ULONG first;

    KeAcquireSpinLock(&g_Audio.Lock, &irql);
    if (Length > WIN0DOOM_AUDIO_RING_BYTES - g_Audio.RingCount) {
        ULONG drop = Length - (WIN0DOOM_AUDIO_RING_BYTES - g_Audio.RingCount);
        g_Audio.RingDroppedBytes += drop;
        g_Audio.RingTail = (g_Audio.RingTail + drop) & (WIN0DOOM_AUDIO_RING_BYTES - 1);
        g_Audio.RingCount -= drop;
    }
    tail = (g_Audio.RingTail + g_Audio.RingCount) & (WIN0DOOM_AUDIO_RING_BYTES - 1);
    first = WIN0DOOM_AUDIO_RING_BYTES - tail;
    if (first > Length) {
        first = Length;
    }
    RtlCopyMemory(g_Audio.Ring + tail, Data, first);
    if (Length > first) {
        RtlCopyMemory(g_Audio.Ring, Data + first, Length - first);
    }
    g_Audio.RingCount += Length;
    g_Audio.Submissions++;
    KeReleaseSpinLock(&g_Audio.Lock, irql);
}

static ULONG Win0DoomAudioRingRead(PUCHAR Destination, ULONG Maximum)
{
    KIRQL irql;
    ULONG count;
    ULONG first;

    KeAcquireSpinLock(&g_Audio.Lock, &irql);
    count = g_Audio.RingCount;
    if (count > Maximum) {
        count = Maximum;
    }
    first = WIN0DOOM_AUDIO_RING_BYTES - g_Audio.RingTail;
    if (first > count) {
        first = count;
    }
    RtlCopyMemory(Destination, g_Audio.Ring + g_Audio.RingTail, first);
    if (count > first) {
        RtlCopyMemory(Destination + first, g_Audio.Ring, count - first);
    }
    g_Audio.RingTail = (g_Audio.RingTail + count) & (WIN0DOOM_AUDIO_RING_BYTES - 1);
    g_Audio.RingCount -= count;
    KeReleaseSpinLock(&g_Audio.Lock, irql);
    return count;
}

/* ------------------------------------------------------------------ */
/* DMA buffer fill                                                    */
/* ------------------------------------------------------------------ */

static VOID Win0DoomAudioCopyToDma(ULONG Offset, const UCHAR *Data, ULONG Length)
{
    ULONG first;

    if (Length == 0) {
        return;
    }
    first = g_Audio.DmaBytes - Offset;
    if (first > Length) {
        first = Length;
    }
    RtlCopyMemory(g_Audio.DmaBuffer + Offset, Data, first);
    if (Length > first) {
        RtlCopyMemory(g_Audio.DmaBuffer, Data + first, Length - first);
    }
}

static VOID Win0DoomAudioFillSilence(ULONG Offset, ULONG Length)
{
    ULONG first;

    if (Length == 0) {
        return;
    }
    first = g_Audio.DmaBytes - Offset;
    if (first > Length) {
        first = Length;
    }
    RtlZeroMemory(g_Audio.DmaBuffer + Offset, first);
    if (Length > first) {
        RtlZeroMemory(g_Audio.DmaBuffer, Length - first);
    }
}

/*
 * Fill the DMA buffer from PlayOffset + *BufferedAhead up to one lead
 * (half a buffer) ahead, taking PCM from the ring and counting the bytes
 * that had to be replaced with silence.  The caller owns the position
 * bookkeeping; this runs only on the pump thread.
 */
static ULONG Win0DoomAudioFillAhead(ULONG PlayOffset, ULONG *BufferedAhead)
{
    ULONG offset = (PlayOffset + *BufferedAhead) & (g_Audio.DmaBytes - 1);
    ULONG silence = 0;

    while (*BufferedAhead < WIN0DOOM_AUDIO_DMA_LEAD) {
        UCHAR chunk[WIN0DOOM_AUDIO_CHUNK];
        ULONG want = WIN0DOOM_AUDIO_DMA_LEAD - *BufferedAhead;
        ULONG got;

        if (want > WIN0DOOM_AUDIO_CHUNK) {
            want = WIN0DOOM_AUDIO_CHUNK;
        }
        got = Win0DoomAudioRingRead(chunk, want);
        Win0DoomAudioCopyToDma(offset, chunk, got);
        if (got < want) {
            Win0DoomAudioFillSilence((offset + got) & (g_Audio.DmaBytes - 1), want - got);
            silence += want - got;
        }
        offset = (offset + want) & (g_Audio.DmaBytes - 1);
        *BufferedAhead += want;
    }
    return silence;
}

static ULONG64 Win0DoomAudioTicksToBytes(ULONG64 Ticks, ULONG64 Frequency)
{
    ULONG64 seconds;
    ULONG64 remainder;

    if (Frequency == 0) {
        return 0;
    }
    seconds = Ticks / Frequency;
    remainder = Ticks % Frequency;
    return seconds * (ULONG64)WIN0DOOM_PCM_BYTES_PER_SECOND +
        (remainder * (ULONG64)WIN0DOOM_PCM_BYTES_PER_SECOND) / Frequency;
}

/* ------------------------------------------------------------------ */
/* Pump thread                                                        */
/* ------------------------------------------------------------------ */

static VOID Win0DoomAudioPumpThread(PVOID Context)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER startTime;
    LARGE_INTEGER now;
    LARGE_INTEGER lastLog;
    LARGE_INTEGER delay;
    ULONG64 playedRun = 0;
    ULONG64 playedBase = 0;
    ULONG primeSilence = 0;
    ULONG primed = 0;
    BOOLEAN running;
    NTSTATUS status;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Context);

    Win0DoomAudioLogInit();

    RtlZeroMemory(g_Audio.DmaBuffer, g_Audio.DmaBytes);
    primed = 0;
    primeSilence = Win0DoomAudioFillAhead(0, &primed);
    status = Win0DoomAudioStartDsp();
    running = NT_SUCCESS(status);
    startTime = KeQueryPerformanceCounter(&frequency);
    KeAcquireSpinLock(&g_Audio.Lock, &irql);
    g_Audio.DeviceStatus = status;
    g_Audio.DeviceReady = running ? 1UL : 0UL;
    g_Audio.SilenceBytes += primeSilence;
    KeReleaseSpinLock(&g_Audio.Lock, irql);
    Win0DoomAudioLogDspStart(status);

    lastLog = startTime;
    delay.QuadPart = WIN0DOOM_AUDIO_PUMP_DELAY_100NS;

    for (;;) {
        if (InterlockedCompareExchange(&g_Audio.StopRequested, 0, 0) != 0) {
            break;
        }

        if (InterlockedExchange(&g_Audio.RestartRequested, 0) != 0) {
            if (running) {
                playedBase += playedRun;
            }
            playedRun = 0;
            primed = 0;
            RtlZeroMemory(g_Audio.DmaBuffer, g_Audio.DmaBytes);
            primeSilence = Win0DoomAudioFillAhead(0, &primed);
            status = Win0DoomAudioStartDsp();
            running = NT_SUCCESS(status);
            startTime = KeQueryPerformanceCounter(&frequency);
            KeAcquireSpinLock(&g_Audio.Lock, &irql);
            g_Audio.DeviceStatus = status;
            g_Audio.DeviceReady = running ? 1UL : 0UL;
            g_Audio.SilenceBytes += primeSilence;
            KeReleaseSpinLock(&g_Audio.Lock, irql);
            Win0DoomAudioLogDspStart(status);
            lastLog = startTime;
        }

        if (running) {
            ULONG64 elapsed;
            ULONG64 total;
            ULONG countNow;
            ULONG position;
            ULONG ahead;
            ULONG silence = 0;
            ULONG chunks = 0;

            now = KeQueryPerformanceCounter(NULL);
            /*
             * The 16-bit auto-init IRQ is raised every block (sb16.c:
             * 1257-1259) and only a read of 0x22F lowers it for channel 5
             * (sb16.c:1044-1049).  Polling the acknowledge port keeps the
             * ISA line low without an ISR; if IRQ 5 is masked this is a
             * no-op inside the emulated PIC.
             */
            (VOID)READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)SB16_DSP_IRQ_ACK16);
            elapsed = (ULONG64)(now.QuadPart - startTime.QuadPart);
            playedRun = Win0DoomAudioTicksToBytes(elapsed, (ULONG64)frequency.QuadPart);

            /*
             * Anchor the refill to the real 8237 position instead of the
             * wall-clock estimate: the 16-bit channel count register holds
             * WORDS remaining, 16383 - fetched_bytes/2, so two bytes are
             * two words.  A count above DmaBytes/2 - 1 is the 0xFFFF wrap
             * read and is rejected; the time estimate is used for that tick
             * only.  Rounding to a frame keeps every write 4-byte aligned
             * so stereo phase can never rotate.
             */
            countNow = Win0DoomAudioReadDmaCount();
            if (g_Audio.DmaAnchorValid && countNow <= g_Audio.DmaBytes / 2UL - 1UL) {
                position = (g_Audio.DmaAnchorPosition +
                    (((g_Audio.DmaAnchorCount - countNow) * 2UL) &
                        (g_Audio.DmaBytes - 1UL))) &
                    (g_Audio.DmaBytes - 1UL);
            } else {
                position = (ULONG)(playedRun & (ULONG64)(g_Audio.DmaBytes - 1));
            }
            position &= ~3UL;

            /*
             * fillOffset is an absolute buffer cursor that advances only
             * with the bytes taken from the ring, so the stream stays
             * byte-locked to the playhead.  Keep it between pos and one
             * lead ahead.  Every fill ends exactly one lead ahead, so a
             * cursor read that lands slightly behind the previous one shows
             * up just above LEAD: the queued PCM is still ahead of the real
             * cursor, so write nothing this pass instead of rewinding over
             * it.  Beyond that slack the fill cursor really fell behind, or
             * less than half a lead is left: re-lock one lead ahead and
             * silence the skipped window, which otherwise still holds PCM
             * from the previous lap of the buffer (~743 ms old) and would
             * play again.
             */
            ahead = (g_Audio.FillOffset - position) & (g_Audio.DmaBytes - 1);
            if (ahead > WIN0DOOM_AUDIO_DMA_LEAD &&
                ahead <= WIN0DOOM_AUDIO_DMA_LEAD + WIN0DOOM_AUDIO_RESYNC_SLACK) {
                ahead = WIN0DOOM_AUDIO_DMA_LEAD;
            } else if (ahead > g_Audio.DmaBytes - WIN0DOOM_AUDIO_DMA_LEAD ||
                       ahead < WIN0DOOM_AUDIO_DMA_LEAD / 2) {
                ULONG skipped;

                if (ahead > g_Audio.DmaBytes - WIN0DOOM_AUDIO_DMA_LEAD) {
                    /* Fell behind: everything from the cursor on is stale. */
                    skipped = WIN0DOOM_AUDIO_DMA_LEAD;
                    Win0DoomAudioFillSilence(position, skipped);
                } else {
                    /* Short: keep what is queued, silence the rest. */
                    skipped = WIN0DOOM_AUDIO_DMA_LEAD - ahead;
                    Win0DoomAudioFillSilence(g_Audio.FillOffset, skipped);
                }
                silence += skipped;
                g_Audio.FillOffset = (position + WIN0DOOM_AUDIO_DMA_LEAD) &
                    (g_Audio.DmaBytes - 1);
                g_Audio.DmaResyncs++;
                ahead = WIN0DOOM_AUDIO_DMA_LEAD;
            }
            while (ahead < WIN0DOOM_AUDIO_DMA_LEAD && chunks < 64) {
                UCHAR chunk[WIN0DOOM_AUDIO_CHUNK];
                ULONG want = WIN0DOOM_AUDIO_DMA_LEAD - ahead;
                ULONG got;

                if (want > WIN0DOOM_AUDIO_CHUNK) {
                    want = WIN0DOOM_AUDIO_CHUNK;
                }
                got = Win0DoomAudioRingRead(chunk, want);
                Win0DoomAudioCopyToDma(g_Audio.FillOffset, chunk, got);
                if (got < want) {
                    Win0DoomAudioFillSilence(
                        (g_Audio.FillOffset + got) & (g_Audio.DmaBytes - 1),
                        want - got
                    );
                    silence += want - got;
                }
                g_Audio.FillOffset = (g_Audio.FillOffset + want) &
                    (g_Audio.DmaBytes - 1);
                ahead = (g_Audio.FillOffset - position) & (g_Audio.DmaBytes - 1);
                chunks++;
            }

            total = playedBase + playedRun;
            KeAcquireSpinLock(&g_Audio.Lock, &irql);
            g_Audio.PlayedBytes = total;
            g_Audio.BlocksPlayed = (ULONG)(total / (ULONG64)WIN0DOOM_PCM_BLOCK_BYTES);
            g_Audio.SilenceBytes += silence;
            KeReleaseSpinLock(&g_Audio.Lock, irql);

            if ((ULONG64)(now.QuadPart - lastLog.QuadPart) >= (ULONG64)frequency.QuadPart) {
                lastLog = now;
                Win0DoomAudioLogRun();
            }
        }

        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Public interface                                                   */
/* ------------------------------------------------------------------ */

NTSTATUS Win0DoomAudioInitialize(VOID)
{
    PHYSICAL_ADDRESS lowest;
    PHYSICAL_ADDRESS highest;
    PHYSICAL_ADDRESS boundary;
    NTSTATUS status;
    ULONG64 physical;

    if (g_Audio.StateReady) {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&g_Audio, sizeof(g_Audio));
    KeInitializeSpinLock(&g_Audio.Lock);
    g_Audio.DmaBytes = WIN0DOOM_AUDIO_DMA_BYTES;
    g_Audio.StateReady = TRUE;

    lowest.QuadPart = 0;
    highest.QuadPart = WIN0DOOM_AUDIO_DMA_LIMIT - 1;
    boundary.QuadPart = WIN0DOOM_AUDIO_DMA_BOUNDARY;
    g_Audio.DmaBuffer = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(
        (SIZE_T)g_Audio.DmaBytes,
        lowest,
        highest,
        boundary,
        MmCached
    );
    if (g_Audio.DmaBuffer == NULL) {
        g_Audio.DeviceStatus = STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_SUCCESS;
    }

    g_Audio.DmaPhysical = MmGetPhysicalAddress(g_Audio.DmaBuffer);
    physical = (ULONG64)g_Audio.DmaPhysical.QuadPart;
    if ((physical & (WIN0DOOM_AUDIO_DMA_BOUNDARY - 1)) + g_Audio.DmaBytes >
            WIN0DOOM_AUDIO_DMA_BOUNDARY ||
        (physical & 0x1ffffULL) + g_Audio.DmaBytes > 0x20000ULL) {
        MmFreeContiguousMemory(g_Audio.DmaBuffer);
        g_Audio.DmaBuffer = NULL;
        g_Audio.DeviceStatus = STATUS_INVALID_ADDRESS;
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(g_Audio.DmaBuffer, g_Audio.DmaBytes);

    status = PsCreateSystemThread(
        &g_Audio.ThreadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        Win0DoomAudioPumpThread,
        NULL
    );
    if (!NT_SUCCESS(status)) {
        MmFreeContiguousMemory(g_Audio.DmaBuffer);
        g_Audio.DmaBuffer = NULL;
        g_Audio.DeviceStatus = status;
        return STATUS_SUCCESS;
    }
    g_Audio.ThreadCreated = TRUE;
    return STATUS_SUCCESS;
}

VOID Win0DoomAudioShutdown(VOID)
{
    if (!g_Audio.StateReady) {
        return;
    }

    InterlockedExchange(&g_Audio.StopRequested, 1);
    if (g_Audio.ThreadCreated && g_Audio.ThreadHandle != NULL) {
        /*
         * Wait on the thread object itself rather than an event the thread
         * sets just before PsTerminateSystemThread: the handle keeps the
         * thread object referenced until termination completes, so the DMA
         * buffer and this image stay alive for the whole join.
         */
        ZwWaitForSingleObject(g_Audio.ThreadHandle, FALSE, NULL);
        ZwClose(g_Audio.ThreadHandle);
        g_Audio.ThreadHandle = NULL;
        g_Audio.ThreadCreated = FALSE;
    }

    if (g_Audio.DmaBuffer != NULL) {
        Win0DoomAudioStopDsp();
        MmFreeContiguousMemory(g_Audio.DmaBuffer);
        g_Audio.DmaBuffer = NULL;
    }
    if (g_Audio.LogHandle != NULL) {
        ZwClose(g_Audio.LogHandle);
        g_Audio.LogHandle = NULL;
    }
    g_Audio.DeviceReady = 0;
    g_Audio.StateReady = FALSE;
}

VOID Win0DoomAudioReset(VOID)
{
    KIRQL irql;

    if (!g_Audio.StateReady) {
        return;
    }
    KeAcquireSpinLock(&g_Audio.Lock, &irql);
    g_Audio.RingTail = 0;
    g_Audio.RingCount = 0;
    KeReleaseSpinLock(&g_Audio.Lock, irql);
    if (g_Audio.ThreadCreated) {
        InterlockedExchange(&g_Audio.RestartRequested, 1);
    }
}

NTSTATUS Win0DoomAudioDeviceControlEx(
    unsigned long Code,
    void *Input,
    unsigned long InputLength,
    void *Output,
    unsigned long OutputLength,
    unsigned long *Information
)
{
    if (Information != NULL) {
        *Information = 0;
    }
    if (!g_Audio.StateReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    switch (Code) {
    case WIN0DOOM_IOCTL_SOUND_WRITE:
        if (Input == NULL && InputLength != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        if (InputLength > WIN0DOOM_AUDIO_MAX_WRITE || (InputLength & 3UL) != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        if (InputLength != 0) {
            Win0DoomAudioRingWrite((const UCHAR *)Input, InputLength);
        } else {
            KIRQL irql;
            KeAcquireSpinLock(&g_Audio.Lock, &irql);
            g_Audio.Submissions++;
            KeReleaseSpinLock(&g_Audio.Lock, irql);
        }
        if (Information != NULL) {
            *Information = InputLength;
        }
        return STATUS_SUCCESS;

    case WIN0DOOM_IOCTL_SOUND_STATUS:
        if (Output == NULL || OutputLength < sizeof(WIN0DOOM_AUDIO_STATUS)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        {
            WIN0DOOM_AUDIO_STATUS *status = (WIN0DOOM_AUDIO_STATUS *)Output;
            KIRQL irql;

            KeAcquireSpinLock(&g_Audio.Lock, &irql);
            status->RingCapacityBytes = WIN0DOOM_AUDIO_RING_BYTES;
            status->RingQueuedBytes = g_Audio.RingCount;
            status->RingDroppedBytes = g_Audio.RingDroppedBytes;
            status->Submissions = g_Audio.Submissions;
            status->BlocksPlayed = g_Audio.BlocksPlayed;
            status->SilenceBytes = g_Audio.SilenceBytes;
            status->SampleRate = WIN0DOOM_PCM_SAMPLE_RATE;
            status->Channels = WIN0DOOM_PCM_CHANNELS;
            status->BitsPerSample = WIN0DOOM_PCM_BITS_PER_SAMPLE;
            status->DeviceReady = g_Audio.DeviceReady;
            status->PlaybackActive = g_Audio.DeviceReady;
            status->DeviceStatus = (unsigned long)g_Audio.DeviceStatus;
            KeReleaseSpinLock(&g_Audio.Lock, irql);
        }
        if (Information != NULL) {
            *Information = (unsigned long)sizeof(WIN0DOOM_AUDIO_STATUS);
        }
        return STATUS_SUCCESS;

    case WIN0DOOM_IOCTL_SOUND_RESET:
        Win0DoomAudioReset();
        return STATUS_SUCCESS;

    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

BOOLEAN Win0DoomAudioDeviceControl(
    unsigned long Code,
    void *Input,
    unsigned long InputLength,
    void *Output,
    unsigned long OutputLength,
    unsigned long *Information
)
{
    if (Code != WIN0DOOM_IOCTL_SOUND_WRITE &&
        Code != WIN0DOOM_IOCTL_SOUND_STATUS &&
        Code != WIN0DOOM_IOCTL_SOUND_RESET) {
        return FALSE;
    }
    (VOID)Win0DoomAudioDeviceControlEx(
        Code,
        Input,
        InputLength,
        Output,
        OutputLength,
        Information
    );
    return TRUE;
}
