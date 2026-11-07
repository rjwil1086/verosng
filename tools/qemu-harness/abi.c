/*
 * PROJECT:     ReactOS ABI harness
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Log NT syscalls and ntoskrnl/hal/wdf calls from a QEMU guest
 * COPYRIGHT:   Copyright 2026 ReactOS fork contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <windows.h>

#define QEMU_PLUGIN_VERSION 7

typedef uint64_t qemu_plugin_id_t;

enum qemu_plugin_cb_flags
{
    QEMU_PLUGIN_CB_NO_REGS,
    QEMU_PLUGIN_CB_R_REGS,
    QEMU_PLUGIN_CB_RW_REGS,
    QEMU_PLUGIN_CB_RW_REGS_PC,
};

struct qemu_plugin_tb;
struct qemu_plugin_insn;
struct qemu_plugin_register;

typedef void (*qemu_plugin_vcpu_udata_cb_t)(unsigned int vcpu_index, void *userdata);
typedef void (*qemu_plugin_vcpu_tb_trans_cb_t)(struct qemu_plugin_tb *tb);

typedef struct
{
    void *handle;
    const char *name;
    const char *feature;
    bool is_readonly;
} RegDesc;

typedef struct
{
    char *data;
    unsigned int len;
} GArray;

typedef struct
{
    unsigned char *data;
    unsigned int len;
} GByteArray;

enum
{
    SITE_SYSCALL = 1,
    SITE_EXPORT = 2,
    SITE_RETURN = 3,
};

typedef struct
{
    int kind;
    char symbol[160];
    char insn_name[16];
    uint64_t vaddr;
    size_t size;
} Site;

typedef struct
{
    uint64_t address;
    char symbol[160];
} ExportEnt;

typedef struct
{
    uint64_t ret_pc;
    char symbol[160];
} Pending;

static uint64_t g_last_pc;
static int g_last_kind;
static int g_repeats;

static bool
should_log(uint64_t pc, int kind)
{
    if (pc == g_last_pc && kind == g_last_kind)
    {
        g_repeats++;
        return g_repeats <= 4;
    }
    g_last_pc = pc;
    g_last_kind = kind;
    g_repeats = 1;
    return true;
}
static FILE *g_log;
static ExportEnt g_exports[4096];
static int g_export_count;
static Pending g_pending[32];
static int g_pending_count;

static void *g_eax;
static void *g_edx;
static bool g_regs_ready;

static size_t (*q_tb_n_insns)(const struct qemu_plugin_tb *tb);
static struct qemu_plugin_insn *(*q_tb_get_insn)(const struct qemu_plugin_tb *tb, size_t idx);
static size_t (*q_insn_data)(const struct qemu_plugin_insn *insn, void *dest, size_t len);
static size_t (*q_insn_size)(const struct qemu_plugin_insn *insn);
static uint64_t (*q_insn_vaddr)(const struct qemu_plugin_insn *insn);
static void (*q_register_tb)(qemu_plugin_id_t id, qemu_plugin_vcpu_tb_trans_cb_t cb);
static void (*q_register_exec)(struct qemu_plugin_insn *insn,
                               qemu_plugin_vcpu_udata_cb_t cb,
                               int flags,
                               void *userdata);
static GArray *(*q_get_registers)(void);
static bool (*q_read_register)(struct qemu_plugin_register *handle, GByteArray *buf);
static GByteArray *(*g_byte_array_new)(void);
static void (*g_byte_array_unref)(GByteArray *array);
static void (*g_array_unref)(GArray *array);

static qemu_plugin_id_t g_id;

static void
json_escape(FILE *out, const char *text)
{
    const unsigned char *p;

    fputc('"', out);
    for (p = (const unsigned char *)text; *p != '\0'; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            fputc('\\', out);
            fputc(*p, out);
        }
        else if (*p >= 0x20)
        {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void
load_map(const char *path)
{
    FILE *file;
    char line[256];

    file = fopen(path, "r");
    if (file == NULL)
    {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL && g_export_count < 4096)
    {
        char symbol[160];
        unsigned long long address;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }
        if (sscanf(line, "%159s 0x%llx", symbol, &address) == 2 ||
            sscanf(line, "%159s %llx", symbol, &address) == 2)
        {
            g_exports[g_export_count].address = (uint64_t)address;
            strncpy(g_exports[g_export_count].symbol, symbol, 159);
            g_exports[g_export_count].symbol[159] = '\0';
            g_export_count++;
        }
    }
    fclose(file);
}

static const char *
export_at(uint64_t address)
{
    int i;

    for (i = 0; i < g_export_count; i++)
    {
        if (g_exports[i].address == address)
        {
            return g_exports[i].symbol;
        }
    }
    return NULL;
}

static bool
decode_branch(const uint8_t *bytes, size_t size, uint64_t vaddr, uint64_t *target)
{
    if (size == 5 && (bytes[0] == 0xE8 || bytes[0] == 0xE9))
    {
        int32_t rel;

        memcpy(&rel, bytes + 1, sizeof(rel));
        *target = vaddr + size + (int64_t)rel;
        return true;
    }
    if (size == 3 && bytes[0] == 0xE8)
    {
        int16_t rel;

        memcpy(&rel, bytes + 1, sizeof(rel));
        *target = vaddr + size + rel;
        return true;
    }
    return false;
}

static const char *
syscall_name(const uint8_t *bytes, size_t size)
{
    if (size >= 2 && bytes[0] == 0x0F && bytes[1] == 0x34)
    {
        return "sysenter";
    }
    if (size >= 2 && bytes[0] == 0x0F && bytes[1] == 0x05)
    {
        return "syscall";
    }
    if (size >= 2 && bytes[0] == 0xCD && bytes[1] == 0x2E)
    {
        return "int2e";
    }
    return NULL;
}

static void *
bind_symbol(HMODULE module, const char *name)
{
    return (void *)GetProcAddress(module, name);
}

static bool
bind_qemu(void)
{
    HMODULE exe;
    HMODULE glib;

    exe = GetModuleHandleA(NULL);
    glib = LoadLibraryA("libglib-2.0-0.dll");
    if (exe == NULL)
    {
        return false;
    }

    q_tb_n_insns = bind_symbol(exe, "qemu_plugin_tb_n_insns");
    q_tb_get_insn = bind_symbol(exe, "qemu_plugin_tb_get_insn");
    q_insn_data = bind_symbol(exe, "qemu_plugin_insn_data");
    q_insn_size = bind_symbol(exe, "qemu_plugin_insn_size");
    q_insn_vaddr = bind_symbol(exe, "qemu_plugin_insn_vaddr");
    q_register_tb = bind_symbol(exe, "qemu_plugin_register_vcpu_tb_trans_cb");
    q_register_exec = bind_symbol(exe, "qemu_plugin_register_vcpu_insn_exec_cb");
    q_get_registers = bind_symbol(exe, "qemu_plugin_get_registers");
    q_read_register = bind_symbol(exe, "qemu_plugin_read_register");
    if (glib != NULL)
    {
        g_byte_array_new = bind_symbol(glib, "g_byte_array_new");
        g_byte_array_unref = bind_symbol(glib, "g_byte_array_unref");
        g_array_unref = bind_symbol(glib, "g_array_unref");
    }

    return q_tb_n_insns && q_tb_get_insn && q_insn_data && q_insn_size &&
           q_insn_vaddr && q_register_tb && q_register_exec;
}

static uint64_t
read_reg(void *handle)
{
    GByteArray *buf;
    uint64_t value;

    if (handle == NULL || q_read_register == NULL || g_byte_array_new == NULL)
    {
        return 0;
    }

    value = 0;
    buf = g_byte_array_new();
    if (q_read_register(handle, buf) && buf != NULL && buf->data != NULL && buf->len >= 4)
    {
        memcpy(&value, buf->data, buf->len >= 8 ? 8 : 4);
    }
    if (g_byte_array_unref != NULL && buf != NULL)
    {
        g_byte_array_unref(buf);
    }
    return value;
}

static void
cache_regs(void)
{
    GArray *list;
    unsigned int i;
    size_t stride;

    if (g_regs_ready || q_get_registers == NULL)
    {
        return;
    }

    stride = sizeof(RegDesc);
    list = q_get_registers();
    if (list == NULL || list->data == NULL)
    {
        return;
    }

    for (i = 0; i < list->len; i++)
    {
        RegDesc *reg;
        const char *name;

        reg = (RegDesc *)(list->data + (i * stride));
        name = reg->name;
        if (name == NULL)
        {
            continue;
        }
        if (_stricmp(name, "eax") == 0 || _stricmp(name, "rax") == 0)
        {
            g_eax = reg->handle;
        }
        else if (_stricmp(name, "edx") == 0 || _stricmp(name, "rdx") == 0)
        {
            g_edx = reg->handle;
        }
    }
    if (g_array_unref != NULL)
    {
        g_array_unref(list);
    }
    g_regs_ready = true;
}

static void
write_event(const char *kind, const char *phase, const char *symbol, const char *insn,
            uint64_t pc, uint64_t eax, uint64_t edx, const char *result)
{
    if (g_log == NULL)
    {
        return;
    }

    fprintf(g_log, "{\"kind\":");
    json_escape(g_log, kind);
    fprintf(g_log, ",\"phase\":");
    json_escape(g_log, phase);
    fprintf(g_log, ",\"symbol\":");
    json_escape(g_log, symbol != NULL ? symbol : "");
    fprintf(g_log, ",\"insn\":");
    json_escape(g_log, insn != NULL ? insn : "");
    fprintf(g_log, ",\"pc\":\"0x%llx\",\"eax\":\"0x%llx\",\"edx\":\"0x%llx\",\"result\":",
            (unsigned long long)pc, (unsigned long long)eax, (unsigned long long)edx);
    json_escape(g_log, result);
    fprintf(g_log, "}\n");
    fflush(g_log);
}

static void
on_insn(unsigned int vcpu_index, void *userdata)
{
    Site *site;
    uint64_t eax;
    uint64_t edx;
    int i;

    (void)vcpu_index;
    site = (Site *)userdata;
    cache_regs();
    eax = read_reg(g_eax);
    edx = read_reg(g_edx);

    if (!should_log(site->vaddr, site->kind))
    {
        return;
    }

    for (i = 0; i < g_pending_count; i++)
    {
        if (g_pending[i].ret_pc == site->vaddr)
        {
            write_event("export", "return", g_pending[i].symbol, "ret", site->vaddr, eax, edx,
                        (eax >= 0x80000000ULL) ? "error" : "success");
            g_pending[i] = g_pending[g_pending_count - 1];
            g_pending_count--;
            break;
        }
    }

    if (site->kind == SITE_SYSCALL)
    {
        write_event("syscall", "enter", site->insn_name, site->insn_name, site->vaddr, eax, edx, "seen");
    }
    else if (site->kind == SITE_EXPORT)
    {
        write_event("export", "enter", site->symbol, "call", site->vaddr, eax, edx, "seen");
        if (g_pending_count < 32)
        {
            g_pending[g_pending_count].ret_pc = site->vaddr + site->size;
            strncpy(g_pending[g_pending_count].symbol, site->symbol, 159);
            g_pending[g_pending_count].symbol[159] = '\0';
            g_pending_count++;
        }
    }
}

static Site *
new_site(int kind, const char *symbol, const char *insn, uint64_t vaddr, size_t size)
{
    Site *site;

    site = (Site *)calloc(1, sizeof(*site));
    if (site == NULL)
    {
        return NULL;
    }
    site->kind = kind;
    site->vaddr = vaddr;
    site->size = size;
    if (symbol != NULL)
    {
        strncpy(site->symbol, symbol, 159);
    }
    if (insn != NULL)
    {
        strncpy(site->insn_name, insn, 15);
    }
    return site;
}

static void
on_tb(struct qemu_plugin_tb *tb)
{
    size_t count;
    size_t i;

    count = q_tb_n_insns(tb);
    for (i = 0; i < count; i++)
    {
        struct qemu_plugin_insn *insn;
        uint8_t bytes[16];
        size_t full;
        size_t size;
        uint64_t vaddr;
        const char *sys;
        uint64_t target;
        const char *symbol;
        Site *site;
        int pending;

        insn = q_tb_get_insn(tb, i);
        full = q_insn_size(insn);
        vaddr = q_insn_vaddr(insn);
        size = full > sizeof(bytes) ? sizeof(bytes) : full;
        q_insn_data(insn, bytes, size);

        for (pending = 0; pending < g_pending_count; pending++)
        {
            if (g_pending[pending].ret_pc == vaddr)
            {
                site = new_site(SITE_RETURN, g_pending[pending].symbol, "ret", vaddr, full);
                if (site != NULL)
                {
                    q_register_exec(insn, on_insn, QEMU_PLUGIN_CB_R_REGS, site);
                }
                break;
            }
        }

        sys = syscall_name(bytes, size);
        if (sys != NULL)
        {
            site = new_site(SITE_SYSCALL, sys, sys, vaddr, full);
            if (site != NULL)
            {
                q_register_exec(insn, on_insn, QEMU_PLUGIN_CB_R_REGS, site);
            }
            continue;
        }

        if (decode_branch(bytes, size, vaddr, &target))
        {
            symbol = export_at(target);
            if (symbol != NULL)
            {
                site = new_site(SITE_EXPORT, symbol, "call", vaddr, full);
                if (site != NULL)
                {
                    q_register_exec(insn, on_insn, QEMU_PLUGIN_CB_R_REGS, site);
                }
            }
        }
    }
}

__declspec(dllexport) int qemu_plugin_version = QEMU_PLUGIN_VERSION;

__declspec(dllexport) int
qemu_plugin_install(qemu_plugin_id_t id, const void *info, int argc, char **argv)
{
    const char *log_path;
    int i;

    (void)info;
    g_id = id;
    log_path = "abi.jsonl";

    for (i = 0; i < argc; i++)
    {
        if (strncmp(argv[i], "log=", 4) == 0)
        {
            log_path = argv[i] + 4;
        }
        else if (strncmp(argv[i], "map=", 4) == 0)
        {
            load_map(argv[i] + 4);
        }
    }

    if (!bind_qemu())
    {
        return 1;
    }

    g_log = fopen(log_path, "w");
    if (g_log == NULL)
    {
        return 1;
    }

    q_register_tb(id, on_tb);
    return 0;
}
