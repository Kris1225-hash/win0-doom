typedef long NTSTATUS;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING;

#define NTAPI __stdcall
#define NTDLL_IMPORT __declspec(dllimport)
#define CURRENT_PROCESS ((void *)(long long)-1)

NTDLL_IMPORT void NTAPI RtlInitUnicodeString(
    UNICODE_STRING *destination,
    const WCHAR *source
);

NTDLL_IMPORT NTSTATUS NTAPI NtDisplayString(
    const UNICODE_STRING *string
);

NTDLL_IMPORT NTSTATUS NTAPI NtDelayExecution(
    unsigned char alertable,
    const long long *delay_interval
);

NTDLL_IMPORT NTSTATUS NTAPI NtTerminateProcess(
    void *process_handle,
    NTSTATUS exit_status
);

void NTAPI NtProcessStartup(void *peb)
{
    UNICODE_STRING message;
    const long long one_second = -10000000LL;
    (void)peb;

    RtlInitUnicodeString(
        &message,
        L"\r\nhello from a custom win0 native executable.\r\n"
        L"ntdll is enough. doom is next.\r\n"
        L"\r\nthis process is now the runlevel-0 shell.\r\n"
        L"it will stay alive until the vm is powered off.\r\n\r\n"
    );
    NtDisplayString(&message);

    for (;;) {
        NtDelayExecution(0, &one_second);
    }
}
