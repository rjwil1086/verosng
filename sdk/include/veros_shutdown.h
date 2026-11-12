/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        sdk/include/veros_shutdown.h
 * PURPOSE:     Shutdown dialog hook for a registered update client
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

#pragma once

#define VEROS_SHUTDOWN_HOOK_KEY L"SOFTWARE\\VerOS\\ShutdownHook"
#define VEROS_SHUTDOWN_HOOK_VALUE L"Library"
#define VEROS_SHUTDOWN_MAX_CHOICES 8

typedef struct _VEROS_SHUTDOWN_CHOICE
{
    UINT Id;
    UINT FinalAction;
    WCHAR Label[64];
    WCHAR Detail[256];
} VEROS_SHUTDOWN_CHOICE;

typedef struct _VEROS_SHUTDOWN_QUERY
{
    UINT Count;
    VEROS_SHUTDOWN_CHOICE Choices[VEROS_SHUTDOWN_MAX_CHOICES];
} VEROS_SHUTDOWN_QUERY;

typedef struct _VEROS_SHUTDOWN_COMMIT
{
    UINT ChoiceId;
    WCHAR Notice[256];
} VEROS_SHUTDOWN_COMMIT;

typedef BOOL (WINAPI *PFN_VEROS_SHUTDOWN_QUERY)(VEROS_SHUTDOWN_QUERY *Query);
typedef BOOL (WINAPI *PFN_VEROS_SHUTDOWN_COMMIT)(VEROS_SHUTDOWN_COMMIT *Commit);
