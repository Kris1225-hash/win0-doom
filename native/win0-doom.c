#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

/* Tiny CRT surface required by clang's optimizer and PureDOOM. */
int _fltused = 0;

void *memset(void *destination, int value, size_t length)
{
    unsigned char *bytes = (unsigned char *)destination;
    while (length-- != 0) {
        *bytes++ = (unsigned char)value;
    }
    return destination;
}

void *memcpy(void *destination, const void *source, size_t length)
{
    unsigned char *output = (unsigned char *)destination;
    const unsigned char *input = (const unsigned char *)source;
    while (length-- != 0) {
        *output++ = *input++;
    }
    return destination;
}

size_t strlen(const char *text)
{
    const char *end = text;
    while (*end != '\0') {
        ++end;
    }
    return (size_t)(end - text);
}

#define DOOM_IMPLEMENTATION
#include "../PureDOOM/PureDOOM.h"

#define IOCTL_WIN0DOOM_PRESENT 0x0022a000UL
#define FILE_STANDARD_INFORMATION_CLASS 5

typedef struct _WIN0DOOM_FILE_STANDARD_INFORMATION {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} WIN0DOOM_FILE_STANDARD_INFORMATION;

typedef struct _WIN0DOOM_FILE {
    HANDLE Handle;
    LONGLONG Position;
    LONGLONG Length;
} WIN0DOOM_FILE;

NTSYSCALLAPI NTSTATUS NTAPI NtAllocateVirtualMemory(
    HANDLE ProcessHandle,
    PVOID *BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect
);
NTSYSCALLAPI NTSTATUS NTAPI NtFreeVirtualMemory(
    HANDLE ProcessHandle,
    PVOID *BaseAddress,
    PSIZE_T RegionSize,
    ULONG FreeType
);
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
NTSYSCALLAPI NTSTATUS NTAPI NtWriteFile(
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
NTSYSCALLAPI NTSTATUS NTAPI NtQueryInformationFile(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass
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

static HANDLE display_handle;

static void native_print(const char *text)
{
    WCHAR wide[240];
    UNICODE_STRING message;

    while (*text != '\0') {
        USHORT count = 0;
        while (*text != '\0' && count < 238) {
            wide[count++] = (WCHAR)(unsigned char)*text++;
        }
        wide[count] = L'\0';
        RtlInitUnicodeString(&message, wide);
        NtDisplayString(&message);
    }
}

static void *native_malloc(int requested_size)
{
    PVOID base = NULL;
    SIZE_T size;
    NTSTATUS status;

    if (requested_size <= 0) {
        return NULL;
    }
    size = (SIZE_T)requested_size;
    status = NtAllocateVirtualMemory(
        (HANDLE)(LONG_PTR)-1,
        &base,
        0,
        &size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );
    return status < 0 ? NULL : base;
}

static void native_free(void *pointer)
{
    PVOID base = pointer;
    SIZE_T size = 0;

    if (base != NULL) {
        NtFreeVirtualMemory((HANDLE)(LONG_PTR)-1, &base, &size, MEM_RELEASE);
    }
}

static BOOLEAN mode_has(const char *mode, char wanted)
{
    while (*mode != '\0') {
        if (*mode++ == wanted) {
            return TRUE;
        }
    }
    return FALSE;
}

static void ascii_path_to_native(const char *filename, WCHAR *path, ULONG capacity)
{
    static const WCHAR prefix[] = L"\\SystemRoot\\System32\\";
    ULONG index = 0;

    if (filename[0] != '\\') {
        for (ULONG prefix_index = 0;
             prefix[prefix_index] != L'\0' && index + 1 < capacity;
             ++prefix_index) {
            path[index++] = prefix[prefix_index];
        }
    }
    while (*filename != '\0' && index + 1 < capacity) {
        char character = *filename++;
        path[index++] = (WCHAR)(character == '/' ? '\\' : (unsigned char)character);
    }
    path[index] = L'\0';
}

static void *native_open(const char *filename, const char *mode)
{
    WCHAR path[320];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io_status;
    WIN0DOOM_FILE_STANDARD_INFORMATION standard_information;
    WIN0DOOM_FILE *file;
    ULONG disposition;
    ACCESS_MASK access;
    NTSTATUS status;

    file = (WIN0DOOM_FILE *)native_malloc(sizeof(*file));
    if (file == NULL) {
        return NULL;
    }

    ascii_path_to_native(filename, path, ARRAYSIZE(path));
    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(
        &attributes,
        &name,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );

    access = SYNCHRONIZE;
    if (mode_has(mode, 'r')) {
        access |= GENERIC_READ;
    }
    if (mode_has(mode, 'w') || mode_has(mode, 'a') || mode_has(mode, '+')) {
        access |= GENERIC_WRITE | GENERIC_READ;
    }
    disposition = mode_has(mode, 'w') ? FILE_OVERWRITE_IF :
        (mode_has(mode, 'a') ? FILE_OPEN_IF : FILE_OPEN);

    status = NtCreateFile(
        &file->Handle,
        access,
        &attributes,
        &io_status,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        disposition,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL,
        0
    );
    if (status < 0) {
        native_free(file);
        return NULL;
    }

    file->Position = 0;
    file->Length = 0;
    status = NtQueryInformationFile(
        file->Handle,
        &io_status,
        &standard_information,
        sizeof(standard_information),
        (FILE_INFORMATION_CLASS)FILE_STANDARD_INFORMATION_CLASS
    );
    if (status >= 0) {
        file->Length = standard_information.EndOfFile.QuadPart;
    }
    if (mode_has(mode, 'a')) {
        file->Position = file->Length;
    }
    return file;
}

static void native_close(void *handle)
{
    WIN0DOOM_FILE *file = (WIN0DOOM_FILE *)handle;
    if (file != NULL) {
        NtClose(file->Handle);
        native_free(file);
    }
}

static int native_read(void *handle, void *buffer, int count)
{
    WIN0DOOM_FILE *file = (WIN0DOOM_FILE *)handle;
    IO_STATUS_BLOCK io_status;
    LARGE_INTEGER offset;
    NTSTATUS status;

    if (file == NULL || count <= 0) {
        return 0;
    }
    offset.QuadPart = file->Position;
    status = NtReadFile(
        file->Handle,
        NULL,
        NULL,
        NULL,
        &io_status,
        buffer,
        (ULONG)count,
        &offset,
        NULL
    );
    if (status < 0) {
        return 0;
    }
    file->Position += (LONGLONG)io_status.Information;
    return (int)io_status.Information;
}

static int native_write(void *handle, const void *buffer, int count)
{
    WIN0DOOM_FILE *file = (WIN0DOOM_FILE *)handle;
    IO_STATUS_BLOCK io_status;
    LARGE_INTEGER offset;
    NTSTATUS status;

    if (file == NULL || count <= 0) {
        return 0;
    }
    offset.QuadPart = file->Position;
    status = NtWriteFile(
        file->Handle,
        NULL,
        NULL,
        NULL,
        &io_status,
        (PVOID)buffer,
        (ULONG)count,
        &offset,
        NULL
    );
    if (status < 0) {
        return 0;
    }
    file->Position += (LONGLONG)io_status.Information;
    if (file->Position > file->Length) {
        file->Length = file->Position;
    }
    return (int)io_status.Information;
}

static int native_seek(void *handle, int offset, doom_seek_t origin)
{
    WIN0DOOM_FILE *file = (WIN0DOOM_FILE *)handle;
    LONGLONG position;

    if (file == NULL) {
        return -1;
    }
    position = (origin == DOOM_SEEK_SET) ? 0 :
        (origin == DOOM_SEEK_CUR ? file->Position : file->Length);
    position += offset;
    if (position < 0) {
        return -1;
    }
    file->Position = position;
    return 0;
}

static int native_tell(void *handle)
{
    WIN0DOOM_FILE *file = (WIN0DOOM_FILE *)handle;
    return file == NULL ? -1 : (int)file->Position;
}

static int native_eof(void *handle)
{
    WIN0DOOM_FILE *file = (WIN0DOOM_FILE *)handle;
    return file == NULL || file->Position >= file->Length;
}

static void native_gettime(int *seconds, int *microseconds)
{
    LARGE_INTEGER system_time;
    ULONGLONG ticks;

    NtQuerySystemTime(&system_time);
    ticks = (ULONGLONG)system_time.QuadPart;
    *seconds = (int)(ticks / 10000000ULL);
    *microseconds = (int)((ticks % 10000000ULL) / 10ULL);
}

static void native_exit(int code)
{
    NtTerminateProcess((HANDLE)(LONG_PTR)-1, (NTSTATUS)code);
}

static char *native_getenv(const char *name)
{
    UNREFERENCED_PARAMETER(name);
    return NULL;
}

static NTSTATUS open_display(void)
{
    UNICODE_STRING device_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io_status;

    RtlInitUnicodeString(&device_name, L"\\Device\\Win0DoomDisplay");
    InitializeObjectAttributes(
        &attributes,
        &device_name,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );
    return NtCreateFile(
        &display_handle,
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
}

static NTSTATUS present_frame(void)
{
    IO_STATUS_BLOCK io_status;
    const unsigned char *frame = doom_get_framebuffer(4);

    return NtDeviceIoControlFile(
        display_handle,
        NULL,
        NULL,
        NULL,
        &io_status,
        IOCTL_WIN0DOOM_PRESENT,
        (PVOID)frame,
        320 * 200 * 4,
        NULL,
        0
    );
}

VOID NTAPI NtProcessStartup(PVOID StartupArgument)
{
    char *arguments[] = {
        "win0doom.exe",
        "-iwad",
        "doom1.wad"
    };
    LARGE_INTEGER frame_delay;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(StartupArgument);

    status = open_display();
    if (status < 0) {
        native_print("\r\nwin0 doom: display driver is unavailable.\r\n");
        native_exit((int)status);
    }

    doom_set_print(native_print);
    doom_set_malloc(native_malloc, native_free);
    doom_set_file_io(
        native_open,
        native_close,
        native_read,
        native_write,
        native_seek,
        native_tell,
        native_eof
    );
    doom_set_gettime(native_gettime);
    doom_set_exit(native_exit);
    doom_set_getenv(native_getenv);
    doom_set_default_int("key_up", DOOM_KEY_W);
    doom_set_default_int("key_down", DOOM_KEY_S);
    doom_set_default_int("key_strafeleft", DOOM_KEY_A);
    doom_set_default_int("key_straferight", DOOM_KEY_D);
    doom_set_default_int("key_use", DOOM_KEY_E);

    native_print("\r\nwin0 doom: initializing puredoom...\r\n");
    doom_init(
        ARRAYSIZE(arguments),
        arguments,
        DOOM_FLAG_HIDE_MOUSE_OPTIONS |
        DOOM_FLAG_HIDE_SOUND_OPTIONS |
        DOOM_FLAG_HIDE_MUSIC_OPTIONS
    );

    frame_delay.QuadPart = -100000LL;
    for (;;) {
        doom_update();
        status = present_frame();
        if (status < 0) {
            native_print("\r\nwin0 doom: present failed.\r\n");
            native_exit((int)status);
        }
        NtDelayExecution(FALSE, &frame_delay);
    }
}
