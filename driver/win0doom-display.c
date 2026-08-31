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
#define WIN0DOOM_KEYBOARD_RING_SIZE 128UL

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
    return STATUS_SUCCESS;
}
