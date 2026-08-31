#include <ntddk.h>

#define WIN0DOOM_FRAME_WIDTH 320UL
#define WIN0DOOM_FRAME_HEIGHT 200UL
#define WIN0DOOM_FRAME_BYTES (WIN0DOOM_FRAME_WIDTH * WIN0DOOM_FRAME_HEIGHT * 4UL)
#define WIN0DOOM_DISPLAY_WIDTH 1280UL
#define WIN0DOOM_DISPLAY_HEIGHT 800UL
#define WIN0DOOM_DISPLAY_BYTES (16UL * 1024UL * 1024UL)
#define IOCTL_WIN0DOOM_PRESENT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct _WIN0DOOM_DEVICE_EXTENSION {
    volatile ULONG *FrameBuffer;
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

static NTSTATUS Win0DoomDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PWIN0DOOM_DEVICE_EXTENSION extension =
        (PWIN0DOOM_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG *source = (const ULONG *)Irp->AssociatedIrp.SystemBuffer;

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
