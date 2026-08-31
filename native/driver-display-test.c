#include <Windows.h>
#include <winternl.h>

#define FRAME_WIDTH 320UL
#define FRAME_HEIGHT 200UL
#define IOCTL_WIN0DOOM_PRESENT 0x0022a000UL

NTSYSCALLAPI NTSTATUS NTAPI NtDisplayString(PUNICODE_STRING String);
NTSYSCALLAPI NTSTATUS NTAPI NtDelayExecution(
    BOOLEAN Alertable,
    PLARGE_INTEGER DelayInterval
);
NTSYSCALLAPI NTSTATUS NTAPI NtTerminateProcess(
    HANDLE ProcessHandle,
    NTSTATUS ExitStatus
);

static ULONG frame[FRAME_WIDTH * FRAME_HEIGHT];

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
    UNICODE_STRING device_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io_status;
    LARGE_INTEGER delay;
    HANDLE display = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(StartupArgument);

    RtlInitUnicodeString(&device_name, L"\\Device\\Win0DoomDisplay");
    InitializeObjectAttributes(
        &attributes,
        &device_name,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );
    status = NtCreateFile(
        &display,
        GENERIC_WRITE | SYNCHRONIZE,
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
        print_status(L"\r\nopen win0 doom display failed: ", status);
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, status);
    }

    for (ULONG y = 0; y < FRAME_HEIGHT; ++y) {
        for (ULONG x = 0; x < FRAME_WIDTH; ++x) {
            ULONG checker = ((x >> 4) ^ (y >> 4)) & 1;
            ULONG red = (x * 255) / (FRAME_WIDTH - 1);
            ULONG green = (y * 255) / (FRAME_HEIGHT - 1);
            ULONG blue = checker ? 0xd0 : 0x20;
            frame[y * FRAME_WIDTH + x] =
                red | (green << 8) | (blue << 16) | 0xff000000UL;
        }
    }

    status = NtDeviceIoControlFile(
        display,
        NULL,
        NULL,
        NULL,
        &io_status,
        IOCTL_WIN0DOOM_PRESENT,
        frame,
        sizeof(frame),
        NULL,
        0
    );
    if (status < 0) {
        print_status(L"\r\npresent ioctl failed: ", status);
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, status);
    }

    delay.QuadPart = -10000000LL;
    for (;;) {
        NtDelayExecution(FALSE, &delay);
    }
}
