/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Calls the public ntoskrnl and HAL names and prints one DIFF line each.
 * Names with no case are left for a later probe. This file links only
 * MmGetSystemRoutineAddress and does not include a WDK header.
 */

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned short WCHAR;
typedef unsigned long UINT32;
typedef unsigned long long UINT64;
typedef long NTSTATUS;
typedef void *PVOID;

typedef struct _USTR
{
    UINT16 Length;
    UINT16 MaximumLength;
    WCHAR *Buffer;
} USTR;

typedef struct _OBJATTR
{
    UINT32 Length;
    PVOID RootDirectory;
    USTR *ObjectName;
    UINT32 Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJATTR;

typedef struct _IO_STATUS_BLOCK
{
    union
    {
        NTSTATUS Status;
        PVOID Pointer;
    } u;
    UINT64 Information;
} IO_STATUS_BLOCK;

typedef struct _LARGE_INTEGER
{
    UINT64 QuadPart;
} LARGE_INTEGER;

__declspec(dllimport) PVOID __stdcall MmGetSystemRoutineAddress(USTR *Name);

typedef UINT32 (__stdcall *FN_DBG)(const char *Text);
typedef void (__stdcall *FN_INITSTR)(USTR *Dest, const WCHAR *Src);
typedef PVOID (__stdcall *FN_ALLOC)(UINT32 PoolType, UINT64 Bytes, UINT32 Tag);
typedef void (__stdcall *FN_FREE)(PVOID P, UINT32 Tag);
typedef void (__stdcall *FN_INIT_EVENT)(PVOID Event, UINT32 Type, UINT8 State);
typedef long (__stdcall *FN_SET_EVENT)(PVOID Event, long Increment, UINT8 Wait);
typedef NTSTATUS (__stdcall *FN_WAIT)(PVOID Object, UINT32 Reason, UINT32 Mode,
                                      UINT8 Alertable, LARGE_INTEGER *Timeout);
typedef PVOID (__stdcall *FN_ALLOC_IRP)(char StackSize, UINT8 ChargeQuota);
typedef void (__stdcall *FN_FREE_IRP)(PVOID Irp);
typedef NTSTATUS (__stdcall *FN_CLOSE)(PVOID Handle);
typedef NTSTATUS (__stdcall *FN_CREATE_FILE)(PVOID *Handle, UINT32 Access, OBJATTR *Attr,
                                             IO_STATUS_BLOCK *Io, LARGE_INTEGER *Alloc,
                                             UINT32 Attrs, UINT32 Share, UINT32 Disp,
                                             UINT32 Options, PVOID Ea, UINT32 EaLen);
typedef NTSTATUS (__stdcall *FN_CREATE_DEV)(PVOID Driver, UINT32 Ext, USTR *Name,
                                            UINT32 Type, UINT32 Chars, UINT8 Exclusive,
                                            PVOID *Device);
typedef void (__stdcall *FN_DEL_DEV)(PVOID Device);
typedef NTSTATUS (__stdcall *FN_LINK)(USTR *Link, USTR *Target);
typedef NTSTATUS (__stdcall *FN_UNLINK)(USTR *Link);

static void
outb(UINT16 Port, UINT8 Value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(Value), "Nd"(Port));
}

static UINT8
inb(UINT16 Port)
{
    UINT8 Value;

    __asm__ __volatile__("inb %1, %0" : "=a"(Value) : "Nd"(Port));
    return Value;
}

static void
uart_open(void)
{
    outb(0x3FB, 0x80);
    outb(0x3F8, 1);
    outb(0x3F9, 0);
    outb(0x3FB, 0x03);
    outb(0x3F9, 0x00);
    outb(0x3FA, 0x07);
}

static void
uart_write(const char *Text)
{
    UINT32 Spins;
    const char *Cursor;

    for (Cursor = Text; *Cursor != '\0'; Cursor++)
    {
        for (Spins = 0; Spins < 100000; Spins++)
        {
            if ((inb(0x3FD) & 0x20) != 0)
            {
                break;
            }
        }
        outb(0x3F8, (UINT8)*Cursor);
    }
}

static void
emit(const char *Name, const char *Token)
{
    uart_write("DIFF ");
    uart_write(Name);
    uart_write(" ");
    uart_write(Token);
    uart_write("\r\n");
}

static void
emit_hex(const char *Name, UINT32 Value)
{
    char Text[11];
    int Index;
    static const char Digits[] = "0123456789ABCDEF";

    Text[0] = '0';
    Text[1] = 'x';
    for (Index = 0; Index < 8; Index++)
    {
        Text[2 + Index] = Digits[(Value >> (28 - (Index * 4))) & 0xF];
    }
    Text[10] = '\0';
    emit(Name, Text);
}

static void
set_name(USTR *Name, const WCHAR *Text)
{
    UINT16 Count;

    Count = 0;
    while (Text[Count] != 0)
    {
        Count++;
    }
    Name->Length = (UINT16)(Count * sizeof(WCHAR));
    Name->MaximumLength = (UINT16)(Name->Length + sizeof(WCHAR));
    Name->Buffer = (WCHAR *)Text;
}

static PVOID
resolve(const WCHAR *Text)
{
    USTR Name;

    set_name(&Name, Text);
    return MmGetSystemRoutineAddress(&Name);
}

static void
probe_strings(void)
{
    FN_INITSTR Init;
    USTR Local;

    Init = (FN_INITSTR)resolve(L"RtlInitUnicodeString");
    if (Init == 0)
    {
        emit("RtlInitUnicodeString", "MISSING");
        return;
    }
    Local.Length = 0;
    Local.MaximumLength = 0;
    Local.Buffer = 0;
    Init(&Local, L"KernelAbi");
    emit("RtlInitUnicodeString", "RETURNED");
}

static void
probe_debug(void)
{
    FN_DBG Print;

    Print = (FN_DBG)resolve(L"DbgPrint");
    if (Print == 0)
    {
        emit("DbgPrint", "MISSING");
        return;
    }
    Print(".");
    emit("DbgPrint", "RETURNED");
}

static void
probe_lookup(void)
{
    if (resolve(L"DbgPrint") == 0)
    {
        emit("MmGetSystemRoutineAddress", "NULL");
        return;
    }
    emit("MmGetSystemRoutineAddress", "RETURNED");
}

static void
probe_pool(void)
{
    FN_ALLOC Alloc;
    FN_FREE Free;
    PVOID Block;

    Alloc = (FN_ALLOC)resolve(L"ExAllocatePoolWithTag");
    if (Alloc == 0)
    {
        emit("ExAllocatePoolWithTag", "MISSING");
    }
    else
    {
        Block = Alloc(0, 32, 0x4142494B);
        if (Block == 0)
        {
            emit("ExAllocatePoolWithTag", "NULL");
        }
        else
        {
            emit("ExAllocatePoolWithTag", "RETURNED");
            Free = (FN_FREE)resolve(L"ExFreePoolWithTag");
            if (Free == 0)
            {
                emit("ExFreePoolWithTag", "MISSING");
            }
            else
            {
                Free(Block, 0x4142494B);
                emit("ExFreePoolWithTag", "RETURNED");
            }
        }
    }
}

static void
probe_event(void)
{
    FN_INIT_EVENT Init;
    FN_SET_EVENT SetEvent;
    FN_WAIT Wait;
    UINT8 Event[64];
    UINT32 Index;
    LARGE_INTEGER Timeout;
    long Previous;

    for (Index = 0; Index < sizeof(Event); Index++)
    {
        Event[Index] = 0;
    }
    Init = (FN_INIT_EVENT)resolve(L"KeInitializeEvent");
    if (Init == 0)
    {
        emit("KeInitializeEvent", "MISSING");
        return;
    }
    Init(Event, 0, 0);
    emit("KeInitializeEvent", "RETURNED");

    SetEvent = (FN_SET_EVENT)resolve(L"KeSetEvent");
    if (SetEvent == 0)
    {
        emit("KeSetEvent", "MISSING");
    }
    else
    {
        Previous = SetEvent(Event, 0, 0);
        emit_hex("KeSetEvent", (UINT32)Previous);
    }

    Wait = (FN_WAIT)resolve(L"KeWaitForSingleObject");
    if (Wait == 0)
    {
        emit("KeWaitForSingleObject", "MISSING");
        return;
    }
    Timeout.QuadPart = 0;
    emit_hex("KeWaitForSingleObject", (UINT32)Wait(Event, 0, 0, 0, &Timeout));
}

static void
probe_irp(void)
{
    FN_ALLOC_IRP Alloc;
    FN_FREE_IRP Free;
    PVOID Irp;

    Alloc = (FN_ALLOC_IRP)resolve(L"IoAllocateIrp");
    if (Alloc == 0)
    {
        emit("IoAllocateIrp", "MISSING");
        return;
    }
    Irp = Alloc(1, 0);
    if (Irp == 0)
    {
        emit("IoAllocateIrp", "NULL");
        return;
    }
    emit("IoAllocateIrp", "RETURNED");
    Free = (FN_FREE_IRP)resolve(L"IoFreeIrp");
    if (Free == 0)
    {
        emit("IoFreeIrp", "MISSING");
        return;
    }
    Free(Irp);
    emit("IoFreeIrp", "RETURNED");
}

static void
probe_files(void)
{
    FN_CLOSE Close;
    FN_CREATE_FILE Create;
    USTR Name;
    OBJATTR Attr;
    IO_STATUS_BLOCK Io;
    PVOID Handle;
    NTSTATUS Status;

    Close = (FN_CLOSE)resolve(L"ZwClose");
    if (Close == 0)
    {
        emit("ZwClose", "MISSING");
    }
    else
    {
        emit_hex("ZwClose", (UINT32)Close(0));
    }

    Create = (FN_CREATE_FILE)resolve(L"ZwCreateFile");
    if (Create == 0)
    {
        emit("ZwCreateFile", "MISSING");
        return;
    }
    set_name(&Name, L"\\??\\C:\\KernelAbiProbe-Missing");
    Attr.Length = sizeof(Attr);
    Attr.RootDirectory = 0;
    Attr.ObjectName = &Name;
    Attr.Attributes = 0x40;
    Attr.SecurityDescriptor = 0;
    Attr.SecurityQualityOfService = 0;
    Io.u.Status = 0;
    Io.Information = 0;
    Handle = 0;
    Status = Create(&Handle, 0x100000, &Attr, &Io, 0, 0, 1, 1, 0x40, 0, 0);
    emit_hex("ZwCreateFile", (UINT32)Status);
    if (Close != 0 && (Status & 0x80000000) == 0 && Handle != 0)
    {
        Close(Handle);
    }
}

static void
probe_device(PVOID Driver)
{
    FN_CREATE_DEV Create;
    FN_DEL_DEV Delete;
    FN_LINK Link;
    FN_UNLINK Unlink;
    USTR DeviceName;
    USTR LinkName;
    PVOID Device;
    NTSTATUS Status;

    set_name(&DeviceName, L"\\Device\\KernelAbiProbe");
    set_name(&LinkName, L"\\DosDevices\\KernelAbiProbe");
    Create = (FN_CREATE_DEV)resolve(L"IoCreateDevice");
    if (Create == 0)
    {
        emit("IoCreateDevice", "MISSING");
        Device = 0;
    }
    else
    {
        Device = 0;
        Status = Create(Driver, 0, &DeviceName, 0x22, 0, 0, &Device);
        emit_hex("IoCreateDevice", (UINT32)Status);
        if ((Status & 0x80000000) != 0)
        {
            Device = 0;
        }
    }

    Link = (FN_LINK)resolve(L"IoCreateSymbolicLink");
    if (Link == 0)
    {
        emit("IoCreateSymbolicLink", "MISSING");
    }
    else
    {
        emit_hex("IoCreateSymbolicLink", (UINT32)Link(&LinkName, &DeviceName));
    }

    Unlink = (FN_UNLINK)resolve(L"IoDeleteSymbolicLink");
    if (Unlink == 0)
    {
        emit("IoDeleteSymbolicLink", "MISSING");
    }
    else
    {
        emit_hex("IoDeleteSymbolicLink", (UINT32)Unlink(&LinkName));
    }

    if (Device == 0)
    {
        return;
    }
    Delete = (FN_DEL_DEV)resolve(L"IoDeleteDevice");
    if (Delete == 0)
    {
        emit("IoDeleteDevice", "MISSING");
        return;
    }
    Delete(Device);
    emit("IoDeleteDevice", "RETURNED");
}

NTSTATUS
DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    (void)RegistryPath;
    uart_open();
    probe_strings();
    probe_debug();
    probe_lookup();
    probe_pool();
    probe_event();
    probe_irp();
    probe_files();
    probe_device(DriverObject);
    emit("END", "OK");
    return 0;
}
