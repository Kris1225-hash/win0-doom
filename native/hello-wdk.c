#include <Windows.h>
#include <winternl.h>

NTSYSCALLAPI NTSTATUS NTAPI NtDisplayString(
    PUNICODE_STRING String
);

NTSYSCALLAPI NTSTATUS NTAPI NtTerminateProcess(
    HANDLE ProcessHandle,
    NTSTATUS ExitStatus
);

VOID NTAPI NtProcessStartup(PVOID StartupArgument)
{
    UNICODE_STRING message;

    UNREFERENCED_PARAMETER(StartupArgument);

    RtlInitUnicodeString(
        &message,
        L"\r\nhello from the real windows 26100 wdk toolchain.\r\n"
        L"if you can read this, the win0 loader finally accepted our exe.\r\n\r\n"
    );
    NtDisplayString(&message);
    NtTerminateProcess((HANDLE)(LONG_PTR)-1, (NTSTATUS)0);
}
