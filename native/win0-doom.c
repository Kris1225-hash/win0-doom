#define NOMINMAX
#include <Windows.h>
#include <winternl.h>
#include <ntddkbd.h>

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
#define IOCTL_WIN0DOOM_GET_KEYBOARD 0x00222004UL
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
static KEYBOARD_INPUT_DATA keyboard_buffer[32];
static BOOLEAN keyboard_available = TRUE;
static LONG data_prefix_index = -1;

static const WCHAR *data_prefixes[] = {
    L"\\GLOBAL??\\C:\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume1\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume2\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume3\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume4\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume5\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume6\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume7\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume8\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume9\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume10\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume11\\Windows\\System32\\",
    L"\\Device\\HarddiskVolume12\\Windows\\System32\\"
};

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

static void native_print_hex(ULONG value)
{
    static const char digits[] = "0123456789abcdef";
    char text[11] = "0x00000000";

    for (int index = 9; index >= 2; --index) {
        text[index] = digits[value & 0xf];
        value >>= 4;
    }
    native_print(text);
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

static void ascii_path_to_native(
    const char *filename,
    const WCHAR *prefix,
    WCHAR *path,
    ULONG capacity
)
{
    ULONG index = 0;

    while (filename[0] == '.' &&
           (filename[1] == '/' || filename[1] == '\\')) {
        filename += 2;
    }
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
    LONG first_prefix;
    LONG final_prefix;

    file = (WIN0DOOM_FILE *)native_malloc(sizeof(*file));
    if (file == NULL) {
        return NULL;
    }

    access = SYNCHRONIZE;
    if (mode_has(mode, 'r')) {
        access |= GENERIC_READ;
    }
    if (mode_has(mode, 'w') || mode_has(mode, 'a') || mode_has(mode, '+')) {
        access |= GENERIC_WRITE | GENERIC_READ;
    }
    disposition = mode_has(mode, 'w') ? FILE_OVERWRITE_IF :
        (mode_has(mode, 'a') ? FILE_OPEN_IF : FILE_OPEN);

    first_prefix = data_prefix_index >= 0 ? data_prefix_index : 0;
    final_prefix = data_prefix_index >= 0 ? data_prefix_index + 1 :
        (LONG)ARRAYSIZE(data_prefixes);
    status = (NTSTATUS)0xc000003aL;
    for (LONG prefix_index = first_prefix;
         prefix_index < final_prefix;
         ++prefix_index) {
        ascii_path_to_native(
            filename,
            data_prefixes[prefix_index],
            path,
            ARRAYSIZE(path)
        );
        RtlInitUnicodeString(&name, path);
        InitializeObjectAttributes(
            &attributes,
            &name,
            OBJ_CASE_INSENSITIVE,
            NULL,
            NULL
        );
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
        if (status >= 0) {
            data_prefix_index = prefix_index;
            break;
        }
    }
    if (status < 0) {
        native_print("\r\nwin0 doom: open failed for '");
        native_print(filename);
        native_print("' with status ");
        native_print_hex((ULONG)status);
        native_print(".\r\n");
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

static doom_key_t scan_code_to_doom(USHORT make_code, USHORT flags)
{
    if ((flags & KEY_E0) != 0) {
        switch (make_code) {
        case 0x48: return DOOM_KEY_UP_ARROW;
        case 0x4b: return DOOM_KEY_LEFT_ARROW;
        case 0x4d: return DOOM_KEY_RIGHT_ARROW;
        case 0x50: return DOOM_KEY_DOWN_ARROW;
        case 0x1d: return DOOM_KEY_CTRL;
        case 0x38: return DOOM_KEY_ALT;
        default: return DOOM_KEY_UNKNOWN;
        }
    }

    switch (make_code) {
    case 0x01: return DOOM_KEY_ESCAPE;
    case 0x02: return DOOM_KEY_1;
    case 0x03: return DOOM_KEY_2;
    case 0x04: return DOOM_KEY_3;
    case 0x05: return DOOM_KEY_4;
    case 0x06: return DOOM_KEY_5;
    case 0x07: return DOOM_KEY_6;
    case 0x08: return DOOM_KEY_7;
    case 0x09: return DOOM_KEY_8;
    case 0x0a: return DOOM_KEY_9;
    case 0x0b: return DOOM_KEY_0;
    case 0x0c: return DOOM_KEY_MINUS;
    case 0x0d: return DOOM_KEY_EQUALS;
    case 0x0e: return DOOM_KEY_BACKSPACE;
    case 0x0f: return DOOM_KEY_TAB;
    case 0x10: return DOOM_KEY_Q;
    case 0x11: return DOOM_KEY_W;
    case 0x12: return DOOM_KEY_E;
    case 0x13: return DOOM_KEY_R;
    case 0x14: return DOOM_KEY_T;
    case 0x15: return DOOM_KEY_Y;
    case 0x16: return DOOM_KEY_U;
    case 0x17: return DOOM_KEY_I;
    case 0x18: return DOOM_KEY_O;
    case 0x19: return DOOM_KEY_P;
    case 0x1a: return DOOM_KEY_LEFT_BRACKET;
    case 0x1b: return DOOM_KEY_RIGHT_BRACKET;
    case 0x1c: return DOOM_KEY_ENTER;
    case 0x1d: return DOOM_KEY_CTRL;
    case 0x1e: return DOOM_KEY_A;
    case 0x1f: return DOOM_KEY_S;
    case 0x20: return DOOM_KEY_D;
    case 0x21: return DOOM_KEY_F;
    case 0x22: return DOOM_KEY_G;
    case 0x23: return DOOM_KEY_H;
    case 0x24: return DOOM_KEY_J;
    case 0x25: return DOOM_KEY_K;
    case 0x26: return DOOM_KEY_L;
    case 0x27: return DOOM_KEY_SEMICOLON;
    case 0x28: return DOOM_KEY_APOSTROPHE;
    case 0x2a:
    case 0x36: return DOOM_KEY_SHIFT;
    case 0x2c: return DOOM_KEY_Z;
    case 0x2d: return DOOM_KEY_X;
    case 0x2e: return DOOM_KEY_C;
    case 0x2f: return DOOM_KEY_V;
    case 0x30: return DOOM_KEY_B;
    case 0x31: return DOOM_KEY_N;
    case 0x32: return DOOM_KEY_M;
    case 0x33: return DOOM_KEY_COMMA;
    case 0x34: return DOOM_KEY_PERIOD;
    case 0x35: return DOOM_KEY_SLASH;
    case 0x37: return DOOM_KEY_MULTIPLY;
    case 0x38: return DOOM_KEY_ALT;
    case 0x39: return DOOM_KEY_SPACE;
    case 0x3b: return DOOM_KEY_F1;
    case 0x3c: return DOOM_KEY_F2;
    case 0x3d: return DOOM_KEY_F3;
    case 0x3e: return DOOM_KEY_F4;
    case 0x3f: return DOOM_KEY_F5;
    case 0x40: return DOOM_KEY_F6;
    case 0x41: return DOOM_KEY_F7;
    case 0x42: return DOOM_KEY_F8;
    case 0x43: return DOOM_KEY_F9;
    default: return DOOM_KEY_UNKNOWN;
    }
}

static void dispatch_keyboard_records(
    const KEYBOARD_INPUT_DATA *records,
    ULONG byte_count
)
{
    ULONG record_count = byte_count / sizeof(records[0]);

    for (ULONG index = 0; index < record_count; ++index) {
        doom_key_t key = scan_code_to_doom(
            records[index].MakeCode,
            records[index].Flags
        );
        if (key == DOOM_KEY_UNKNOWN) {
            continue;
        }
        if ((records[index].Flags & KEY_BREAK) != 0) {
            doom_key_up(key);
        } else {
            doom_key_down(key);
        }
    }
}

static void poll_keyboard(void)
{
    IO_STATUS_BLOCK io_status;
    NTSTATUS status;

    if (!keyboard_available) {
        return;
    }
    status = NtDeviceIoControlFile(
        display_handle,
        NULL,
        NULL,
        NULL,
        &io_status,
        IOCTL_WIN0DOOM_GET_KEYBOARD,
        NULL,
        0,
        keyboard_buffer,
        sizeof(keyboard_buffer)
    );
    if (status < 0) {
        LARGE_INTEGER diagnostic_delay;

        native_print("\r\nwin0 doom: keyboard ioctl failed with status ");
        native_print_hex((ULONG)status);
        native_print("; demo mode only.\r\n");
        keyboard_available = FALSE;
        diagnostic_delay.QuadPart = -50000000LL;
        NtDelayExecution(FALSE, &diagnostic_delay);
        return;
    }
    if (io_status.Information != 0) {
        dispatch_keyboard_records(
            keyboard_buffer,
            (ULONG)io_status.Information
        );
    }
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
        poll_keyboard();
        doom_update();
        status = present_frame();
        if (status < 0) {
            native_print("\r\nwin0 doom: present failed.\r\n");
            native_exit((int)status);
        }
        NtDelayExecution(FALSE, &frame_delay);
    }
}
