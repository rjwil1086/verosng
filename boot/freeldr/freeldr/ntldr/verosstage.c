/*
 * PROJECT:     VerOS NT Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        boot/freeldr/freeldr/ntldr/verosstage.c
 * PURPOSE:     Apply a signed file manifest before the kernel loads
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

#include <freeldr.h>
#include <debug.h>

DBG_DEFAULT_CHANNEL(WINDOWS);

#define VEROS_STAGE_MAX 64

typedef struct
{
    CHAR Staged[160];
    CHAR Destination[160];
    CHAR Aside[180];
    BOOLEAN Done;
} VEROS_STAGE_ENTRY;

static BOOLEAN
VerosStageSignatureOk(PCSTR Body)
{
    /* The publisher signature is checked by the client before this file is
     * written. FreeLoader refuses a manifest that does not start with the
     * signed marker the client and the bootloader share. */
    return Body != NULL && strncmp(Body, "veros-stage-1\n", 14) == 0;
}

static VOID
VerosStageUndo(VEROS_STAGE_ENTRY *Entries, ULONG Count)
{
    while (Count > 0)
    {
        Count--;
        if (Entries[Count].Done)
        {
            Entries[Count].Done = FALSE;
        }
    }
}

static VOID
VerosStageClearNotice(VOID)
{
    ULONG Top;

    if (!UiProgressBar.Show || UiProgressBar.Top < 2)
    {
        return;
    }
    Top = UiProgressBar.Top - 2;
    UiFillArea(0, Top, UiScreenWidth - 1, Top,
               ' ', ATTR(UiBackdropFgColor, UiBackdropBgColor));
    VideoCopyOffScreenBufferToVRAM();
}

static VOID
VerosStageShowProgress(ULONG Done, ULONG Total)
{
    CHAR Percent[8];
    ULONG Value;
    ULONG Top;

    if (Total == 0 || !UiProgressBar.Show || UiProgressBar.Top < 2)
    {
        return;
    }
    Value = (Done * 100) / Total;
    if (Value > 100)
    {
        Value = 100;
    }
    RtlStringCbPrintfA(Percent, sizeof(Percent), "%lu%%", Value);
    Top = UiProgressBar.Top - 2;
    UiDrawCenteredText(0, Top, UiScreenWidth - 1, Top,
                       "Do not turn off your pc - updates in progress",
                       ATTR(UiBackdropFgColor, UiBackdropBgColor));
    UiSetProgressBarSubset(0, 100);
    UiUpdateProgressBar(Value, Percent);
}

static BOOLEAN
VerosStageCanWrite(VOID)
{
    /* NTFS create, rename, and delete are not available in this loader yet. */
    return FALSE;
}

static BOOLEAN
VerosStageReplace(VEROS_STAGE_ENTRY *Entry)
{
    UNREFERENCED_PARAMETER(Entry);
    return FALSE;
}

static BOOLEAN
VerosStageCopy(VEROS_STAGE_ENTRY *Entries, ULONG Count)
{
    ULONG Index;

    if (Count == 0)
    {
        return TRUE;
    }
    if (!VerosStageCanWrite())
    {
        TRACE("VerOS stage needs NTFS write; booting the current kernel\n");
        return FALSE;
    }
    VerosStageShowProgress(0, Count);
    for (Index = 0; Index < Count; Index++)
    {
        if (!VerosStageReplace(&Entries[Index]))
        {
            VerosStageClearNotice();
            return FALSE;
        }
        Entries[Index].Done = TRUE;
        VerosStageShowProgress(Index + 1, Count);
    }
    VerosStageClearNotice();
    return TRUE;
}

BOOLEAN
VerosStageApply(PCSTR BootPath, PCSTR BootOptions)
{
    CHAR manifestPath[260];
    ULONG fileId;
    CHAR body[4096];
    ULONG readCount = 0;
    VEROS_STAGE_ENTRY entries[VEROS_STAGE_MAX];
    ULONG count = 0;
    PCHAR cursor;

    BOOLEAN SkipOs = FALSE;
    ULONG skipId;

    UNREFERENCED_PARAMETER(BootPath);
    if (BootOptions != NULL && strstr(BootOptions, "VEROSSKIP") != NULL)
    {
        SkipOs = TRUE;
    }
    if (ArcOpen("veros.skip", OpenReadOnly, &skipId) == ESUCCESS)
    {
        ArcClose(skipId);
        SkipOs = TRUE;
    }
    if (BootOptions != NULL && strstr(BootOptions, "VEROSROLLBACK=1") != NULL)
    {
        RtlStringCbCopyA(manifestPath, sizeof(manifestPath), "veros.reverse.1");
    }
    else if (BootOptions != NULL && strstr(BootOptions, "VEROSROLLBACK=0") != NULL)
    {
        RtlStringCbCopyA(manifestPath, sizeof(manifestPath), "veros.reverse.0");
    }
    else
    {
        RtlStringCbCopyA(manifestPath, sizeof(manifestPath), "veros.stage");
    }
    if (ArcOpen(manifestPath, OpenReadOnly, &fileId) != ESUCCESS)
    {
        return TRUE;
    }
    ArcRead(fileId, body, sizeof(body) - 1, &readCount);
    ArcClose(fileId);
    body[readCount] = ANSI_NULL;
    if (!VerosStageSignatureOk(body))
    {
        TRACE("VerOS stage manifest signature was rejected\n");
        return TRUE;
    }

    cursor = body;
    while (cursor != NULL && *cursor != ANSI_NULL && count < VEROS_STAGE_MAX)
    {
        PCHAR line = cursor;
        PCHAR rest;
        PCHAR split;
        BOOLEAN Updater = FALSE;
        cursor = strchr(cursor, '\n');
        if (cursor != NULL)
        {
            *cursor = ANSI_NULL;
            cursor++;
        }
        if (strncmp(line, "file ", 5) != 0)
        {
            continue;
        }
        rest = line + 5;
        if (strncmp(rest, "updater ", 8) == 0)
        {
            Updater = TRUE;
            rest += 8;
        }
        else if (strncmp(rest, "os ", 3) == 0)
        {
            rest += 3;
        }
        if (!Updater && SkipOs)
        {
            continue;
        }
        split = strchr(rest, ' ');
        if (split == NULL)
        {
            VerosStageUndo(entries, count);
            TRACE("VerOS stage manifest is incomplete; booting the current kernel\n");
            return TRUE;
        }
        *split = ANSI_NULL;
        RtlStringCbCopyA(entries[count].Staged, sizeof(entries[count].Staged), rest);
        RtlStringCbCopyA(entries[count].Destination, sizeof(entries[count].Destination), split + 1);
        RtlStringCbCopyA(entries[count].Aside, sizeof(entries[count].Aside), entries[count].Destination);
        RtlStringCbCatA(entries[count].Aside, sizeof(entries[count].Aside), ".veros-aside");
        entries[count].Done = FALSE;
        count++;
    }
    if (!VerosStageCopy(entries, count))
    {
        VerosStageUndo(entries, count);
        TRACE("VerOS stage was not applied; booting the current kernel\n");
    }
    return TRUE;
}
