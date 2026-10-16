// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026
// sp00nz and contributors.
// The NT file API over wl/platform's files: create/open with the six dispositions, reads and writes
// at offsets (synchronous handles keep a position; overlapped ones complete at once and still signal
// their event and queue their APC, unless the request failed), the information classes titles query and set, volume queries,
// directory listing with wildcards, the DVD drive and the disk's partitions as devices.
//
// Guest buffers are filled through a host bounce buffer: on the Wii the SD card's driver may DMA,
// and DMA must never see a page-table address.
//
// Sparse files (the cache partitions, Z:): a file's logical size is kept in a sidecar "<name>.wlsz"
// (16 hex digits) when it is larger than what was written; reads between the written length and the
// logical size give zeros, and the sidecar never shows in a listing. Halo sets its cache slots to
// 277 MB and writes only what it inflates. The handles on one file share its size, and only a
// handle that changed it writes the sidecar.
//
// Every file and folder open under a mount has one XkFcb, as NT's file control block: its handles,
// a sparse file's size, and a delete asked for (FILE_DELETE_ON_CLOSE, FileDispositionInformation,
// NtDeleteFile of an open file), done when the last handle closes.
#include <stdio.h>
#include <stdlib.h>

#include "wl/base/endian.h"
#include "wl/xboxkrnl/nt_file.h"

#define BOUNCE 65536u
#define SIDECAR ".wlsz"
#define RAW_KEEP 0x10000u

#define FILE_SUPERSEDE 0u
#define FILE_OPEN 1u
#define FILE_CREATE 2u
#define FILE_OPEN_IF 3u
#define FILE_OVERWRITE 4u
#define FILE_OVERWRITE_IF 5u
#define FILE_DIRECTORY_FILE 0x1u
#define FILE_SYNCHRONOUS_IO_ALERT 0x10u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20u
#define FILE_NON_DIRECTORY_FILE 0x40u
#define FILE_DELETE_ON_CLOSE 0x1000u
#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define WRITE_ACCESS (0x40000000u | 0x10000000u | 0x2u | 0x4u | 0x10000u | 0x100u | 0x10u)
// what NtWriteFile needs: GENERIC_WRITE, GENERIC_ALL, MAXIMUM_ALLOWED, FILE_WRITE_DATA, FILE_APPEND_DATA
#define DATA_WRITE_ACCESS (0x40000000u | 0x10000000u | 0x02000000u | 0x2u | 0x4u)

static uint8_t *g_bounce;
static char g_data_dir[192];
static uint32_t g_files_open;

int xk_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y || x == 0) {
            return x - y;
        }
    }
}

int xk_strnicmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++, a++, b++) {
        int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y || x == 0) {
            return x - y;
        }
    }
    return 0;
}

void xk_files_init(const WlXkConfig *cfg) {
    g_bounce = malloc(BOUNCE);
    snprintf(g_data_dir, sizeof g_data_dir, "%s", cfg->data_dir);
    xk_paths_init(cfg);
}

// ---- Sparse sizes ----------------------------------------------------------------------------------

static void sidecar_path(const char *host, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", host, SIDECAR);
}

// A sparse file's logical size from its sidecar: WL_OK with it, WL_ERR_NOT_FOUND when there is none
// (0), another error when the sidecar could not be read (every file slot in use: 0, and unknown).
static WlResult sidecar_read(const char *host, uint64_t *out) {
    char path[340];
    sidecar_path(host, path, sizeof path);
    *out = 0;
    WlPlatformFile *f = NULL;
    WlResult r = wl_platform_fs_open(path, WL_FILE_READ, &f);
    if (r != WL_OK) {
        return r;
    }
    char text[17] = {0};
    uint32_t got = 0;
    r = wl_platform_fs_read_some_at(f, 0, text, 16, &got);
    if (r == WL_OK) {
        *out = strtoull(text, NULL, 16);
    }
    wl_platform_fs_close(f);
    return r;
}

static void sidecar_write(const char *host, uint64_t logical, uint64_t physical) {
    char path[340];
    sidecar_path(host, path, sizeof path);
    if (logical <= physical) {
        wl_platform_fs_remove(path);
        return;
    }
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(path, WL_FILE_WRITE_NEW, &f) == WL_OK) {
        char text[20];
        snprintf(text, sizeof text, "%016llX", (unsigned long long)logical);
        wl_platform_fs_write(f, text, 16);
        wl_platform_fs_close(f);
    }
}

struct XkFcb {
    char host[320];
    uint64_t logical; // a sparse file's size (what the title sees)
    uint32_t opens;   // the XkFiles on it ...
    XkFile *files;    // ... linked by fcb_next
    bool delete_pending;
    bool no_sidecar;  // known to have none on disk (a removal would only cost a path walk on libfat)
    XkFcb *next;
};

static XkFcb *g_fcbs;

// An open file's sidecar after its size changed: written when the size passes its bytes, else removed
// unless the file is known to have none.
static void fcb_sidecar_write(XkFcb *c, uint64_t physical) {
    bool none = c->logical <= physical;
    if (!(none && c->no_sidecar)) {
        sidecar_write(c->host, c->logical, physical);
    }
    c->no_sidecar = none;
}

static XkFcb *fcb_find(const char *host) {
    for (XkFcb *c = g_fcbs; c != NULL; c = c->next) {
        if (strcasecmp(c->host, host) == 0) { // FATX names: one file in any case
            return c;
        }
    }
    return NULL;
}

// Joins `f` to its file's record (f->fcb), made by the file's first open (with `logical` as its
// size); NULL when out of memory.
static XkFcb *fcb_open(XkFile *f, uint64_t logical) {
    XkFcb *c = fcb_find(f->host);
    if (c == NULL) {
        c = calloc(1, sizeof *c);
        if (c == NULL) {
            return NULL;
        }
        snprintf(c->host, sizeof c->host, "%s", f->host);
        c->logical = logical;
        c->next = g_fcbs;
        g_fcbs = c;
    }
    c->opens++;
    f->fcb = c;
    f->fcb_next = c->files;
    c->files = f;
    return c;
}

static void fcb_leave(XkFcb *c, XkFile *f) {
    for (XkFile **p = &c->files; *p != NULL; p = &(*p)->fcb_next) {
        if (*p == f) {
            *p = f->fcb_next;
            break;
        }
    }
    f->fcb_next = NULL;
}

static void fcb_unlink(XkFcb *c) {
    for (XkFcb **p = &g_fcbs; *p != NULL; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    free(c);
}

static uint64_t physical_size(const XkFile *f) {
    return f->pf != NULL ? wl_platform_fs_size(f->pf) : 0u;
}

static uint64_t file_size(const XkFile *f) {
    if (f->kind == XK_FILE_PARTITION) {
        return xk_raw_size(f->partition);
    }
    uint64_t p = physical_size(f);
    return f->sparse && f->fcb != NULL && f->fcb->logical > p ? f->fcb->logical : p;
}

// The size a listing or an attribute query shows for a sparse file: what its open handles know,
// else its sidecar's (the bytes written when the sidecar cannot be read).
static uint64_t sparse_size(const char *host, uint64_t physical) {
    XkFcb *c = fcb_find(host);
    uint64_t logical = 0;
    if (c != NULL) {
        logical = c->logical;
    } else {
        sidecar_read(host, &logical);
    }
    return logical > physical ? logical : physical;
}

// ---- Raw partitions --------------------------------------------------------------------------------

static const struct {
    uint32_t start, sectors;
    const char *name;
} k_parts[] = {
    {0x00000400u, 0x00177000u, "XBOX_PART_X"}, {0x00177400u, 0x00177000u, "XBOX_PART_Y"},
    {0x002EE400u, 0x00177000u, "XBOX_PART_Z"}, {0x00465400u, 0x000FA000u, "XBOX_PART_C"},
    {0x0055F400u, 0x00465400u, "XBOX_PART_E"},
};

uint64_t xk_raw_size(uint32_t partition) {
    // 1 E:, 2 C:, 3-5 X: Y: Z:, 0 the disk (8 GB)
    static const uint8_t index[6] = {0xFF, 4, 3, 0, 1, 2};
    if (partition == 0 || partition > 5) {
        return 8ull << 30;
    }
    return (uint64_t)k_parts[index[partition]].sectors * 512u;
}

static void raw_path(uint32_t partition, char *out, size_t cap) {
    snprintf(out, cap, "%s/raw%u.bin", g_data_dir, (unsigned)partition);
}

void xk_raw_read(uint32_t partition, uint64_t offset, uint8_t *dst, uint32_t n) {
    memset(dst, 0, n);
    if (offset < RAW_KEEP) {
        char path[220];
        raw_path(partition, path, sizeof path);
        WlPlatformFile *f = NULL;
        if (wl_platform_fs_open(path, WL_FILE_READ, &f) == WL_OK) {
            uint32_t got = 0;
            uint32_t take = (uint32_t)(RAW_KEEP - offset < n ? RAW_KEEP - offset : n);
            wl_platform_fs_read_some_at(f, offset, dst, take, &got);
            wl_platform_fs_close(f);
        }
    }
    // the disk's partition table in sector 0, where the homebrew convention keeps it
    // ("****PARTINFO****", 16 + 32 reserved, then 32-byte entries). Not in sector 4 (0x800): XAPI
    // keeps its utility drive's cache database there and must read back what it wrote.
    if (partition == 0 && offset < 0x200u) {
        uint8_t table[512] = {0};
        memcpy(table, "****PARTINFO****", 16);
        for (size_t i = 0; i < WL_ARRAY_COUNT(k_parts); i++) {
            uint8_t *e = table + 48 + i * 32;
            memset(e, ' ', 16);
            memcpy(e, k_parts[i].name, strlen(k_parts[i].name));
            wl_wr_le32(e + 16, 0x80000000u); // in use
            wl_wr_le32(e + 20, k_parts[i].start);
            wl_wr_le32(e + 24, k_parts[i].sectors);
        }
        for (uint32_t i = 0; i < n && offset + i < 0x200u; i++) {
            dst[i] = table[offset + i];
        }
    }
}

void xk_raw_write(uint32_t partition, uint64_t offset, const uint8_t *src, uint32_t n) {
    if (offset >= RAW_KEEP) {
        return;
    }
    char path[220];
    raw_path(partition, path, sizeof path);
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(path, WL_FILE_READ_WRITE, &f) == WL_OK) {
        uint32_t take = (uint32_t)(RAW_KEEP - offset < n ? RAW_KEEP - offset : n);
        wl_platform_fs_write_at(f, offset, src, take);
        wl_platform_fs_close(f);
    }
}

// ---- Objects ---------------------------------------------------------------------------------------

// Guest pages a read filled from a file remember its bytes (wl_xb_file_pages): before the bytes
// [off, off + len) of the file `c` names change, every handle's pages over them forget them.
static void fcb_pages_forget(const XkFcb *c, uint64_t off, uint64_t len) {
    for (const XkFile *o = c != NULL ? c->files : NULL; o != NULL; o = o->fcb_next) {
        if (o->pf != NULL) {
            wl_xb_file_forget(o->pf, off, len);
        }
    }
}

static void pages_forget(const XkFile *f, uint64_t off, uint64_t len) {
    if (f->fcb != NULL) {
        fcb_pages_forget(f->fcb, off, len);
    } else if (f->pf != NULL) {
        wl_xb_file_forget(f->pf, off, len);
    }
}

void wl_xk_file_changed(const char *host) {
    XkFcb *c = fcb_find(host);
    if (c != NULL) {
        c->no_sidecar = false; // (its sidecar may have been written too)
    }
    WL_LOG_I(XK_TAG, "changed outside the kernel: %s (%s)", host, c != NULL ? "open" : "not open");
    fcb_pages_forget(c, 0, UINT64_MAX);
}

void xk_file_close(XkFile *f) {
    if (f == NULL) {
        return;
    }
    if (f->pf != NULL) {
        wl_xb_file_forget(f->pf, 0, UINT64_MAX); // its pages' bytes are read back while it is open
        if (f->sparse && f->resized && f->fcb != NULL) {
            fcb_sidecar_write(f->fcb, physical_size(f));
        }
        wl_platform_fs_close(f->pf);
        g_files_open--;
        g_xk_stats.files_open = g_files_open;
    }
    if (f->dir != NULL) {
        wl_platform_fs_dir_close(f->dir);
    }
    XkFcb *c = f->fcb;
    if (c != NULL) {
        fcb_leave(c, f);
        if (f->delete_on_close) {
            c->delete_pending = true;
        }
        if (--c->opens == 0) {
            if (c->delete_pending) {
                if (f->kind == XK_FILE_DIRECTORY) {
                    wl_platform_fs_rmdir(c->host);
                } else {
                    wl_platform_fs_remove(c->host);
                    if (!c->no_sidecar) {
                        char side[340];
                        sidecar_path(c->host, side, sizeof side);
                        wl_platform_fs_remove(side);
                    }
                }
            }
            fcb_unlink(c);
        }
    }
    free(f);
}

static XkFile *file_of(uint32_t handle) {
    XkObject *o = xk_handle_get_type(handle, XK_OBJ_FILE);
    return o != NULL ? o->file : NULL;
}

bool xk_object_name(uint32_t oa, char *out, uint32_t cap, uint32_t *status) {
    *status = STATUS_OBJECT_NAME_INVALID;
    out[0] = '\0';
    if (oa == 0 || !wl_xb_range_ok(oa, 12)) {
        *status = STATUS_INVALID_PARAMETER;
        return false;
    }
    char name[300];
    if (!xk_read_ansi(wl_xb_rd32(oa + 4u), name, sizeof name)) {
        return false;
    }
    uint32_t root = wl_xb_rd32(oa);
    XkFile *rf = root != 0 && root != 0xFFFFFFFDu ? file_of(root) : NULL;
    int n;
    if (rf != NULL) {
        size_t len = strlen(rf->name);
        n = snprintf(out, cap, "%s%s%s", rf->name, (len > 0 && rf->name[len - 1] == '\\') ? "" : "\\", name);
    } else {
        n = snprintf(out, cap, "%s", name);
    }
    return n >= 0 && (uint32_t)n < cap; // a name cut short names another file: invalid

}

static uint32_t open_file(uint32_t handle_out, uint32_t access, uint32_t oa, uint32_t iosb, uint32_t disposition,
                          uint32_t options) {
    char name[400];
    uint32_t status = 0;
    if (!xk_object_name(oa, name, sizeof name, &status)) {
        xk_iosb(iosb, status, 0);
        return status;
    }
    XkResolved r;
    if (!xk_resolve(name, &r)) {
        WL_LOG_W(XK_TAG, "open \"%s\": no device serves it", name);
        xk_iosb(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    XkFile *f = calloc(1, sizeof *f);
    if (f == NULL) {
        return STATUS_NO_MEMORY;
    }
    snprintf(f->name, sizeof f->name, "%s", r.device_path);
    f->synchronous = (options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0;
    uint32_t info = 1; // FILE_OPENED
    bool want_write = (access & WRITE_ACCESS) != 0;
    if (r.kind == XK_RES_CDROM) {
        f->kind = XK_FILE_CDROM;
    } else if (r.kind == XK_RES_PARTITION) {
        f->kind = XK_FILE_PARTITION;
        f->partition = r.partition;
        f->writable = true;
        f->can_write = (access & DATA_WRITE_ACCESS) != 0;
    } else {
        WlPlatformStat st;
        bool exists = xk_resolve_case(&r, &st);
        snprintf(f->host, sizeof f->host, "%s", r.host);
        f->writable = r.mount.writable;
        f->can_write = f->writable && (access & DATA_WRITE_ACCESS) != 0;
        f->sparse = r.mount.sparse;
        bool is_dir = exists && st.is_dir;
        uint32_t fail = 0;
        const XkFcb *open_fcb = exists ? fcb_find(f->host) : NULL;
        if (open_fcb != NULL && open_fcb->delete_pending) {
            // going at its last close: no new handle, whatever the disposition, as NT
            fail = STATUS_DELETE_PENDING;
        } else if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)) {
            // a missing parent is a missing path
            char parent[340];
            snprintf(parent, sizeof parent, "%s", f->host);
            char *slash = strrchr(parent, '/');
            if (slash != NULL && (size_t)(slash - parent) > r.root_len) {
                *slash = '\0';
            }
            fail = (slash != NULL && (size_t)(slash - parent) > r.root_len && !wl_platform_fs_exists(parent))
                       ? STATUS_OBJECT_PATH_NOT_FOUND
                       : STATUS_OBJECT_NAME_NOT_FOUND;
        } else if (exists && disposition == FILE_CREATE) {
            fail = STATUS_OBJECT_NAME_COLLISION;
        } else if (is_dir && (options & FILE_NON_DIRECTORY_FILE)) {
            fail = STATUS_FILE_IS_A_DIRECTORY;
        } else if (exists && !is_dir && (options & FILE_DIRECTORY_FILE)) {
            fail = STATUS_NOT_A_DIRECTORY;
        } else if (!f->writable && (!exists || want_write || disposition == FILE_OVERWRITE ||
                                    disposition == FILE_OVERWRITE_IF || disposition == FILE_SUPERSEDE)) {
            fail = exists ? STATUS_ACCESS_DENIED : STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (fail != 0) {
            WL_LOG_D(XK_TAG, "open \"%s\" -> %s: 0x%08x", name, f->host, (unsigned)fail);
            free(f);
            xk_iosb(iosb, fail, 0);
            return fail;
        }
        if (is_dir || (!exists && (options & FILE_DIRECTORY_FILE))) {
            f->kind = XK_FILE_DIRECTORY;
            if (!exists) {
                if (wl_platform_fs_mkdir(f->host) != WL_OK) {
                    free(f);
                    xk_iosb(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
                    return STATUS_OBJECT_PATH_NOT_FOUND;
                }
                info = 2; // FILE_CREATED
            }
            if (fcb_open(f, 0) == NULL) {
                free(f);
                return STATUS_NO_MEMORY;
            }
        } else {
            f->kind = XK_FILE_REGULAR;
            bool truncate = exists && (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF ||
                                       disposition == FILE_SUPERSEDE);
            WlPlatformFileMode mode = !f->writable ? WL_FILE_READ : truncate ? WL_FILE_WRITE_NEW : WL_FILE_READ_WRITE;
            if (f->writable && exists && !want_write && !truncate) {
                mode = WL_FILE_READ_WRITE; // keep it writable for a later SetInformation (EOF)
            }
            // a sparse file's size: its other handles', else its sidecar's; a sidecar that cannot
            // be read (every slot in use) fails the open rather than lose the size
            uint64_t logical = 0;
            bool no_sidecar = false;
            if (f->sparse && exists && !truncate && fcb_find(f->host) == NULL) {
                WlResult sr = sidecar_read(f->host, &logical);
                no_sidecar = sr == WL_ERR_NOT_FOUND;
                if (sr != WL_OK && sr != WL_ERR_NOT_FOUND) {
                    WL_LOG_W(XK_TAG, "open \"%s\": its size cannot be read: %s", name, wl_result_name(sr));
                    free(f);
                    uint32_t s = sr == WL_ERR_LIMIT ? STATUS_INSUFFICIENT_RESOURCES : STATUS_UNSUCCESSFUL;
                    xk_iosb(iosb, s, 0);
                    return s;
                }
            }
            if (truncate) {
                fcb_pages_forget(fcb_find(f->host), 0, UINT64_MAX); // its other handles' pages
            }
            WlResult pr = wl_platform_fs_open(f->host, mode, &f->pf);
            if (pr == WL_OK && mode == WL_FILE_WRITE_NEW) {
                // WRITE_NEW cannot read: reopen read-write now that it is empty
                wl_platform_fs_close(f->pf);
                f->pf = NULL;
                pr = wl_platform_fs_open(f->host, WL_FILE_READ_WRITE, &f->pf);
            }
            if (pr != WL_OK) {
                WL_LOG_W(XK_TAG, "open \"%s\" -> %s failed: %s", name, f->host, wl_result_name(pr));
                free(f);
                uint32_t s = pr == WL_ERR_NOT_FOUND ? STATUS_OBJECT_NAME_NOT_FOUND
                             : pr == WL_ERR_LIMIT  ? STATUS_INSUFFICIENT_RESOURCES
                                                   : STATUS_ACCESS_DENIED;
                xk_iosb(iosb, s, 0);
                return s;
            }
            g_files_open++;
            if (fcb_open(f, logical) == NULL) {
                wl_platform_fs_close(f->pf);
                g_files_open--;
                free(f);
                xk_iosb(iosb, STATUS_NO_MEMORY, 0);
                return STATUS_NO_MEMORY;
            }
            if (no_sidecar) {
                f->fcb->no_sidecar = true;
            }
            if (f->sparse && (truncate || !exists)) {
                f->fcb->logical = 0;
                fcb_sidecar_write(f->fcb, 0); // gone with the old contents (or left by a lost file)
            }
            info = !exists ? 2u : truncate ? (disposition == FILE_SUPERSEDE ? 0u : 3u) : 1u;
        }
        f->delete_on_close = (options & FILE_DELETE_ON_CLOSE) != 0;
    }
    XkObject *o = xk_object_new(XK_OBJ_FILE, 16);
    if (o == NULL) {
        xk_file_close(f);
        return STATUS_NO_MEMORY;
    }
    xk_header_init(o->body, OBJ_EVENT_NOTIFICATION, 4, 1); // a file object is signalled when idle
    o->file = f;
    uint32_t h = xk_handle_open(o);
    if (h == 0) {
        o->file = NULL; // the object is lost with the full table; the file is closed
        xk_file_close(f);
        xk_iosb(iosb, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (handle_out != 0 && wl_xb_range_ok(handle_out, 4)) {
        wl_xb_wr32(handle_out, h);
    }
    xk_iosb(iosb, STATUS_SUCCESS, info);
    g_xk_stats.files_open = g_files_open;
    WL_LOG_I(XK_TAG, "%s \"%s\" -> %s (handle 0x%x)", info == 2 ? "create" : "open", name,
             f->kind == XK_FILE_CDROM       ? "the DVD drive"
             : f->kind == XK_FILE_PARTITION ? "a disk partition"
                                            : f->host,
             (unsigned)h);
    return STATUS_SUCCESS;
}

// NTSTATUS NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES,
//     PIO_STATUS_BLOCK, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
//     ULONG CreateDisposition, ULONG CreateOptions)
void k_NtCreateFile(void) {
    RET(open_file(ARG(0), ARG(1), ARG(2), ARG(3), ARG(7), ARG(8)));
}

// NTSTATUS NtOpenFile(PHANDLE FileHandle, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//                     ULONG ShareAccess, ULONG OpenOptions)
void k_NtOpenFile(void) {
    RET(open_file(ARG(0), ARG(1), ARG(2), ARG(3), FILE_OPEN, ARG(5)));
}

// NTSTATUS IoCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
//     ULONG FileAttributes, ULONG ShareAccess, ULONG Disposition, ULONG CreateOptions, ULONG Options)
void k_IoCreateFile(void) {
    RET(open_file(ARG(0), ARG(1), ARG(2), ARG(3), ARG(7), ARG(8)));
}

// ---- Reads and writes --------------------------------------------------------------------------------

// The offset an I/O starts at: the argument, or the file's position (no argument, or the
// FILE_USE_FILE_POINTER_POSITION marker: low -2, high -1), or its end (FILE_WRITE_TO_END_OF_FILE:
// low -1, high -1).
static uint64_t io_offset(XkFile *f, uint32_t offset_va) {
    if (offset_va != 0 && wl_xb_range_ok(offset_va, 8)) {
        uint32_t lo = wl_xb_rd32(offset_va), hi = wl_xb_rd32(offset_va + 4u);
        if (lo == 0xFFFFFFFFu && hi == 0xFFFFFFFFu) {
            return file_size(f);
        }
        if (!(lo == 0xFFFFFFFEu && hi == 0xFFFFFFFFu)) {
            return (uint64_t)lo | ((uint64_t)hi << 32);
        }
    }
    return f->pos;
}

static uint32_t do_read(XkFile *f, uint64_t off, uint32_t buf, uint32_t len, uint32_t *done) {
    *done = 0;
    if (f->kind == XK_FILE_DIRECTORY) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (!wl_xb_range_ok(buf, len)) {
        return STATUS_ACCESS_VIOLATION;
    }
    uint64_t size = f->kind == XK_FILE_CDROM ? 0x1000000000ull : file_size(f);
    if (len == 0) {
        return STATUS_SUCCESS;
    }
    if (off >= size) {
        return STATUS_END_OF_FILE;
    }
    uint32_t n = size - off < len ? (uint32_t)(size - off) : len;
    uint64_t phys = f->kind == XK_FILE_REGULAR ? physical_size(f) : 0;
    while (*done < n) {
        uint32_t chunk = n - *done < BOUNCE ? n - *done : BOUNCE;
        uint64_t at = off + *done;
        if (f->kind == XK_FILE_PARTITION) {
            xk_raw_read(f->partition, at, g_bounce, chunk);
        } else if (f->kind == XK_FILE_CDROM) {
            memset(g_bounce, 0, chunk);
        } else {
            uint32_t got = 0;
            if (at < phys) {
                uint32_t want = phys - at < chunk ? (uint32_t)(phys - at) : chunk;
                if (wl_platform_fs_read_some_at(f->pf, at, g_bounce, want, &got) != WL_OK) {
                    return *done > 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                }
            }
            memset(g_bounce + got, 0, chunk - got); // past the written bytes of a sparse file
        }
        wl_xb_copy_in(buf + *done, g_bounce, chunk);
        *done += chunk;
    }
    if (off < phys) {
        // the whole pages filled from the file's bytes remember them: unchanged, they are read
        // from it again rather than written to the swap file
        wl_xb_file_pages(buf, phys - off < n ? (uint32_t)(phys - off) : n, f->pf, off);
    }
    return STATUS_SUCCESS;
}

static uint32_t do_write(XkFile *f, uint64_t off, uint32_t buf, uint32_t len, uint32_t *done) {
    *done = 0;
    if (f->kind == XK_FILE_DIRECTORY || f->kind == XK_FILE_CDROM) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (!wl_xb_range_ok(buf, len)) {
        return STATUS_ACCESS_VIOLATION;
    }
    uint32_t status = STATUS_SUCCESS;
    if (f->kind != XK_FILE_PARTITION) {
        pages_forget(f, off, len);
    }
    while (*done < len) {
        uint32_t chunk = len - *done < BOUNCE ? len - *done : BOUNCE;
        wl_xb_copy_out(g_bounce, buf + *done, chunk);
        if (f->kind == XK_FILE_PARTITION) {
            xk_raw_write(f->partition, off + *done, g_bounce, chunk);
        } else if (wl_platform_fs_write_at(f->pf, off + *done, g_bounce, chunk) != WL_OK) {
            status = STATUS_DISK_FULL; // the card is full: what was written stays, and counts
            break;
        }
        *done += chunk;
    }
    if (f->sparse && f->fcb != NULL && off + *done > f->fcb->logical) {
        f->fcb->logical = off + *done;
        f->resized = true;
    }
    return status;
}

// Before an I/O is dispatched: the caller's event and the file object's are cleared, as NT does, so
// a request that then fails leaves both non-signalled.
static void begin_io(uint32_t handle, uint32_t event) {
    uint32_t file_body = xk_handle_waitable(handle);
    if (file_body != 0) {
        xk_event_reset(file_body);
    }
    if (event != 0) {
        uint32_t ev = xk_handle_waitable(event);
        if (ev != 0) {
            xk_event_reset(ev);
        }
    }
}

// After an I/O: the status block, the event, the file object, the APC (overlapped handles). An error
// (an NT_ERROR status, STATUS_END_OF_FILE among them) is returned alone: NT sets none of them for a
// request that fails before it is pending (begin_io cleared the events).
static uint32_t complete_io(XkFile *f, uint32_t handle, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb,
                            uint32_t status, uint32_t done) {
    if (NT_ERROR(status)) {
        return status;
    }
    xk_iosb(iosb, status, done);
    uint32_t file_body = xk_handle_waitable(handle);
    if (file_body != 0) {
        xk_event_set(file_body);
    }
    if (event != 0) {
        uint32_t ev = xk_handle_waitable(event);
        if (ev != 0) {
            xk_event_set(ev);
        }
    }
    if (apc != 0 && !f->synchronous) {
        xk_apc_queue(wl_xb_current(), apc, apc_ctx, iosb, 0);
    }
    if (!f->synchronous && g_xk_cfg.async_io_pending && NT_SUCCESS(status)) {
        return STATUS_PENDING;
    }
    return status;
}

// NTSTATUS NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
//     PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset)
void k_NtReadFile(void) {
    uint32_t handle = ARG(0);
    XkFile *f = file_of(handle);
    if (f == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    begin_io(handle, ARG(1));
    uint64_t off = io_offset(f, ARG(7));
    uint32_t done = 0;
    uint32_t status = do_read(f, off, ARG(5), ARG(6), &done);
    if (NT_SUCCESS(status)) {
        f->pos = off + done;
    }
    RET(complete_io(f, handle, ARG(1), ARG(2), ARG(3), ARG(4), status, done));
}

// NTSTATUS NtWriteFile(...the same arguments...)
void k_NtWriteFile(void) {
    uint32_t handle = ARG(0);
    XkFile *f = file_of(handle);
    if (f == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    if (f->kind != XK_FILE_DIRECTORY && f->kind != XK_FILE_CDROM && !f->can_write) {
        // a read-only mount, or a handle opened without write access: refused with the handle,
        // before NT looks at the event
        RET(STATUS_ACCESS_DENIED);
        return;
    }
    begin_io(handle, ARG(1));
    uint64_t off = io_offset(f, ARG(7));
    uint32_t done = 0;
    uint32_t status = do_write(f, off, ARG(5), ARG(6), &done);
    if (NT_SUCCESS(status)) {
        f->pos = off + done;
    }
    RET(complete_io(f, handle, ARG(1), ARG(2), ARG(3), ARG(4), status, done));
}

// NTSTATUS NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock)
void k_NtFlushBuffersFile(void) {
    XkFile *f = file_of(ARG(0));
    if (f == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    if (f->pf != NULL) {
        wl_platform_fs_flush(f->pf);
        if (f->sparse && f->resized && f->fcb != NULL) {
            fcb_sidecar_write(f->fcb, physical_size(f));
        }
    }
    xk_iosb(ARG(1), STATUS_SUCCESS, 0);
    RET(STATUS_SUCCESS);
}

// ---- Information -------------------------------------------------------------------------------------

static uint64_t filetime_of(uint64_t unix_us) {
    if (wl_xb_deterministic()) {
        return 127449504000000000ull; // a fixed time (2004-11-15): the host's file times differ per copy
    }
    return unix_us != 0 ? unix_us * 10u + 116444736000000000ull : 116444736000000000ull;
}

static uint32_t attributes_of(const XkFile *f, const WlPlatformStat *st) {
    if (f->kind == XK_FILE_DIRECTORY || (st != NULL && st->is_dir)) {
        return FILE_ATTRIBUTE_DIRECTORY;
    }
    return (st != NULL && st->read_only) || !f->writable ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
}

static void stat_of(const XkFile *f, WlPlatformStat *st) {
    memset(st, 0, sizeof *st);
    if (f->pf != NULL) {
        wl_platform_fs_fstat(f->pf, st);
    } else if (f->kind == XK_FILE_DIRECTORY) {
        wl_platform_fs_stat(f->host, st);
    }
}

static void put_times(uint32_t at, uint64_t ft) {
    for (uint32_t i = 0; i < 4; i++) {
        wl_xb_wr64(at + 8u * i, ft);
    }
}

// FILE_BASIC_INFORMATION (40 bytes): four times, attributes
static void put_basic(uint32_t at, const XkFile *f, const WlPlatformStat *st) {
    put_times(at, filetime_of(st->mtime_us));
    wl_xb_wr32(at + 32u, attributes_of(f, st));
    wl_xb_wr32(at + 36u, 0);
}

// FILE_STANDARD_INFORMATION (24): AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory
static void put_standard(uint32_t at, const XkFile *f) {
    uint64_t size = file_size(f);
    wl_xb_wr64(at + 0u, (size + 4095u) & ~4095ull);
    wl_xb_wr64(at + 8u, size);
    wl_xb_wr32(at + 16u, 1);
    wl_xb_wr8(at + 20u, f->fcb != NULL && f->fcb->delete_pending ? 1u : 0u);
    wl_xb_wr8(at + 21u, f->kind == XK_FILE_DIRECTORY ? 1u : 0u);
    wl_xb_wr16(at + 22u, 0);
}

// FILE_NETWORK_OPEN_INFORMATION (56): four times, AllocationSize, EndOfFile, attributes
static void put_network_open(uint32_t at, const XkFile *f, const WlPlatformStat *st) {
    uint64_t size = file_size(f);
    put_times(at, filetime_of(st->mtime_us));
    wl_xb_wr64(at + 32u, (size + 4095u) & ~4095ull);
    wl_xb_wr64(at + 40u, size);
    wl_xb_wr32(at + 48u, attributes_of(f, st));
    wl_xb_wr32(at + 52u, 0);
}

// NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length,
//                                 FILE_INFORMATION_CLASS)
void k_NtQueryInformationFile(void) {
    XkFile *f = file_of(ARG(0));
    uint32_t iosb = ARG(1), out = ARG(2), len = ARG(3), cls = ARG(4);
    if (f == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    WlPlatformStat st;
    stat_of(f, &st);
    uint32_t need;
    switch (cls) {
    case 4: need = 40; break;  // FileBasicInformation
    case 5: need = 24; break;  // FileStandardInformation
    case 6: need = 8; break;   // FileInternalInformation
    case 7: need = 4; break;   // FileEaInformation
    case 8: need = 4; break;   // FileAccessInformation
    case 9: need = 4u + (uint32_t)strlen(f->name); break; // FileNameInformation
    case 14: need = 8; break;  // FilePositionInformation
    case 16: need = 4; break;  // FileModeInformation
    case 17: need = 4; break;  // FileAlignmentInformation
    case 18: need = 100u + (uint32_t)strlen(f->name); break; // FileAllInformation
    case 34: need = 56; break; // FileNetworkOpenInformation
    case 35: need = 8; break;  // FileAttributeTagInformation
    default:
        WL_LOG_W(XK_TAG, "NtQueryInformationFile class %u not supported", (unsigned)cls);
        xk_iosb(iosb, STATUS_INVALID_PARAMETER, 0);
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    uint32_t fixed = cls == 9 ? 4u : cls == 18 ? 100u : need;
    if (len < fixed || !wl_xb_range_ok(out, fixed)) {
        xk_iosb(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
        RET(STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    uint32_t status = STATUS_SUCCESS, wrote = need;
    switch (cls) {
    case 4:
        put_basic(out, f, &st);
        break;
    case 5:
        put_standard(out, f);
        break;
    case 6:
        wl_xb_wr64(out, (uint64_t)(uintptr_t)f);
        break;
    case 7:
    case 8:
    case 16:
        wl_xb_wr32(out, cls == 8 ? 0x001F01FFu : 0u);
        break;
    case 14:
        wl_xb_wr64(out, f->pos);
        break;
    case 17:
        wl_xb_wr32(out, 0); // FILE_BYTE_ALIGNMENT
        break;
    case 9:
    case 18: {
        uint32_t at = out;
        if (cls == 18) {
            put_basic(out, f, &st);
            put_standard(out + 40u, f);
            wl_xb_wr64(out + 64u, (uint64_t)(uintptr_t)f);
            wl_xb_wr32(out + 72u, 0);
            wl_xb_wr32(out + 76u, 0x001F01FFu);
            wl_xb_wr64(out + 80u, f->pos);
            wl_xb_wr32(out + 88u, 0);
            wl_xb_wr32(out + 92u, 0);
            at = out + 96u;
        }
        uint32_t nlen = (uint32_t)strlen(f->name);
        uint32_t room = len - (at - out) - 4u;
        uint32_t copy = nlen < room ? nlen : room;
        wl_xb_wr32(at, nlen);
        wl_xb_copy_in(at + 4u, f->name, copy);
        wrote = (at - out) + 4u + copy;
        if (copy < nlen) {
            status = STATUS_BUFFER_OVERFLOW;
        }
        break;
    }
    case 34:
        put_network_open(out, f, &st);
        break;
    case 35:
        wl_xb_wr32(out, attributes_of(f, &st));
        wl_xb_wr32(out + 4u, 0);
        break;
    default:
        break;
    }
    xk_iosb(iosb, status, wrote);
    RET(status);
}

static uint32_t set_size(XkFile *f, uint64_t size) {
    if (!f->writable || f->pf == NULL) {
        return f->kind == XK_FILE_PARTITION ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    }
    uint64_t phys = physical_size(f);
    pages_forget(f, size, UINT64_MAX); // (a sparse file reads zero past its bytes: none remembered)
    if (f->sparse) {
        if (size < phys && wl_platform_fs_truncate(f->pf, size) != WL_OK) {
            return STATUS_DISK_FULL;
        }
        f->resized = true;
        if (f->fcb != NULL) {
            f->fcb->logical = size;
            fcb_sidecar_write(f->fcb, physical_size(f));
        } else {
            sidecar_write(f->host, size, physical_size(f));
        }
        return STATUS_SUCCESS;
    }
    return wl_platform_fs_truncate(f->pf, size) == WL_OK ? STATUS_SUCCESS : STATUS_DISK_FULL;
}

// NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length,
//                               FILE_INFORMATION_CLASS)
void k_NtSetInformationFile(void) {
    uint32_t handle = ARG(0), iosb = ARG(1), in = ARG(2), len = ARG(3), cls = ARG(4);
    XkFile *f = file_of(handle);
    if (f == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t status = STATUS_SUCCESS;
    switch (cls) {
    case 4: // FileBasicInformation: times and attributes, accepted
        break;
    case 10: { // FileRenameInformation: ReplaceIfExists, RootDirectory, FileName (OBJECT_STRING)
        char name[300], full[720];
        if (len < 16 || !wl_xb_range_ok(in, 16) || !xk_read_ansi(in + 8u, name, sizeof name)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        uint32_t root = wl_xb_rd32(in + 4u);
        XkFile *rf = root != 0 ? file_of(root) : NULL;
        snprintf(full, sizeof full, "%s%s%s", rf != NULL ? rf->name : "", rf != NULL ? "\\" : "", name);
        XkResolved r;
        if (!xk_resolve(full, &r) || r.kind != XK_RES_HOST || !r.mount.writable || f->pf == NULL ||
            f->fcb == NULL) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        bool there = xk_resolve_case(&r, NULL);
        bool replace = wl_xb_rd8(in) != 0;
        if (!replace && there) {
            status = STATUS_OBJECT_NAME_COLLISION;
            break;
        }
        const XkFcb *target = fcb_find(r.host);
        if (target != NULL && target != f->fcb) {
            status = STATUS_ACCESS_DENIED; // the file it would replace is open, as NT refuses
            break;
        }
        // every handle on the file lets go of the host file while it is renamed (a FAT driver may
        // not rename an open file), and all of them follow it to its new name
        for (XkFile *o = f->fcb->files; o != NULL; o = o->fcb_next) {
            if (o->pf != NULL) {
                wl_xb_file_forget(o->pf, 0, UINT64_MAX);
                wl_platform_fs_close(o->pf);
                o->pf = NULL;
                g_files_open--;
            }
        }
        char old_host[320];
        snprintf(old_host, sizeof old_host, "%s", f->host);
        bool moved = wl_platform_fs_rename(old_host, r.host) == WL_OK;
        if (!moved) {
            status = STATUS_ACCESS_DENIED;
        } else {
            char a[340], b[340];
            sidecar_path(old_host, a, sizeof a);
            sidecar_path(r.host, b, sizeof b);
            wl_platform_fs_remove(b); // a replaced file's size goes with it
            wl_platform_fs_rename(a, b);
            snprintf(f->fcb->host, sizeof f->fcb->host, "%s", r.host);
        }
        for (XkFile *o = f->fcb->files; o != NULL; o = o->fcb_next) {
            if (moved) {
                snprintf(o->host, sizeof o->host, "%s", r.host);
                snprintf(o->name, sizeof o->name, "%s", r.device_path);
            }
            if (wl_platform_fs_open(o->host, WL_FILE_READ_WRITE, &o->pf) == WL_OK) {
                g_files_open++;
            }
        }
        g_xk_stats.files_open = g_files_open;
        break;
    }
    case 13: // FileDispositionInformation: DeleteFile
        if (len < 1 || !wl_xb_range_ok(in, 1)) {
            status = STATUS_INVALID_PARAMETER;
        } else if (!f->writable || f->fcb == NULL) {
            status = STATUS_CANNOT_DELETE;
        } else {
            f->fcb->delete_pending = wl_xb_rd8(in) != 0; // done when its last handle closes
        }
        break;
    case 14: // FilePositionInformation
        if (len < 8 || !wl_xb_range_ok(in, 8)) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            f->pos = wl_xb_rd64(in);
        }
        break;
    case 19: // FileAllocationInformation: the space reserved, nothing to do
        break;
    case 20: // FileEndOfFileInformation
        if (len < 8 || !wl_xb_range_ok(in, 8)) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            status = set_size(f, wl_xb_rd64(in));
            WL_LOG_D(XK_TAG, "end of file of %s: 0x%llx (0x%08x)", f->host, (unsigned long long)wl_xb_rd64(in),
                     (unsigned)status);
        }
        break;
    case 16: // FileModeInformation
        break;
    default:
        WL_LOG_W(XK_TAG, "NtSetInformationFile class %u not supported", (unsigned)cls);
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    xk_iosb(iosb, status, 0);
    RET(status);
}

// NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION)
void k_NtQueryFullAttributesFile(void) {
    char name[400];
    uint32_t status = 0, out = ARG(1);
    XkResolved r;
    if (!xk_object_name(ARG(0), name, sizeof name, &status)) {
        RET(status);
        return;
    }
    if (!xk_resolve(name, &r) || r.kind != XK_RES_HOST) {
        RET(STATUS_OBJECT_PATH_NOT_FOUND);
        return;
    }
    WlPlatformStat st;
    if (!xk_resolve_case(&r, &st)) {
        RET(STATUS_OBJECT_NAME_NOT_FOUND);
        return;
    }
    if (!wl_xb_range_ok(out, 56)) {
        RET(STATUS_ACCESS_VIOLATION);
        return;
    }
    XkFile tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.kind = st.is_dir ? XK_FILE_DIRECTORY : XK_FILE_REGULAR;
    tmp.writable = r.mount.writable;
    tmp.sparse = r.mount.sparse;
    uint64_t size = st.size;
    if (r.mount.sparse && !st.is_dir) {
        size = sparse_size(r.host, size);
    }
    put_times(out, filetime_of(st.mtime_us));
    wl_xb_wr64(out + 32u, (size + 4095u) & ~4095ull);
    wl_xb_wr64(out + 40u, size);
    wl_xb_wr32(out + 48u, attributes_of(&tmp, &st));
    wl_xb_wr32(out + 52u, 0);
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID FsInformation, ULONG Length,
//                                       FS_INFORMATION_CLASS)
void k_NtQueryVolumeInformationFile(void) {
    XkFile *f = file_of(ARG(0));
    uint32_t iosb = ARG(1), out = ARG(2), len = ARG(3), cls = ARG(4);
    if (f == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    uint64_t total = f->kind == XK_FILE_PARTITION ? xk_raw_size(f->partition) : (f->writable ? 750ull << 20 : 0x1C0000000ull);
    uint64_t units = total / 16384u, free_units = f->writable ? units - units / 8u : 0;
    uint32_t status = STATUS_SUCCESS, wrote = 0;
    switch (cls) {
    case 1: // FileFsVolumeInformation: CreationTime, SerialNumber, LabelLength, SupportsObjects, Label
        if (len < 18 || !wl_xb_range_ok(out, 18)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        wl_xb_wr64(out, 0);
        wl_xb_wr32(out + 8u, 0x57494C42u);
        wl_xb_wr32(out + 12u, 0);
        wl_xb_wr8(out + 16u, 0);
        wrote = 18;
        break;
    case 3: // FileFsSizeInformation: Total, Available, SectorsPerAllocationUnit, BytesPerSector
        if (len < 24 || !wl_xb_range_ok(out, 24)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        wl_xb_wr64(out, units);
        wl_xb_wr64(out + 8u, free_units);
        wl_xb_wr32(out + 16u, 32);
        wl_xb_wr32(out + 20u, 512);
        wrote = 24;
        break;
    case 4: // FileFsDeviceInformation: DeviceType, Characteristics
        if (len < 8 || !wl_xb_range_ok(out, 8)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        wl_xb_wr32(out, f->writable ? 0x07u : 0x02u); // FILE_DEVICE_DISK / CD_ROM
        wl_xb_wr32(out + 4u, f->writable ? 0u : 0x1u); // FILE_REMOVABLE_MEDIA
        wrote = 8;
        break;
    case 5: { // FileFsAttributeInformation: attributes, max component length, name length, name
        const char *fs = f->writable ? "FATX" : "UDF";
        uint32_t nlen = (uint32_t)strlen(fs);
        if (len < 12u + nlen || !wl_xb_range_ok(out, 12u + nlen)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        wl_xb_wr32(out, 0x2u); // FILE_CASE_PRESERVED_NAMES
        wl_xb_wr32(out + 4u, 42);
        wl_xb_wr32(out + 8u, nlen);
        wl_xb_copy_in(out + 12u, fs, nlen);
        wrote = 12u + nlen;
        break;
    }
    case 7: // FileFsFullSizeInformation
        if (len < 32 || !wl_xb_range_ok(out, 32)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        wl_xb_wr64(out, units);
        wl_xb_wr64(out + 8u, free_units);
        wl_xb_wr64(out + 16u, free_units);
        wl_xb_wr32(out + 24u, 32);
        wl_xb_wr32(out + 28u, 512);
        wrote = 32;
        break;
    default:
        WL_LOG_W(XK_TAG, "NtQueryVolumeInformationFile class %u not supported", (unsigned)cls);
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    xk_iosb(iosb, status, wrote);
    RET(status);
}

// ---- Directories ---------------------------------------------------------------------------------------

static bool wild_match(const char *pat, const char *name) {
    if (*pat == '\0') {
        return *name == '\0';
    }
    if (*pat == '*') {
        for (const char *n = name;; n++) {
            if (wild_match(pat + 1, n)) {
                return true;
            }
            if (*n == '\0') {
                return false;
            }
        }
    }
    if (*name == '\0') {
        return false;
    }
    char a = (*pat >= 'A' && *pat <= 'Z') ? (char)(*pat + 32) : *pat;
    char b = (*name >= 'A' && *name <= 'Z') ? (char)(*name + 32) : *name;
    return (a == '?' || a == b) && wild_match(pat + 1, name + 1);
}

// A listing's mask against a name, with the DOS rule NT keeps: a mask ending in ".*" also matches
// a name without that extension ("*.*" every name, "save*.*" "save01").
static bool name_matches(const char *pat, const char *name) {
    if (wild_match(pat, name)) {
        return true;
    }
    size_t pl = strlen(pat);
    if (pl >= 2 && pat[pl - 2] == '.' && pat[pl - 1] == '*') {
        char base[128];
        snprintf(base, sizeof base, "%.*s", (int)(pl - 2), pat);
        return wild_match(base, name);
    }
    return false;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//     PIO_STATUS_BLOCK, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS,
//     POBJECT_STRING FileName, BOOLEAN RestartScan): one entry a call
void k_NtQueryDirectoryFile(void) {
    uint32_t handle = ARG(0), iosb = ARG(4), out = ARG(5), len = ARG(6), cls = ARG(7), mask = ARG(8);
    bool restart = (ARG(9) & 0xFFu) != 0;
    XkFile *f = file_of(handle);
    if (f == NULL || f->kind != XK_FILE_DIRECTORY) {
        RET(f == NULL ? STATUS_INVALID_HANDLE : STATUS_INVALID_PARAMETER);
        return;
    }
    if (cls != 1 && cls != 2 && cls != 3 && cls != 12) {
        WL_LOG_W(XK_TAG, "NtQueryDirectoryFile class %u not supported", (unsigned)cls);
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    if (restart || !f->listing) {
        if (f->dir != NULL) {
            wl_platform_fs_dir_close(f->dir);
            f->dir = NULL;
        }
        if (mask != 0 && xk_read_ansi(mask, f->pattern, sizeof f->pattern) && f->pattern[0] != '\0') {
            // a mask only arrives with the first call
        } else if (!f->listing) {
            snprintf(f->pattern, sizeof f->pattern, "*");
        }
        if (wl_platform_fs_dir_open(f->host, &f->dir) != WL_OK) {
            RET(STATUS_NO_SUCH_FILE);
            return;
        }
        f->listing = true;
        f->listed = 0;
        f->held = false;
    }
    char name[256];
    WlPlatformStat st;
    for (;;) {
        if (f->held) { // the entry the last call's buffer could not take
            f->held = false;
            snprintf(name, sizeof name, "%s", f->held_name);
            st = f->held_st;
            break;
        }
        if (f->dir == NULL || wl_platform_fs_dir_next(f->dir, name, sizeof name, &st) != WL_OK) {
            if (f->dir != NULL) {
                wl_platform_fs_dir_close(f->dir);
                f->dir = NULL;
            }
            uint32_t s = f->listed == 0 ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
            xk_iosb(iosb, s, 0);
            RET(s);
            return;
        }
        size_t nl = strlen(name);
        if (nl > strlen(SIDECAR) && strcasecmp(name + nl - strlen(SIDECAR), SIDECAR) == 0) {
            continue;
        }
        if (name_matches(f->pattern, name)) {
            break;
        }
    }
    uint32_t nlen = (uint32_t)strlen(name);
    uint64_t size = st.size;
    if (f->sparse && !st.is_dir) {
        char full[600];
        snprintf(full, sizeof full, "%s/%s", f->host, name);
        size = sparse_size(full, size);
    }
    uint32_t header = cls == 12 ? 12u : cls == 1 ? 0x40u : cls == 2 ? 0x44u : 0x5Eu;
    if (len < header + nlen || !wl_xb_range_ok(out, header + nlen)) {
        f->held = true; // not lost: the next call returns it
        snprintf(f->held_name, sizeof f->held_name, "%s", name);
        f->held_st = st;
        xk_iosb(iosb, STATUS_BUFFER_OVERFLOW, 0);
        RET(STATUS_BUFFER_OVERFLOW);
        return;
    }
    wl_xb_fill(out, 0, header);
    wl_xb_wr32(out + 0u, 0);          // NextEntryOffset: the only entry
    wl_xb_wr32(out + 4u, f->listed);  // FileIndex
    if (cls == 12) {                  // FileNamesInformation
        wl_xb_wr32(out + 8u, nlen);
    } else {
        put_times(out + 8u, filetime_of(st.mtime_us));
        wl_xb_wr64(out + 40u, size);
        wl_xb_wr64(out + 48u, (size + 4095u) & ~4095ull);
        wl_xb_wr32(out + 56u, st.is_dir ? FILE_ATTRIBUTE_DIRECTORY : (f->writable ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY));
        wl_xb_wr32(out + 60u, nlen);
    }
    wl_xb_copy_in(out + header, name, nlen);
    f->listed++;
    xk_iosb(iosb, STATUS_SUCCESS, header + nlen);
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtDeleteFile(POBJECT_ATTRIBUTES)
void k_NtDeleteFile(void) {
    char name[400];
    uint32_t status = 0;
    XkResolved r;
    if (!xk_object_name(ARG(0), name, sizeof name, &status)) {
        RET(status);
        return;
    }
    if (!xk_resolve(name, &r) || r.kind != XK_RES_HOST || !r.mount.writable) {
        RET(STATUS_ACCESS_DENIED);
        return;
    }
    WlPlatformStat st;
    if (!xk_resolve_case(&r, &st)) {
        RET(STATUS_OBJECT_NAME_NOT_FOUND);
        return;
    }
    XkFcb *c = fcb_find(r.host);
    if (c != NULL) {
        c->delete_pending = true; // open: deleted when its last handle closes, as NT does
        RET(STATUS_SUCCESS);
        return;
    }
    WlResult rr = st.is_dir ? wl_platform_fs_rmdir(r.host) : wl_platform_fs_remove(r.host);
    if (rr == WL_OK && !st.is_dir && r.mount.sparse) {
        char side[340];
        sidecar_path(r.host, side, sizeof side);
        wl_platform_fs_remove(side);
    }
    RET(rr == WL_OK ? STATUS_SUCCESS : rr == WL_ERR_DENIED ? STATUS_DIRECTORY_NOT_EMPTY : STATUS_CANNOT_DELETE);
}

// ---- Device and file system controls ---------------------------------------------------------------

#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u
#define IOCTL_SCSI_PASS_THROUGH_DIRECT 0x0004D014u

// NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext,
//     PIO_STATUS_BLOCK, ULONG IoControlCode, PVOID InputBuffer, ULONG InputBufferLength,
//     PVOID OutputBuffer, ULONG OutputBufferLength)
void k_NtDeviceIoControlFile(void) {
    XkFile *f = file_of(ARG(0));
    uint32_t iosb = ARG(4), code = ARG(5), in = ARG(6), in_len = ARG(7), out = ARG(8), out_len = ARG(9);
    if (f == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t status = STATUS_SUCCESS, info = 0;
    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        // DISK_GEOMETRY: Cylinders (8), MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector
        if (out_len < 24 || !wl_xb_range_ok(out, 24)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            uint64_t sectors = xk_raw_size(f->partition) / 512u;
            wl_xb_wr64(out, sectors / (255u * 63u));
            wl_xb_wr32(out + 8u, 0x0Cu); // FixedMedia
            wl_xb_wr32(out + 12u, 255);
            wl_xb_wr32(out + 16u, 63);
            wl_xb_wr32(out + 20u, 512);
            info = 24;
        }
    } else if (code == IOCTL_DISK_GET_PARTITION_INFO) {
        // PARTITION_INFORMATION: StartingOffset, PartitionLength, HiddenSectors, PartitionNumber, type...
        if (out_len < 32 || !wl_xb_range_ok(out, 32)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            wl_xb_fill(out, 0, 32);
            wl_xb_wr64(out + 8u, xk_raw_size(f->partition));
            wl_xb_wr32(out + 20u, f->partition);
            wl_xb_wr8(out + 24u, 0x06); // PartitionType
            wl_xb_wr8(out + 26u, 1);    // RecognizedPartition
            info = 32;
        }
    } else if (code == IOCTL_SCSI_PASS_THROUGH_DIRECT && in_len >= 44 && wl_xb_range_ok(in, 44)) {
        // the DVD's media check (MODE SENSE(10) of the authentication page): a game disc in the
        // drive, as xboxrecomp answers it - byte 11 of the page 1, bytes 10 and 12 not zero
        uint32_t data = wl_xb_rd32(in + 20u), data_len = wl_xb_rd32(in + 12u);
        if (wl_xb_rd8(in + 28u) == 0x5Au && data != 0 && wl_xb_range_ok(data, data_len)) {
            wl_xb_fill(data, 0, data_len);
            if (data_len >= 2) {
                wl_xb_wr8(data + 1u, 26);
            }
            if (data_len >= 10) {
                wl_xb_wr8(data + 8u, 0x3E);
                wl_xb_wr8(data + 9u, 18);
            }
            if (data_len >= 13) {
                wl_xb_wr8(data + 10u, 1);
                wl_xb_wr8(data + 11u, 1);
                wl_xb_wr8(data + 12u, 1);
            }
        }
        wl_xb_wr8(in + 2u, 0); // ScsiStatus: good
        info = in_len;
    } else {
        WL_LOG_I(XK_TAG, "NtDeviceIoControlFile(0x%08x) on %s: zeros", (unsigned)code, f->name);
        if (out_len > 0 && wl_xb_range_ok(out, out_len)) {
            wl_xb_fill(out, 0, out_len);
        }
        info = out_len;
    }
    xk_iosb(iosb, status, info);
    RET(status);
}

// NTSTATUS NtFsControlFile(...the same shape...): dismounts and the like succeed
void k_NtFsControlFile(void) {
    uint32_t iosb = ARG(4), code = ARG(5), out = ARG(8), out_len = ARG(9);
    WL_LOG_I(XK_TAG, "NtFsControlFile(0x%08x): success", (unsigned)code);
    if (out_len > 0 && wl_xb_range_ok(out, out_len)) {
        wl_xb_fill(out, 0, out_len);
    }
    xk_iosb(iosb, STATUS_SUCCESS, 0);
    RET(STATUS_SUCCESS);
}

// ---- Symbolic links ----------------------------------------------------------------------------------

// NTSTATUS IoCreateSymbolicLink(POBJECT_STRING SymbolicLinkName, POBJECT_STRING DeviceName)
void k_IoCreateSymbolicLink(void) {
    char link[64], target[200];
    if (!xk_read_ansi(ARG(0), link, sizeof link) || !xk_read_ansi(ARG(1), target, sizeof target)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    RET(xk_link_create(link, target) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES);
}

// NTSTATUS IoDeleteSymbolicLink(POBJECT_STRING SymbolicLinkName)
void k_IoDeleteSymbolicLink(void) {
    char link[64];
    if (!xk_read_ansi(ARG(0), link, sizeof link)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    RET(xk_link_delete(link) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND);
}

// NTSTATUS NtOpenSymbolicLinkObject(PHANDLE LinkHandle, POBJECT_ATTRIBUTES ObjectAttributes)
void k_NtOpenSymbolicLinkObject(void) {
    char name[200];
    uint32_t status = 0;
    if (!xk_object_name(ARG(1), name, sizeof name, &status)) {
        RET(status);
        return;
    }
    const char *target = xk_link_lookup(name);
    if (target == NULL) {
        RET(STATUS_OBJECT_NAME_NOT_FOUND);
        return;
    }
    XkObject *o = xk_object_new(XK_OBJ_SYMLINK, 0);
    if (o == NULL) {
        RET(STATUS_NO_MEMORY);
        return;
    }
    size_t tl = strlen(target);
    o->link_target = malloc(tl + 1u);
    if (o->link_target != NULL) {
        memcpy(o->link_target, target, tl + 1u);
    }
    uint32_t h = xk_handle_open(o);
    if (ARG(0) != 0 && wl_xb_range_ok(ARG(0), 4)) {
        wl_xb_wr32(ARG(0), h);
    }
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtQuerySymbolicLinkObject(HANDLE LinkHandle, POBJECT_STRING LinkTarget, PULONG ReturnedLength)
void k_NtQuerySymbolicLinkObject(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_SYMLINK);
    if (o == NULL || o->link_target == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    if (ARG(2) != 0 && wl_xb_range_ok(ARG(2), 4)) {
        wl_xb_wr32(ARG(2), (uint32_t)strlen(o->link_target));
    }
    RET(xk_write_ansi(ARG(1), o->link_target));
}

void xk_link_nt(void) {
}
