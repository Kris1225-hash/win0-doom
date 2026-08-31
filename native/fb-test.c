#include <Windows.h>
#include <winternl.h>

typedef enum _SECTION_INHERIT {
    ViewShare = 1,
    ViewUnmap = 2
} SECTION_INHERIT;

NTSYSCALLAPI NTSTATUS NTAPI NtOpenSection(
    PHANDLE SectionHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes
);

NTSYSCALLAPI NTSTATUS NTAPI NtMapViewOfSection(
    HANDLE SectionHandle,
    HANDLE ProcessHandle,
    PVOID *BaseAddress,
    ULONG_PTR ZeroBits,
    SIZE_T CommitSize,
    PLARGE_INTEGER SectionOffset,
    PSIZE_T ViewSize,
    SECTION_INHERIT InheritDisposition,
    ULONG AllocationType,
    ULONG Win32Protect
);

NTSYSCALLAPI NTSTATUS NTAPI NtDisplayString(PUNICODE_STRING String);
NTSYSCALLAPI NTSTATUS NTAPI NtDelayExecution(
    BOOLEAN Alertable,
    PLARGE_INTEGER DelayInterval
);
NTSYSCALLAPI NTSTATUS NTAPI NtTerminateProcess(
    HANDLE ProcessHandle,
    NTSTATUS ExitStatus
);
NTSYSAPI NTSTATUS NTAPI RtlAdjustPrivilege(
    ULONG Privilege,
    BOOLEAN Enable,
    BOOLEAN Client,
    PBOOLEAN WasEnabled
);

static void print_status(PCWSTR prefix, NTSTATUS status)
{
    WCHAR buffer[96];
    UNICODE_STRING output;
    ULONG value = (ULONG)status;
    ULONG prefix_length = 0;
    ULONG index;

    while (prefix[prefix_length] != L'\0') {
        buffer[prefix_length] = prefix[prefix_length];
        ++prefix_length;
    }

    index = prefix_length;
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
    static const ULONG stride_pixels = 1280;
    UNICODE_STRING physical_memory_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK unused_io_status;
    LARGE_INTEGER physical_offset;
    LARGE_INTEGER delay;
    HANDLE physical_memory = NULL;
    PVOID mapping = NULL;
    SIZE_T mapping_size = 16 * 1024 * 1024;
    NTSTATUS status;
    BOOLEAN was_enabled;

    UNREFERENCED_PARAMETER(StartupArgument);
    UNREFERENCED_PARAMETER(unused_io_status);

    RtlAdjustPrivilege(20, TRUE, FALSE, &was_enabled); /* SeDebugPrivilege */
    RtlAdjustPrivilege(4, TRUE, FALSE, &was_enabled);  /* SeLockMemoryPrivilege */

    RtlInitUnicodeString(&physical_memory_name, L"\\Device\\PhysicalMemory");
    InitializeObjectAttributes(
        &attributes,
        &physical_memory_name,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );

    status = NtOpenSection(
        &physical_memory,
        SECTION_MAP_READ | SECTION_MAP_WRITE,
        &attributes
    );
    if (status < 0) {
        print_status(L"\r\nNtOpenSection failed: ", status);
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, status);
    }

    physical_offset.QuadPart = 0x80000000ULL;
    status = NtMapViewOfSection(
        physical_memory,
        (HANDLE)(LONG_PTR)-1,
        &mapping,
        0,
        0,
        &physical_offset,
        &mapping_size,
        ViewUnmap,
        0,
        PAGE_READWRITE | PAGE_NOCACHE
    );
    if (status < 0) {
        print_status(L"\r\nNtMapViewOfSection failed: ", status);
        NtTerminateProcess((HANDLE)(LONG_PTR)-1, status);
    }

    for (ULONG y = 0; y < height; ++y) {
        volatile ULONG *row = (volatile ULONG *)mapping + y * stride_pixels;
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
