#include <ntddk.h>
#include <kbdmou.h>

#define WIN0DOOM_FRAME_WIDTH 320UL
#define WIN0DOOM_FRAME_HEIGHT 200UL
#define WIN0DOOM_FRAME_BYTES (WIN0DOOM_FRAME_WIDTH * WIN0DOOM_FRAME_HEIGHT * 4UL)
#define WIN0DOOM_DISPLAY_WIDTH 1280UL
#define WIN0DOOM_DISPLAY_HEIGHT 800UL
#define WIN0DOOM_DISPLAY_BYTES (16UL * 1024UL * 1024UL)
#define IOCTL_WIN0DOOM_PRESENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WIN0DOOM_GET_KEYBOARD CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WIN0DOOM_SUBMIT_AUDIO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WIN0DOOM_AUDIO_WANT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define WIN0DOOM_KEYBOARD_RING_SIZE 128UL

/*
 * Intel High Definition Audio output bridge.
 *
 * runlevel 0 has no windows audio stack, exactly as it has no display stack, so
 * this driver talks to the qemu ich9-intel-hda / a real intel hda controller
 * the same way it talks to stdvga: find the controller on the pci bus, map its
 * mmio bar, and program it directly. doom hands us signed 16-bit stereo pcm at
 * 11025 hz (see native/win0-doom.c); we play it out of a small cyclic dma ring.
 *
 * register offsets are from the intel hda specification, revision 1.0a.
 */
#define WIN0DOOM_HDA_CLASS_CODE 0x040300UL   /* base 04 / sub 03 / prog-if 00 */

/* global controller registers (offsets from bar0). */
#define HDA_REG_GCAP 0x00
#define HDA_REG_GCTL 0x08
#define HDA_REG_STATESTS 0x0e
#define HDA_REG_INTCTL 0x20
#define HDA_REG_CORBLBASE 0x40
#define HDA_REG_CORBUBASE 0x44
#define HDA_REG_CORBWP 0x48
#define HDA_REG_CORBRP 0x4a
#define HDA_REG_CORBCTL 0x4c
#define HDA_REG_CORBSIZE 0x4e
#define HDA_REG_RIRBLBASE 0x50
#define HDA_REG_RIRBUBASE 0x54
#define HDA_REG_RIRBWP 0x58
#define HDA_REG_RINTCNT 0x5a
#define HDA_REG_RIRBCTL 0x5c
#define HDA_REG_RIRBSTS 0x5d
#define HDA_REG_RIRBSIZE 0x5e

#define HDA_GCTL_CRST 0x00000001UL
#define HDA_CORBCTL_RUN 0x02
#define HDA_CORBRP_RST 0x8000
#define HDA_RIRBCTL_RUN 0x02
#define HDA_RIRBWP_RST 0x8000

/* per-stream descriptor registers (offsets from the stream descriptor base). */
#define HDA_SD_CTL 0x00
#define HDA_SD_STS 0x03
#define HDA_SD_LPIB 0x04
#define HDA_SD_CBL 0x08
#define HDA_SD_LVI 0x0c
#define HDA_SD_FMT 0x12
#define HDA_SD_BDPL 0x18
#define HDA_SD_BDPU 0x1c

#define HDA_SDCTL_SRST 0x00000001UL
#define HDA_SDCTL_RUN 0x00000002UL

/* codec verb payloads (12-bit verb << 8 | data, or 4-bit verb << 16 | data). */
#define HDA_VERB_GET_PARAMETER 0xf0000UL
#define HDA_VERB_SET_CONNECTION_SELECT 0x70100UL
#define HDA_VERB_SET_STREAM_CHANNEL 0x70600UL
#define HDA_VERB_SET_PIN_WIDGET_CONTROL 0x70700UL
#define HDA_VERB_SET_POWER_STATE 0x70500UL
#define HDA_VERB_SET_EAPD_BTL 0x70c00UL
#define HDA_VERB_SET_CONVERTER_FORMAT 0x20000UL
#define HDA_VERB_SET_AMP_GAIN_MUTE 0x30000UL

/* get-parameter selectors. */
#define HDA_PARAM_NODE_COUNT 0x04
#define HDA_PARAM_FUNCTION_TYPE 0x05
#define HDA_PARAM_WIDGET_CAP 0x09
#define HDA_PARAM_PIN_CAP 0x0c

#define HDA_FUNCTION_TYPE_AUDIO 0x01
#define HDA_WIDGET_TYPE_OUTPUT 0x0   /* audio output converter (DAC) */
#define HDA_WIDGET_TYPE_PIN 0x4      /* pin complex */
#define HDA_PIN_CAP_OUTPUT 0x10      /* param 0x0c bit 4: output capable */
#define HDA_PIN_CTL_OUT_ENABLE 0x40
#define HDA_EAPD_ENABLE 0x02
#define HDA_AMP_OUT_UNMUTE_MAX 0xb07f /* set-output, both channels, gain 0x7f */

/* the doom mix buffer is 512 stereo 16-bit frames = 2048 bytes per fetch. */
#define WIN0DOOM_AUDIO_STREAM_TAG 1UL
#define WIN0DOOM_AUDIO_BLOCK_BYTES 2048UL
#define WIN0DOOM_AUDIO_BLOCKS 8UL
#define WIN0DOOM_AUDIO_RING_BYTES (WIN0DOOM_AUDIO_BLOCK_BYTES * WIN0DOOM_AUDIO_BLOCKS)
/* 11025 hz, 16-bit, stereo: base 44.1khz, div 4, 16-bit, 2 channels. */
#define WIN0DOOM_AUDIO_FORMAT 0x4311

typedef struct _WIN0DOOM_BDL_ENTRY {
    ULONGLONG Address;
    ULONG Length;
    ULONG Flags;
} WIN0DOOM_BDL_ENTRY;

typedef struct _WIN0DOOM_AUDIO {
    BOOLEAN Enabled;
    volatile UCHAR *Bar;        /* mapped hda controller mmio */
    ULONG StreamBase;           /* output stream descriptor offset from Bar */
    PVOID Dma;                  /* contiguous block holding corb/rirb/bdl/pcm */
    PHYSICAL_ADDRESS DmaPhysical;
    SIZE_T DmaBytes;
    volatile ULONG *Corb;
    volatile ULONGLONG *Rirb;
    volatile WIN0DOOM_BDL_ENTRY *Bdl;
    volatile UCHAR *Pcm;        /* the cyclic pcm ring */
    ULONG PcmOffset;            /* Pcm offset from the dma block base */
    ULONG WriteBlock;           /* next ring block the submit path will fill */
    BOOLEAN Primed;             /* FALSE until the ring has been filled once */
    ULONG CodecAddress;
} WIN0DOOM_AUDIO;

typedef enum _WIN0DOOM_DEVICE_KIND {
    Win0DoomControlDevice = 0x57443043,
    Win0DoomKeyboardFilter = 0x5744304b
} WIN0DOOM_DEVICE_KIND;

typedef struct _WIN0DOOM_COMMON_EXTENSION {
    WIN0DOOM_DEVICE_KIND Kind;
} WIN0DOOM_COMMON_EXTENSION, *PWIN0DOOM_COMMON_EXTENSION;

typedef struct _WIN0DOOM_CONTROL_EXTENSION {
    WIN0DOOM_DEVICE_KIND Kind;
    volatile ULONG *FrameBuffer;
    KSPIN_LOCK KeyboardLock;
    KEYBOARD_INPUT_DATA KeyboardRing[WIN0DOOM_KEYBOARD_RING_SIZE];
    ULONG KeyboardReadIndex;
    ULONG KeyboardWriteIndex;
} WIN0DOOM_CONTROL_EXTENSION, *PWIN0DOOM_CONTROL_EXTENSION;

typedef struct _WIN0DOOM_FILTER_EXTENSION {
    WIN0DOOM_DEVICE_KIND Kind;
    PDEVICE_OBJECT LowerDevice;
    CONNECT_DATA UpperConnectData;
    BOOLEAN Connected;
} WIN0DOOM_FILTER_EXTENSION, *PWIN0DOOM_FILTER_EXTENSION;

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE Win0DoomAddDevice;

static PWIN0DOOM_CONTROL_EXTENSION g_ControlExtension;
static WIN0DOOM_AUDIO g_Audio;

static NTSTATUS Win0DoomCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static BOOLEAN Win0DoomIsControlDevice(PDEVICE_OBJECT DeviceObject)
{
    PWIN0DOOM_COMMON_EXTENSION extension =
        (PWIN0DOOM_COMMON_EXTENSION)DeviceObject->DeviceExtension;
    return extension != NULL && extension->Kind == Win0DoomControlDevice;
}

static NTSTATUS Win0DoomForwardIrp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PWIN0DOOM_FILTER_EXTENSION extension =
        (PWIN0DOOM_FILTER_EXTENSION)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(extension->LowerDevice, Irp);
}

static NTSTATUS Win0DoomCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    PWIN0DOOM_CONTROL_EXTENSION extension;
    KIRQL old_irql;

    if (!Win0DoomIsControlDevice(DeviceObject)) {
        return Win0DoomForwardIrp(DeviceObject, Irp);
    }
    stack = IoGetCurrentIrpStackLocation(Irp);
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        extension = (PWIN0DOOM_CONTROL_EXTENSION)DeviceObject->DeviceExtension;
        KeAcquireSpinLock(&extension->KeyboardLock, &old_irql);
        extension->KeyboardReadIndex = extension->KeyboardWriteIndex;
        KeReleaseSpinLock(&extension->KeyboardLock, old_irql);
    }
    return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static VOID Win0DoomQueueKeyboardAtDpc(const KEYBOARD_INPUT_DATA *Records, ULONG RecordCount)
{
    PWIN0DOOM_CONTROL_EXTENSION extension = g_ControlExtension;

    if (extension == NULL) {
        return;
    }
    KeAcquireSpinLockAtDpcLevel(&extension->KeyboardLock);
    for (ULONG index = 0; index < RecordCount; ++index) {
        ULONG next = (extension->KeyboardWriteIndex + 1) % WIN0DOOM_KEYBOARD_RING_SIZE;
        if (next == extension->KeyboardReadIndex) {
            extension->KeyboardReadIndex =
                (extension->KeyboardReadIndex + 1) % WIN0DOOM_KEYBOARD_RING_SIZE;
        }
        extension->KeyboardRing[extension->KeyboardWriteIndex] = Records[index];
        extension->KeyboardWriteIndex = next;
    }
    KeReleaseSpinLockFromDpcLevel(&extension->KeyboardLock);
}

static VOID Win0DoomKeyboardServiceCallback(
    PVOID NormalContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2,
    PVOID SystemArgument3
)
{
    PDEVICE_OBJECT filter_device = (PDEVICE_OBJECT)NormalContext;
    PWIN0DOOM_FILTER_EXTENSION extension =
        (PWIN0DOOM_FILTER_EXTENSION)filter_device->DeviceExtension;
    PKEYBOARD_INPUT_DATA begin = (PKEYBOARD_INPUT_DATA)SystemArgument1;
    PKEYBOARD_INPUT_DATA end = (PKEYBOARD_INPUT_DATA)SystemArgument2;
    PSERVICE_CALLBACK_ROUTINE upper_callback =
        (PSERVICE_CALLBACK_ROUTINE)extension->UpperConnectData.ClassService;

    if (end >= begin) {
        Win0DoomQueueKeyboardAtDpc(begin, (ULONG)(end - begin));
    }
    if (upper_callback != NULL) {
        upper_callback(
            extension->UpperConnectData.ClassDeviceObject,
            SystemArgument1,
            SystemArgument2,
            SystemArgument3
        );
    }
}

static ULONG Win0DoomDrainKeyboard(
    PWIN0DOOM_CONTROL_EXTENSION Extension,
    PUCHAR Output,
    ULONG Capacity
)
{
    KIRQL old_irql;
    ULONG written = 0;

    KeAcquireSpinLock(&Extension->KeyboardLock, &old_irql);
    while (Extension->KeyboardReadIndex != Extension->KeyboardWriteIndex &&
           written + sizeof(KEYBOARD_INPUT_DATA) <= Capacity) {
        RtlCopyMemory(
            Output + written,
            &Extension->KeyboardRing[Extension->KeyboardReadIndex],
            sizeof(KEYBOARD_INPUT_DATA)
        );
        Extension->KeyboardReadIndex =
            (Extension->KeyboardReadIndex + 1) % WIN0DOOM_KEYBOARD_RING_SIZE;
        written += sizeof(KEYBOARD_INPUT_DATA);
    }
    KeReleaseSpinLock(&Extension->KeyboardLock, old_irql);
    return written;
}

/* ---- intel hd audio output bridge ---------------------------------------- */

static ULONG Win0DoomHdaRead32(ULONG offset)
{
    return *(volatile ULONG *)(g_Audio.Bar + offset);
}

static void Win0DoomHdaWrite32(ULONG offset, ULONG value)
{
    *(volatile ULONG *)(g_Audio.Bar + offset) = value;
}

static USHORT Win0DoomHdaRead16(ULONG offset)
{
    return *(volatile USHORT *)(g_Audio.Bar + offset);
}

static void Win0DoomHdaWrite16(ULONG offset, USHORT value)
{
    *(volatile USHORT *)(g_Audio.Bar + offset) = value;
}

static UCHAR Win0DoomHdaRead8(ULONG offset)
{
    return *(volatile UCHAR *)(g_Audio.Bar + offset);
}

static void Win0DoomHdaWrite8(ULONG offset, UCHAR value)
{
    *(volatile UCHAR *)(g_Audio.Bar + offset) = value;
}

static void Win0DoomStall(ULONG microseconds)
{
    KeStallExecutionProcessor(microseconds);
}

/* pci configuration space access on bus 0 via the hal (no port intrinsics). */
static ULONG Win0DoomPciConfigRead(ULONG device, ULONG function, ULONG offset)
{
    PCI_SLOT_NUMBER slot;
    ULONG value = 0xffffffffUL;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = device;
    slot.u.bits.FunctionNumber = function;
    if (HalGetBusDataByOffset(
            PCIConfiguration,
            0,
            slot.u.AsULONG,
            &value,
            offset,
            sizeof(value)
        ) != sizeof(value)) {
        return 0xffffffffUL;
    }
    return value;
}

static void Win0DoomPciConfigWrite(
    ULONG device,
    ULONG function,
    ULONG offset,
    ULONG value
)
{
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = device;
    slot.u.bits.FunctionNumber = function;
    HalSetBusDataByOffset(
        PCIConfiguration,
        0,
        slot.u.AsULONG,
        &value,
        offset,
        sizeof(value)
    );
}

/* locate an hda controller on pci bus 0; return its mmio base, or 0. */
static ULONGLONG Win0DoomHdaFindController(ULONG *device_out, ULONG *function_out)
{
    for (ULONG device = 0; device < 32; ++device) {
        ULONG vendor = Win0DoomPciConfigRead(device, 0, 0x00);
        if ((vendor & 0xffff) == 0xffff) {
            continue;
        }
        ULONG header = Win0DoomPciConfigRead(device, 0, 0x0c);
        ULONG functions = ((header >> 16) & 0x80) != 0 ? 8 : 1;
        for (ULONG function = 0; function < functions; ++function) {
            ULONG id = Win0DoomPciConfigRead(device, function, 0x00);
            if ((id & 0xffff) == 0xffff) {
                continue;
            }
            ULONG class_code = Win0DoomPciConfigRead(device, function, 0x08) >> 8;
            if (class_code != WIN0DOOM_HDA_CLASS_CODE) {
                continue;
            }
            ULONG bar0 = Win0DoomPciConfigRead(device, function, 0x10);
            if ((bar0 & 0x1) != 0) {
                continue; /* io-space bar, not the mmio we need */
            }
            ULONGLONG base = bar0 & 0xfffffff0UL;
            if (((bar0 >> 1) & 0x3) == 0x2) {
                /* 64-bit bar: fold in the upper half. */
                base |= (ULONGLONG)Win0DoomPciConfigRead(device, function, 0x14) << 32;
            }
            if (base == 0) {
                continue;
            }
            *device_out = device;
            *function_out = function;
            return base;
        }
    }
    return 0;
}

/* send one codec verb through corb/rirb and return the 32-bit response. */
static BOOLEAN Win0DoomHdaCommand(ULONG nid, ULONG verb, ULONG *response)
{
    ULONG command = (g_Audio.CodecAddress << 28) | (nid << 20) | (verb & 0xfffff);
    USHORT write_pointer = Win0DoomHdaRead16(HDA_REG_CORBWP) & 0xff;
    USHORT next = (USHORT)((write_pointer + 1) & 0xff);

    g_Audio.Corb[next] = command;
    KeMemoryBarrier();
    Win0DoomHdaWrite16(HDA_REG_CORBWP, next);

    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead16(HDA_REG_RIRBWP) & 0xff) == next) {
            if (response != NULL) {
                *response = (ULONG)(g_Audio.Rirb[next] & 0xffffffffULL);
            }
            return TRUE;
        }
        Win0DoomStall(10);
    }
    return FALSE;
}

static BOOLEAN Win0DoomHdaGetParameter(ULONG nid, ULONG parameter, ULONG *value)
{
    return Win0DoomHdaCommand(nid, HDA_VERB_GET_PARAMETER | parameter, value);
}

/* walk the audio function group and pick the first DAC and output pin. */
static BOOLEAN Win0DoomHdaFindWidgets(ULONG *dac_out, ULONG *pin_out)
{
    ULONG root_nodes;
    ULONG afg = 0;
    BOOLEAN afg_found = FALSE;

    if (!Win0DoomHdaGetParameter(0, HDA_PARAM_NODE_COUNT, &root_nodes)) {
        return FALSE;
    }
    ULONG fg_start = (root_nodes >> 16) & 0xff;
    ULONG fg_count = root_nodes & 0xff;
    for (ULONG index = 0; index < fg_count; ++index) {
        ULONG nid = fg_start + index;
        ULONG type;
        if (Win0DoomHdaGetParameter(nid, HDA_PARAM_FUNCTION_TYPE, &type) &&
            (type & 0xff) == HDA_FUNCTION_TYPE_AUDIO) {
            afg = nid;
            afg_found = TRUE;
            break;
        }
    }
    if (!afg_found) {
        return FALSE;
    }

    /* wake the whole function group before probing its widgets. */
    Win0DoomHdaCommand(afg, HDA_VERB_SET_POWER_STATE | 0x0, NULL);

    ULONG widget_nodes;
    if (!Win0DoomHdaGetParameter(afg, HDA_PARAM_NODE_COUNT, &widget_nodes)) {
        return FALSE;
    }
    ULONG widget_start = (widget_nodes >> 16) & 0xff;
    ULONG widget_count = widget_nodes & 0xff;
    BOOLEAN dac_found = FALSE;
    BOOLEAN pin_found = FALSE;
    for (ULONG index = 0; index < widget_count; ++index) {
        ULONG nid = widget_start + index;
        ULONG caps;
        if (!Win0DoomHdaGetParameter(nid, HDA_PARAM_WIDGET_CAP, &caps)) {
            continue;
        }
        ULONG type = (caps >> 20) & 0xf;
        if (!dac_found && type == HDA_WIDGET_TYPE_OUTPUT) {
            *dac_out = nid;
            dac_found = TRUE;
        } else if (!pin_found && type == HDA_WIDGET_TYPE_PIN) {
            ULONG pin_caps;
            if (Win0DoomHdaGetParameter(nid, HDA_PARAM_PIN_CAP, &pin_caps) &&
                (pin_caps & HDA_PIN_CAP_OUTPUT) != 0) {
                *pin_out = nid;
                pin_found = TRUE;
            }
        }
        if (dac_found && pin_found) {
            return TRUE;
        }
    }
    return FALSE;
}

/* reset the controller and bring up corb/rirb so codec verbs can flow. */
static BOOLEAN Win0DoomHdaResetController(void)
{
    Win0DoomHdaWrite32(HDA_REG_GCTL, 0);
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0) {
            break;
        }
        Win0DoomStall(10);
    }
    Win0DoomHdaWrite32(HDA_REG_GCTL, HDA_GCTL_CRST);
    BOOLEAN out_of_reset = FALSE;
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead32(HDA_REG_GCTL) & HDA_GCTL_CRST) != 0) {
            out_of_reset = TRUE;
            break;
        }
        Win0DoomStall(10);
    }
    if (!out_of_reset) {
        return FALSE;
    }
    /* codecs need time to announce themselves in STATESTS. */
    Win0DoomStall(1000);

    USHORT statests = Win0DoomHdaRead16(HDA_REG_STATESTS);
    if (statests == 0) {
        return FALSE;
    }
    for (ULONG bit = 0; bit < 15; ++bit) {
        if ((statests & (1u << bit)) != 0) {
            g_Audio.CodecAddress = bit;
            break;
        }
    }

    ULONGLONG corb_physical =
        g_Audio.DmaPhysical.QuadPart + 0;                 /* corb at block start */
    ULONGLONG rirb_physical =
        g_Audio.DmaPhysical.QuadPart + 0x400;             /* rirb 1 KiB in */

    /* stop the rings before repointing them. */
    Win0DoomHdaWrite8(HDA_REG_CORBCTL, 0);
    Win0DoomHdaWrite8(HDA_REG_RIRBCTL, 0);

    Win0DoomHdaWrite32(HDA_REG_CORBLBASE, (ULONG)(corb_physical & 0xffffffffULL));
    Win0DoomHdaWrite32(HDA_REG_CORBUBASE, (ULONG)(corb_physical >> 32));
    Win0DoomHdaWrite8(HDA_REG_CORBSIZE, 0x02);            /* 256 entries */
    Win0DoomHdaWrite16(HDA_REG_CORBRP, HDA_CORBRP_RST);
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead16(HDA_REG_CORBRP) & HDA_CORBRP_RST) != 0) {
            break;
        }
        Win0DoomStall(10);
    }
    Win0DoomHdaWrite16(HDA_REG_CORBRP, 0);
    Win0DoomHdaWrite16(HDA_REG_CORBWP, 0);

    Win0DoomHdaWrite32(HDA_REG_RIRBLBASE, (ULONG)(rirb_physical & 0xffffffffULL));
    Win0DoomHdaWrite32(HDA_REG_RIRBUBASE, (ULONG)(rirb_physical >> 32));
    Win0DoomHdaWrite8(HDA_REG_RIRBSIZE, 0x02);            /* 256 entries */
    Win0DoomHdaWrite16(HDA_REG_RIRBWP, HDA_RIRBWP_RST);
    Win0DoomHdaWrite16(HDA_REG_RINTCNT, 1);

    Win0DoomHdaWrite8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    Win0DoomHdaWrite8(HDA_REG_RIRBCTL, HDA_RIRBCTL_RUN);
    return TRUE;
}

/* program the output stream descriptor to loop over the cyclic pcm ring. */
static void Win0DoomHdaStartStream(void)
{
    ULONG base = g_Audio.StreamBase;
    ULONGLONG bdl_physical = g_Audio.DmaPhysical.QuadPart + 0xc00; /* bdl 3 KiB in */

    /* reset the stream engine. */
    Win0DoomHdaWrite32(base + HDA_SD_CTL, HDA_SDCTL_SRST);
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead32(base + HDA_SD_CTL) & HDA_SDCTL_SRST) != 0) {
            break;
        }
        Win0DoomStall(10);
    }
    Win0DoomHdaWrite32(base + HDA_SD_CTL, 0);
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead32(base + HDA_SD_CTL) & HDA_SDCTL_SRST) == 0) {
            break;
        }
        Win0DoomStall(10);
    }

    Win0DoomHdaWrite32(base + HDA_SD_BDPL, (ULONG)(bdl_physical & 0xffffffffULL));
    Win0DoomHdaWrite32(base + HDA_SD_BDPU, (ULONG)(bdl_physical >> 32));
    Win0DoomHdaWrite16(base + HDA_SD_LVI, (USHORT)(WIN0DOOM_AUDIO_BLOCKS - 1));
    Win0DoomHdaWrite32(base + HDA_SD_CBL, WIN0DOOM_AUDIO_RING_BYTES);
    Win0DoomHdaWrite16(base + HDA_SD_FMT, WIN0DOOM_AUDIO_FORMAT);

    /*
     * assign the stream number first, then set the run bit in a second write.
     * some real controllers latch the tag only when run transitions 0->1.
     */
    Win0DoomHdaWrite32(base + HDA_SD_CTL, WIN0DOOM_AUDIO_STREAM_TAG << 20);
    Win0DoomHdaWrite32(
        base + HDA_SD_CTL,
        (WIN0DOOM_AUDIO_STREAM_TAG << 20) | HDA_SDCTL_RUN
    );
}

/*
 * stop every dma engine that may reference the dma block - the output stream,
 * corb, and rirb - wait for each to report idle, then hold the controller in
 * reset. only after that may the block be freed.
 */
static void Win0DoomHdaQuiesce(void)
{
    if (g_Audio.Bar == NULL) {
        return;
    }
    if (g_Audio.StreamBase != 0) {
        Win0DoomHdaWrite32(g_Audio.StreamBase + HDA_SD_CTL, 0);
        for (ULONG attempt = 0; attempt < 1000; ++attempt) {
            if ((Win0DoomHdaRead32(g_Audio.StreamBase + HDA_SD_CTL) &
                 HDA_SDCTL_RUN) == 0) {
                break;
            }
            Win0DoomStall(10);
        }
    }
    Win0DoomHdaWrite8(HDA_REG_CORBCTL, 0);
    Win0DoomHdaWrite8(HDA_REG_RIRBCTL, 0);
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead8(HDA_REG_CORBCTL) & HDA_CORBCTL_RUN) == 0 &&
            (Win0DoomHdaRead8(HDA_REG_RIRBCTL) & HDA_RIRBCTL_RUN) == 0) {
            break;
        }
        Win0DoomStall(10);
    }
    Win0DoomHdaWrite32(HDA_REG_GCTL, 0);
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        if ((Win0DoomHdaRead32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0) {
            break;
        }
        Win0DoomStall(10);
    }
}

/* quiesce the controller, then release the dma block and the bar mapping. */
static void Win0DoomHdaRelease(void)
{
    Win0DoomHdaQuiesce();
    if (g_Audio.Dma != NULL) {
        MmFreeContiguousMemory(g_Audio.Dma);
        g_Audio.Dma = NULL;
    }
    if (g_Audio.Bar != NULL) {
        MmUnmapIoSpace((PVOID)g_Audio.Bar, 0x4000);
        g_Audio.Bar = NULL;
    }
}

/* one-time best-effort audio bring-up; failure just leaves doom silent. */
static void Win0DoomHdaInitialize(void)
{
    ULONG device = 0;
    ULONG function = 0;
    PHYSICAL_ADDRESS highest;
    PHYSICAL_ADDRESS controller_physical;
    ULONGLONG controller_base;
    ULONG dac = 0;
    ULONG pin = 0;

    RtlZeroMemory(&g_Audio, sizeof(g_Audio));

    controller_base = Win0DoomHdaFindController(&device, &function);
    if (controller_base == 0) {
        return;
    }

    /* enable memory space + bus mastering so the controller can dma. */
    ULONG command = Win0DoomPciConfigRead(device, function, 0x04);
    Win0DoomPciConfigWrite(device, function, 0x04, command | 0x06);

    controller_physical.QuadPart = (LONGLONG)controller_base;
    g_Audio.Bar = (volatile UCHAR *)MmMapIoSpaceEx(
        controller_physical,
        0x4000,
        PAGE_READWRITE | PAGE_NOCACHE
    );
    if (g_Audio.Bar == NULL) {
        return;
    }

    /* one contiguous, physically contiguous block below 4 GiB for all dma. */
    highest.QuadPart = 0xffffffffLL;
    g_Audio.DmaBytes = 0x10000; /* 64 KiB: corb+rirb+bdl+pcm with room to spare */
    g_Audio.Dma = MmAllocateContiguousMemory(g_Audio.DmaBytes, highest);
    if (g_Audio.Dma == NULL) {
        MmUnmapIoSpace((PVOID)g_Audio.Bar, 0x4000);
        g_Audio.Bar = NULL;
        return;
    }
    RtlZeroMemory(g_Audio.Dma, g_Audio.DmaBytes);
    g_Audio.DmaPhysical = MmGetPhysicalAddress(g_Audio.Dma);

    /* layout inside the block: corb @0, rirb @1K, bdl @3K, pcm ring @4K. */
    g_Audio.Corb = (volatile ULONG *)((PUCHAR)g_Audio.Dma + 0x000);
    g_Audio.Rirb = (volatile ULONGLONG *)((PUCHAR)g_Audio.Dma + 0x400);
    g_Audio.Bdl = (volatile WIN0DOOM_BDL_ENTRY *)((PUCHAR)g_Audio.Dma + 0xc00);
    g_Audio.PcmOffset = 0x1000;
    g_Audio.Pcm = (volatile UCHAR *)((PUCHAR)g_Audio.Dma + g_Audio.PcmOffset);

    if (!Win0DoomHdaResetController()) {
        goto fail;
    }
    if (!Win0DoomHdaFindWidgets(&dac, &pin)) {
        goto fail;
    }

    /* wake, format, and route the converter and the output pin. */
    Win0DoomHdaCommand(dac, HDA_VERB_SET_POWER_STATE | 0x0, NULL);
    Win0DoomHdaCommand(pin, HDA_VERB_SET_POWER_STATE | 0x0, NULL);
    Win0DoomHdaCommand(dac, HDA_VERB_SET_CONVERTER_FORMAT | WIN0DOOM_AUDIO_FORMAT, NULL);
    Win0DoomHdaCommand(
        dac,
        HDA_VERB_SET_STREAM_CHANNEL | (WIN0DOOM_AUDIO_STREAM_TAG << 4),
        NULL
    );
    Win0DoomHdaCommand(dac, HDA_VERB_SET_AMP_GAIN_MUTE | HDA_AMP_OUT_UNMUTE_MAX, NULL);
    Win0DoomHdaCommand(pin, HDA_VERB_SET_CONNECTION_SELECT | 0x0, NULL);
    Win0DoomHdaCommand(
        pin,
        HDA_VERB_SET_PIN_WIDGET_CONTROL | HDA_PIN_CTL_OUT_ENABLE,
        NULL
    );
    Win0DoomHdaCommand(pin, HDA_VERB_SET_EAPD_BTL | HDA_EAPD_ENABLE, NULL);
    Win0DoomHdaCommand(pin, HDA_VERB_SET_AMP_GAIN_MUTE | HDA_AMP_OUT_UNMUTE_MAX, NULL);

    /* one bdl entry per ring block, each raising interrupt-on-completion. */
    for (ULONG index = 0; index < WIN0DOOM_AUDIO_BLOCKS; ++index) {
        g_Audio.Bdl[index].Address = (ULONGLONG)g_Audio.DmaPhysical.QuadPart +
            g_Audio.PcmOffset + index * WIN0DOOM_AUDIO_BLOCK_BYTES;
        g_Audio.Bdl[index].Length = WIN0DOOM_AUDIO_BLOCK_BYTES;
        g_Audio.Bdl[index].Flags = 0x1; /* IOC */
    }

    /* find the first output stream descriptor: it follows the input streams. */
    ULONG capabilities = Win0DoomHdaRead16(HDA_REG_GCAP);
    ULONG input_streams = (capabilities >> 8) & 0xf;
    g_Audio.StreamBase = 0x80 + input_streams * 0x20;

    Win0DoomHdaStartStream();
    g_Audio.WriteBlock = 0;
    g_Audio.Enabled = TRUE;
    return;

fail:
    Win0DoomHdaRelease();
}

static void Win0DoomHdaTeardown(void)
{
    Win0DoomHdaRelease();
    g_Audio.Enabled = FALSE;
}

/*
 * how many ring blocks doom may hand us right now. the dma read position is the
 * clock: we may refill every block the hardware has already played, but never
 * the one it is playing. reporting this lets the game pull exactly that many
 * freshly mixed blocks from doom, so no consumed audio is ever dropped and the
 * whole feed self-paces to the 11025 hz drain rate.
 */
static ULONG Win0DoomAudioFreeBlocks(void)
{
    ULONG position;
    ULONG playing_block;

    if (!g_Audio.Enabled) {
        return 0;
    }
    if (!g_Audio.Primed) {
        /* nothing queued yet: prime all but one block. */
        return WIN0DOOM_AUDIO_BLOCKS - 1;
    }
    position = Win0DoomHdaRead32(g_Audio.StreamBase + HDA_SD_LPIB);
    playing_block = (position / WIN0DOOM_AUDIO_BLOCK_BYTES) % WIN0DOOM_AUDIO_BLOCKS;
    return (playing_block - g_Audio.WriteBlock + WIN0DOOM_AUDIO_BLOCKS) %
        WIN0DOOM_AUDIO_BLOCKS;
}

/*
 * copy one 2048-byte pcm block from doom into the next ring slot. the game only
 * calls this after Win0DoomAudioFreeBlocks reported room, so it never clobbers a
 * block the hardware is still reading.
 */
static void Win0DoomAudioSubmit(const UCHAR *pcm, ULONG length)
{
    volatile UCHAR *destination;

    if (!g_Audio.Enabled || pcm == NULL) {
        return;
    }
    if (length > WIN0DOOM_AUDIO_BLOCK_BYTES) {
        length = WIN0DOOM_AUDIO_BLOCK_BYTES;
    }

    destination = g_Audio.Pcm + g_Audio.WriteBlock * WIN0DOOM_AUDIO_BLOCK_BYTES;
    RtlCopyMemory((PVOID)destination, pcm, length);
    if (length < WIN0DOOM_AUDIO_BLOCK_BYTES) {
        RtlZeroMemory(
            (PVOID)(destination + length),
            WIN0DOOM_AUDIO_BLOCK_BYTES - length
        );
    }
    g_Audio.WriteBlock = (g_Audio.WriteBlock + 1) % WIN0DOOM_AUDIO_BLOCKS;
    g_Audio.Primed = TRUE;
}

/* -------------------------------------------------------------------------- */

static NTSTATUS Win0DoomDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    PWIN0DOOM_CONTROL_EXTENSION extension;
    ULONG code;
    ULONG input_length;
    ULONG output_length;
    const ULONG *source;

    if (!Win0DoomIsControlDevice(DeviceObject)) {
        return Win0DoomForwardIrp(DeviceObject, Irp);
    }
    stack = IoGetCurrentIrpStackLocation(Irp);
    extension = (PWIN0DOOM_CONTROL_EXTENSION)DeviceObject->DeviceExtension;
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    source = (const ULONG *)Irp->AssociatedIrp.SystemBuffer;

    if (code == IOCTL_WIN0DOOM_GET_KEYBOARD) {
        PUCHAR output = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
        ULONG written;
        if (output == NULL || output_length < sizeof(KEYBOARD_INPUT_DATA)) {
            return Win0DoomCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        written = Win0DoomDrainKeyboard(extension, output, output_length);
        return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, written);
    }
    if (code == IOCTL_WIN0DOOM_AUDIO_WANT) {
        PULONG want = (PULONG)Irp->AssociatedIrp.SystemBuffer;
        if (want == NULL || output_length < sizeof(ULONG)) {
            return Win0DoomCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        *want = Win0DoomAudioFreeBlocks();
        return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, sizeof(ULONG));
    }
    if (code == IOCTL_WIN0DOOM_SUBMIT_AUDIO) {
        const UCHAR *pcm = (const UCHAR *)Irp->AssociatedIrp.SystemBuffer;
        if (pcm != NULL && input_length != 0) {
            Win0DoomAudioSubmit(pcm, input_length);
        }
        /* audio is always best-effort: never fail the game's frame loop. */
        return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }
    if (code != IOCTL_WIN0DOOM_PRESENT) {
        return Win0DoomCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    if (source == NULL || input_length < WIN0DOOM_FRAME_BYTES) {
        return Win0DoomCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }

    for (ULONG source_y = 0; source_y < WIN0DOOM_FRAME_HEIGHT; ++source_y) {
        for (ULONG scale_y = 0; scale_y < 4; ++scale_y) {
            volatile ULONG *destination = extension->FrameBuffer +
                (source_y * 4 + scale_y) * WIN0DOOM_DISPLAY_WIDTH;
            const ULONG *source_row = source + source_y * WIN0DOOM_FRAME_WIDTH;
            for (ULONG source_x = 0; source_x < WIN0DOOM_FRAME_WIDTH; ++source_x) {
                ULONG rgba = source_row[source_x];
                ULONG red = rgba & 0xff;
                ULONG green = (rgba >> 8) & 0xff;
                ULONG blue = (rgba >> 16) & 0xff;
                ULONG pixel = (red << 16) | (green << 8) | blue;
                ULONG destination_x = source_x * 4;
                destination[destination_x + 0] = pixel;
                destination[destination_x + 1] = pixel;
                destination[destination_x + 2] = pixel;
                destination[destination_x + 3] = pixel;
            }
        }
    }
    return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS Win0DoomInternalDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    PWIN0DOOM_FILTER_EXTENSION extension;
    PCONNECT_DATA connect_data;

    if (Win0DoomIsControlDevice(DeviceObject)) {
        return Win0DoomCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    stack = IoGetCurrentIrpStackLocation(Irp);
    extension = (PWIN0DOOM_FILTER_EXTENSION)DeviceObject->DeviceExtension;
    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_KEYBOARD_CONNECT) {
        if (extension->Connected) {
            return Win0DoomCompleteIrp(Irp, STATUS_SHARING_VIOLATION, 0);
        }
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA)) {
            return Win0DoomCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        connect_data = (PCONNECT_DATA)stack->Parameters.DeviceIoControl.Type3InputBuffer;
        if (connect_data == NULL || connect_data->ClassService == NULL) {
            return Win0DoomCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        extension->UpperConnectData = *connect_data;
        connect_data->ClassDeviceObject = DeviceObject;
        connect_data->ClassService = Win0DoomKeyboardServiceCallback;
        extension->Connected = TRUE;
    }
    return Win0DoomForwardIrp(DeviceObject, Irp);
}

static NTSTATUS Win0DoomPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    PWIN0DOOM_FILTER_EXTENSION extension;
    PDEVICE_OBJECT lower_device;
    NTSTATUS status;

    if (Win0DoomIsControlDevice(DeviceObject)) {
        return Win0DoomCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    stack = IoGetCurrentIrpStackLocation(Irp);
    extension = (PWIN0DOOM_FILTER_EXTENSION)DeviceObject->DeviceExtension;
    if (stack->MinorFunction != IRP_MN_REMOVE_DEVICE) {
        return Win0DoomForwardIrp(DeviceObject, Irp);
    }
    lower_device = extension->LowerDevice;
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(lower_device, Irp);
    IoDetachDevice(lower_device);
    IoDeleteDevice(DeviceObject);
    return status;
}

static NTSTATUS Win0DoomPower(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PWIN0DOOM_FILTER_EXTENSION extension;

    if (Win0DoomIsControlDevice(DeviceObject)) {
        return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }
    extension = (PWIN0DOOM_FILTER_EXTENSION)DeviceObject->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(extension->LowerDevice, Irp);
}

NTSTATUS Win0DoomAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT filter_device = NULL;
    PWIN0DOOM_FILTER_EXTENSION extension;
    NTSTATUS status;

    status = IoCreateDevice(
        DriverObject,
        sizeof(WIN0DOOM_FILTER_EXTENSION),
        NULL,
        FILE_DEVICE_KEYBOARD,
        0,
        FALSE,
        &filter_device
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }
    extension = (PWIN0DOOM_FILTER_EXTENSION)filter_device->DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));
    extension->Kind = Win0DoomKeyboardFilter;
    extension->LowerDevice = IoAttachDeviceToDeviceStack(filter_device, PhysicalDeviceObject);
    if (extension->LowerDevice == NULL) {
        IoDeleteDevice(filter_device);
        return STATUS_NO_SUCH_DEVICE;
    }
    filter_device->Flags |= extension->LowerDevice->Flags &
        (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    filter_device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID Win0DoomUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symbolic_link;
    PDEVICE_OBJECT device = DriverObject->DeviceObject;

    RtlInitUnicodeString(&symbolic_link, L"\\DosDevices\\Win0DoomDisplay");
    IoDeleteSymbolicLink(&symbolic_link);
    Win0DoomHdaTeardown();
    g_ControlExtension = NULL;
    while (device != NULL) {
        PDEVICE_OBJECT next = device->NextDevice;
        PWIN0DOOM_COMMON_EXTENSION common =
            (PWIN0DOOM_COMMON_EXTENSION)device->DeviceExtension;
        if (common->Kind == Win0DoomControlDevice) {
            PWIN0DOOM_CONTROL_EXTENSION control = (PWIN0DOOM_CONTROL_EXTENSION)common;
            if (control->FrameBuffer != NULL) {
                MmUnmapIoSpace((PVOID)control->FrameBuffer, WIN0DOOM_DISPLAY_BYTES);
            }
        } else if (common->Kind == Win0DoomKeyboardFilter) {
            PWIN0DOOM_FILTER_EXTENSION filter = (PWIN0DOOM_FILTER_EXTENSION)common;
            if (filter->LowerDevice != NULL) {
                IoDetachDevice(filter->LowerDevice);
            }
        }
        IoDeleteDevice(device);
        device = next;
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING device_name;
    UNICODE_STRING symbolic_link;
    PDEVICE_OBJECT device = NULL;
    PWIN0DOOM_CONTROL_EXTENSION extension;
    PHYSICAL_ADDRESS framebuffer_address;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);
    RtlInitUnicodeString(&device_name, L"\\Device\\Win0DoomDisplay");
    status = IoCreateDevice(
        DriverObject,
        sizeof(WIN0DOOM_CONTROL_EXTENSION),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &device
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }
    extension = (PWIN0DOOM_CONTROL_EXTENSION)device->DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));
    extension->Kind = Win0DoomControlDevice;
    KeInitializeSpinLock(&extension->KeyboardLock);
    framebuffer_address.QuadPart = 0x80000000ULL;
    extension->FrameBuffer = (volatile ULONG *)MmMapIoSpaceEx(
        framebuffer_address,
        WIN0DOOM_DISPLAY_BYTES,
        PAGE_READWRITE | PAGE_NOCACHE
    );
    if (extension->FrameBuffer == NULL) {
        IoDeleteDevice(device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlInitUnicodeString(&symbolic_link, L"\\DosDevices\\Win0DoomDisplay");
    status = IoCreateSymbolicLink(&symbolic_link, &device_name);
    if (!NT_SUCCESS(status)) {
        MmUnmapIoSpace((PVOID)extension->FrameBuffer, WIN0DOOM_DISPLAY_BYTES);
        IoDeleteDevice(device);
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = Win0DoomCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = Win0DoomCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Win0DoomDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = Win0DoomInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = Win0DoomPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = Win0DoomPower;
    DriverObject->DriverExtension->AddDevice = Win0DoomAddDevice;
    DriverObject->DriverUnload = Win0DoomUnload;
    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    g_ControlExtension = extension;

    /* best-effort: doom still runs (silently) if no hda controller is found. */
    Win0DoomHdaInitialize();
    return STATUS_SUCCESS;
}
