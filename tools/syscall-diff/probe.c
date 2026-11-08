/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Calls the public ntdll names and prints the status each one returns.
 * Argument counts are the stdcall arities of those documented functions.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef UINT_PTR SLOT;

#define CURRENT_PROCESS ((SLOT)(INT_PTR)-1)
#define FILE_SHARE_BOTH (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define FILE_OPTIONS (FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)
#define FILE_ACCESS (SYNCHRONIZE | FILE_READ_DATA | FILE_WRITE_DATA | \
    FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES)
#define SECTION_MAP_BOTH (SECTION_MAP_READ | SECTION_MAP_WRITE)

typedef struct
{
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t *Buffer;
} USTR;

typedef struct
{
    unsigned long Length;
    void *RootDirectory;
    USTR *ObjectName;
    unsigned long Attributes;
    void *SecurityDescriptor;
    void *SecurityQualityOfService;
} OBJATTR;

typedef struct
{
    union
    {
        long Status;
        void *Pointer;
    } u;
    SLOT Information;
} IO_STATUS_BLOCK;

typedef struct
{
    const char *Name;
    int Argc;
    int Timeout;
} CALL;

typedef long (__stdcall *FN0)(void);
typedef long (__stdcall *FN1)(SLOT);
typedef long (__stdcall *FN2)(SLOT, SLOT);
typedef long (__stdcall *FN3)(SLOT, SLOT, SLOT);
typedef long (__stdcall *FN4)(SLOT, SLOT, SLOT, SLOT);
typedef long (__stdcall *FN5)(SLOT, SLOT, SLOT, SLOT, SLOT);
typedef long (__stdcall *FN6)(SLOT, SLOT, SLOT, SLOT, SLOT, SLOT);
typedef long (__stdcall *FN7)(SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT);
typedef long (__stdcall *FN8)(SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT);
typedef long (__stdcall *FN9)(SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT);
typedef long (__stdcall *FN10)(SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT);
typedef long (__stdcall *FN11)(SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT, SLOT);

static const CALL g_calls[] =
{
    { "NtClose", 1, -1 },
    { "NtCreateFile", 11, -1 },
    { "NtReadFile", 9, -1 },
    { "NtWriteFile", 9, -1 },
    { "NtDeviceIoControlFile", 10, -1 },
    { "NtCreateUserProcess", 11, -1 },
    { "NtCreateThreadEx", 11, -1 },
    { "NtAllocateVirtualMemory", 6, -1 },
    { "NtFreeVirtualMemory", 4, -1 },
    { "NtMapViewOfSection", 10, -1 },
    { "NtUnmapViewOfSection", 2, -1 },
    { "NtQueryInformationProcess", 5, -1 },
    { "NtQueryInformationThread", 5, -1 },
    { "NtSetInformationProcess", 4, -1 },
    { "NtSetInformationThread", 4, -1 },
    { "NtOpenProcess", 4, -1 },
    { "NtOpenThread", 4, -1 },
    { "NtTerminateProcess", 2, -1 },
    { "NtTerminateThread", 2, -1 },
    { "NtCreateProcess", 8, -1 },
    { "NtCreateProcessEx", 9, -1 },
    { "NtCreateThread", 8, -1 },
    { "NtResumeThread", 2, -1 },
    { "NtSuspendThread", 2, -1 },
    { "NtWaitForSingleObject", 3, 2 },
    { "NtWaitForMultipleObjects", 5, 4 },
    { "NtDelayExecution", 2, 1 },
    { "NtQuerySystemInformation", 4, -1 },
    { "NtSetSystemInformation", 3, -1 },
    { "NtOpenFile", 6, -1 },
    { "NtQueryInformationFile", 5, -1 },
    { "NtSetInformationFile", 5, -1 },
    { "NtQueryDirectoryFile", 11, -1 },
    { "NtQueryAttributesFile", 2, -1 },
    { "NtQueryFullAttributesFile", 2, -1 },
    { "NtFlushBuffersFile", 2, -1 },
    { "NtCreateSection", 7, -1 },
    { "NtOpenSection", 3, -1 },
    { "NtQuerySection", 5, -1 },
    { "NtProtectVirtualMemory", 5, -1 },
    { "NtQueryVirtualMemory", 6, -1 },
    { "NtReadVirtualMemory", 5, -1 },
    { "NtWriteVirtualMemory", 5, -1 },
    { "NtCreateEvent", 5, -1 },
    { "NtOpenEvent", 3, -1 },
    { "NtSetEvent", 2, -1 },
    { "NtResetEvent", 2, -1 },
    { "NtCreateMutant", 4, -1 },
    { "NtOpenMutant", 3, -1 },
    { "NtReleaseMutant", 2, -1 },
    { "NtCreateSemaphore", 5, -1 },
    { "NtOpenSemaphore", 3, -1 },
    { "NtReleaseSemaphore", 3, -1 },
    { "NtCreateTimer", 4, -1 },
    { "NtOpenTimer", 3, -1 },
    { "NtSetTimer", 7, 1 },
    { "NtCancelTimer", 2, -1 },
    { "NtCreateKey", 7, -1 },
    { "NtOpenKey", 3, -1 },
    { "NtDeleteKey", 1, -1 },
    { "NtEnumerateKey", 6, -1 },
    { "NtEnumerateValueKey", 6, -1 },
    { "NtQueryKey", 5, -1 },
    { "NtQueryValueKey", 6, -1 },
    { "NtSetValueKey", 6, -1 },
    { "NtDeleteValueKey", 2, -1 },
    { "NtFlushKey", 1, -1 },
    { "NtQueryVolumeInformationFile", 5, -1 },
    { "NtQueryObject", 5, -1 },
    { "NtDuplicateObject", 7, -1 },
    { "NtCreateSymbolicLinkObject", 4, -1 },
    { "NtOpenSymbolicLinkObject", 3, -1 },
    { "NtQuerySymbolicLinkObject", 3, -1 },
    { "NtFsControlFile", 10, -1 },
    { "NtNotifyChangeDirectoryFile", 9, -1 },
    { "NtLockFile", 10, -1 },
    { "NtUnlockFile", 5, -1 },
    { "NtCancelIoFile", 2, -1 },
    { "NtYieldExecution", 0, -1 },
    { "NtContinue", 2, -1 },
    { "NtRaiseException", 3, -1 },
    { "NtOpenProcessToken", 3, -1 },
    { "NtOpenThreadToken", 4, -1 },
    { "NtAdjustPrivilegesToken", 6, -1 },
    { "NtQueryInformationToken", 5, -1 },
    { "NtCreateDirectoryObject", 3, -1 },
    { "NtOpenDirectoryObject", 3, -1 },
    { "NtCreatePort", 5, -1 },
    { "NtConnectPort", 8, -1 },
    { "NtRequestWaitReplyPort", 3, -1 },
    { "NtReplyWaitReceivePort", 4, -1 },
    { "NtCreateIoCompletion", 4, -1 },
    { "NtOpenIoCompletion", 3, -1 },
    { "NtSetIoCompletion", 5, -1 },
    { "NtRemoveIoCompletion", 5, 4 },
    { "NtPowerInformation", 5, -1 },
    { "NtInitiatePowerAction", 4, -1 },
    { "NtAlpcConnectPort", 11, -1 },
    { "NtAlpcSendWaitReceivePort", 8, -1 },
    { "NtCreateWorkerFactory", 10, -1 },
    { "NtWaitForWorkViaWorkerFactory", 5, -1 }
};

static HANDLE g_com = INVALID_HANDLE_VALUE;
static HANDLE g_com2 = INVALID_HANDLE_VALUE;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static jmp_buf g_jump;
static volatile int g_guard;
static void *g_guard_handle;
static const char *g_done[16];
static int g_done_count;
static unsigned long long g_zero;

static void
emit(const char *name, const char *status)
{
    char line[160];
    int n;
    DWORD wrote;

    n = _snprintf(line, sizeof(line), "DIFF %s %s\r\n", name, status);
    if (n < 0)
    {
        return;
    }
    if (g_com != INVALID_HANDLE_VALUE)
    {
        WriteFile(g_com, line, (DWORD)n, &wrote, NULL);
        FlushFileBuffers(g_com);
    }
    if (g_com2 != INVALID_HANDLE_VALUE)
    {
        WriteFile(g_com2, line, (DWORD)n, &wrote, NULL);
        FlushFileBuffers(g_com2);
    }
    if (g_file != INVALID_HANDLE_VALUE)
    {
        WriteFile(g_file, line, (DWORD)n, &wrote, NULL);
        FlushFileBuffers(g_file);
    }
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    /* The kernel debugger owns COM1 on a debug boot, so also send the
     * line through the debug print path that the serial log captures. */
    OutputDebugStringA(line);
}

static void
emit_code(const char *name, long status)
{
    char text[16];

    _snprintf(text, sizeof(text), "0x%08lX", (unsigned long)status);
    emit(name, text);
}

static LONG WINAPI
on_fault(EXCEPTION_POINTERS *info)
{
    (void)info;
    if (g_guard)
    {
        longjmp(g_jump, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static long
call_n(void *fn, int argc, SLOT *arg)
{
    switch (argc)
    {
        case 0:
            return ((FN0)fn)();
        case 1:
            return ((FN1)fn)(arg[0]);
        case 2:
            return ((FN2)fn)(arg[0], arg[1]);
        case 3:
            return ((FN3)fn)(arg[0], arg[1], arg[2]);
        case 4:
            return ((FN4)fn)(arg[0], arg[1], arg[2], arg[3]);
        case 5:
            return ((FN5)fn)(arg[0], arg[1], arg[2], arg[3], arg[4]);
        case 6:
            return ((FN6)fn)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5]);
        case 7:
            return ((FN7)fn)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6]);
        case 8:
            return ((FN8)fn)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7]);
        case 9:
            return ((FN9)fn)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7],
                arg[8]);
        case 10:
            return ((FN10)fn)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7],
                arg[8], arg[9]);
        default:
            return ((FN11)fn)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7],
                arg[8], arg[9], arg[10]);
    }
}

static int
guarded(void *fn, int argc, SLOT *arg, long *status)
{
    if (setjmp(g_jump) != 0)
    {
        g_guard = 0;
        return 0;
    }
    g_guard = 1;
    *status = call_n(fn, argc, arg);
    g_guard = 0;
    return 1;
}

static void *
lookup(const char *name)
{
    return (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
}

static const CALL *
find_call(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(g_calls) / sizeof(g_calls[0]); i++)
    {
        if (strcmp(g_calls[i].Name, name) == 0)
        {
            return &g_calls[i];
        }
    }
    return NULL;
}

static void
mark_done(const char *name)
{
    if (g_done_count < (int)(sizeof(g_done) / sizeof(g_done[0])))
    {
        g_done[g_done_count++] = name;
    }
}

static int
is_done(const char *name)
{
    int i;

    for (i = 0; i < g_done_count; i++)
    {
        if (strcmp(g_done[i], name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void
invoke(const char *name, int argc, int timeout, SLOT *arg)
{
    void *fn;
    long status;
    SLOT local[11];
    int i;

    emit(name, "STARTED");
    fn = lookup(name);
    if (fn == NULL)
    {
        emit(name, "MISSING");
        mark_done(name);
        return;
    }
    if (arg == NULL)
    {
        memset(local, 0, sizeof(local));
        if (timeout >= 0 && timeout < 11)
        {
            local[timeout] = (SLOT)&g_zero;
        }
        arg = local;
    }
    else if (timeout >= 0 && timeout < argc)
    {
        arg[timeout] = (SLOT)&g_zero;
    }
    for (i = argc; i < 11; i++)
    {
        arg[i] = 0;
    }
    if (!guarded(fn, argc, arg, &status))
    {
        emit(name, "CRASH");
        mark_done(name);
        return;
    }
    emit_code(name, status);
    mark_done(name);
}

static void
fill_nt_path(wchar_t *path, size_t cap)
{
    wchar_t temp[280];
    const wchar_t *tail = L"syscall-diff.bin";
    size_t used;
    DWORD n;

    if (cap < 8)
    {
        path[0] = 0;
        return;
    }
    path[0] = L'\\';
    path[1] = L'?';
    path[2] = L'?';
    path[3] = L'\\';
    used = 4;
    n = GetTempPathW((DWORD)(sizeof(temp) / sizeof(temp[0])) - 1, temp);
    if (n == 0)
    {
        const wchar_t *fallback = L"C:\\syscall-diff.bin";
        size_t i;
        for (i = 0; fallback[i] && used + 1 < cap; i++)
        {
            path[used++] = fallback[i];
        }
        path[used] = 0;
        return;
    }
    {
        size_t i;
        for (i = 0; temp[i] && used + 20 < cap; i++)
        {
            path[used++] = temp[i];
        }
        for (i = 0; tail[i] && used + 1 < cap; i++)
        {
            path[used++] = tail[i];
        }
        path[used] = 0;
    }
}

static void real_thread(void);
static void real_process(void);

static void
run_real(void)
{
    wchar_t path[520];
    USTR name;
    OBJATTR obj;
    IO_STATUS_BLOCK iosb;
    SLOT file = 0;
    SLOT section = 0;
    SLOT base = 0;
    SLOT size = 0;
    SLOT view = 0x1000;
    unsigned long long maximum = 0x1000;
    SLOT retlen = 0;
    unsigned char info[64];
    unsigned char bytes[4] = { 1, 2, 3, 4 };
    unsigned char back[4];
    unsigned long offset_slot[2];
    SLOT arg[11];
    const CALL *spec;

    memset(&iosb, 0, sizeof(iosb));
    memset(info, 0, sizeof(info));
    memset(back, 0, sizeof(back));
    fill_nt_path(path, sizeof(path) / sizeof(path[0]));
    name.Length = (unsigned short)(wcslen(path) * sizeof(wchar_t));
    name.MaximumLength = (unsigned short)(name.Length + sizeof(wchar_t));
    name.Buffer = path;
    memset(&obj, 0, sizeof(obj));
    obj.Length = sizeof(obj);
    obj.ObjectName = &name;
    obj.Attributes = 0x40UL;

    spec = find_call("NtCreateFile");
    memset(arg, 0, sizeof(arg));
    arg[0] = (SLOT)&file;
    arg[1] = FILE_ACCESS;
    arg[2] = (SLOT)&obj;
    arg[3] = (SLOT)&iosb;
    arg[5] = FILE_ATTRIBUTE_NORMAL;
    arg[6] = FILE_SHARE_BOTH;
    arg[7] = FILE_OPEN_IF;
    arg[8] = FILE_OPTIONS;
    invoke("NtCreateFile", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtWriteFile");
    memset(arg, 0, sizeof(arg));
    arg[0] = file;
    arg[4] = (SLOT)&iosb;
    arg[5] = (SLOT)bytes;
    arg[6] = 4;
    invoke("NtWriteFile", spec->Argc, spec->Timeout, arg);

    offset_slot[0] = 0;
    offset_slot[1] = 0;
    spec = find_call("NtReadFile");
    memset(arg, 0, sizeof(arg));
    arg[0] = file;
    arg[4] = (SLOT)&iosb;
    arg[5] = (SLOT)back;
    arg[6] = 4;
    arg[7] = (SLOT)offset_slot;
    invoke("NtReadFile", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtDeviceIoControlFile");
    memset(arg, 0, sizeof(arg));
    arg[0] = file;
    arg[4] = (SLOT)&iosb;
    invoke("NtDeviceIoControlFile", spec->Argc, spec->Timeout, arg);

    base = 0;
    size = 0x1000;
    spec = find_call("NtAllocateVirtualMemory");
    memset(arg, 0, sizeof(arg));
    arg[0] = CURRENT_PROCESS;
    arg[1] = (SLOT)&base;
    arg[3] = (SLOT)&size;
    arg[4] = MEM_COMMIT | MEM_RESERVE;
    arg[5] = PAGE_READWRITE;
    invoke("NtAllocateVirtualMemory", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtQueryInformationProcess");
    memset(arg, 0, sizeof(arg));
    arg[0] = CURRENT_PROCESS;
    arg[2] = (SLOT)info;
    arg[3] = sizeof(info);
    arg[4] = (SLOT)&retlen;
    invoke("NtQueryInformationProcess", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtCreateSection");
    memset(arg, 0, sizeof(arg));
    arg[0] = (SLOT)&section;
    arg[1] = SECTION_MAP_BOTH;
    arg[3] = (SLOT)&maximum;
    arg[4] = PAGE_READWRITE;
    arg[5] = SEC_COMMIT;
    invoke("NtCreateSection", spec->Argc, spec->Timeout, arg);

    view = 0x1000;
    base = 0;
    spec = find_call("NtMapViewOfSection");
    memset(arg, 0, sizeof(arg));
    arg[0] = section;
    arg[1] = CURRENT_PROCESS;
    arg[2] = (SLOT)&base;
    arg[6] = (SLOT)&view;
    arg[7] = 1;
    arg[9] = PAGE_READWRITE;
    invoke("NtMapViewOfSection", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtUnmapViewOfSection");
    memset(arg, 0, sizeof(arg));
    arg[0] = CURRENT_PROCESS;
    arg[1] = base;
    invoke("NtUnmapViewOfSection", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtFreeVirtualMemory");
    size = 0;
    memset(arg, 0, sizeof(arg));
    arg[0] = CURRENT_PROCESS;
    arg[1] = (SLOT)&base;
    arg[2] = (SLOT)&size;
    arg[3] = MEM_RELEASE;
    invoke("NtFreeVirtualMemory", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtClose");
    memset(arg, 0, sizeof(arg));
    arg[0] = section;
    invoke("NtClose", spec->Argc, spec->Timeout, arg);

    spec = find_call("NtClose");
    memset(arg, 0, sizeof(arg));
    arg[0] = file;
    invoke("NtClose", spec->Argc, spec->Timeout, arg);

    real_thread();
    real_process();
}

static DWORD WINAPI
thread_start(void *parameter)
{
    (void)parameter;
    return 0;
}

static void
real_thread(void)
{
    SLOT thread = 0;
    SLOT arg[11];
    const CALL *spec;

    spec = find_call("NtCreateThreadEx");
    memset(arg, 0, sizeof(arg));
    arg[0] = (SLOT)&thread;
    arg[1] = THREAD_ALL_ACCESS;
    arg[3] = CURRENT_PROCESS;
    arg[4] = (SLOT)thread_start;
    arg[6] = 1;
    arg[8] = 0x1000;
    arg[9] = 0x100000;
    invoke("NtCreateThreadEx", spec->Argc, spec->Timeout, arg);
    if (thread)
    {
        CloseHandle((HANDLE)thread);
    }
}

static void
real_process(void)
{
    typedef long (__stdcall *CREATE_PARAMS)(void **, USTR *, USTR *, USTR *, USTR *, void *, USTR *, USTR *, USTR *, USTR *, unsigned long);
    typedef unsigned char (__stdcall *DOS_TO_NT)(const wchar_t *, USTR *, wchar_t **, void *);
    typedef void (__stdcall *FREE_USTR)(USTR *);
    typedef void (__stdcall *DESTROY_PARAMS)(void *);
    typedef void (__stdcall *INIT_USTR)(USTR *, const wchar_t *);
    typedef struct
    {
        unsigned long long attribute;
        unsigned long long size;
        unsigned long long value;
        unsigned long long return_length;
    } ATTR;
    typedef struct
    {
        unsigned long long total;
        ATTR attr[3];
    } ATTRLIST;
    wchar_t image[520];
    USTR image_name;
    USTR nt_name;
    void *params = NULL;
    void *ntdll;
    CREATE_PARAMS create_params;
    DOS_TO_NT dos_to_nt;
    FREE_USTR free_ustr;
    DESTROY_PARAMS destroy_params;
    INIT_USTR init_ustr;
    ATTRLIST list;
    OBJATTR process_attr;
    OBJATTR thread_attr;
    unsigned char client[16];
    unsigned char image_info[72];
    unsigned char create_info[0x58];
    SLOT process = 0;
    SLOT thread = 0;
    SLOT arg[11];
    const CALL *spec;
    DWORD length;

    length = GetModuleFileNameW(NULL, image, (DWORD)(sizeof(image) / sizeof(image[0])));
    if (length == 0 || length >= (sizeof(image) / sizeof(image[0])))
    {
        return;
    }
    ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll == NULL)
    {
        return;
    }
    create_params = (CREATE_PARAMS)GetProcAddress(ntdll, "RtlCreateProcessParametersEx");
    dos_to_nt = (DOS_TO_NT)GetProcAddress(ntdll, "RtlDosPathNameToNtPathName_U");
    free_ustr = (FREE_USTR)GetProcAddress(ntdll, "RtlFreeUnicodeString");
    destroy_params = (DESTROY_PARAMS)GetProcAddress(ntdll, "RtlDestroyProcessParameters");
    init_ustr = (INIT_USTR)GetProcAddress(ntdll, "RtlInitUnicodeString");
    if (!create_params || !dos_to_nt || !free_ustr || !init_ustr)
    {
        return;
    }
    init_ustr(&image_name, image);
    if (create_params(&params, &image_name, NULL, NULL, &image_name, NULL, &image_name, NULL, NULL, NULL, 1) != 0)
    {
        return;
    }
    memset(&nt_name, 0, sizeof(nt_name));
    if (!dos_to_nt(image, &nt_name, NULL, NULL) || nt_name.Buffer == NULL)
    {
        if (destroy_params && params)
        {
            destroy_params(params);
        }
        return;
    }
    memset(&list, 0, sizeof(list));
    memset(client, 0, sizeof(client));
    memset(image_info, 0, sizeof(image_info));
    memset(create_info, 0, sizeof(create_info));
    create_info[0] = 0x58;
    list.total = sizeof(list.total) + (3 * sizeof(ATTR));
    list.attr[0].attribute = 0x00020005ULL;
    list.attr[0].size = nt_name.Length;
    list.attr[0].value = (unsigned long long)(SLOT)nt_name.Buffer;
    list.attr[1].attribute = 0x00010003ULL;
    list.attr[1].size = sizeof(client);
    list.attr[1].value = (unsigned long long)(SLOT)client;
    list.attr[2].attribute = 0x00000006ULL;
    list.attr[2].size = sizeof(image_info);
    list.attr[2].value = (unsigned long long)(SLOT)image_info;
    memset(&process_attr, 0, sizeof(process_attr));
    memset(&thread_attr, 0, sizeof(thread_attr));
    process_attr.Length = sizeof(process_attr);
    thread_attr.Length = sizeof(thread_attr);
    spec = find_call("NtCreateUserProcess");
    memset(arg, 0, sizeof(arg));
    arg[0] = (SLOT)&process;
    arg[1] = (SLOT)&thread;
    arg[2] = PROCESS_ALL_ACCESS;
    arg[3] = THREAD_ALL_ACCESS;
    arg[4] = (SLOT)&process_attr;
    arg[5] = (SLOT)&thread_attr;
    arg[7] = 1;
    arg[8] = (SLOT)params;
    arg[9] = (SLOT)create_info;
    arg[10] = (SLOT)&list;
    invoke("NtCreateUserProcess", spec->Argc, spec->Timeout, arg);
    if (process)
    {
        CloseHandle((HANDLE)process);
    }
    if (thread)
    {
        CloseHandle((HANDLE)thread);
    }
    free_ustr(&nt_name);
    if (destroy_params && params)
    {
        destroy_params(params);
    }
}

static int
is_late(const char *name)
{
    return strcmp(name, "NtTerminateProcess") == 0 ||
        strcmp(name, "NtTerminateThread") == 0 ||
        strcmp(name, "NtContinue") == 0 ||
        strcmp(name, "NtRaiseException") == 0;
}

static void
run_zeros(int late)
{
    size_t i;

    for (i = 0; i < sizeof(g_calls) / sizeof(g_calls[0]); i++)
    {
        if (is_done(g_calls[i].Name))
        {
            continue;
        }
        if (is_late(g_calls[i].Name) != late)
        {
            continue;
        }
        invoke(g_calls[i].Name, g_calls[i].Argc, g_calls[i].Timeout, NULL);
    }
}

static void
use_sink_temp(void)
{
    char marker[16];
    wchar_t temp[4];
    int letter;

    for (letter = 'A'; letter <= 'Z'; letter++)
    {
        _snprintf(marker, sizeof(marker), "%c:\\SINK.TXT", letter);
        if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES)
        {
            continue;
        }
        temp[0] = (wchar_t)letter;
        temp[1] = L':';
        temp[2] = L'\\';
        temp[3] = 0;
        SetEnvironmentVariableW(L"TMP", temp);
        SetEnvironmentVariableW(L"TEMP", temp);
        return;
    }
}

static void
open_sink(void)
{
    char marker[16];
    char path[20];
    int letter;

    for (letter = 'A'; letter <= 'Z'; letter++)
    {
        _snprintf(marker, sizeof(marker), "%c:\\SINK.TXT", letter);
        if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES)
        {
            continue;
        }
        _snprintf(path, sizeof(path), "%c:\\result.log", letter);
        g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        return;
    }
}

static HANDLE
open_com(const char *path)
{
    HANDLE handle;
    DCB dcb;

    handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return handle;
    }
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb))
    {
        BuildCommDCBA("baud=115200 parity=N data=8 stop=1", &dcb);
    }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(handle, &dcb);
    return handle;
}

int
main(void)
{
    g_guard_handle = AddVectoredExceptionHandler(1, on_fault);
    g_com = open_com("\\\\.\\COM1");
    g_com2 = open_com("\\\\.\\COM2");
    open_sink();
    use_sink_temp();
    run_real();
    run_zeros(0);
    run_zeros(1);
    emit("END", "OK");
    if (g_com != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_com);
    }
    if (g_com2 != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_com2);
    }
    if (g_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_file);
    }
    if (g_guard_handle != NULL)
    {
        RemoveVectoredExceptionHandler(g_guard_handle);
    }
    return 0;
}
