#include <ntddk.h>
#include <ntddkbd.h>

NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(
    HANDLE Handle,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout
);

#define WIN0DOOM_FRAME_WIDTH 320UL
#define WIN0DOOM_FRAME_HEIGHT 200UL
#define WIN0DOOM_FRAME_BYTES (WIN0DOOM_FRAME_WIDTH * WIN0DOOM_FRAME_HEIGHT * 4UL)
#define WIN0DOOM_DISPLAY_WIDTH 1280UL
#define WIN0DOOM_DISPLAY_HEIGHT 800UL
#define WIN0DOOM_DISPLAY_BYTES (16UL * 1024UL * 1024UL)
#define IOCTL_WIN0DOOM_PRESENT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WIN0DOOM_GET_KEYBOARD \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define WIN0DOOM_KEYBOARD_RING_SIZE 128UL

struct _WIN0DOOM_DEVICE_EXTENSION;

typedef struct _WIN0DOOM_KEYBOARD_CHANNEL {
    HANDLE FileHandle;
    HANDLE ThreadHandle;
    struct _WIN0DOOM_DEVICE_EXTENSION *Owner;
} WIN0DOOM_KEYBOARD_CHANNEL, *PWIN0DOOM_KEYBOARD_CHANNEL;

typedef struct _WIN0DOOM_DEVICE_EXTENSION {
    volatile ULONG *FrameBuffer;
    WIN0DOOM_KEYBOARD_CHANNEL Keyboards[2];
    BOOLEAN KeyboardsInitialized;
    NTSTATUS KeyboardStatus;
    volatile BOOLEAN StopKeyboardThreads;
    KSPIN_LOCK KeyboardLock;
    KEYBOARD_INPUT_DATA KeyboardRing[WIN0DOOM_KEYBOARD_RING_SIZE];
    ULONG KeyboardReadIndex;
    ULONG KeyboardWriteIndex;
} WIN0DOOM_DEVICE_EXTENSION, *PWIN0DOOM_DEVICE_EXTENSION;

DRIVER_INITIALIZE DriverEntry;

static NTSTATUS Win0DoomCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS Win0DoomCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static VOID Win0DoomQueueKeyboardRecords(
    PWIN0DOOM_DEVICE_EXTENSION Extension,
    const KEYBOARD_INPUT_DATA *Records,
    ULONG RecordCount
)
{
    KIRQL old_irql;

    KeAcquireSpinLock(&Extension->KeyboardLock, &old_irql);
    for (ULONG index = 0; index < RecordCount; ++index) {
        ULONG next = (Extension->KeyboardWriteIndex + 1) %
            WIN0DOOM_KEYBOARD_RING_SIZE;
        if (next == Extension->KeyboardReadIndex) {
            Extension->KeyboardReadIndex =
                (Extension->KeyboardReadIndex + 1) %
                WIN0DOOM_KEYBOARD_RING_SIZE;
        }
        Extension->KeyboardRing[Extension->KeyboardWriteIndex] = Records[index];
        Extension->KeyboardWriteIndex = next;
    }
    KeReleaseSpinLock(&Extension->KeyboardLock, old_irql);
}

static VOID Win0DoomKeyboardThread(PVOID Context)
{
    PWIN0DOOM_KEYBOARD_CHANNEL channel =
        (PWIN0DOOM_KEYBOARD_CHANNEL)Context;
    PWIN0DOOM_DEVICE_EXTENSION extension = channel->Owner;
    KEYBOARD_INPUT_DATA records[32];
    NTSTATUS status = STATUS_SUCCESS;

    while (!extension->StopKeyboardThreads) {
        IO_STATUS_BLOCK io_status;

        status = ZwReadFile(
            channel->FileHandle,
            NULL,
            NULL,
            NULL,
            &io_status,
            records,
            sizeof(records),
            NULL,
            NULL
        );
        if (!NT_SUCCESS(status)) {
            break;
        }
        Win0DoomQueueKeyboardRecords(
            extension,
            records,
            (ULONG)io_status.Information / sizeof(records[0])
        );
    }
    PsTerminateSystemThread(status);
}

static VOID Win0DoomInitializeKeyboards(
    PWIN0DOOM_DEVICE_EXTENSION Extension
)
{
    static const WCHAR *device_names[] = {
        L"\\Device\\KeyboardClass1",
        L"\\Device\\KeyboardClass0"
    };
    BOOLEAN opened = FALSE;

    if (Extension->KeyboardsInitialized) {
        return;
    }
    Extension->KeyboardsInitialized = TRUE;
    Extension->KeyboardStatus = STATUS_OBJECT_NAME_NOT_FOUND;

    for (ULONG index = 0; index < RTL_NUMBER_OF(device_names); ++index) {
        PWIN0DOOM_KEYBOARD_CHANNEL channel = &Extension->Keyboards[index];
        UNICODE_STRING device_name;
        OBJECT_ATTRIBUTES file_attributes;
        IO_STATUS_BLOCK io_status;
        NTSTATUS status;

        RtlInitUnicodeString(&device_name, device_names[index]);
        InitializeObjectAttributes(
            &file_attributes,
            &device_name,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
            NULL,
            NULL
        );
        status = ZwCreateFile(
            &channel->FileHandle,
            GENERIC_READ | SYNCHRONIZE,
            &file_attributes,
            &io_status,
            NULL,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
            NULL,
            0
        );
        if (!NT_SUCCESS(status)) {
            Extension->KeyboardStatus = status;
            channel->FileHandle = NULL;
            continue;
        }

        channel->Owner = Extension;
        status = PsCreateSystemThread(
            &channel->ThreadHandle,
            THREAD_ALL_ACCESS,
            NULL,
            NULL,
            NULL,
            Win0DoomKeyboardThread,
            channel
        );
        if (!NT_SUCCESS(status)) {
            Extension->KeyboardStatus = status;
            ZwClose(channel->FileHandle);
            channel->FileHandle = NULL;
            channel->ThreadHandle = NULL;
            continue;
        }
        opened = TRUE;
    }
    if (opened) {
        Extension->KeyboardStatus = STATUS_SUCCESS;
    }
}

static ULONG Win0DoomDrainKeyboard(
    PWIN0DOOM_DEVICE_EXTENSION Extension,
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
            (Extension->KeyboardReadIndex + 1) %
            WIN0DOOM_KEYBOARD_RING_SIZE;
        written += sizeof(KEYBOARD_INPUT_DATA);
    }
    KeReleaseSpinLock(&Extension->KeyboardLock, old_irql);
    return written;
}

static NTSTATUS Win0DoomDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PWIN0DOOM_DEVICE_EXTENSION extension =
        (PWIN0DOOM_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    const ULONG *source = (const ULONG *)Irp->AssociatedIrp.SystemBuffer;

    if (code == IOCTL_WIN0DOOM_GET_KEYBOARD) {
        PUCHAR output = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
        ULONG written = 0;

        Win0DoomInitializeKeyboards(extension);
        if (!NT_SUCCESS(extension->KeyboardStatus)) {
            return Win0DoomCompleteIrp(
                Irp,
                extension->KeyboardStatus,
                0
            );
        }
        if (output == NULL || output_length < sizeof(KEYBOARD_INPUT_DATA)) {
            return Win0DoomCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        written = Win0DoomDrainKeyboard(extension, output, output_length);
        return Win0DoomCompleteIrp(Irp, STATUS_SUCCESS, written);
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

static VOID Win0DoomUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symbolic_link;
    PDEVICE_OBJECT device = DriverObject->DeviceObject;

    RtlInitUnicodeString(&symbolic_link, L"\\DosDevices\\Win0DoomDisplay");
    IoDeleteSymbolicLink(&symbolic_link);

    if (device != NULL) {
        PWIN0DOOM_DEVICE_EXTENSION extension =
            (PWIN0DOOM_DEVICE_EXTENSION)device->DeviceExtension;
        if (extension->FrameBuffer != NULL) {
            MmUnmapIoSpace((PVOID)extension->FrameBuffer, WIN0DOOM_DISPLAY_BYTES);
        }
        for (ULONG index = 0;
             index < RTL_NUMBER_OF(extension->Keyboards);
             ++index) {
            if (extension->Keyboards[index].EventHandle != NULL) {
                ZwClose(extension->Keyboards[index].EventHandle);
            }
            if (extension->Keyboards[index].FileHandle != NULL) {
                ZwClose(extension->Keyboards[index].FileHandle);
            }
        }
        IoDeleteDevice(device);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING device_name;
    UNICODE_STRING symbolic_link;
    PDEVICE_OBJECT device = NULL;
    PWIN0DOOM_DEVICE_EXTENSION extension;
    PHYSICAL_ADDRESS framebuffer_address;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&device_name, L"\\Device\\Win0DoomDisplay");
    status = IoCreateDevice(
        DriverObject,
        sizeof(WIN0DOOM_DEVICE_EXTENSION),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &device
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    extension = (PWIN0DOOM_DEVICE_EXTENSION)device->DeviceExtension;
    extension->FrameBuffer = NULL;
    RtlZeroMemory(extension->Keyboards, sizeof(extension->Keyboards));
    extension->KeyboardsInitialized = FALSE;
    extension->KeyboardStatus = STATUS_PENDING;
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
    DriverObject->DriverUnload = Win0DoomUnload;
    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
