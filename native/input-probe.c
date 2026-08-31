#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

typedef struct _WIN0DOOM_PROCESS_PARAMETERS_HEAD {
    ULONG MaximumLength;
    ULONG Length;
    ULONG Flags;
    ULONG DebugFlags;
    HANDLE ConsoleHandle;
    ULONG ConsoleFlags;
    ULONG ConsolePadding;
    HANDLE StandardInput;
    HANDLE StandardOutput;
    HANDLE StandardError;
} WIN0DOOM_PROCESS_PARAMETERS_HEAD;

NTSYSCALLAPI NTSTATUS NTAPI NtDisplayString(PUNICODE_STRING String);
NTSYSCALLAPI NTSTATUS NTAPI NtReadFile(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER ByteOffset,
    PULONG Key
);
NTSYSCALLAPI NTSTATUS NTAPI NtTerminateProcess(
    HANDLE ProcessHandle,
    NTSTATUS ExitStatus
);

static void print_text(const char *text)
{
    WCHAR wide[160];
    UNICODE_STRING message;

    while (*text != '\0') {
        USHORT count = 0;
        while (*text != '\0' && count < 158) {
            wide[count++] = (WCHAR)(unsigned char)*text++;
        }
        wide[count] = L'\0';
        RtlInitUnicodeString(&message, wide);
        NtDisplayString(&message);
    }
}

static void print_hex(ULONG_PTR value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19] = "0x0000000000000000";

    for (int index = 17; index >= 2; --index) {
        text[index] = digits[value & 0xf];
        value >>= 4;
    }
    print_text(text);
}

VOID NTAPI NtProcessStartup(PVOID PebArgument)
{
    PPEB peb = (PPEB)PebArgument;
    WIN0DOOM_PROCESS_PARAMETERS_HEAD *parameters = NULL;
    IO_STATUS_BLOCK io_status;
    UCHAR buffer[64];
    NTSTATUS status;

    if (peb != NULL) {
        parameters = (WIN0DOOM_PROCESS_PARAMETERS_HEAD *)
            peb->ProcessParameters;
    }

    print_text("\r\nwin0 input probe\r\npeb: ");
    print_hex((ULONG_PTR)peb);
    print_text("\r\nprocess parameters: ");
    print_hex((ULONG_PTR)parameters);
    if (parameters == NULL) {
        print_text("\r\nno process parameters.\r\n");
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, STATUS_INVALID_PARAMETER);
    }

    print_text("\r\nconsole: ");
    print_hex((ULONG_PTR)parameters->ConsoleHandle);
    print_text("\r\nstdin:   ");
    print_hex((ULONG_PTR)parameters->StandardInput);
    print_text("\r\nstdout:  ");
    print_hex((ULONG_PTR)parameters->StandardOutput);
    print_text("\r\nstderr:  ");
    print_hex((ULONG_PTR)parameters->StandardError);
    print_text("\r\n\r\nwaiting for one ntreadfile on stdin; press a key...\r\n");

    status = NtReadFile(
        parameters->StandardInput,
        NULL,
        NULL,
        NULL,
        &io_status,
        buffer,
        sizeof(buffer),
        NULL,
        NULL
    );
    print_text("ntreadfile returned ");
    print_hex((ULONG)status);
    print_text("; bytes: ");
    print_hex(io_status.Information);
    print_text("\r\n");
    NtTerminateProcess((HANDLE)(LONG_PTR)-1, status);
}
