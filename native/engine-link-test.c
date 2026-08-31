typedef unsigned long long SIZE_T;

#define NTAPI __stdcall
#define NTDLL_IMPORT __declspec(dllimport)
#define CURRENT_PROCESS ((void *)(long long)-1)

typedef long NTSTATUS;

NTDLL_IMPORT NTSTATUS NTAPI NtTerminateProcess(
    void *process_handle,
    NTSTATUS exit_status
);

static void test_print(const char *text) { (void)text; }
static void *test_malloc(int size) { (void)size; return (void *)0; }
static void test_free(void *ptr) { (void)ptr; }
static void *test_open(const char *filename, const char *mode)
{
    (void)filename;
    (void)mode;
    return (void *)0;
}
static void test_close(void *handle) { (void)handle; }
static int test_read(void *handle, void *buffer, int count)
{
    (void)handle;
    (void)buffer;
    (void)count;
    return 0;
}
static int test_write(void *handle, const void *buffer, int count)
{
    (void)handle;
    (void)buffer;
    (void)count;
    return 0;
}
static int test_seek(void *handle, int offset, doom_seek_t origin);
static int test_tell(void *handle) { (void)handle; return 0; }
static int test_eof(void *handle) { (void)handle; return 1; }
static void test_gettime(int *seconds, int *microseconds)
{
    *seconds = 0;
    *microseconds = 0;
}
static void test_exit(int code)
{
    NtTerminateProcess(CURRENT_PROCESS, code);
}
static char *test_getenv(const char *name) { (void)name; return (char *)0; }

#define DOOM_IMPLEMENTATION
#include "../PureDOOM/PureDOOM.h"

static int test_seek(void *handle, int offset, doom_seek_t origin)
{
    (void)handle;
    (void)offset;
    (void)origin;
    return -1;
}

void NTAPI NtProcessStartup(void *peb)
{
    char *argv[] = { "win0doom.exe", "-iwad", "doom1.wad" };
    (void)peb;

    doom_set_print(test_print);
    doom_set_malloc(test_malloc, test_free);
    doom_set_file_io(
        test_open,
        test_close,
        test_read,
        test_write,
        test_seek,
        test_tell,
        test_eof
    );
    doom_set_gettime(test_gettime);
    doom_set_exit(test_exit);
    doom_set_getenv(test_getenv);
    doom_init(3, argv, 0);
    NtTerminateProcess(CURRENT_PROCESS, 0);
}
