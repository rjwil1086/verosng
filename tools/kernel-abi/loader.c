/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Loads probe.sys from the same directory and returns. The driver prints
 * the DIFF lines. This program only reports whether the load started.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef long (__stdcall *ADJUST)(unsigned long, unsigned char, unsigned char, unsigned char *);
typedef long (__stdcall *LOAD)(void *);

typedef struct _USTR
{
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t *Buffer;
} USTR;

static void
emit(const char *text)
{
    HANDLE com;
    DWORD wrote;
    int n;
    char line[160];

    n = _snprintf(line, sizeof(line), "%s", text);
    if (n < 0)
    {
        return;
    }
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    com = CreateFileA("\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (com != INVALID_HANDLE_VALUE)
    {
        WriteFile(com, line, (DWORD)n, &wrote, NULL);
        CloseHandle(com);
    }
}

static int
write_service(const wchar_t *image)
{
    HKEY key;
    DWORD type;
    DWORD start;
    DWORD error;
    LONG rc;

    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                          L"System\\CurrentControlSet\\Services\\KernelAbiProbe",
                          0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS)
    {
        return 0;
    }
    type = 1;
    start = 3;
    error = 1;
    RegSetValueExW(key, L"Type", 0, REG_DWORD, (BYTE *)&type, sizeof(type));
    RegSetValueExW(key, L"Start", 0, REG_DWORD, (BYTE *)&start, sizeof(start));
    RegSetValueExW(key, L"ErrorControl", 0, REG_DWORD, (BYTE *)&error, sizeof(error));
    RegSetValueExW(key, L"ImagePath", 0, REG_SZ, (BYTE *)image,
                   (DWORD)((wcslen(image) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return 1;
}

int
main(void)
{
    wchar_t path[MAX_PATH];
    wchar_t image[MAX_PATH + 8];
    wchar_t *slash;
    HMODULE ntdll;
    ADJUST adjust;
    LOAD load;
    USTR service;
    unsigned char was;
    long status;
    char text[48];
    const wchar_t *reg =
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\KernelAbiProbe";

    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0)
    {
        emit("LOAD MISSING\r\n");
        return 1;
    }
    slash = wcsrchr(path, L'\\');
    if (slash == NULL)
    {
        emit("LOAD MISSING\r\n");
        return 1;
    }
    *(slash + 1) = 0;
    _snwprintf(image, MAX_PATH, L"\\??\\%sprobe.sys", path);
    if (!write_service(image))
    {
        emit("LOAD MISSING\r\n");
        return 1;
    }
    ntdll = GetModuleHandleA("ntdll.dll");
    adjust = (ADJUST)GetProcAddress(ntdll, "RtlAdjustPrivilege");
    load = (LOAD)GetProcAddress(ntdll, "NtLoadDriver");
    if (adjust == NULL || load == NULL)
    {
        emit("LOAD MISSING\r\n");
        return 1;
    }
    was = 0;
    adjust(10, 1, 0, &was);
    service.Length = (unsigned short)(wcslen(reg) * sizeof(wchar_t));
    service.MaximumLength = (unsigned short)(service.Length + sizeof(wchar_t));
    service.Buffer = (wchar_t *)reg;
    status = load(&service);
    _snprintf(text, sizeof(text), "LOAD 0x%08lX\r\n", (unsigned long)status);
    emit(text);
    return (status < 0) ? 1 : 0;
}
