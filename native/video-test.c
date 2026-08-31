#include <Windows.h>
#include <winternl.h>

#define IOCTL_VIDEO_MAP_VIDEO_MEMORY 0x00230458UL

typedef struct _VIDEO_MEMORY {
    PVOID RequestedVirtualAddress;
} VIDEO_MEMORY;

typedef struct _VIDEO_MEMORY_INFORMATION {
    PVOID VideoRamBase;
    ULONG VideoRamLength;
    PVOID FrameBufferBase;
    ULONG FrameBufferLength;
} VIDEO_MEMORY_INFORMATION;

NTSYSCALLAPI NTSTATUS NTAPI NtDisplayString(PUNICODE_STRING String);
NTSYSCALLAPI NTSTATUS NTAPI NtDelayExecution(
    BOOLEAN Alertable,
    PLARGE_INTEGER DelayInterval
);
NTSYSCALLAPI NTSTATUS NTAPI NtTerminateProcess(
    HANDLE ProcessHandle,
    NTSTATUS ExitStatus
);

static void print_status(PCWSTR prefix, NTSTATUS status)
{
    WCHAR buffer[96];
    UNICODE_STRING output;
    ULONG value = (ULONG)status;
    ULONG index = 0;

    while (prefix[index] != L'\0') {
        buffer[index] = prefix[index];
        ++index;
    }
    buffer[index++] = L'0';
    buffer[index++] = L'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        ULONG digit = (value >> shift) & 0xf;
        buffer[index++] = (WCHAR)(digit < 10 ? L'0' + digit : L'a' + digit - 10);
    }
    buffer[index++] = L'\r';
    buffer[index++] = L'\n';
    buffer[index] = L'\0';
    RtlInitUnicodeString(&output, buffer);
    NtDisplayString(&output);
}

VOID NTAPI NtProcessStartup(PVOID StartupArgument)
{
    static const ULONG width = 1280;
    static const ULONG height = 800;
    UNICODE_STRING video_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io_status;
    VIDEO_MEMORY request;
    VIDEO_MEMORY_INFORMATION memory;
    LARGE_INTEGER delay;
    HANDLE video = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(StartupArgument);
    request.RequestedVirtualAddress = NULL;

    RtlInitUnicodeString(&video_name, L"\\Device\\Video0");
    InitializeObjectAttributes(
        &attributes,
        &video_name,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );
    status = NtCreateFile(
        &video,
        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
        &attributes,
        &io_status,
        NULL,
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL,
        0
    );
    if (status < 0) {
        print_status(L"\r\nopen \\Device\\Video0 failed: ", status);
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, status);
    }

    status = NtDeviceIoControlFile(
        video,
        NULL,
        NULL,
        NULL,
        &io_status,
        IOCTL_VIDEO_MAP_VIDEO_MEMORY,
        &request,
        sizeof(request),
        &memory,
        sizeof(memory)
    );
    if (status < 0) {
        print_status(L"\r\nmap video memory ioctl failed: ", status);
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, status);
    }

    if (memory.FrameBufferBase == NULL || memory.FrameBufferLength < width * height * 4) {
        print_status(L"\r\nframebuffer too small, length: ", (NTSTATUS)memory.FrameBufferLength);
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, (NTSTATUS)0xc0000023L);
    }

    for (ULONG y = 0; y < height; ++y) {
        volatile ULONG *row = (volatile ULONG *)memory.FrameBufferBase + y * width;
        for (ULONG x = 0; x < width; ++x) {
            ULONG checker = ((x >> 6) ^ (y >> 6)) & 1;
            ULONG red = (x * 255) / (width - 1);
            ULONG green = (y * 255) / (height - 1);
            ULONG blue = checker ? 0xd0 : 0x20;
            row[x] = (red << 16) | (green << 8) | blue;
        }
    }

    delay.QuadPart = -10000000LL;
    for (;;) {
        NtDelayExecution(FALSE, &delay);
    }
}
