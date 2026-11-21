#include <ntddk.h>
#include <kbdmou.h>

#include "win0doom-audio.h"

#define WIN0DOOM_FRAME_WIDTH 320UL
#define WIN0DOOM_FRAME_HEIGHT 200UL
#define WIN0DOOM_FRAME_BYTES (WIN0DOOM_FRAME_WIDTH * WIN0DOOM_FRAME_HEIGHT * 4UL)
#define WIN0DOOM_DISPLAY_WIDTH 1280UL
#define WIN0DOOM_DISPLAY_HEIGHT 800UL
#define WIN0DOOM_DISPLAY_BYTES (16UL * 1024UL * 1024UL)
#define WIN0DOOM_DISPLAY_BPP 32UL
#define IOCTL_WIN0DOOM_PRESENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WIN0DOOM_GET_KEYBOARD CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define WIN0DOOM_KEYBOARD_RING_SIZE 128UL

/*
 * PCI configuration space, mechanism #1. The QEMU stdvga device is
 * PCI vendor 0x1234 (Red Hat/QEMU) device 0x1111 (Bochs VGA).
 */
#define WIN0DOOM_PCI_CONFIG_ADDRESS_PORT 0xCF8UL
#define WIN0DOOM_PCI_CONFIG_DATA_PORT 0xCFCUL
#define WIN0DOOM_PCI_CONFIG_ENABLE 0x80000000UL
#define WIN0DOOM_PCI_BUS_COUNT 256UL
#define WIN0DOOM_PCI_DEVICE_COUNT 32UL
#define WIN0DOOM_PCI_FUNCTION_COUNT 8UL
#define WIN0DOOM_PCI_VENDOR_QEMU 0x1234UL
#define WIN0DOOM_PCI_DEVICE_STDVGA 0x1111UL
#define WIN0DOOM_PCI_REG_VENDOR_DEVICE 0x00UL
#define WIN0DOOM_PCI_REG_COMMAND 0x04UL
#define WIN0DOOM_PCI_REG_HEADER_TYPE 0x0CUL
#define WIN0DOOM_PCI_REG_BAR0 0x10UL
#define WIN0DOOM_PCI_REG_BAR1 0x14UL
#define WIN0DOOM_PCI_COMMAND_MEMORY 0x0002UL
#define WIN0DOOM_PCI_BAR_SPACE_IO 0x00000001UL
#define WIN0DOOM_PCI_BAR_TYPE_MASK 0x00000006UL
#define WIN0DOOM_PCI_BAR_TYPE_64BIT 0x00000004UL
#define WIN0DOOM_PCI_BAR_ADDRESS_MASK 0xFFFFFFF0UL
#define WIN0DOOM_PCI_HEADER_MULTIFUNCTION 0x00800000UL

/*
 * Bochs VBE DISPI registers. QEMU's stdvga exposes the mode programmed by
 * the firmware (OVMF's GOP driver) through this index/data pair. Per
 * include/hw/display/bochs-vbe.h the indexes are: 0x1 xres, 0x2 yres,
 * 0x3 bpp, 0x6 virtual width, 0x7 virtual height. (0x8/0x9 are the X/Y
 * panning offsets and 0x4 is the enable word.) Note that QEMU makes
 * virtual height read-only: vbe_fixup_regs() reports the number of lines
 * that fit in VBE memory, while virtual width is the programmable stride
 * input, and stride = virtual_width * bpp / 8.
 */
#define WIN0DOOM_VBE_DISPI_INDEX_PORT 0x1CEUL
#define WIN0DOOM_VBE_DISPI_DATA_PORT 0x1CFUL
#define WIN0DOOM_VBE_DISPI_INDEX_XRES 0x0001UL
#define WIN0DOOM_VBE_DISPI_INDEX_YRES 0x0002UL
#define WIN0DOOM_VBE_DISPI_INDEX_BPP 0x0003UL
#define WIN0DOOM_VBE_DISPI_INDEX_VIRT_WIDTH 0x0006UL
#define WIN0DOOM_VBE_DISPI_INDEX_VIRT_HEIGHT 0x0007UL

/*
 * Port I/O. wdm.h's READ_PORT and WRITE_PORT wrappers expand to MSVC
 * intrinsics (__indword and friends) that clang-cl leaves as external
 * symbols, so drive the instructions directly. The memory clobber keeps
 * surrounding accesses from being reordered across the port operation.
 */
static USHORT Win0DoomPortReadWord(USHORT Port)
{
    USHORT value;
    __asm__ __volatile__("inw %w1, %w0" : "=a"(value) : "Nd"(Port) : "memory");
    return value;
}

static ULONG Win0DoomPortReadDword(USHORT Port)
{
    ULONG value;
    __asm__ __volatile__("inl %w1, %k0" : "=a"(value) : "Nd"(Port) : "memory");
    return value;
}

static VOID Win0DoomPortWriteWord(USHORT Port, USHORT Value)
{
    __asm__ __volatile__("outw %w0, %w1" : : "a"(Value), "Nd"(Port) : "memory");
}

static VOID Win0DoomPortWriteDword(USHORT Port, ULONG Value)
{
    __asm__ __volatile__("outl %k0, %w1" : : "a"(Value), "Nd"(Port) : "memory");
}

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
    ULONG DisplayStride;      /* bytes per display scan line (virt width * bpp/8) */
    ULONG DisplayWidth;       /* visible horizontal pixels (DISPI xres) */
    ULONG DisplayHeight;      /* visible vertical pixels (DISPI yres) */
    ULONG DisplayScale;       /* integer scale applied to the 320x200 frame */
    ULONG DisplayOffsetX;     /* left edge of the centered image */
    ULONG DisplayOffsetY;     /* top edge of the centered image */
} WIN0DOOM_CONTROL_EXTENSION, *PWIN0DOOM_CONTROL_EXTENSION;

typedef struct _WIN0DOOM_FILTER_EXTENSION {
    WIN0DOOM_DEVICE_KIND Kind;
    PDEVICE_OBJECT LowerDevice;
    EX_RUNDOWN_REF Rundown;
    KSPIN_LOCK ConnectLock;
    CONNECT_DATA UpperConnectData;
    BOOLEAN Connected;
    BOOLEAN Disconnected;
    BOOLEAN RundownWaited;
} WIN0DOOM_FILTER_EXTENSION, *PWIN0DOOM_FILTER_EXTENSION;

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE Win0DoomAddDevice;

static PWIN0DOOM_CONTROL_EXTENSION g_ControlExtension;
static EX_RUNDOWN_REF g_ControlRundown;
static BOOLEAN g_ControlRundownInitialized;

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

/*
 * PCI configuration space access. Discovery runs once on the single thread
 * that is executing DriverEntry, so these register accesses do not race with
 * themselves. Raw 0xCF8/0xCFC access is not serialized against a concurrent
 * PCI configuration access performed by another component (Windows normally
 * uses the PCI bus driver's MMCONFIG path on q35); the only write below is
 * guarded by re-checking the device identity immediately beforehand, and it
 * is skipped entirely when the firmware already set memory decode.
 */
static ULONG Win0DoomPciAddress(ULONG Bus, ULONG Device, ULONG Function, ULONG Offset)
{
    return WIN0DOOM_PCI_CONFIG_ENABLE |
        (Bus << 16) |
        (Device << 11) |
        (Function << 8) |
        (Offset & 0xFCUL);
}

static ULONG Win0DoomPciReadConfig(ULONG Bus, ULONG Device, ULONG Function, ULONG Offset)
{
    Win0DoomPortWriteDword(
        (USHORT)WIN0DOOM_PCI_CONFIG_ADDRESS_PORT,
        Win0DoomPciAddress(Bus, Device, Function, Offset)
    );
    return Win0DoomPortReadDword((USHORT)WIN0DOOM_PCI_CONFIG_DATA_PORT);
}

static VOID Win0DoomPciWriteCommand(
    ULONG Bus,
    ULONG Device,
    ULONG Function,
    USHORT Command
)
{
    Win0DoomPortWriteDword(
        (USHORT)WIN0DOOM_PCI_CONFIG_ADDRESS_PORT,
        Win0DoomPciAddress(Bus, Device, Function, WIN0DOOM_PCI_REG_COMMAND)
    );
    /* A word write to the data port updates only the command register and
       leaves the read/write-one-to-clear status word untouched. */
    Win0DoomPortWriteWord((USHORT)WIN0DOOM_PCI_CONFIG_DATA_PORT, Command);
}

static NTSTATUS Win0DoomTakeStdvgaBar(
    ULONG Bus,
    ULONG Device,
    ULONG Function,
    PHYSICAL_ADDRESS *FramebufferAddress
)
{
    ULONG bar0 = Win0DoomPciReadConfig(Bus, Device, Function, WIN0DOOM_PCI_REG_BAR0);
    ULONG bar1 = 0;
    ULONG command;
    ULONG64 base;

    if ((bar0 & WIN0DOOM_PCI_BAR_SPACE_IO) != 0) {
        DbgPrint("Win0DoomDisplay: stdvga BAR0 %08lx is an I/O-space BAR\n", bar0);
        return STATUS_DEVICE_NOT_READY;
    }
    if ((bar0 & WIN0DOOM_PCI_BAR_TYPE_MASK) == WIN0DOOM_PCI_BAR_TYPE_64BIT) {
        bar1 = Win0DoomPciReadConfig(Bus, Device, Function, WIN0DOOM_PCI_REG_BAR1);
        base = ((ULONG64)bar1 << 32) | (ULONG64)(bar0 & WIN0DOOM_PCI_BAR_ADDRESS_MASK);
    } else {
        base = (ULONG64)(bar0 & WIN0DOOM_PCI_BAR_ADDRESS_MASK);
    }
    if (base == 0) {
        DbgPrint("Win0DoomDisplay: stdvga BAR0 has no firmware-assigned address\n");
        return STATUS_DEVICE_NOT_READY;
    }
    if ((base & (ULONG64)(WIN0DOOM_DISPLAY_BYTES - 1)) != 0) {
        DbgPrint(
            "Win0DoomDisplay: stdvga BAR0 %llx is not aligned to %lu bytes\n",
            (unsigned long long)base,
            WIN0DOOM_DISPLAY_BYTES
        );
        return STATUS_DEVICE_NOT_READY;
    }

    /*
     * The BAR length is not probed by writing all ones: that would temporarily
     * unassign a live display aperture, and another component touching PCI
     * configuration space at the same time could observe or corrupt the
     * intermediate value. QEMU's stdvga registers a fixed 16 MiB BAR0, the
     * base is required to be 16 MiB aligned, and the DISPI geometry is later
     * checked against WIN0DOOM_DISPLAY_BYTES before any pixel is written.
     * Restoring the firmware-assigned address is out of scope for this
     * prototype; if this driver ever cleared the BAR, the machine would need a
     * reboot, so probing is deliberately not attempted.
     */
    command = Win0DoomPciReadConfig(Bus, Device, Function, WIN0DOOM_PCI_REG_COMMAND);
    if ((command & WIN0DOOM_PCI_COMMAND_MEMORY) == 0) {
        /*
         * The previous command value is deliberately not restored: this
         * prototype leaves memory decode enabled because the mapped
         * framebuffer must stay decodable for the lifetime of the driver.
         */
        /* Re-check identity just before the single config-space write. */
        ULONG identity = Win0DoomPciReadConfig(
            Bus,
            Device,
            Function,
            WIN0DOOM_PCI_REG_VENDOR_DEVICE
        );
        if ((identity & 0xFFFFUL) != WIN0DOOM_PCI_VENDOR_QEMU ||
            (identity >> 16) != WIN0DOOM_PCI_DEVICE_STDVGA) {
            DbgPrint("Win0DoomDisplay: stdvga vanished before enabling memory decode\n");
            return STATUS_DEVICE_NOT_READY;
        }
        DbgPrint(
            "Win0DoomDisplay: enabling PCI memory decode on %lu:%lu.%lu (command %08lx)\n",
            Bus,
            Device,
            Function,
            command
        );
        Win0DoomPciWriteCommand(
            Bus,
            Device,
            Function,
            (USHORT)(command | WIN0DOOM_PCI_COMMAND_MEMORY)
        );
    }

    FramebufferAddress->QuadPart = (LONGLONG)base;
    return STATUS_SUCCESS;
}

static NTSTATUS Win0DoomDiscoverFramebuffer(PHYSICAL_ADDRESS *FramebufferAddress)
{
    for (ULONG bus = 0; bus < WIN0DOOM_PCI_BUS_COUNT; ++bus) {
        for (ULONG device = 0; device < WIN0DOOM_PCI_DEVICE_COUNT; ++device) {
            ULONG identity = Win0DoomPciReadConfig(
                bus,
                device,
                0,
                WIN0DOOM_PCI_REG_VENDOR_DEVICE
            );
            ULONG function_count = 1;

            if ((identity & 0xFFFFUL) == 0xFFFFUL) {
                continue;
            }
            if ((Win0DoomPciReadConfig(
                    bus,
                    device,
                    0,
                    WIN0DOOM_PCI_REG_HEADER_TYPE
                 ) & WIN0DOOM_PCI_HEADER_MULTIFUNCTION) != 0) {
                function_count = WIN0DOOM_PCI_FUNCTION_COUNT;
            }
            for (ULONG function = 0; function < function_count; ++function) {
                if (function != 0) {
                    identity = Win0DoomPciReadConfig(
                        bus,
                        device,
                        function,
                        WIN0DOOM_PCI_REG_VENDOR_DEVICE
                    );
                }
                if ((identity & 0xFFFFUL) != WIN0DOOM_PCI_VENDOR_QEMU ||
                    (identity >> 16) != WIN0DOOM_PCI_DEVICE_STDVGA) {
                    continue;
                }
                DbgPrint(
                    "Win0DoomDisplay: QEMU stdvga at PCI %lu:%lu.%lu\n",
                    bus,
                    device,
                    function
                );
                return Win0DoomTakeStdvgaBar(bus, device, function, FramebufferAddress);
            }
        }
    }
    DbgPrint("Win0DoomDisplay: QEMU stdvga 1234:1111 not present in PCI config space\n");
    return STATUS_DEVICE_NOT_READY;
}

static USHORT Win0DoomVbeDispiRead(ULONG Index)
{
    Win0DoomPortWriteWord((USHORT)WIN0DOOM_VBE_DISPI_INDEX_PORT, (USHORT)Index);
    return Win0DoomPortReadWord((USHORT)WIN0DOOM_VBE_DISPI_DATA_PORT);
}

/*
 * The DISPI interface is not guaranteed to answer after the firmware hands
 * the machine to Windows (the registers can read back as zero when VGA I/O
 * decoding is disabled). The verified boot configuration of this project is
 * 1280x800x32 at stride 5120, which also fits inside the 16 MiB aperture, so
 * an unusable DISPI read falls back to that layout instead of failing the
 * driver: the 4x fast path is then byte-for-byte the original behavior.
 */
static VOID Win0DoomUseFallbackGeometry(PWIN0DOOM_CONTROL_EXTENSION Extension, PCSTR Reason)
{
    Extension->DisplayStride = WIN0DOOM_DISPLAY_WIDTH * (WIN0DOOM_DISPLAY_BPP / 8);
    Extension->DisplayWidth = WIN0DOOM_DISPLAY_WIDTH;
    Extension->DisplayHeight = WIN0DOOM_DISPLAY_HEIGHT;
    Extension->DisplayScale = 4;
    Extension->DisplayOffsetX = 0;
    Extension->DisplayOffsetY = 0;
    DbgPrint(
        "Win0DoomDisplay: DISPI %s; assuming verified 1280x800x32 layout\n",
        Reason
    );
}

static VOID Win0DoomReadDisplayGeometry(PWIN0DOOM_CONTROL_EXTENSION Extension)
{
    ULONG xres = Win0DoomVbeDispiRead(WIN0DOOM_VBE_DISPI_INDEX_XRES);
    ULONG yres = Win0DoomVbeDispiRead(WIN0DOOM_VBE_DISPI_INDEX_YRES);
    ULONG bpp = Win0DoomVbeDispiRead(WIN0DOOM_VBE_DISPI_INDEX_BPP);
    ULONG virtual_width = Win0DoomVbeDispiRead(WIN0DOOM_VBE_DISPI_INDEX_VIRT_WIDTH);
    ULONG virtual_height = Win0DoomVbeDispiRead(WIN0DOOM_VBE_DISPI_INDEX_VIRT_HEIGHT);
    ULONG stride;
    ULONG scale;
    ULONG64 aperture_needed;

    if (xres == 0 || yres == 0 || virtual_width == 0 || virtual_height == 0) {
        Win0DoomUseFallbackGeometry(Extension, "reports zero geometry");
        return;
    }
    if (bpp != WIN0DOOM_DISPLAY_BPP) {
        DbgPrint("Win0DoomDisplay: DISPI bpp %lu is unsupported (32 required)\n", bpp);
        Win0DoomUseFallbackGeometry(Extension, "reports an unsupported bpp");
        return;
    }
    if (virtual_width < xres || virtual_height < yres) {
        DbgPrint(
            "Win0DoomDisplay: DISPI virtual %lux%lu is smaller than visible %lux%lu\n",
            virtual_width,
            virtual_height,
            xres,
            yres
        );
        Win0DoomUseFallbackGeometry(Extension, "reports an inconsistent virtual size");
        return;
    }
    stride = virtual_width * (bpp / 8);
    aperture_needed = (ULONG64)stride * (ULONG64)virtual_height;
    if (aperture_needed > (ULONG64)WIN0DOOM_DISPLAY_BYTES) {
        DbgPrint(
            "Win0DoomDisplay: DISPI geometry needs %llu bytes, aperture is %lu bytes\n",
            (unsigned long long)aperture_needed,
            WIN0DOOM_DISPLAY_BYTES
        );
        Win0DoomUseFallbackGeometry(Extension, "reports a mode outside the aperture");
        return;
    }

    scale = xres / WIN0DOOM_FRAME_WIDTH;
    if (yres / WIN0DOOM_FRAME_HEIGHT < scale) {
        scale = yres / WIN0DOOM_FRAME_HEIGHT;
    }
    if (scale == 0) {
        DbgPrint(
            "Win0DoomDisplay: DISPI mode %lux%lu cannot hold a 320x200 frame\n",
            xres,
            yres
        );
        Win0DoomUseFallbackGeometry(Extension, "reports a mode smaller than the frame");
        return;
    }

    Extension->DisplayStride = stride;
    Extension->DisplayWidth = xres;
    Extension->DisplayHeight = yres;
    Extension->DisplayScale = scale;
    Extension->DisplayOffsetX = (xres - WIN0DOOM_FRAME_WIDTH * scale) / 2;
    Extension->DisplayOffsetY = (yres - WIN0DOOM_FRAME_HEIGHT * scale) / 2;
    DbgPrint(
        "Win0DoomDisplay: DISPI %lux%lu bpp %lu virt %lux%lu stride %lu scale %lu offset %lu,%lu\n",
        xres,
        yres,
        bpp,
        virtual_width,
        virtual_height,
        stride,
        scale,
        Extension->DisplayOffsetX,
        Extension->DisplayOffsetY
    );
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
        /* Drop PCM queued before this client connected (launch-time silence). */
        Win0DoomAudioReset();
    }
    return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static VOID Win0DoomQueueKeyboardAtDpc(const KEYBOARD_INPUT_DATA *Records, ULONG RecordCount)
{
    PWIN0DOOM_CONTROL_EXTENSION extension;

    if (g_ControlExtension == NULL) {
        return;
    }
    if (!ExAcquireRundownProtection(&g_ControlRundown)) {
        return;
    }
    extension = g_ControlExtension;
    if (extension != NULL) {
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
    ExReleaseRundownProtection(&g_ControlRundown);
}

static VOID Win0DoomKeyboardServiceCallback(
    PVOID NormalContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2,
    PVOID SystemArgument3
)
{
    PDEVICE_OBJECT filter_device = (PDEVICE_OBJECT)NormalContext;
    PWIN0DOOM_FILTER_EXTENSION extension;
    CONNECT_DATA connect_data;
    BOOLEAN connected;
    PKEYBOARD_INPUT_DATA begin;
    PKEYBOARD_INPUT_DATA end;

    if (filter_device == NULL) {
        return;
    }
    extension = (PWIN0DOOM_FILTER_EXTENSION)filter_device->DeviceExtension;
    if (extension == NULL || !ExAcquireRundownProtection(&extension->Rundown)) {
        return;
    }

    KeAcquireSpinLockAtDpcLevel(&extension->ConnectLock);
    connected = extension->Connected;
    connect_data = extension->UpperConnectData;
    KeReleaseSpinLockFromDpcLevel(&extension->ConnectLock);

    if (connected) {
        begin = (PKEYBOARD_INPUT_DATA)SystemArgument1;
        end = (PKEYBOARD_INPUT_DATA)SystemArgument2;
        if (end >= begin) {
            Win0DoomQueueKeyboardAtDpc(begin, (ULONG)(end - begin));
        }
        if (connect_data.ClassService != NULL) {
            PSERVICE_CALLBACK_ROUTINE upper_callback =
                (PSERVICE_CALLBACK_ROUTINE)connect_data.ClassService;
            upper_callback(
                connect_data.ClassDeviceObject,
                SystemArgument1,
                SystemArgument2,
                SystemArgument3
            );
        }
    }
    ExReleaseRundownProtection(&extension->Rundown);
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

/*
 * Blit one 320x200 RGBA frame. When the discovered mode is exactly
 * 1280x800x32 the loop below is the original fixed 4x path; any other mode
 * centers the largest integer scale that fits, using the discovered
 * destination stride.
 */
static VOID Win0DoomBlitFrame(PWIN0DOOM_CONTROL_EXTENSION Extension, const ULONG *Source)
{
    volatile ULONG *framebuffer = Extension->FrameBuffer;
    ULONG stride_pixels = (ULONG)(Extension->DisplayStride / (ULONG)sizeof(ULONG));
    ULONG scale = Extension->DisplayScale;
    ULONG offset_x = Extension->DisplayOffsetX;
    ULONG offset_y = Extension->DisplayOffsetY;

    if (scale == 4 && offset_x == 0 && offset_y == 0 &&
        stride_pixels == WIN0DOOM_DISPLAY_WIDTH) {
        for (ULONG source_y = 0; source_y < WIN0DOOM_FRAME_HEIGHT; ++source_y) {
            for (ULONG scale_y = 0; scale_y < 4; ++scale_y) {
                volatile ULONG *destination = framebuffer +
                    (source_y * 4 + scale_y) * WIN0DOOM_DISPLAY_WIDTH;
                const ULONG *source_row = Source + source_y * WIN0DOOM_FRAME_WIDTH;
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
        return;
    }

    for (ULONG source_y = 0; source_y < WIN0DOOM_FRAME_HEIGHT; ++source_y) {
        const ULONG *source_row = Source + source_y * WIN0DOOM_FRAME_WIDTH;
        for (ULONG scale_y = 0; scale_y < scale; ++scale_y) {
            volatile ULONG *destination = framebuffer +
                (offset_y + source_y * scale + scale_y) * stride_pixels +
                offset_x;
            for (ULONG source_x = 0; source_x < WIN0DOOM_FRAME_WIDTH; ++source_x) {
                ULONG rgba = source_row[source_x];
                ULONG pixel =
                    ((rgba & 0xff) << 16) | (rgba & 0xff00) | ((rgba >> 16) & 0xff);
                for (ULONG scale_x = 0; scale_x < scale; ++scale_x) {
                    destination[source_x * scale + scale_x] = pixel;
                }
            }
        }
    }
}

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

    if (code == WIN0DOOM_IOCTL_SOUND_WRITE ||
        code == WIN0DOOM_IOCTL_SOUND_STATUS ||
        code == WIN0DOOM_IOCTL_SOUND_RESET) {
        ULONG audio_information = 0;
        NTSTATUS audio_status = Win0DoomAudioDeviceControlEx(
            code,
            Irp->AssociatedIrp.SystemBuffer,
            input_length,
            Irp->AssociatedIrp.SystemBuffer,
            output_length,
            &audio_information
        );
        return Win0DoomCompleteIrp(Irp, audio_status, audio_information);
    }
    if (code == IOCTL_WIN0DOOM_GET_KEYBOARD) {
        PUCHAR output = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
        ULONG written;
        if (output == NULL || output_length < sizeof(KEYBOARD_INPUT_DATA)) {
            return Win0DoomCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        written = Win0DoomDrainKeyboard(extension, output, output_length);
        return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, written);
    }
    if (code != IOCTL_WIN0DOOM_PRESENT) {
        return Win0DoomCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    if (source == NULL || input_length < WIN0DOOM_FRAME_BYTES ||
        extension->FrameBuffer == NULL) {
        return Win0DoomCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
    Win0DoomBlitFrame(extension, source);
    return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS Win0DoomInternalDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    PWIN0DOOM_FILTER_EXTENSION extension;
    PCONNECT_DATA connect_data;
    KIRQL old_irql;
    NTSTATUS failure = STATUS_SUCCESS;

    if (Win0DoomIsControlDevice(DeviceObject)) {
        return Win0DoomCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    stack = IoGetCurrentIrpStackLocation(Irp);
    extension = (PWIN0DOOM_FILTER_EXTENSION)DeviceObject->DeviceExtension;
    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_KEYBOARD_CONNECT) {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA)) {
            return Win0DoomCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        connect_data = (PCONNECT_DATA)stack->Parameters.DeviceIoControl.Type3InputBuffer;
        if (connect_data == NULL || connect_data->ClassService == NULL) {
            return Win0DoomCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        }

        KeAcquireSpinLock(&extension->ConnectLock, &old_irql);
        if (extension->Connected || extension->Disconnected) {
            failure = extension->Connected ?
                STATUS_SHARING_VIOLATION : STATUS_DEVICE_NOT_CONNECTED;
        } else {
            extension->UpperConnectData = *connect_data;
            connect_data->ClassDeviceObject = DeviceObject;
            connect_data->ClassService = Win0DoomKeyboardServiceCallback;
            extension->Connected = TRUE;
        }
        KeReleaseSpinLock(&extension->ConnectLock, old_irql);

        if (!NT_SUCCESS(failure)) {
            return Win0DoomCompleteIrp(Irp, failure, 0);
        }
    }
    return Win0DoomForwardIrp(DeviceObject, Irp);
}

static VOID Win0DoomFilterSetDisconnected(PWIN0DOOM_FILTER_EXTENSION Extension)
{
    KIRQL old_irql;

    KeAcquireSpinLock(&Extension->ConnectLock, &old_irql);
    Extension->Connected = FALSE;
    Extension->Disconnected = TRUE;
    KeReleaseSpinLock(&Extension->ConnectLock, old_irql);
}

static VOID Win0DoomFilterWaitForCallbacks(PWIN0DOOM_FILTER_EXTENSION Extension)
{
    KIRQL old_irql;
    BOOLEAN wait;

    KeAcquireSpinLock(&Extension->ConnectLock, &old_irql);
    wait = !Extension->RundownWaited;
    Extension->RundownWaited = TRUE;
    KeReleaseSpinLock(&Extension->ConnectLock, old_irql);
    if (wait) {
        /* After this returns no callback can acquire the rundown again. */
        ExWaitForRundownProtectionRelease(&Extension->Rundown);
    }
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
    if (stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL) {
        /*
         * Stop delivering game input, but keep the device object and the
         * lower attachment until IRP_MN_REMOVE_DEVICE. Callbacks already in
         * flight observe Connected == FALSE; the remove path then waits for
         * them with the rundown before detaching.
         */
        Win0DoomFilterSetDisconnected(extension);
        return Win0DoomForwardIrp(DeviceObject, Irp);
    }
    if (stack->MinorFunction != IRP_MN_REMOVE_DEVICE) {
        return Win0DoomForwardIrp(DeviceObject, Irp);
    }

    /*
     * Block new callbacks and wait out the running ones before the extension
     * can disappear. This is a real rundown, not just a flag check.
     */
    Win0DoomFilterSetDisconnected(extension);
    Win0DoomFilterWaitForCallbacks(extension);

    lower_device = extension->LowerDevice;
    extension->LowerDevice = NULL;
    if (lower_device != NULL) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(lower_device, Irp);
        IoDetachDevice(lower_device);
    } else {
        status = Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }
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
    KeInitializeSpinLock(&extension->ConnectLock);
    ExInitializeRundownProtection(&extension->Rundown);
    filter_device->Flags |= extension->LowerDevice->Flags &
        (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    filter_device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID Win0DoomStopControlInput(VOID)
{
    g_ControlExtension = NULL;
    if (g_ControlRundownInitialized) {
        g_ControlRundownInitialized = FALSE;
        /* Wait for a keyboard DPC that already entered the ring path. */
        ExWaitForRundownProtectionRelease(&g_ControlRundown);
    }
}

static VOID Win0DoomDeleteDevices(PDRIVER_OBJECT DriverObject)
{
    PDEVICE_OBJECT device;
    PDEVICE_OBJECT next;

    /* Unmap and remove the control device first. */
    device = DriverObject->DeviceObject;
    while (device != NULL) {
        next = device->NextDevice;
        if (Win0DoomIsControlDevice(device)) {
            PWIN0DOOM_CONTROL_EXTENSION control =
                (PWIN0DOOM_CONTROL_EXTENSION)device->DeviceExtension;
            if (control->FrameBuffer != NULL) {
                MmUnmapIoSpace((PVOID)control->FrameBuffer, WIN0DOOM_DISPLAY_BYTES);
                control->FrameBuffer = NULL;
            }
            IoDeleteDevice(device);
        }
        device = next;
    }

    /* Then detach and delete any filter device that never saw IRP_MN_REMOVE_DEVICE. */
    device = DriverObject->DeviceObject;
    while (device != NULL) {
        next = device->NextDevice;
        if (Win0DoomIsControlDevice(device) == FALSE) {
            PWIN0DOOM_FILTER_EXTENSION filter =
                (PWIN0DOOM_FILTER_EXTENSION)device->DeviceExtension;
            Win0DoomFilterSetDisconnected(filter);
            Win0DoomFilterWaitForCallbacks(filter);
            if (filter->LowerDevice != NULL) {
                IoDetachDevice(filter->LowerDevice);
                filter->LowerDevice = NULL;
            }
        }
        IoDeleteDevice(device);
        device = next;
    }
}

static VOID Win0DoomUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symbolic_link;

    RtlInitUnicodeString(&symbolic_link, L"\\DosDevices\\Win0DoomDisplay");
    IoDeleteSymbolicLink(&symbolic_link);
    Win0DoomStopControlInput();
    /*
     * The PCM pump holds no pointer into the control extension, but it must be
     * stopped before the framebuffer is unmapped and the devices disappear so
     * a failing unload cannot leave a thread poking a dead device.
     */
    Win0DoomAudioShutdown();
    Win0DoomDeleteDevices(DriverObject);
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

    /*
     * Install the dispatch table and unload before any fallible step so a
     * partially initialized driver still tears down through one path.
     */
    DriverObject->MajorFunction[IRP_MJ_CREATE] = Win0DoomCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = Win0DoomCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Win0DoomDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = Win0DoomInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = Win0DoomPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = Win0DoomPower;
    DriverObject->DriverExtension->AddDevice = Win0DoomAddDevice;
    DriverObject->DriverUnload = Win0DoomUnload;

    ExInitializeRundownProtection(&g_ControlRundown);
    g_ControlRundownInitialized = TRUE;

    framebuffer_address.QuadPart = 0;
    status = Win0DoomDiscoverFramebuffer(&framebuffer_address);
    if (!NT_SUCCESS(status)) {
        DbgPrint(
            "Win0DoomDisplay: PCI discovery failed (0x%08lx); refusing to map a guessed aperture\n",
            (ULONG)status
        );
        Win0DoomUnload(DriverObject);
        return STATUS_DEVICE_NOT_READY;
    }

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
        Win0DoomUnload(DriverObject);
        return status;
    }
    extension = (PWIN0DOOM_CONTROL_EXTENSION)device->DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));
    extension->Kind = Win0DoomControlDevice;
    KeInitializeSpinLock(&extension->KeyboardLock);

    Win0DoomReadDisplayGeometry(extension);

    extension->FrameBuffer = (volatile ULONG *)MmMapIoSpaceEx(
        framebuffer_address,
        WIN0DOOM_DISPLAY_BYTES,
        PAGE_READWRITE | PAGE_NOCACHE
    );
    if (extension->FrameBuffer == NULL) {
        DbgPrint(
            "Win0DoomDisplay: MmMapIoSpaceEx failed for %llx (%lu bytes)\n",
            (unsigned long long)framebuffer_address.QuadPart,
            WIN0DOOM_DISPLAY_BYTES
        );
        Win0DoomUnload(DriverObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    DbgPrint(
        "Win0DoomDisplay: mapped %lu bytes of framebuffer at %llx\n",
        WIN0DOOM_DISPLAY_BYTES,
        (unsigned long long)framebuffer_address.QuadPart
    );

    RtlInitUnicodeString(&symbolic_link, L"\\DosDevices\\Win0DoomDisplay");
    status = IoCreateSymbolicLink(&symbolic_link, &device_name);
    if (!NT_SUCCESS(status)) {
        DbgPrint("Win0DoomDisplay: IoCreateSymbolicLink failed 0x%08lx\n", (ULONG)status);
        Win0DoomUnload(DriverObject);
        return status;
    }

    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    g_ControlExtension = extension;

    /*
     * Non-fatal by contract: without an SB16 the game still renders and reads
     * the keyboard, and SOUND_STATUS reports DeviceReady = 0.
     */
    (VOID)Win0DoomAudioInitialize();
    return STATUS_SUCCESS;
}
