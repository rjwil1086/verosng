/*
 * PROJECT:     VerOS NT Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        boot/freeldr/freeldr/ntldr/updatestage.c
 * PURPOSE:     Apply a signed file manifest before the kernel loads
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

#include <freeldr.h>
#include <debug.h>

DBG_DEFAULT_CHANNEL(WINDOWS);

#define UPDATE_STAGE_MAX 64

typedef struct
{
    CHAR Staged[260];
    CHAR Destination[260];
    CHAR Aside[280];
    BOOLEAN MovedAside;
    BOOLEAN MovedNew;
    BOOLEAN Exchanged;
} UPDATE_STAGE_ENTRY;

static ULONG UpdateStageDeviceId;

static VOID
UpdateStageSay(PCSTR Text)
{
    UNREFERENCED_PARAMETER(Text);
    TRACE("Update stage: %s\n", Text);
}

static BOOLEAN
UpdateStageSignatureOk(PCSTR Body)
{
    /* The publisher signature is checked by the client before this file is
     * written. FreeLoader refuses a manifest that does not start with the
     * signed marker the client and the bootloader share. */
    return Body != NULL && strncmp(Body, "update-stage-1\n", 14) == 0;
}

static BOOLEAN
UpdateStageBootRoot(PCSTR BootPath, PCHAR Root, ULONG RootSize)
{
    CHAR Trimmed[260];
    SIZE_T Length;
    PCSTR Slash;

    Length = strlen(BootPath);
    while (Length > 0 && (BootPath[Length - 1] == '\\' || BootPath[Length - 1] == '/'))
        Length--;
    if (Length == 0 || Length >= sizeof(Trimmed))
        return FALSE;
    RtlCopyMemory(Trimmed, BootPath, Length);
    Trimmed[Length] = ANSI_NULL;
    Slash = strrchr(Trimmed, '\\');
    if (Slash == NULL)
        Slash = strrchr(Trimmed, '/');
    if (Slash == NULL)
        return FALSE;
    return RtlStringCbCopyA(Root, RootSize, Slash) == STATUS_SUCCESS;
}

static BOOLEAN
UpdateStageRemovedPath(PCSTR Root, ULONG Slot, PCHAR Hold, ULONG HoldSize)
{
    return RtlStringCbPrintfA(Hold,
                              HoldSize,
                              "%s\\System32\\config\\sysupdate\\stage\\removed-%lu",
                              Root,
                              Slot) == STATUS_SUCCESS;
}

/* NtfsMoveFile refuses a destination that is already on disk. Earlier
 * deletes leave removed-0 in place, so a later pass must take the next
 * free name or the manifest stays armed. */
static BOOLEAN
UpdateStageClaimRemoved(PCSTR Root, PULONG Slot, PCHAR Hold, ULONG HoldSize)
{
    ULONG Try;

    for (Try = 0; Try < UPDATE_STAGE_MAX; Try++)
    {
        if (!UpdateStageRemovedPath(Root, Try, Hold, HoldSize))
            return FALSE;
        if (!NtfsPathExists(UpdateStageDeviceId, Hold))
        {
            *Slot = Try;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
UpdateStageVolumePath(PCSTR BootPath, PCSTR WindowsPath, PCHAR Out, ULONG OutSize)
{
    CHAR RootBuf[260];
    PCSTR Root;
    PCSTR Here;
    SIZE_T RootLen;

    if (WindowsPath[0] == '\\' && WindowsPath[1] != '\\')
        return RtlStringCbCopyA(Out, OutSize, WindowsPath) == STATUS_SUCCESS;
    if (!UpdateStageBootRoot(BootPath, RootBuf, sizeof(RootBuf)))
        return FALSE;
    Root = RootBuf;
    RootLen = strlen(Root);
    for (Here = WindowsPath; *Here != ANSI_NULL; Here++)
    {
        if (_strnicmp(Here, Root, RootLen) == 0 &&
            (Here[RootLen] == '\\' || Here[RootLen] == ANSI_NULL))
        {
            return RtlStringCbCopyA(Out, OutSize, Here) == STATUS_SUCCESS;
        }
    }
    return FALSE;
}

static VOID
UpdateStageUndo(UPDATE_STAGE_ENTRY *Entries, ULONG Count)
{
    while (Count > 0)
    {
        Count--;
        if (Entries[Count].MovedNew)
        {
            NtfsMoveFile(UpdateStageDeviceId, Entries[Count].Destination, Entries[Count].Staged);
            Entries[Count].MovedNew = FALSE;
        }
        if (Entries[Count].MovedAside)
        {
            NtfsMoveFile(UpdateStageDeviceId, Entries[Count].Aside, Entries[Count].Destination);
            Entries[Count].MovedAside = FALSE;
        }
        if (Entries[Count].Exchanged)
        {
            NtfsExchangeLinks(UpdateStageDeviceId,
                              Entries[Count].Staged,
                              Entries[Count].Destination);
            Entries[Count].Exchanged = FALSE;
        }
    }
}

static VOID
UpdateStageClearNotice(VOID)
{
    UiDrawStatusText("");
}

static VOID
UpdateStageShowProgress(ULONG Done, ULONG Total)
{
    CHAR Percent[8];
    ULONG Value;

    if (Total == 0)
    {
        return;
    }
    Value = (Done * 100) / Total;
    if (Value > 100)
    {
        Value = 100;
    }
    RtlStringCbPrintfA(Percent, sizeof(Percent), "%lu%%", Value);
    UiDrawStatusText("Do not turn off your pc - updates in progress");
    UiSetProgressBarSubset(0, 100);
    UiUpdateProgressBar(Value, Percent);
}

static VOID
UpdateStageCommitReverse(PCSTR Root)
{
    CHAR Committed[260];
    CHAR Pending[260];

    /* The client leaves the new undo at update.reverse.1 while the stage is
     * armed. A finished forward pass makes that the one committed undo. */
    if (RtlStringCbPrintfA(Committed, sizeof(Committed),
                           "%s\\update.reverse.0", Root) != STATUS_SUCCESS ||
        RtlStringCbPrintfA(Pending, sizeof(Pending),
                           "%s\\update.reverse.1", Root) != STATUS_SUCCESS ||
        !NtfsPathExists(UpdateStageDeviceId, Pending))
    {
        return;
    }
    /* A rename inside one directory removes every entry for that file index,
     * including the name just added. Park the pending undo in the stage
     * directory, drop the old undo, then move it back onto the committed name. */
    {
        CHAR Hold[260];

        if (RtlStringCbPrintfA(Hold, sizeof(Hold),
                "%s\\System32\\config\\sysupdate\\stage\\commit-reverse", Root) != STATUS_SUCCESS ||
            !NtfsMoveFile(UpdateStageDeviceId, Pending, Hold))
        {
            return;
        }
        if (NtfsPathExists(UpdateStageDeviceId, Committed) &&
            !NtfsDeleteFile(UpdateStageDeviceId, Committed))
        {
            NtfsMoveFile(UpdateStageDeviceId, Hold, Pending);
            return;
        }
        if (!NtfsMoveFile(UpdateStageDeviceId, Hold, Committed))
            NtfsMoveFile(UpdateStageDeviceId, Hold, Pending);
    }
}

static BOOLEAN
UpdateStageCanWrite(VOID)
{
    return UpdateStageDeviceId < MAX_FDS && NtfsPathExists(UpdateStageDeviceId, "\\");
}

static BOOLEAN
UpdateStageReplace(UPDATE_STAGE_ENTRY *Entry)
{
    if (NtfsPathExists(UpdateStageDeviceId, Entry->Destination))
    {
        if (!NtfsExchangeLinks(UpdateStageDeviceId, Entry->Staged, Entry->Destination))
            return FALSE;
        Entry->Exchanged = TRUE;
        return TRUE;
    }
    if (!NtfsMoveFile(UpdateStageDeviceId, Entry->Staged, Entry->Destination))
        return FALSE;
    Entry->MovedNew = TRUE;
    return TRUE;
}

static BOOLEAN
UpdateStageCopy(UPDATE_STAGE_ENTRY *Entries, ULONG Count)
{
    ULONG Index;

    if (Count == 0)
    {
        return TRUE;
    }
    if (!UpdateStageCanWrite())
    {
        UpdateStageSay("not writable");
        return FALSE;
    }
    UpdateStageShowProgress(0, Count);
    for (Index = 0; Index < Count; Index++)
    {
        if (!UpdateStageReplace(&Entries[Index]))
        {
            UpdateStageSay("replace failed");
            UpdateStageClearNotice();
            return FALSE;
        }
        UpdateStageShowProgress(Index + 1, Count);
    }
    UpdateStageClearNotice();
    return TRUE;
}

BOOLEAN
UpdateStageApply(PCSTR BootPath, PCSTR BootOptions)
{
    CHAR manifestName[32];
    ULONG fileId;
    CHAR body[4096];
    ULONG readCount = 0;
    UPDATE_STAGE_ENTRY *entries = NULL;
    CHAR (*deletes)[260] = NULL;
    ULONG count = 0;
    ULONG deleteCount = 0;
    PCHAR cursor;
    BOOLEAN SkipOs = FALSE;
    ULONG Index;
    ULONG removedSlot[UPDATE_STAGE_MAX];
    CHAR deleteRoot[260];

    if (BootOptions != NULL && strstr(BootOptions, "UPDATESKIP") != NULL)
        SkipOs = TRUE;
    if (FsOpenFile("update.skip", BootPath, OpenReadOnly, &fileId) == ESUCCESS)
    {
        ArcClose(fileId);
        SkipOs = TRUE;
    }
    if (BootOptions != NULL && strstr(BootOptions, "UPDATEROLLBACK=1") != NULL)
        RtlStringCbCopyA(manifestName, sizeof(manifestName), "update.reverse.1");
    else if (BootOptions != NULL && strstr(BootOptions, "UPDATEROLLBACK=0") != NULL)
        RtlStringCbCopyA(manifestName, sizeof(manifestName), "update.reverse.0");
    else
        RtlStringCbCopyA(manifestName, sizeof(manifestName), "update.stage");
    if (FsOpenFile(manifestName, BootPath, OpenReadOnly, &fileId) != ESUCCESS)
    {
        UpdateStageSay("no manifest");
        return TRUE;
    }
    UpdateStageDeviceId = FsGetDeviceId(fileId);
    ArcRead(fileId, body, sizeof(body) - 1, &readCount);
    body[readCount] = ANSI_NULL;
    if (!UpdateStageSignatureOk(body))
    {
        ArcClose(fileId);
        return TRUE;
    }

    entries = FrLdrTempAlloc(sizeof(UPDATE_STAGE_ENTRY) * UPDATE_STAGE_MAX, 'egtS');
    deletes = FrLdrTempAlloc(sizeof(CHAR[UPDATE_STAGE_MAX][260]), 'egtS');
    if (entries == NULL || deletes == NULL)
    {
        if (entries != NULL)
            FrLdrTempFree(entries, 'egtS');
        if (deletes != NULL)
            FrLdrTempFree(deletes, 'egtS');
        ArcClose(fileId);
        return TRUE;
    }
    RtlZeroMemory(entries, sizeof(UPDATE_STAGE_ENTRY) * UPDATE_STAGE_MAX);
    cursor = body;
    while (cursor != NULL && *cursor != ANSI_NULL)
    {
        PCHAR line = cursor;
        PCHAR rest;
        PCHAR split;
        BOOLEAN Updater = FALSE;
        BOOLEAN Delete = FALSE;
        cursor = strchr(cursor, '\n');
        if (cursor != NULL)
        {
            *cursor = ANSI_NULL;
            cursor++;
        }
        if (strncmp(line, "file\t", 5) == 0 || strncmp(line, "file ", 5) == 0)
            rest = line + 5;
        else if (strncmp(line, "delete\t", 7) == 0 || strncmp(line, "delete ", 7) == 0)
        {
            rest = line + 7;
            Delete = TRUE;
        }
        else
        {
            continue;
        }
        if (strncmp(rest, "updater\t", 8) == 0)
        {
            Updater = TRUE;
            rest += 8;
        }
        else if (strncmp(rest, "os\t", 3) == 0)
        {
            rest += 3;
        }
        else
        {
            UpdateStageUndo(entries, count);
            UpdateStageSay("bad path");
            goto Done;
        }
        if (!Updater && SkipOs)
            continue;
        if (Delete)
        {
            if (deleteCount >= UPDATE_STAGE_MAX ||
                !UpdateStageVolumePath(BootPath,
                                       rest,
                                       deletes[deleteCount],
                                       sizeof(deletes[deleteCount])))
            {
                UpdateStageUndo(entries, count);
                TRACE("Update stage manifest is incomplete; booting the current kernel\n");
                goto Done;
            }
            deleteCount++;
            continue;
        }
        split = strchr(rest, '\t');
        if (split == NULL || count >= UPDATE_STAGE_MAX)
        {
            UpdateStageUndo(entries, count);
            UpdateStageSay("bad path");
            goto Done;
        }
        *split = ANSI_NULL;
        if (!UpdateStageVolumePath(BootPath,
                                   rest,
                                   entries[count].Staged,
                                   sizeof(entries[count].Staged)) ||
            !UpdateStageVolumePath(BootPath,
                                   split + 1,
                                   entries[count].Destination,
                                   sizeof(entries[count].Destination)))
        {
            UpdateStageUndo(entries, count);
            UpdateStageSay("bad path");
            goto Done;
        }
        RtlStringCbCopyA(entries[count].Aside,
                         sizeof(entries[count].Aside),
                         entries[count].Destination);
        RtlStringCbCatA(entries[count].Aside,
                        sizeof(entries[count].Aside),
                        ".update-aside");
        count++;
    }
    if (!UpdateStageCopy(entries, count))
    {
        UpdateStageUndo(entries, count);
        UpdateStageSay("not applied");
        goto Done;
    }
    if (deleteCount > 0 &&
        !UpdateStageBootRoot(BootPath, deleteRoot, sizeof(deleteRoot)))
    {
        UpdateStageUndo(entries, count);
        UpdateStageSay("delete failed");
        goto Done;
    }
    RtlZeroMemory(removedSlot, sizeof(removedSlot));
    for (Index = 0; Index < deleteCount; Index++)
    {
        CHAR Hold[260];

        if (!UpdateStageClaimRemoved(deleteRoot,
                                    &removedSlot[Index],
                                    Hold,
                                    sizeof(Hold)) ||
            !NtfsMoveFile(UpdateStageDeviceId, deletes[Index], Hold))
        {
            while (Index > 0)
            {
                Index--;
                if (UpdateStageRemovedPath(deleteRoot,
                                           removedSlot[Index],
                                           Hold,
                                           sizeof(Hold)))
                    NtfsMoveFile(UpdateStageDeviceId, Hold, deletes[Index]);
            }
            UpdateStageUndo(entries, count);
            UpdateStageSay("delete failed");
            goto Done;
        }
    }
    /* Skip ignores the OS entries for this boot and leaves the manifest armed. */
    if (SkipOs)
        goto Done;
    {
        CHAR StagePath[260];
        CHAR Root[260];

        if (!UpdateStageBootRoot(BootPath, Root, sizeof(Root)) ||
            RtlStringCbPrintfA(StagePath, sizeof(StagePath),
                               "%s\\%s", Root, manifestName) != STATUS_SUCCESS ||
            !NtfsStampFile(UpdateStageDeviceId, StagePath, "update-spent-1\n"))
        {
            while (deleteCount > 0)
            {
                CHAR Hold[260];

                deleteCount--;
                if (UpdateStageRemovedPath(deleteRoot,
                                           removedSlot[deleteCount],
                                           Hold,
                                           sizeof(Hold)))
                    NtfsMoveFile(UpdateStageDeviceId, Hold, deletes[deleteCount]);
            }
            UpdateStageUndo(entries, count);
            UpdateStageSay("stamp failed");
        }
        else
        {
            if (strcmp(manifestName, "update.stage") == 0)
                UpdateStageCommitReverse(Root);
            UpdateStageSay("applied");
        }
    }
Done:
    ArcClose(fileId);
    FrLdrTempFree(entries, 'egtS');
    FrLdrTempFree(deletes, 'egtS');
    return TRUE;
}
