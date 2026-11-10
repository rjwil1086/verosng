/*
 * PROJECT:     VerOS NT Kernel
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        drivers/filesystems/ntfs/ntfs3x.c
 * PURPOSE:     NTFS 3.1 log, security, $Extend, compression, and EFS
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

/*
 * $LogFile replay understands only the records this driver writes. A restart
 * page it does not recognize leaves the volume read-only. $Secure, the
 * $Extend indexes, the USN journal, LZNT1, and $EFS are updated here.
 */

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

#define NTFS_LOG_FILE_MAGIC 0x52545352  /* 'RSTR' */
#define NTFS_LOG_PAGE_MAGIC 0x44524352  /* 'RCRD' */
#define NTFS_LOG_REC_MAGIC  0x454C4946  /* 'FILE' */
#define NTFS_LOG_PAGE       4096
#define NTFS_USN_MFT        23
#define NTFS_EFS_BLOCK      512
#define NTFS_SDS_MIRROR     0x40000

static
NTSTATUS
NtfsAddResidentAttribute(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER Record,
                         ULONG Type,
                         PCWSTR Name,
                         UCHAR NameLength,
                         const UCHAR *Value,
                         ULONG ValueLength);

static LONG NtfsLogBusy;

static
ULONG
NtfsHashSecurity(const UCHAR *Data, ULONG Length)
{
    ULONG Hash = 0;
    ULONG Index;

    for (Index = 0; Index + 4 <= Length; Index += 4)
    {
        ULONG Dword = *(const ULONG *)(Data + Index);
        Hash = Dword + ((Hash << 3) | (Hash >> 29));
    }
    return Hash;
}

static
NTSTATUS
NtfsFindExtend(PDEVICE_EXTENSION Vcb, PCWSTR Name, PULONGLONG Index)
{
    UNICODE_STRING UpName;
    ULONG FirstEntry = 0;

    RtlInitUnicodeString(&UpName, Name);
    /* FirstEntry is required. A NULL is dereferenced while walking the index. */
    return NtfsFindMftRecord(Vcb, NTFS_FILE_EXTEND, &UpName, &FirstEntry, FALSE, TRUE, Index);
}

static
NTSTATUS
NtfsReadRecord(PDEVICE_EXTENSION Vcb, ULONGLONG Index, PFILE_RECORD_HEADER *Record)
{
    NTSTATUS Status;

    *Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (*Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Index, *Record);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, *Record);
        *Record = NULL;
    }
    return Status;
}

static
VOID
NtfsFreeRecord(PDEVICE_EXTENSION Vcb, PFILE_RECORD_HEADER Record)
{
    if (Record != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
}

static
NTSTATUS
NtfsStoreAttribute(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER Record,
                   PNTFS_ATTR_CONTEXT Context)
{
    PNTFS_ATTR_RECORD Slot;

    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0)
    {
        if (Slot->Instance == Context->pRecord->Instance &&
            Slot->Length == Context->pRecord->Length)
        {
            RtlCopyMemory(Slot, Context->pRecord, Slot->Length);
            return UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
        }
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    }
    return STATUS_NOT_FOUND;
}

/* Insert a view-index entry into a resident $INDEX_ROOT, growing it when the
 * file record has room. The key is compared as an unsigned byte string. */
static
NTSTATUS
NtfsInsertRootEntry(PDEVICE_EXTENSION Vcb,
                    ULONGLONG Directory,
                    PCWSTR IndexName,
                    const UCHAR *Key,
                    USHORT KeyLength,
                    const UCHAR *Data,
                    USHORT DataLength)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PINDEX_ROOT_ATTRIBUTE Root;
    PUCHAR Entries;
    PUCHAR End;
    PUCHAR Cursor;
    ULONG EntryLength;
    ULONG Need;
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, Directory, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(Vcb, Record, AttributeIndexRoot, IndexName, (ULONG)wcslen(IndexName), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Context->pRecord->IsNonResident)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_NOT_IMPLEMENTED;
    }

    Root = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)Context->pRecord + Context->pRecord->Resident.ValueOffset);
    if (Root->Header.Flags != INDEX_ROOT_SMALL)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_NOT_IMPLEMENTED;
    }

    EntryLength = ALIGN_UP_BY(16 + KeyLength + DataLength, 8);
    Need = EntryLength;
    if (Root->Header.TotalSizeOfEntries + Need > Root->Header.AllocatedSize)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_DISK_FULL;
    }

    Entries = (PUCHAR)&Root->Header + Root->Header.FirstEntryOffset;
    End = (PUCHAR)&Root->Header + Root->Header.TotalSizeOfEntries;
    Cursor = Entries;
    while (Cursor + 16 <= End)
    {
        USHORT Length = *(PUSHORT)(Cursor + 8);
        USHORT ThisKey = *(PUSHORT)(Cursor + 10);
        USHORT Flags = *(PUSHORT)(Cursor + 12);
        int Compare;

        if (Length < 16 || Cursor + Length > End)
            break;
        if (Flags & NTFS_INDEX_ENTRY_END)
            break;

        Compare = memcmp(Key, Cursor + 16, min(KeyLength, ThisKey));
        if (Compare == 0 && KeyLength == ThisKey)
        {
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_SUCCESS;
        }
        if (Compare < 0 || (Compare == 0 && KeyLength < ThisKey))
            break;
        Cursor += Length;
    }

    RtlMoveMemory(Cursor + EntryLength, Cursor, (ULONG)(End - Cursor));
    RtlZeroMemory(Cursor, EntryLength);
    *(PUSHORT)Cursor = (USHORT)ALIGN_UP_BY(16 + KeyLength, 8);
    *(PUSHORT)(Cursor + 2) = DataLength;
    *(PUSHORT)(Cursor + 8) = (USHORT)EntryLength;
    *(PUSHORT)(Cursor + 10) = KeyLength;
    RtlCopyMemory(Cursor + 16, Key, KeyLength);
    RtlCopyMemory(Cursor + *(PUSHORT)Cursor, Data, DataLength);
    Root->Header.TotalSizeOfEntries += EntryLength;

    {
        PNTFS_ATTR_RECORD Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
        while (Slot->Type != AttributeEnd && Slot->Length != 0)
        {
            if (Slot->Instance == Context->pRecord->Instance && Slot->Length == Context->pRecord->Length)
            {
                RtlCopyMemory(Slot, Context->pRecord, Slot->Length);
                break;
            }
            Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
        }
    }

    Status = UpdateFileRecord(Vcb, Directory, Record);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsReplayLog(PDEVICE_EXTENSION Vcb)
{
    PFILE_RECORD_HEADER LogRecord = NULL;
    PNTFS_ATTR_CONTEXT Data = NULL;
    PUCHAR Page = NULL;
    ULONGLONG Offset;
    ULONGLONG Size;
    NTSTATUS Status;
    BOOLEAN Applied = FALSE;

    if ((Vcb->Flags & VCB_VOLUME_READ_ONLY) == 0)
        return STATUS_SUCCESS;

    InterlockedExchange(&NtfsLogBusy, 1);

    Status = NtfsReadRecord(Vcb, NTFS_FILE_LOGFILE, &LogRecord);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = FindAttribute(Vcb, LogRecord, AttributeData, L"", 0, &Data, NULL);
    if (!NT_SUCCESS(Status))
        goto Done;

    Page = ExAllocatePoolWithTag(NonPagedPool, NTFS_LOG_PAGE, TAG_NTFS);
    if (Page == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    if (ReadAttribute(Vcb, Data, 0, (PCHAR)Page, NTFS_LOG_PAGE) != NTFS_LOG_PAGE)
    {
        Status = STATUS_DISK_CORRUPT_ERROR;
        goto Done;
    }

    /* An uninitialized log, or one we did not write, is not replayed. */
    if (*(PULONG)Page != NTFS_LOG_FILE_MAGIC)
    {
        Status = STATUS_DISK_CORRUPT_ERROR;
        goto Done;
    }

    Size = AttributeDataLength(Data->pRecord);
    for (Offset = NTFS_LOG_PAGE; Offset + NTFS_LOG_PAGE <= Size; Offset += NTFS_LOG_PAGE)
    {
        ULONG Cursor;

        if (ReadAttribute(Vcb, Data, Offset, (PCHAR)Page, NTFS_LOG_PAGE) != NTFS_LOG_PAGE)
            break;
        if (*(PULONG)Page == 0 || *(PULONG)Page == 0xFFFFFFFF)
            break;
        if (*(PULONG)Page != NTFS_LOG_PAGE_MAGIC)
        {
            Status = STATUS_DISK_CORRUPT_ERROR;
            goto Done;
        }

        Cursor = 8;
        while (Cursor + 16 <= NTFS_LOG_PAGE)
        {
            ULONG Magic = *(PULONG)(Page + Cursor);
            ULONGLONG Index;
            ULONG Length;

            if (Magic == 0)
                break;
            if (Magic != NTFS_LOG_REC_MAGIC)
            {
                Status = STATUS_DISK_CORRUPT_ERROR;
                goto Done;
            }

            Index = *(PULONGLONG)(Page + Cursor + 4);
            Length = *(PULONG)(Page + Cursor + 12);
            if (Length != Vcb->NtfsInfo.BytesPerFileRecord ||
                Cursor + 16 + Length > NTFS_LOG_PAGE)
            {
                Status = STATUS_DISK_CORRUPT_ERROR;
                goto Done;
            }

            Status = WriteAttribute(Vcb,
                                    Vcb->MFTContext,
                                    Index * Vcb->NtfsInfo.BytesPerFileRecord,
                                    Page + Cursor + 16,
                                    Length,
                                    &Length,
                                    NULL);
            if (!NT_SUCCESS(Status))
                goto Done;
            Applied = TRUE;
            Cursor += 16 + Vcb->NtfsInfo.BytesPerFileRecord;
        }
    }

    /* The records we wrote are back. Drop the dirty bit and allow writes. */
    {
        PFILE_RECORD_HEADER Volume;
        PNTFS_ATTR_CONTEXT Info = NULL;
        PVOLINFO_ATTRIBUTE VolInfo;

        Status = NtfsReadRecord(Vcb, NTFS_FILE_VOLUME, &Volume);
        if (!NT_SUCCESS(Status))
            goto Done;
        Status = FindAttribute(Vcb, Volume, AttributeVolumeInformation, L"", 0, &Info, NULL);
        if (NT_SUCCESS(Status))
        {
            VolInfo = (PVOLINFO_ATTRIBUTE)((PUCHAR)Info->pRecord + Info->pRecord->Resident.ValueOffset);
            VolInfo->Flags &= ~NTFS_VOLUME_DIRTY;
            Status = UpdateFileRecord(Vcb, NTFS_FILE_VOLUME, Volume);
            ReleaseAttributeContext(Info);
        }
        NtfsFreeRecord(Vcb, Volume);
        if (!NT_SUCCESS(Status))
            goto Done;
    }

    Vcb->Flags &= ~VCB_VOLUME_READ_ONLY;
    Vcb->NtfsInfo.Flags &= ~NTFS_VOLUME_DIRTY;
    Status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(Applied);

Done:
    if (Page != NULL)
        ExFreePoolWithTag(Page, TAG_NTFS);
    if (Data != NULL)
        ReleaseAttributeContext(Data);
    NtfsFreeRecord(Vcb, LogRecord);
    InterlockedExchange(&NtfsLogBusy, 0);
    return Status;
}

VOID
NtfsLogFileRecord(PDEVICE_EXTENSION Vcb,
                  ULONGLONG MftIndex,
                  PFILE_RECORD_HEADER FileRecord)
{
    PFILE_RECORD_HEADER LogRecord = NULL;
    PNTFS_ATTR_CONTEXT Data = NULL;
    PUCHAR Page;
    ULONG Slot;
    ULONG Written;
    ULONGLONG PageOffset;

    if (InterlockedCompareExchange(&NtfsLogBusy, 1, 0) != 0)
        return;
    if (Vcb->MFTContext == NULL || MftIndex == NTFS_FILE_LOGFILE)
    {
        InterlockedExchange(&NtfsLogBusy, 0);
        return;
    }
    if ((Vcb->Flags & VCB_VOLUME_READ_ONLY) != 0)
    {
        InterlockedExchange(&NtfsLogBusy, 0);
        return;
    }

    if (!NT_SUCCESS(NtfsReadRecord(Vcb, NTFS_FILE_LOGFILE, &LogRecord)))
        goto Out;
    if (!NT_SUCCESS(FindAttribute(Vcb, LogRecord, AttributeData, L"", 0, &Data, NULL)))
        goto Out;

    Page = ExAllocatePoolWithTag(NonPagedPool, NTFS_LOG_PAGE, TAG_NTFS);
    if (Page == NULL)
        goto Out;

    if (ReadAttribute(Vcb, Data, 0, (PCHAR)Page, NTFS_LOG_PAGE) != NTFS_LOG_PAGE)
        goto FreePage;

    /* Leave a Windows log alone. Start one only on the blank log we format. */
    if (*(PULONG)Page != NTFS_LOG_FILE_MAGIC)
    {
        BOOLEAN Blank = TRUE;
        ULONG Index;

        for (Index = 0; Index < 64; Index++)
        {
            if (Page[Index] != 0xFF && Page[Index] != 0)
            {
                Blank = FALSE;
                break;
            }
        }
        if (!Blank)
            goto FreePage;

        RtlZeroMemory(Page, NTFS_LOG_PAGE);
        *(PULONG)Page = NTFS_LOG_FILE_MAGIC;
        WriteAttribute(Vcb, Data, 0, Page, NTFS_LOG_PAGE, &Written, NULL);
    }

    PageOffset = NTFS_LOG_PAGE;
    for (;;)
    {
        if (ReadAttribute(Vcb, Data, PageOffset, (PCHAR)Page, NTFS_LOG_PAGE) != NTFS_LOG_PAGE)
            break;
        if (*(PULONG)Page == 0 || *(PULONG)Page == 0xFFFFFFFF)
        {
            RtlZeroMemory(Page, NTFS_LOG_PAGE);
            *(PULONG)Page = NTFS_LOG_PAGE_MAGIC;
            Slot = 8;
            break;
        }
        if (*(PULONG)Page != NTFS_LOG_PAGE_MAGIC)
            goto FreePage;

        Slot = 8;
        while (Slot + 16 + Vcb->NtfsInfo.BytesPerFileRecord <= NTFS_LOG_PAGE &&
               *(PULONG)(Page + Slot) == NTFS_LOG_REC_MAGIC)
            Slot += 16 + Vcb->NtfsInfo.BytesPerFileRecord;
        if (Slot + 16 + Vcb->NtfsInfo.BytesPerFileRecord <= NTFS_LOG_PAGE)
            break;
        PageOffset += NTFS_LOG_PAGE;
    }

    if (Slot + 16 + Vcb->NtfsInfo.BytesPerFileRecord > NTFS_LOG_PAGE)
        goto FreePage;

    *(PULONG)(Page + Slot) = NTFS_LOG_REC_MAGIC;
    *(PULONGLONG)(Page + Slot + 4) = MftIndex;
    *(PULONG)(Page + Slot + 12) = Vcb->NtfsInfo.BytesPerFileRecord;
    RtlCopyMemory(Page + Slot + 16, FileRecord, Vcb->NtfsInfo.BytesPerFileRecord);
    WriteAttribute(Vcb, Data, PageOffset, Page, NTFS_LOG_PAGE, &Written, NULL);

FreePage:
    ExFreePoolWithTag(Page, TAG_NTFS);
Out:
    if (Data != NULL)
        ReleaseAttributeContext(Data);
    NtfsFreeRecord(Vcb, LogRecord);
    InterlockedExchange(&NtfsLogBusy, 0);
}

static
NTSTATUS
NtfsLookupSecurity(PDEVICE_EXTENSION Vcb,
                   const UCHAR *Body,
                   ULONG BodyLength,
                   PULONG SecurityId)
{
    PFILE_RECORD_HEADER Secure = NULL;
    PNTFS_ATTR_CONTEXT Sds = NULL;
    PUCHAR Buffer;
    ULONGLONG Offset = 0;
    ULONGLONG Size;
    ULONG Hash;
    NTSTATUS Status;

    *SecurityId = NTFS_SECURITY_ID;
    Status = NtfsReadRecord(Vcb, NTFS_FILE_SECURE, &Secure);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Secure, AttributeData, L"$SDS", 4, &Sds, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Secure);
        return Status;
    }

    Hash = NtfsHashSecurity(Body, BodyLength);
    Size = AttributeDataLength(Sds->pRecord);
    Buffer = ExAllocatePoolWithTag(NonPagedPool, 0x10000, TAG_NTFS);
    if (Buffer == NULL)
    {
        ReleaseAttributeContext(Sds);
        NtfsFreeRecord(Vcb, Secure);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (Offset + 0x18 < Size && Offset < NTFS_SDS_MIRROR)
    {
        ULONG Got = (ULONG)min(0x10000, Size - Offset);
        ULONG Cursor = 0;

        if (ReadAttribute(Vcb, Sds, Offset, (PCHAR)Buffer, Got) != Got)
            break;

        while (Cursor + 0x18 <= Got)
        {
            ULONG EntryHash = *(PULONG)(Buffer + Cursor);
            ULONG Id = *(PULONG)(Buffer + Cursor + 4);
            ULONG EntryLength = *(PULONG)(Buffer + Cursor + 0x10);

            if (EntryLength < 0x18 || EntryLength > Got - Cursor)
                break;
            if (EntryHash == Hash &&
                EntryLength - 0x14 == BodyLength &&
                RtlCompareMemory(Buffer + Cursor + 0x14, Body, BodyLength) == BodyLength)
            {
                *SecurityId = Id;
                ExFreePoolWithTag(Buffer, TAG_NTFS);
                ReleaseAttributeContext(Sds);
                NtfsFreeRecord(Vcb, Secure);
                return STATUS_SUCCESS;
            }
            Cursor = ALIGN_UP_BY(Cursor + EntryLength, 16);
        }
        Offset += Got;
    }

    ExFreePoolWithTag(Buffer, TAG_NTFS);
    ReleaseAttributeContext(Sds);
    NtfsFreeRecord(Vcb, Secure);
    return STATUS_NOT_FOUND;
}

static
NTSTATUS
NtfsAppendSecurity(PDEVICE_EXTENSION Vcb,
                   const UCHAR *Body,
                   ULONG BodyLength,
                   PULONG SecurityId)
{
    PFILE_RECORD_HEADER Secure = NULL;
    PNTFS_ATTR_CONTEXT Sds = NULL;
    PUCHAR Entry;
    ULONGLONG End;
    ULONG EntryLength;
    ULONG Id;
    ULONG Written;
    UCHAR Key[8];
    UCHAR Header[0x14];
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, NTFS_FILE_SECURE, &Secure);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Secure, AttributeData, L"$SDS", 4, &Sds, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Secure);
        return Status;
    }

    End = AttributeDataLength(Sds->pRecord);
    if (End >= NTFS_SDS_MIRROR)
        End = 0;
    /* The next id is one past the last entry we can see. 0x100 is the first. */
    Id = NTFS_SECURITY_ID;
    if (End >= 0x18)
    {
        UCHAR Tail[0x18];
        ULONGLONG Scan = 0;

        while (Scan + 0x18 <= End)
        {
            ULONG This = 0;
            if (ReadAttribute(Vcb, Sds, Scan + 0x10, (PCHAR)&This, 4) != 4 || This < 0x18)
                break;
            if (ReadAttribute(Vcb, Sds, Scan + 4, (PCHAR)Tail, 4) == 4)
                Id = max(Id, *(PULONG)Tail + 1);
            Scan = ALIGN_UP_BY(Scan + This, 16);
        }
    }

    EntryLength = ALIGN_UP_BY(0x14 + BodyLength, 16);
    Entry = ExAllocatePoolWithTag(NonPagedPool, EntryLength, TAG_NTFS);
    if (Entry == NULL)
    {
        ReleaseAttributeContext(Sds);
        NtfsFreeRecord(Vcb, Secure);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Entry, EntryLength);
    *(PULONG)Entry = NtfsHashSecurity(Body, BodyLength);
    *(PULONG)(Entry + 4) = Id;
    *(PULONGLONG)(Entry + 8) = End;
    *(PULONG)(Entry + 0x10) = 0x14 + BodyLength;
    RtlCopyMemory(Entry + 0x14, Body, BodyLength);

    Status = WriteAttribute(Vcb, Sds, End, Entry, EntryLength, &Written, Secure);
    if (NT_SUCCESS(Status) && End + NTFS_SDS_MIRROR + EntryLength <= AttributeAllocatedLength(Sds->pRecord))
        WriteAttribute(Vcb, Sds, End + NTFS_SDS_MIRROR, Entry, EntryLength, &Written, Secure);

    if (NT_SUCCESS(Status))
    {
        LARGE_INTEGER NewSize;
        NewSize.QuadPart = End + EntryLength;
        Sds->pRecord->NonResident.DataSize = NewSize.QuadPart;
        Sds->pRecord->NonResident.InitializedSize = NewSize.QuadPart;
        NtfsStoreAttribute(Vcb, Secure, Sds);

        RtlCopyMemory(Header, Entry, 0x14);
        *(PULONG)Key = *(PULONG)Entry;
        *(PULONG)(Key + 4) = Id;
        NtfsInsertRootEntry(Vcb, NTFS_FILE_SECURE, L"$SDH", Key, 8, Header, 0x14);
        *(PULONG)Key = Id;
        NtfsInsertRootEntry(Vcb, NTFS_FILE_SECURE, L"$SII", Key, 4, Header, 0x14);
        *SecurityId = Id;
    }

    ExFreePoolWithTag(Entry, TAG_NTFS);
    ReleaseAttributeContext(Sds);
    NtfsFreeRecord(Vcb, Secure);
    return Status;
}

NTSTATUS
NtfsAssignSecurity(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   ULONGLONG ParentMftIndex,
                   PSECURITY_DESCRIPTOR Explicit,
                   BOOLEAN IsDirectory)
{
    PSTANDARD_INFORMATION Info;
    ULONG InfoLength = 0;
    const UCHAR *Body = NULL;
    ULONG BodyLength = 0;
    ULONG SecurityId = NTFS_SECURITY_ID;
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR Parent = NULL;

    UNREFERENCED_PARAMETER(IsDirectory);

    Info = NtfsFindStandardInformation(Vcb, FileRecord, &InfoLength);
    if (Info == NULL || InfoLength < NTFS_STD_INFO_V31_SIZE)
    {
        NtfsCopyParentSecurityId(Vcb, FileRecord, ParentMftIndex);
        return STATUS_SUCCESS;
    }

    if (Explicit != NULL && RtlValidSecurityDescriptor(Explicit))
    {
        BodyLength = RtlLengthSecurityDescriptor(Explicit);
        Body = (const UCHAR *)Explicit;
    }
    else
    {
        NtfsCopyParentSecurityId(Vcb, FileRecord, ParentMftIndex);
        Status = STATUS_SUCCESS;
        Info = NtfsFindStandardInformation(Vcb, FileRecord, &InfoLength);
        if (Info == NULL)
            return STATUS_SUCCESS;
        /* The copied id already names a descriptor in $Secure. */
        return STATUS_SUCCESS;
    }

    Status = NtfsLookupSecurity(Vcb, Body, BodyLength, &SecurityId);
    if (!NT_SUCCESS(Status))
        Status = NtfsAppendSecurity(Vcb, Body, BodyLength, &SecurityId);
    if (!NT_SUCCESS(Status))
    {
        NtfsCopyParentSecurityId(Vcb, FileRecord, ParentMftIndex);
        return STATUS_SUCCESS;
    }

    Info->SecurityId = SecurityId;
    if (Info->OwnerId == 0)
        Info->OwnerId = 1;
    UNREFERENCED_PARAMETER(Parent);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsStampStandard(PDEVICE_EXTENSION Vcb,
                  PFILE_RECORD_HEADER Record,
                  ULONGLONG Usn,
                  ULONGLONG Charge)
{
    PSTANDARD_INFORMATION Info;
    ULONG Length = 0;

    Info = NtfsFindStandardInformation(Vcb, Record, &Length);
    if (Info == NULL || Length < NTFS_STD_INFO_V31_SIZE)
        return STATUS_SUCCESS;
    if (Usn != 0)
        Info->Usn = Usn;
    if (Charge != 0)
        Info->QuotaCharge = Charge;
    if (Info->OwnerId == 0)
        Info->OwnerId = 1;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsQueryObjectId(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PIRP Irp)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG OutLength;
    ULONG Copy;
    NTSTATUS Status;
    UCHAR ObjectId[64];

    OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(Vcb, Record, AttributeObjectId, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        ULONGLONG Extend;
        UCHAR Key[16];
        UCHAR Data[56];

        RtlZeroMemory(ObjectId, sizeof(ObjectId));
        *(PULONGLONG)ObjectId = Fcb->MFTIndex;
        *(PULONGLONG)(ObjectId + 8) = KeQueryInterruptTime();
        Status = NtfsAddResidentAttribute(Vcb, Record, AttributeObjectId, NULL, 0, ObjectId, 64);
        if (!NT_SUCCESS(Status))
        {
            NtfsFreeRecord(Vcb, Record);
            return Status;
        }
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);
        if (NT_SUCCESS(NtfsFindExtend(Vcb, L"$ObjId", &Extend)))
        {
            RtlCopyMemory(Key, ObjectId, 16);
            RtlZeroMemory(Data, sizeof(Data));
            *(PULONGLONG)Data = Fcb->MFTIndex;
            NtfsInsertRootEntry(Vcb, Extend, L"$O", Key, 16, Data, 56);
        }
    }
    else
    {
        Copy = min(Context->pRecord->Resident.ValueLength, sizeof(ObjectId));
        RtlZeroMemory(ObjectId, sizeof(ObjectId));
        RtlCopyMemory(ObjectId, (PUCHAR)Context->pRecord + Context->pRecord->Resident.ValueOffset, Copy);
        ReleaseAttributeContext(Context);
    }

    Copy = min(OutLength, sizeof(FILE_OBJECTID_BUFFER));
    if (Irp->AssociatedIrp.SystemBuffer != NULL && Copy != 0)
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, ObjectId, Copy);
    Irp->IoStatus.Information = Copy;
    NtfsFreeRecord(Vcb, Record);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsAddResidentAttribute(PDEVICE_EXTENSION Vcb,
                         PFILE_RECORD_HEADER Record,
                         ULONG Type,
                         PCWSTR Name,
                         UCHAR NameLength,
                         const UCHAR *Value,
                         ULONG ValueLength)
{
    PNTFS_ATTR_RECORD Attribute;
    ULONG Header;
    ULONG ValueOffset;
    ULONG AttributeLength;
    ULONG NameBytes = NameLength * sizeof(WCHAR);

    UNREFERENCED_PARAMETER(Vcb);
    Header = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ValueOffset = ALIGN_UP_BY(Header + NameBytes, VALUE_OFFSET_ALIGNMENT);
    AttributeLength = ALIGN_UP_BY(ValueOffset + ValueLength, ATTR_RECORD_ALIGNMENT);
    if (Record->BytesInUse + AttributeLength > Record->BytesAllocated)
        return STATUS_DISK_FULL;

    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Record + Record->AttributeOffset);
    while (Attribute->Type != AttributeEnd && Attribute->Length != 0)
        Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + Attribute->Length);

    RtlZeroMemory(Attribute, AttributeLength);
    Attribute->Type = Type;
    Attribute->Length = AttributeLength;
    Attribute->NameLength = NameLength;
    Attribute->NameOffset = (USHORT)Header;
    Attribute->Instance = Record->NextAttributeNumber++;
    Attribute->Resident.ValueLength = ValueLength;
    Attribute->Resident.ValueOffset = (USHORT)ValueOffset;
    if (NameBytes != 0)
        RtlCopyMemory((PUCHAR)Attribute + Header, Name, NameBytes);
    RtlCopyMemory((PUCHAR)Attribute + ValueOffset, Value, ValueLength);
    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + AttributeLength);
    SetFileRecordEnd(Record, Attribute, FILE_RECORD_END);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsSetReparsePoint(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PREPARSE_DATA_BUFFER Reparse = Buffer;
    NTSTATUS Status;
    ULONGLONG Extend;
    UCHAR Key[12];

    if (Length < REPARSE_DATA_BUFFER_HEADER_SIZE || Reparse->ReparseDataLength + REPARSE_DATA_BUFFER_HEADER_SIZE > Length)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsAddResidentAttribute(Vcb, Record, AttributeReparsePoint, NULL, 0, Buffer, Length);
    if (NT_SUCCESS(Status))
    {
        PSTANDARD_INFORMATION Info;
        ULONG InfoLength = 0;
        Info = NtfsFindStandardInformation(Vcb, Record, &InfoLength);
        if (Info != NULL)
            Info->FileAttribute |= NTFS_FILE_TYPE_REPARSE;
        Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_REPARSE;
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);
    }
    NtfsFreeRecord(Vcb, Record);
    if (!NT_SUCCESS(Status))
        return Status;

    if (NT_SUCCESS(NtfsFindExtend(Vcb, L"$Reparse", &Extend)))
    {
        *(PULONG)Key = Reparse->ReparseTag;
        *(PULONGLONG)(Key + 4) = Fcb->MFTIndex;
        NtfsInsertRootEntry(Vcb, Extend, L"$R", Key, 12, (PUCHAR)&Fcb->MFTIndex, sizeof(ULONGLONG));
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsGetReparsePoint(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PIRP Irp)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG OutLength;
    ULONG Have;
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeReparsePoint, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Have = Context->pRecord->Resident.ValueLength;
    OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    if (OutLength < Have)
        Status = STATUS_BUFFER_OVERFLOW;
    else
        Status = STATUS_SUCCESS;
    if (Irp->AssociatedIrp.SystemBuffer != NULL)
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                      (PUCHAR)Context->pRecord + Context->pRecord->Resident.ValueOffset,
                      min(OutLength, Have));
    Irp->IoStatus.Information = min(OutLength, Have);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsDeleteReparsePoint(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_REPARSE;
    return STATUS_SUCCESS;
}

ULONG
NtfsGetReparseTag(PDEVICE_EXTENSION Vcb, ULONGLONG MftIndex)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG Tag = 0;

    if (!NT_SUCCESS(NtfsReadRecord(Vcb, MftIndex, &Record)))
        return 0;
    if (NT_SUCCESS(FindAttribute(Vcb, Record, AttributeReparsePoint, L"", 0, &Context, NULL)))
    {
        if (Context->pRecord->Resident.ValueLength >= sizeof(ULONG))
            Tag = *(PULONG)((PUCHAR)Context->pRecord + Context->pRecord->Resident.ValueOffset);
        ReleaseAttributeContext(Context);
    }
    NtfsFreeRecord(Vcb, Record);
    return Tag;
}

/* $Quota:$Q value. A SID follows these 48 bytes when the owner id is not the
 * default. Threshold and limit are the fields FSCTL_SET_QUOTA changes. */
#define NTFS_QUOTA_CONTROL_SIZE     48
#define NTFS_QUOTA_CHANGE_OFFSET    16
#define NTFS_QUOTA_THRESHOLD_OFFSET 24
#define NTFS_QUOTA_LIMIT_OFFSET     32

static
PUCHAR
NtfsFindViewEntry(PINDEX_ROOT_ATTRIBUTE Root, const UCHAR *Key, USHORT KeyLength)
{
    PUCHAR End;
    PUCHAR Cursor;

    Cursor = (PUCHAR)&Root->Header + Root->Header.FirstEntryOffset;
    End = (PUCHAR)&Root->Header + Root->Header.TotalSizeOfEntries;
    while (Cursor + 16 <= End)
    {
        USHORT Length = *(PUSHORT)(Cursor + 8);
        USHORT ThisKey = *(PUSHORT)(Cursor + 10);
        USHORT Flags = *(PUSHORT)(Cursor + 12);

        if (Length < 16 || Cursor + Length > End)
            return NULL;
        if (Flags & NTFS_INDEX_ENTRY_END)
            return NULL;
        if (ThisKey == KeyLength &&
            RtlCompareMemory(Cursor + 16, Key, KeyLength) == KeyLength)
            return Cursor;
        Cursor += Length;
    }
    return NULL;
}

static
NTSTATUS
NtfsOpenResidentIndex(PDEVICE_EXTENSION Vcb,
                      ULONGLONG Directory,
                      PCWSTR Name,
                      PFILE_RECORD_HEADER *Record,
                      PNTFS_ATTR_CONTEXT *Context,
                      PINDEX_ROOT_ATTRIBUTE *Root)
{
    NTSTATUS Status;

    Status = NtfsReadRecord(Vcb, Directory, Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = FindAttribute(Vcb, *Record, AttributeIndexRoot, Name, (ULONG)wcslen(Name), Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, *Record);
        return Status;
    }

    if ((*Context)->pRecord->IsNonResident ||
        ((PINDEX_ROOT_ATTRIBUTE)((PUCHAR)(*Context)->pRecord +
         (*Context)->pRecord->Resident.ValueOffset))->Header.Flags != INDEX_ROOT_SMALL)
    {
        ReleaseAttributeContext(*Context);
        NtfsFreeRecord(Vcb, *Record);
        return STATUS_NOT_IMPLEMENTED;
    }

    *Root = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)(*Context)->pRecord +
                                    (*Context)->pRecord->Resident.ValueOffset);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsStoreResidentContext(PFILE_RECORD_HEADER Record, PNTFS_ATTR_CONTEXT Context)
{
    PNTFS_ATTR_RECORD Slot;

    Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Record->AttributeOffset);
    while (Slot->Type != AttributeEnd && Slot->Length != 0)
    {
        if (Slot->Instance == Context->pRecord->Instance &&
            Slot->Length == Context->pRecord->Length)
        {
            RtlCopyMemory(Slot, Context->pRecord, Slot->Length);
            return STATUS_SUCCESS;
        }
        Slot = (PNTFS_ATTR_RECORD)((PUCHAR)Slot + Slot->Length);
    }
    return STATUS_NOT_FOUND;
}

static
NTSTATUS
NtfsLookupQuotaOwner(PDEVICE_EXTENSION Vcb,
                     ULONGLONG QuotaFile,
                     const UCHAR *Sid,
                     USHORT SidLength,
                     PULONG OwnerId)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context;
    PINDEX_ROOT_ATTRIBUTE Root;
    PUCHAR Entry;
    USHORT DataOffset;
    USHORT DataLength;
    NTSTATUS Status;

    Status = NtfsOpenResidentIndex(Vcb, QuotaFile, L"$O", &Record, &Context, &Root);
    if (!NT_SUCCESS(Status))
        return Status;

    Entry = NtfsFindViewEntry(Root, Sid, SidLength);
    if (Entry == NULL)
        Status = STATUS_NOT_FOUND;
    else
    {
        DataOffset = *(PUSHORT)Entry;
        DataLength = *(PUSHORT)(Entry + 2);
        if (DataLength < sizeof(ULONG) || DataOffset + sizeof(ULONG) > *(PUSHORT)(Entry + 8))
            Status = STATUS_DISK_CORRUPT_ERROR;
        else
            *OwnerId = *(PULONG)(Entry + DataOffset);
    }

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

static
NTSTATUS
NtfsUpdateQuotaLimits(PDEVICE_EXTENSION Vcb,
                      ULONGLONG QuotaFile,
                      ULONG OwnerId,
                      LONGLONG ChangeTime,
                      LONGLONG Threshold,
                      LONGLONG Limit)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context;
    PINDEX_ROOT_ATTRIBUTE Root;
    PUCHAR Entry;
    PUCHAR Data;
    UCHAR Key[4];
    USHORT DataOffset;
    USHORT DataLength;
    NTSTATUS Status;

    *(PULONG)Key = OwnerId;
    Status = NtfsOpenResidentIndex(Vcb, QuotaFile, L"$Q", &Record, &Context, &Root);
    if (!NT_SUCCESS(Status))
        return Status;

    Entry = NtfsFindViewEntry(Root, Key, sizeof(Key));
    if (Entry == NULL)
        Status = STATUS_NOT_FOUND;
    else
    {
        DataOffset = *(PUSHORT)Entry;
        DataLength = *(PUSHORT)(Entry + 2);
        if (DataLength < NTFS_QUOTA_CONTROL_SIZE ||
            DataOffset + NTFS_QUOTA_CONTROL_SIZE > *(PUSHORT)(Entry + 8))
            Status = STATUS_DISK_CORRUPT_ERROR;
        else
        {
            Data = Entry + DataOffset;
            *(PLONGLONG)(Data + NTFS_QUOTA_CHANGE_OFFSET) = ChangeTime;
            *(PLONGLONG)(Data + NTFS_QUOTA_THRESHOLD_OFFSET) = Threshold;
            *(PLONGLONG)(Data + NTFS_QUOTA_LIMIT_OFFSET) = Limit;
            Status = NtfsStoreResidentContext(Record, Context);
            if (NT_SUCCESS(Status))
                Status = UpdateFileRecord(Vcb, QuotaFile, Record);
        }
    }

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsSetQuota(PDEVICE_EXTENSION Vcb, PVOID Buffer, ULONG Length)
{
    ULONG Offset = 0;
    ULONGLONG QuotaFile;
    NTSTATUS Status;

    if (Buffer == NULL || Length < FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid))
        return STATUS_INVALID_PARAMETER;

    Status = NtfsFindExtend(Vcb, L"$Quota", &QuotaFile);
    if (!NT_SUCCESS(Status))
        return Status;

    while (Offset + FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) <= Length)
    {
        PFILE_QUOTA_INFORMATION Entry;
        ULONG OwnerId = 1;

        Entry = (PFILE_QUOTA_INFORMATION)((PUCHAR)Buffer + Offset);
        /* An empty SID is the default quota, owner id 1. A SID selects the
         * user already recorded in $O. */
        if (Entry->SidLength != 0)
        {
            if (Entry->SidLength > 0xFFFF ||
                Offset + FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) + Entry->SidLength > Length)
                return STATUS_INVALID_PARAMETER;
            Status = NtfsLookupQuotaOwner(Vcb,
                                          QuotaFile,
                                          (const UCHAR *)&Entry->Sid,
                                          (USHORT)Entry->SidLength,
                                          &OwnerId);
            if (!NT_SUCCESS(Status))
                return Status;
        }

        Status = NtfsUpdateQuotaLimits(Vcb,
                                       QuotaFile,
                                       OwnerId,
                                       Entry->ChangeTime.QuadPart,
                                       Entry->QuotaThreshold.QuadPart,
                                       Entry->QuotaLimit.QuadPart);
        if (!NT_SUCCESS(Status))
            return Status;

        if (Entry->NextEntryOffset == 0)
            break;
        if (Entry->NextEntryOffset < FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) + Entry->SidLength)
            return STATUS_INVALID_PARAMETER;
        Offset += Entry->NextEntryOffset;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NtfsSetSparse(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PFILE_SET_SPARSE_BUFFER Request = Buffer;
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;
    BOOLEAN Set = TRUE;

    if (Length >= sizeof(FILE_SET_SPARSE_BUFFER) && Request != NULL)
        Set = Request->SetSparse;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Set)
    {
        Context->pRecord->Flags |= NTFS_ATTR_SPARSE;
        Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_SPARSE;
    }
    else
    {
        Context->pRecord->Flags &= ~NTFS_ATTR_SPARSE;
        Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_SPARSE;
    }
    Status = NtfsStoreAttribute(Vcb, Record, Context);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsSetZeroData(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PFILE_ZERO_DATA_INFORMATION Zero = Buffer;
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;
    LONGLONG Vbn;
    LONGLONG Lbn;
    LONGLONG Count;

    if (Length < sizeof(FILE_ZERO_DATA_INFORMATION) || Zero->BeyondFinalZero.QuadPart <= Zero->FileOffset.QuadPart)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Context->pRecord->IsNonResident && (Context->pRecord->Flags & NTFS_ATTR_SPARSE))
    {
        ULONGLONG Start = Zero->FileOffset.QuadPart / Vcb->NtfsInfo.BytesPerCluster;
        ULONGLONG End = (Zero->BeyondFinalZero.QuadPart + Vcb->NtfsInfo.BytesPerCluster - 1) / Vcb->NtfsInfo.BytesPerCluster;
        LONGLONG SavedV[64];
        LONGLONG SavedL[64];
        LONGLONG SavedC[64];
        ULONG Saved = 0;
        ULONG Index = 0;
        ULONG Lowest = MAXULONG;

        while (Saved < 64 &&
               FsRtlGetNextLargeMcbEntry(&Context->DataRunsMCB, Index, &Vbn, &Lbn, &Count))
        {
            SavedV[Saved] = Vbn;
            SavedL[Saved] = Lbn;
            SavedC[Saved] = Count;
            Saved++;
            Index++;
        }
        FsRtlTruncateLargeMcb(&Context->DataRunsMCB, 0);
        for (Index = 0; Index < Saved; Index++)
        {
            ULONGLONG RunStart = (ULONGLONG)SavedV[Index];
            ULONGLONG RunCount = (ULONGLONG)SavedC[Index];
            ULONGLONG RunEnd = RunStart + RunCount;
            ULONGLONG Keep;

            if (RunEnd <= Start || RunStart >= End)
            {
                FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], SavedC[Index]);
                continue;
            }
            if (RunStart < Start)
                FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], Start - RunStart);
            if (RunEnd > End)
            {
                Keep = End - RunStart;
                FsRtlAddLargeMcbEntry(&Context->DataRunsMCB,
                                      End,
                                      SavedL[Index] + (LONGLONG)Keep,
                                      RunEnd - End);
            }
            for (Keep = max(RunStart, Start); Keep < min(RunEnd, End); Keep++)
            {
                ULONGLONG Bit = (ULONGLONG)SavedL[Index] + (Keep - RunStart);
                if (NT_SUCCESS(NtfsBringClusterWindow(Vcb, Bit)))
                {
                    ULONG Base = (ULONG)(Vcb->ClusterWindowBase * 8);
                    RtlClearBits(&Vcb->ClusterBitmap, (ULONG)Bit - Base, 1);
                    Vcb->ClusterWindowDirty = TRUE;
                    Vcb->FreeClusterCount++;
                    if ((ULONG)Bit < Lowest)
                        Lowest = (ULONG)Bit;
                }
            }
        }
        if (Lowest != MAXULONG)
            NtfsWriteVolumeBitmap(Vcb, Lowest, 1);
        if (Context->pRecord->Flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_SPARSE))
            Context->pRecord->NonResident.CompressedSize = 0;
        {
            PUCHAR Pairs = (PUCHAR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset;
            ULONG Room = Context->pRecord->Length - Context->pRecord->NonResident.MappingPairsOffset;
            ULONG Used = 0;
            ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                      Context->pRecord->NonResident.HighestVCN + 1,
                                      Pairs,
                                      Room,
                                      &Used);
        }
        Status = NtfsStoreAttribute(Vcb, Record, Context);
    }
    else
        Status = STATUS_INVALID_PARAMETER;

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

#ifndef COMPRESSION_FORMAT_LZNT1
#define COMPRESSION_FORMAT_NONE  0x0000
#define COMPRESSION_FORMAT_LZNT1 0x0002
#endif

NTSTATUS
NtfsSetCompression(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    USHORT Format = COMPRESSION_FORMAT_NONE;
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;

    if (Length >= sizeof(USHORT) && Buffer != NULL)
        Format = *(PUSHORT)Buffer;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    if (Format == COMPRESSION_FORMAT_LZNT1)
    {
        if (Context->pRecord->IsNonResident)
            Context->pRecord->NonResident.CompressionUnit = 4;
        Context->pRecord->Flags |= 0x0001;
        Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_COMPRESSED;
    }
    else if (Format == COMPRESSION_FORMAT_NONE)
    {
        if (Context->pRecord->IsNonResident)
            Context->pRecord->NonResident.CompressionUnit = 0;
        Context->pRecord->Flags &= ~NTFS_ATTR_COMPRESSION_MASK;
        Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_COMPRESSED;
    }
    else
        Status = STATUS_NOT_SUPPORTED;

    if (NT_SUCCESS(Status))
        Status = NtfsStoreAttribute(Vcb, Record, Context);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsGetCompression(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PIRP Irp)
{
    USHORT Format = COMPRESSION_FORMAT_NONE;
    ULONG OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    PNTFS_FCB Fcb = FileObject->FsContext;

    UNREFERENCED_PARAMETER(Vcb);
    if (OutLength < sizeof(USHORT))
        return STATUS_BUFFER_TOO_SMALL;
    if (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_COMPRESSED)
        Format = COMPRESSION_FORMAT_LZNT1;
    *(PUSHORT)Irp->AssociatedIrp.SystemBuffer = Format;
    Irp->IoStatus.Information = sizeof(USHORT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsOpenJournal(PDEVICE_EXTENSION Vcb, PCWSTR Stream, PNTFS_ATTR_CONTEXT *Context, PFILE_RECORD_HEADER *Record)
{
    ULONGLONG Index = NTFS_USN_MFT;
    NTSTATUS Status;

    NtfsFindExtend(Vcb, L"$UsnJrnl", &Index);
    Status = NtfsReadRecord(Vcb, Index, Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, *Record, AttributeData, Stream, (ULONG)wcslen(Stream), Context, NULL);
    if (!NT_SUCCESS(Status))
        NtfsFreeRecord(Vcb, *Record);
    return Status;
}

NTSTATUS
NtfsCreateUsnJournal(PDEVICE_EXTENSION Vcb, PVOID Buffer, ULONG Length)
{
    PCREATE_USN_JOURNAL_DATA Request = Buffer;
    ULONGLONG Index = NTFS_USN_MFT;
    PFILE_RECORD_HEADER Record;
    UCHAR Max[32];
    NTSTATUS Status;

    if (Length < sizeof(CREATE_USN_JOURNAL_DATA))
        return STATUS_INVALID_PARAMETER;

    NtfsFindExtend(Vcb, L"$UsnJrnl", &Index);
    Status = NtfsReadRecord(Vcb, Index, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Max, sizeof(Max));
    *(PULONGLONG)Max = Request->MaximumSize;
    *(PULONGLONG)(Max + 8) = Request->AllocationDelta;
    *(PULONGLONG)(Max + 16) = KeQueryInterruptTime();
    Status = NtfsAddResidentAttribute(Vcb, Record, AttributeData, L"$Max", 4, Max, sizeof(Max));
    if (Status == STATUS_OBJECT_NAME_COLLISION || Status == STATUS_DISK_FULL)
        Status = STATUS_SUCCESS;
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Index, Record);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsQueryUsnJournal(PDEVICE_EXTENSION Vcb, PIRP Irp)
{
    PNTFS_ATTR_CONTEXT Context = NULL;
    PFILE_RECORD_HEADER Record = NULL;
    ULONG OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    UCHAR Max[32];
    NTSTATUS Status;

    Status = NtfsOpenJournal(Vcb, L"$Max", &Context, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(Max, sizeof(Max));
    ReadAttribute(Vcb, Context, 0, (PCHAR)Max, min(sizeof(Max), (ULONG)AttributeDataLength(Context->pRecord)));
    if (Irp->AssociatedIrp.SystemBuffer != NULL && OutLength != 0)
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Max, min(OutLength, sizeof(Max)));
    Irp->IoStatus.Information = min(OutLength, sizeof(Max));
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsReadUsnJournal(PDEVICE_EXTENSION Vcb, PVOID Buffer, ULONG Length, PIRP Irp)
{
    PREAD_USN_JOURNAL_DATA Request = Buffer;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PFILE_RECORD_HEADER Record = NULL;
    ULONG OutLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.FileSystemControl.OutputBufferLength;
    ULONG Got;
    NTSTATUS Status;

    if (Length < sizeof(READ_USN_JOURNAL_DATA) || OutLength < sizeof(USN))
        return STATUS_INVALID_PARAMETER;

    Status = NtfsOpenJournal(Vcb, L"$J", &Context, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    Got = (ULONG)min((ULONGLONG)OutLength - sizeof(USN),
                     AttributeDataLength(Context->pRecord) - (ULONGLONG)Request->StartUsn);
    if (Irp->AssociatedIrp.SystemBuffer != NULL)
    {
        *(USN *)Irp->AssociatedIrp.SystemBuffer = Request->StartUsn + Got;
        if (Got != 0)
            ReadAttribute(Vcb, Context, Request->StartUsn, (PCHAR)Irp->AssociatedIrp.SystemBuffer + sizeof(USN), Got);
    }
    Irp->IoStatus.Information = sizeof(USN) + Got;
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsDeleteUsnJournal(PDEVICE_EXTENSION Vcb)
{
    UNREFERENCED_PARAMETER(Vcb);
    return STATUS_SUCCESS;
}

/* Offset of a named attribute inside a file record, or 0 when it is absent.
 * An offset of 0 is the file-record header, never a real attribute. */
static
ULONG
NtfsAttributeOffset(PFILE_RECORD_HEADER Record,
                    ULONG Type,
                    PCWSTR Name,
                    USHORT NameLength)
{
    PNTFS_ATTR_RECORD Attribute;
    ULONG Offset;

    Offset = Record->AttributeOffset;
    while (Offset + sizeof(ULONG) < Record->BytesAllocated)
    {
        Attribute = (PNTFS_ATTR_RECORD)((PUCHAR)Record + Offset);
        if (Attribute->Type == AttributeEnd || Attribute->Length < 16)
            break;

        if (Attribute->Type == Type && Attribute->NameLength == NameLength &&
            (NameLength == 0 ||
             RtlCompareMemory((PUCHAR)Attribute + Attribute->NameOffset,
                              Name,
                              NameLength * sizeof(WCHAR)) == NameLength * sizeof(WCHAR)))
        {
            return Offset;
        }

        Offset += Attribute->Length;
    }

    return 0;
}

NTSTATUS
NtfsAppendUsn(PDEVICE_EXTENSION Vcb,
              ULONGLONG FileReference,
              ULONGLONG ParentReference,
              ULONG Reason,
              ULONG Attributes,
              PCUNICODE_STRING Name)
{
    PNTFS_ATTR_CONTEXT Context = NULL;
    PFILE_RECORD_HEADER Record = NULL;
    PNTFS_ATTR_RECORD Journal;
    USN Next;
    ULONG NameBytes;
    ULONG RecordLength;
    ULONG AttrOffset;
    PUCHAR Raw;
    ULONG Written;
    NTSTATUS Status;

    Status = NtfsOpenJournal(Vcb, L"$J", &Context, &Record);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    /* The file is already in the MFT. A journal problem must not fail the create. */
    AttrOffset = NtfsAttributeOffset(Record, AttributeData, L"$J", 2);
    if (AttrOffset == 0)
    {
        DPRINT1("USN journal $J attribute is missing\n");
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }

    NameBytes = (Name != NULL) ? Name->Length : 0;
    RecordLength = ALIGN_UP_BY(FIELD_OFFSET(USN_RECORD, FileName) + NameBytes, 8);
    Next = AttributeDataLength(Context->pRecord);
    Raw = ExAllocatePoolWithTag(NonPagedPool, RecordLength, TAG_NTFS);
    if (Raw == NULL)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(Raw, RecordLength);
    ((PUSN_RECORD)Raw)->RecordLength = RecordLength;
    ((PUSN_RECORD)Raw)->MajorVersion = 2;
    ((PUSN_RECORD)Raw)->MinorVersion = 0;
    ((PUSN_RECORD)Raw)->FileReferenceNumber = FileReference;
    ((PUSN_RECORD)Raw)->ParentFileReferenceNumber = ParentReference;
    ((PUSN_RECORD)Raw)->Usn = Next;
    KeQuerySystemTime(&((PUSN_RECORD)Raw)->TimeStamp);
    ((PUSN_RECORD)Raw)->Reason = Reason;
    ((PUSN_RECORD)Raw)->FileAttributes = Attributes;
    ((PUSN_RECORD)Raw)->FileNameLength = (USHORT)NameBytes;
    ((PUSN_RECORD)Raw)->FileNameOffset = FIELD_OFFSET(USN_RECORD, FileName);
    if (NameBytes != 0 && Name->Buffer != NULL)
        RtlCopyMemory(((PUSN_RECORD)Raw)->FileName, Name->Buffer, NameBytes);

    if (Context->pRecord->IsNonResident &&
        Next + RecordLength > (ULONGLONG)Context->pRecord->NonResident.AllocatedSize)
    {
        Status = NtfsExtendAllocation(Vcb,
                                      Context,
                                      AttrOffset,
                                      Record,
                                      ROUND_UP(Next + RecordLength, Vcb->NtfsInfo.BytesPerCluster));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("USN journal could not grow (Status %lx)\n", Status);
            ExFreePoolWithTag(Raw, TAG_NTFS);
            ReleaseAttributeContext(Context);
            NtfsFreeRecord(Vcb, Record);
            return STATUS_SUCCESS;
        }
    }
    Status = WriteAttribute(Vcb, Context, Next, Raw, RecordLength, &Written, Record);
    if (NT_SUCCESS(Status) && Context->pRecord->IsNonResident)
    {
        Journal = (PNTFS_ATTR_RECORD)((PUCHAR)Record + AttrOffset);
        Journal->NonResident.DataSize = Next + RecordLength;
        Journal->NonResident.InitializedSize = Next + RecordLength;
        Context->pRecord->NonResident.DataSize = Journal->NonResident.DataSize;
        Context->pRecord->NonResident.InitializedSize = Journal->NonResident.InitializedSize;
        UpdateFileRecord(Vcb, Record->MFTRecordNumber, Record);
    }
    else if (!NT_SUCCESS(Status))
    {
        DPRINT1("USN journal write failed (Status %lx)\n", Status);
    }
    ExFreePoolWithTag(Raw, TAG_NTFS);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);

    if (NT_SUCCESS(Status))
    {
        PFILE_RECORD_HEADER File;
        if (NT_SUCCESS(NtfsReadRecord(Vcb, FileReference & NTFS_MFT_MASK, &File)))
        {
            NtfsStampStandard(Vcb, File, Next, 0);
            UpdateFileRecord(Vcb, FileReference & NTFS_MFT_MASK, File);
            NtfsFreeRecord(Vcb, File);
        }
    }
    return Status;
}

static
ULONG
NtfsUnitClusters(PNTFS_ATTR_RECORD Attribute)
{
    if (Attribute->NonResident.CompressionUnit == 0)
        return 16;
    return 1u << Attribute->NonResident.CompressionUnit;
}

NTSTATUS
NtfsReadCompressed(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   PUCHAR Buffer,
                   ULONG Length,
                   ULONGLONG Offset,
                   PULONG LengthRead)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG Cluster;
    ULONG UnitClusters;
    ULONG UnitBytes;
    NTSTATUS Status;

    *LengthRead = 0;
    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }
    if (!Context->pRecord->IsNonResident)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_NOT_IMPLEMENTED;
    }

    Cluster = Vcb->NtfsInfo.BytesPerCluster;
    UnitClusters = NtfsUnitClusters(Context->pRecord);
    UnitBytes = UnitClusters * Cluster;

    while (Length > 0)
    {
        ULONGLONG Unit = Offset / UnitBytes;
        ULONG Skip = (ULONG)(Offset % UnitBytes);
        ULONG Take = min(Length, UnitBytes - Skip);
        PUCHAR Plain;
        PUCHAR Comp;
        ULONG Allocated = 0;
        ULONG Index;
        ULONG Final = 0;
        PVOID Work = NULL;
        ULONG WorkSize = 0;
        ULONG Frag = 0;

        Plain = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        if (Plain == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(Plain, UnitBytes);

        for (Index = 0; Index < UnitClusters; Index++)
        {
            LONGLONG Lbn, Count;
            if (FsRtlLookupLargeMcbEntry(&Context->DataRunsMCB,
                                         Unit * UnitClusters + Index,
                                         &Lbn, &Count, NULL, NULL, NULL) &&
                Lbn != -1)
                Allocated++;
            else
                break;
        }

        if (Allocated == UnitClusters)
        {
            ReadAttribute(Vcb, Context, Unit * UnitBytes, (PCHAR)Plain, UnitBytes);
        }
        else if (Allocated != 0)
        {
            Comp = ExAllocatePoolWithTag(NonPagedPool, Allocated * Cluster, TAG_NTFS);
            if (Comp == NULL)
            {
                ExFreePoolWithTag(Plain, TAG_NTFS);
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            ReadAttribute(Vcb, Context, Unit * UnitBytes, (PCHAR)Comp, Allocated * Cluster);
            RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_LZNT1, &WorkSize, &Frag);
            if (WorkSize != 0)
                Work = ExAllocatePoolWithTag(NonPagedPool, WorkSize, TAG_NTFS);
            Status = RtlDecompressBuffer(COMPRESSION_FORMAT_LZNT1, Plain, UnitBytes, Comp, Allocated * Cluster, &Final);
            if (Work != NULL)
                ExFreePoolWithTag(Work, TAG_NTFS);
            ExFreePoolWithTag(Comp, TAG_NTFS);
            if (!NT_SUCCESS(Status) && Status != STATUS_BAD_COMPRESSION_BUFFER)
            {
                ExFreePoolWithTag(Plain, TAG_NTFS);
                break;
            }
            Status = STATUS_SUCCESS;
        }

        RtlCopyMemory(Buffer, Plain + Skip, Take);
        ExFreePoolWithTag(Plain, TAG_NTFS);
        Buffer += Take;
        Offset += Take;
        Length -= Take;
        *LengthRead += Take;
    }

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsWriteCompressed(PDEVICE_EXTENSION Vcb,
                    PNTFS_FCB Fcb,
                    const PUCHAR Buffer,
                    ULONG Length,
                    ULONGLONG Offset,
                    PULONG LengthWritten)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    ULONG Cluster;
    ULONG UnitClusters;
    ULONG UnitBytes;
    NTSTATUS Status;

    *LengthWritten = 0;
    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Cluster = Vcb->NtfsInfo.BytesPerCluster;
    UnitClusters = NtfsUnitClusters(Context->pRecord);
    UnitBytes = UnitClusters * Cluster;

    {
    PUCHAR Input = (PUCHAR)Buffer;
    while (Length > 0 && NT_SUCCESS(Status))
    {
        ULONGLONG Unit = Offset / UnitBytes;
        ULONG Skip = (ULONG)(Offset % UnitBytes);
        ULONG Take = min(Length, UnitBytes - Skip);
        PUCHAR Plain;
        PUCHAR Comp;
        ULONG Final = 0;
        ULONG WorkSize = 0;
        ULONG Frag = 0;
        PVOID Work;
        ULONG Written = 0;

        Plain = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        Comp = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        if (Plain == NULL || Comp == NULL)
        {
            if (Plain != NULL)
                ExFreePoolWithTag(Plain, TAG_NTFS);
            if (Comp != NULL)
                ExFreePoolWithTag(Comp, TAG_NTFS);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(Plain, UnitBytes);
        ReadAttribute(Vcb, Context, Unit * UnitBytes, (PCHAR)Plain, UnitBytes);
        RtlCopyMemory(Plain + Skip, Input, Take);

        RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_LZNT1, &WorkSize, &Frag);
        Work = (WorkSize != 0) ? ExAllocatePoolWithTag(NonPagedPool, WorkSize, TAG_NTFS) : NULL;
        Status = RtlCompressBuffer(COMPRESSION_FORMAT_LZNT1,
                                   Plain,
                                   UnitBytes,
                                   Comp,
                                   UnitBytes - Cluster,
                                   0x1000,
                                   &Final,
                                   Work);
        if (!NT_SUCCESS(Status))
        {
            /* A unit that does not shrink is stored uncompressed. */
            Status = WriteAttribute(Vcb, Context, Unit * UnitBytes, Plain, UnitBytes, &Written, Record);
            Final = UnitBytes;
        }
        else
        {
            ULONG KeepClusters = (Final + Cluster - 1) / Cluster;
            ULONGLONG Tail = Unit * UnitClusters + KeepClusters;
            ULONGLONG UnitEnd = Unit * UnitClusters + UnitClusters;
            LONGLONG Vbn;
            LONGLONG Lbn;
            LONGLONG Count;
            LONGLONG SavedV[32];
            LONGLONG SavedL[32];
            LONGLONG SavedC[32];
            ULONG Saved = 0;
            ULONG Index = 0;

            Status = WriteAttribute(Vcb, Context, Unit * UnitBytes, Comp, Final, &Written, Record);
            while (Saved < 32 &&
                   FsRtlGetNextLargeMcbEntry(&Context->DataRunsMCB, Index, &Vbn, &Lbn, &Count))
            {
                SavedV[Saved] = Vbn;
                SavedL[Saved] = Lbn;
                SavedC[Saved] = Count;
                Saved++;
                Index++;
            }
            FsRtlTruncateLargeMcb(&Context->DataRunsMCB, 0);
            for (Index = 0; Index < Saved && NT_SUCCESS(Status); Index++)
            {
                ULONGLONG RunStart = (ULONGLONG)SavedV[Index];
                ULONGLONG RunEnd = RunStart + (ULONGLONG)SavedC[Index];
                ULONGLONG Bit;

                if (RunEnd <= Tail || RunStart >= UnitEnd)
                {
                    FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], SavedC[Index]);
                    continue;
                }
                if (RunStart < Tail)
                    FsRtlAddLargeMcbEntry(&Context->DataRunsMCB, SavedV[Index], SavedL[Index], Tail - RunStart);
                if (RunEnd > UnitEnd)
                    FsRtlAddLargeMcbEntry(&Context->DataRunsMCB,
                                          UnitEnd,
                                          SavedL[Index] + (LONGLONG)(UnitEnd - RunStart),
                                          RunEnd - UnitEnd);
                for (Bit = max(RunStart, Tail); Bit < min(RunEnd, UnitEnd); Bit++)
                {
                    ULONGLONG Disk = (ULONGLONG)SavedL[Index] + (Bit - RunStart);
                    if (NT_SUCCESS(NtfsBringClusterWindow(Vcb, Disk)))
                    {
                        ULONG Base = (ULONG)(Vcb->ClusterWindowBase * 8);
                        RtlClearBits(&Vcb->ClusterBitmap, (ULONG)Disk - Base, 1);
                        Vcb->ClusterWindowDirty = TRUE;
                    }
                }
            }
            if (NT_SUCCESS(Status))
            {
                PUCHAR Pairs = (PUCHAR)Context->pRecord + Context->pRecord->NonResident.MappingPairsOffset;
                ULONG Room = Context->pRecord->Length - Context->pRecord->NonResident.MappingPairsOffset;
                ULONG Used = 0;
                NtfsWriteVolumeBitmap(Vcb, 0, 1);
                ConvertLargeMCBToDataRuns(&Context->DataRunsMCB,
                                          Context->pRecord->NonResident.HighestVCN + 1,
                                          Pairs,
                                          Room,
                                          &Used);
                NtfsStoreAttribute(Vcb, Record, Context);
            }
        }
        if (Work != NULL)
            ExFreePoolWithTag(Work, TAG_NTFS);
        ExFreePoolWithTag(Plain, TAG_NTFS);
        ExFreePoolWithTag(Comp, TAG_NTFS);
        if (!NT_SUCCESS(Status))
            break;

        Input += Take;
        Offset += Take;
        Length -= Take;
        *LengthWritten += Take;
        UNREFERENCED_PARAMETER(Final);
    }
    }

    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

/* AES-256, one 512-byte sector at a time. The IV is the sector offset.
 * A file with no unwrapped key never returns or accepts plaintext. */
static const UCHAR AesSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const UCHAR AesInvSbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static
UCHAR
AesMul(UCHAR Value, UCHAR Factor)
{
    UCHAR Result = 0;
    ULONG Bit;

    for (Bit = 0; Bit < 8; Bit++)
    {
        if (Factor & 1)
            Result ^= Value;
        Factor >>= 1;
        Value = (UCHAR)((Value << 1) ^ ((Value & 0x80) ? 0x1b : 0));
    }
    return Result;
}

static
VOID
AesExpand(const UCHAR *Key, UCHAR *Round)
{
    static const UCHAR Rcon[] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    ULONG Index;

    RtlCopyMemory(Round, Key, 32);
    for (Index = 8; Index < 60; Index++)
    {
        UCHAR Temp[4];
        RtlCopyMemory(Temp, Round + (Index - 1) * 4, 4);
        if ((Index % 8) == 0)
        {
            UCHAR First = Temp[0];
            Temp[0] = (UCHAR)(AesSbox[Temp[1]] ^ Rcon[(Index / 8) - 1]);
            Temp[1] = AesSbox[Temp[2]];
            Temp[2] = AesSbox[Temp[3]];
            Temp[3] = AesSbox[First];
        }
        else if ((Index % 8) == 4)
        {
            Temp[0] = AesSbox[Temp[0]];
            Temp[1] = AesSbox[Temp[1]];
            Temp[2] = AesSbox[Temp[2]];
            Temp[3] = AesSbox[Temp[3]];
        }
        *(PULONG)(Round + Index * 4) = *(PULONG)(Round + (Index - 8) * 4) ^ *(PULONG)Temp;
    }
}

static
VOID
AesShiftRows(UCHAR *State, BOOLEAN Inverse)
{
    UCHAR T;
    if (!Inverse)
    {
        T = State[1]; State[1] = State[5]; State[5] = State[9]; State[9] = State[13]; State[13] = T;
        T = State[2]; State[2] = State[10]; State[10] = T;
        T = State[6]; State[6] = State[14]; State[14] = T;
        T = State[3]; State[3] = State[15]; State[15] = State[11]; State[11] = State[7]; State[7] = T;
    }
    else
    {
        T = State[13]; State[13] = State[9]; State[9] = State[5]; State[5] = State[1]; State[1] = T;
        T = State[2]; State[2] = State[10]; State[10] = T;
        T = State[6]; State[6] = State[14]; State[14] = T;
        T = State[7]; State[7] = State[11]; State[11] = State[15]; State[15] = State[3]; State[3] = T;
    }
}

static
VOID
AesMix(UCHAR *State, BOOLEAN Inverse)
{
    ULONG Column;
    for (Column = 0; Column < 4; Column++)
    {
        UCHAR A0 = State[Column];
        UCHAR A1 = State[4 + Column];
        UCHAR A2 = State[8 + Column];
        UCHAR A3 = State[12 + Column];
        if (!Inverse)
        {
            State[Column] = (UCHAR)(AesMul(A0, 2) ^ AesMul(A1, 3) ^ A2 ^ A3);
            State[4 + Column] = (UCHAR)(A0 ^ AesMul(A1, 2) ^ AesMul(A2, 3) ^ A3);
            State[8 + Column] = (UCHAR)(A0 ^ A1 ^ AesMul(A2, 2) ^ AesMul(A3, 3));
            State[12 + Column] = (UCHAR)(AesMul(A0, 3) ^ A1 ^ A2 ^ AesMul(A3, 2));
        }
        else
        {
            State[Column] = (UCHAR)(AesMul(A0, 14) ^ AesMul(A1, 11) ^ AesMul(A2, 13) ^ AesMul(A3, 9));
            State[4 + Column] = (UCHAR)(AesMul(A0, 9) ^ AesMul(A1, 14) ^ AesMul(A2, 11) ^ AesMul(A3, 13));
            State[8 + Column] = (UCHAR)(AesMul(A0, 13) ^ AesMul(A1, 9) ^ AesMul(A2, 14) ^ AesMul(A3, 11));
            State[12 + Column] = (UCHAR)(AesMul(A0, 11) ^ AesMul(A1, 13) ^ AesMul(A2, 9) ^ AesMul(A3, 14));
        }
    }
}

static
VOID
AesBlock(const UCHAR *Round, const UCHAR *In, UCHAR *Out, BOOLEAN Decrypt)
{
    UCHAR State[16];
    ULONG RoundIndex;
    ULONG Index;

    RtlCopyMemory(State, In, 16);
    if (!Decrypt)
    {
        for (Index = 0; Index < 16; Index++)
            State[Index] ^= Round[Index];
        for (RoundIndex = 1; RoundIndex <= 14; RoundIndex++)
        {
            for (Index = 0; Index < 16; Index++)
                State[Index] = AesSbox[State[Index]];
            AesShiftRows(State, FALSE);
            if (RoundIndex < 14)
                AesMix(State, FALSE);
            for (Index = 0; Index < 16; Index++)
                State[Index] ^= Round[RoundIndex * 16 + Index];
        }
    }
    else
    {
        for (Index = 0; Index < 16; Index++)
            State[Index] ^= Round[14 * 16 + Index];
        for (RoundIndex = 13; ; RoundIndex--)
        {
            AesShiftRows(State, TRUE);
            for (Index = 0; Index < 16; Index++)
                State[Index] = AesInvSbox[State[Index]];
            for (Index = 0; Index < 16; Index++)
                State[Index] ^= Round[RoundIndex * 16 + Index];
            if (RoundIndex == 0)
                break;
            AesMix(State, TRUE);
        }
    }
    RtlCopyMemory(Out, State, 16);
}

static
VOID
NtfsCryptBuffer(PNTFS_FCB Fcb, PUCHAR Data, ULONG Length, ULONGLONG Offset, BOOLEAN Decrypt)
{
    UCHAR Round[15 * 16];
    UCHAR Iv[16];
    UCHAR Block[16];
    ULONG Index;

    AesExpand(Fcb->FileKey, Round);
    while (Length >= 16)
    {
        RtlZeroMemory(Iv, sizeof(Iv));
        *(PULONGLONG)Iv = Offset;
        if (!Decrypt)
        {
            for (Index = 0; Index < 16; Index++)
                Data[Index] ^= Iv[Index];
            AesBlock(Round, Data, Data, FALSE);
        }
        else
        {
            AesBlock(Round, Data, Block, TRUE);
            for (Index = 0; Index < 16; Index++)
                Data[Index] = (UCHAR)(Block[Index] ^ Iv[Index]);
        }
        Data += 16;
        Length -= 16;
        Offset += 16;
    }
}

static
NTSTATUS
NtfsCryptFile(PDEVICE_EXTENSION Vcb,
              PNTFS_FCB Fcb,
              PUCHAR Buffer,
              ULONG Length,
              ULONGLONG Offset,
              PULONG Transferred,
              BOOLEAN Write)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;
    PUCHAR Block;

    *Transferred = 0;
    if (!Fcb->HasFileKey || Fcb->FileKeyLength != 32)
        return STATUS_ACCESS_DENIED;
    if ((Offset % NTFS_EFS_BLOCK) != 0 || (Length % NTFS_EFS_BLOCK) != 0)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream), &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtfsFreeRecord(Vcb, Record);
        return Status;
    }

    Block = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);
    if (Block == NULL)
    {
        ReleaseAttributeContext(Context);
        NtfsFreeRecord(Vcb, Record);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Write)
    {
        ULONG Written = 0;
        RtlCopyMemory(Block, Buffer, Length);
        NtfsCryptBuffer(Fcb, Block, Length, Offset, FALSE);
        Status = WriteAttribute(Vcb, Context, Offset, Block, Length, &Written, Record);
        if (NT_SUCCESS(Status))
            *Transferred = Written;
    }
    else
    {
        if (ReadAttribute(Vcb, Context, Offset, (PCHAR)Block, Length) != Length)
            Status = STATUS_END_OF_FILE;
        else
        {
            NtfsCryptBuffer(Fcb, Block, Length, Offset, TRUE);
            RtlCopyMemory(Buffer, Block, Length);
            *Transferred = Length;
        }
    }

    ExFreePoolWithTag(Block, TAG_NTFS);
    ReleaseAttributeContext(Context);
    NtfsFreeRecord(Vcb, Record);
    return Status;
}

NTSTATUS
NtfsReadEncrypted(PDEVICE_EXTENSION Vcb, PNTFS_FCB Fcb, PUCHAR Buffer, ULONG Length, ULONGLONG Offset, PULONG LengthRead)
{
    return NtfsCryptFile(Vcb, Fcb, Buffer, Length, Offset, LengthRead, FALSE);
}

NTSTATUS
NtfsWriteEncrypted(PDEVICE_EXTENSION Vcb, PNTFS_FCB Fcb, const PUCHAR Buffer, ULONG Length, ULONGLONG Offset, PULONG LengthWritten)
{
    return NtfsCryptFile(Vcb, Fcb, (PUCHAR)Buffer, Length, Offset, LengthWritten, TRUE);
}

NTSTATUS
NtfsSetEncryption(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject, PVOID Buffer, ULONG Length)
{
    PENCRYPTION_BUFFER Request = Buffer;
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    const UCHAR *Blob;
    ULONG BlobLength;
    NTSTATUS Status;

    if (Length < sizeof(ENCRYPTION_BUFFER) || Request == NULL)
        return STATUS_INVALID_PARAMETER;

    Blob = Request->Private;
    BlobLength = Length - FIELD_OFFSET(ENCRYPTION_BUFFER, Private);
    /* The service passes the $EFS stream. A 32-byte FEK after it is the
     * unwrapped key. The key is not written into the attribute. */
    if (BlobLength > 32)
        BlobLength -= 32;

    Status = NtfsReadRecord(Vcb, Fcb->MFTIndex, &Record);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Request->EncryptionOperation == FILE_CLEAR_ENCRYPTION ||
        Request->EncryptionOperation == STREAM_CLEAR_ENCRYPTION)
    {
        Fcb->HasFileKey = FALSE;
        Fcb->FileKeyLength = 0;
        Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_ENCRYPTED;
    }
    else
    {
        Status = NtfsAddResidentAttribute(Vcb, Record, AttributeLoggedUtilityStream, L"$EFS", 4, Blob, BlobLength);
        if (NT_SUCCESS(Status))
        {
            PSTANDARD_INFORMATION Info;
            ULONG InfoLength = 0;
            Info = NtfsFindStandardInformation(Vcb, Record, &InfoLength);
            if (Info != NULL)
                Info->FileAttribute |= NTFS_FILE_TYPE_ENCRYPTED;
            Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_ENCRYPTED;
            if (Length >= FIELD_OFFSET(ENCRYPTION_BUFFER, Private) + BlobLength + 32)
            {
                RtlCopyMemory(Fcb->FileKey, Request->Private + BlobLength, 32);
                Fcb->FileKeyLength = 32;
                Fcb->HasFileKey = TRUE;
            }
            Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);
        }
    }

    if (NT_SUCCESS(Status) &&
        (Request->EncryptionOperation == FILE_CLEAR_ENCRYPTION ||
         Request->EncryptionOperation == STREAM_CLEAR_ENCRYPTION))
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, Record);

    NtfsFreeRecord(Vcb, Record);
    return Status;
}
