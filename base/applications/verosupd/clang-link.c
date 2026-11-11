/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/clang-link.c
 * PURPOSE:     Drive the amd64 Clang PE link for the update client
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

/*
 * Rustc invokes this program with the full link line. A cmd wrapper cannot
 * see the tail of that line, where -shared and the .dll output sit, so the
 * Control Panel applet was linked with the executable startup object.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const wchar_t *
SkipArg0(const wchar_t *Command)
{
    if (*Command == L'"') {
        Command++;
        while (*Command != L'\0' && *Command != L'"') {
            Command++;
        }
        if (*Command == L'"') {
            Command++;
        }
    } else {
        while (*Command != L'\0' && *Command != L' ' && *Command != L'\t') {
            Command++;
        }
    }
    while (*Command == L' ' || *Command == L'\t') {
        Command++;
    }
    return Command;
}

static int
TextHasShared(const wchar_t *Command)
{
    const wchar_t *Found = Command;

    while ((Found = wcsstr(Found, L"-shared")) != NULL) {
        wchar_t Before = (Found == Command) ? L' ' : Found[-1];
        wchar_t After = Found[7];

        if ((Before == L' ' || Before == L'\t' || Before == L'"') &&
            (After == L'\0' || After == L' ' || After == L'\t' || After == L'"')) {
            return 1;
        }
        Found += 7;
    }
    return 0;
}

static int
ResponseHasShared(const wchar_t *Command)
{
    const wchar_t *At = wcschr(Command, L'@');
    wchar_t Path[MAX_PATH * 2];
    size_t Length = 0;
    HANDLE File;
    DWORD Size;
    DWORD Read = 0;
    char *Bytes;
    int Shared = 0;

    if (At == NULL) {
        return 0;
    }
    At++;
    if (*At == L'"') {
        At++;
        while (At[Length] != L'\0' && At[Length] != L'"' && Length + 1 < sizeof(Path) / sizeof(Path[0])) {
            Path[Length] = At[Length];
            Length++;
        }
    } else {
        while (At[Length] != L'\0' && At[Length] != L' ' && At[Length] != L'\t' &&
               Length + 1 < sizeof(Path) / sizeof(Path[0])) {
            Path[Length] = At[Length];
            Length++;
        }
    }
    Path[Length] = L'\0';
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE) {
        return 0;
    }
    Size = GetFileSize(File, NULL);
    Bytes = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)Size + 2);
    if (Bytes != NULL && ReadFile(File, Bytes, Size, &Read, NULL)) {
        Bytes[Read] = 0;
        Bytes[Read + 1] = 0;
        if (Read >= 2 && (unsigned char)Bytes[0] == 0xFF && (unsigned char)Bytes[1] == 0xFE) {
            Shared = TextHasShared((const wchar_t *)(Bytes + 2));
        } else if (Read >= 2 && Bytes[1] == 0) {
            Shared = TextHasShared((const wchar_t *)Bytes);
        } else {
            Shared = strstr(Bytes, "-shared") != NULL;
        }
    }
    if (Bytes != NULL) {
        HeapFree(GetProcessHeap(), 0, Bytes);
    }
    CloseHandle(File);
    return Shared;
}

int
main(void)
{
    wchar_t Self[MAX_PATH];
    wchar_t Directory[MAX_PATH];
    wchar_t Crt[MAX_PATH];
    const wchar_t *Rest;
    wchar_t *Command;
    size_t RestLength;
    size_t Bytes;
    STARTUPINFOW Startup;
    PROCESS_INFORMATION Process;
    DWORD ExitCode = 1;
    wchar_t *Slash;

    if (GetModuleFileNameW(NULL, Self, MAX_PATH) == 0) {
        return 1;
    }
    lstrcpynW(Directory, Self, MAX_PATH);
    Slash = wcsrchr(Directory, L'\\');
    if (Slash != NULL) {
        *Slash = L'\0';
    }

    Rest = SkipArg0(GetCommandLineW());
    if (_snwprintf(Crt, MAX_PATH,
                   L"%s\\..\\..\\..\\..\\tools\\rust\\rustup\\toolchains\\1.99.1-x86_64-pc-windows-gnu\\lib\\rustlib\\x86_64-pc-windows-gnu\\lib\\self-contained\\%s",
                   Directory,
                   (TextHasShared(Rest) || ResponseHasShared(Rest)) ? L"dllcrt2.o" : L"crt2.o") < 0) {
        return 1;
    }

    RestLength = wcslen(Rest);
    Bytes = (RestLength + MAX_PATH + 64) * sizeof(wchar_t);
    Command = HeapAlloc(GetProcessHeap(), 0, Bytes);
    if (Command == NULL) {
        return 1;
    }
    if (_snwprintf(Command, RestLength + MAX_PATH + 64,
                   L"clang --target=x86_64-w64-mingw32 -nostartfiles -fuse-ld=lld \"%s\" %s",
                   Crt, Rest) < 0) {
        HeapFree(GetProcessHeap(), 0, Command);
        return 1;
    }

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    if (!CreateProcessW(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Process)) {
        HeapFree(GetProcessHeap(), 0, Command);
        return 1;
    }
    WaitForSingleObject(Process.hProcess, INFINITE);
    GetExitCodeProcess(Process.hProcess, &ExitCode);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    HeapFree(GetProcessHeap(), 0, Command);
    return (int)ExitCode;
}
