// SPDX-License-Identifier: MIT
// XBE images: the header, sections, certificate and TLS directory parsed from the file's bytes
// (all little-endian), and the loader that maps them into guest memory and resolves the kernel
// thunk table. The entry point and thunk address are XOR-encoded with the retail or the debug
// key; the one that lands inside the image is taken.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XBE_MAX_SECTIONS 64u
#define WL_XBE_ENTRY_KEY_RETAIL 0xA8FC57ABu
#define WL_XBE_ENTRY_KEY_DEBUG 0x94859D4Bu
#define WL_XBE_THUNK_KEY_RETAIL 0x5B6D40B6u
#define WL_XBE_THUNK_KEY_DEBUG 0xEFB1F152u

#define WL_XBE_SEC_WRITABLE 0x1u
#define WL_XBE_SEC_PRELOAD 0x2u
#define WL_XBE_SEC_EXECUTABLE 0x4u
#define WL_XBE_SEC_INSERTED 0x8u

typedef struct {
    char name[16];
    uint32_t flags;
    uint32_t va, vsize;
    uint32_t raw_offset, raw_size;
} WlXbeSection;

typedef struct {
    uint32_t base;          // 0x00010000
    uint32_t headers_size;  // bytes of headers copied to base
    uint32_t image_size;
    uint32_t entry;         // decoded
    uint32_t thunk_table;   // decoded
    bool debug_keys;        // the debug XOR keys decoded them
    uint32_t init_flags;
    uint32_t stack_commit;
    uint32_t heap_reserve, heap_commit;
    uint32_t tls_dir;       // VA of the TLS directory (0: none)
    uint32_t cert_va;
    uint32_t section_count;
    WlXbeSection sections[WL_XBE_MAX_SECTIONS];
    // certificate
    uint32_t title_id;
    char title_name[41];    // ASCII of the UTF-16 name
    uint32_t game_region, game_ratings, allowed_media, version;
    uint8_t lan_key[16], signature_key[16];
} WlXbe;

// Parses the headers from `head` (the file's first `head_bytes` bytes: at least headers_size, so
// read 0x1000 first and again if headers_size says more). WL_ERR_FORMAT for a file that is not an
// XBE or whose tables lie outside `head`, WL_ERR_RANGE when headers_size exceeds `head_bytes`.
WlResult wl_xbe_parse(const uint8_t *head, uint32_t head_bytes, WlXbe *out);

// Reads the file's bytes [offset, offset + n) for the loader.
typedef WlResult (*WlXbeRead)(void *user, uint32_t offset, void *dst, uint32_t n);

// Maps an image: reserves and commits [base, base + image_size) of user space, copies the headers
// and every section's raw bytes (the rest of each section is zero), sets the code bounds
// (executable sections) for the generated code's indirect-call check and the TLS template for new
// threads. Needs wl_xb_mem_init. The file is read through `read` in chunks (a bounce buffer: the
// Wii's file layer must never DMA into guest memory).
WlResult wl_xbe_load(const WlXbe *xbe, WlXbeRead read, void *user);
// wl_xbe_load, and each whole guest page of a section's raw bytes remembers that it came from
// `file` (mem.h, wl_xb_file_pages): while the title leaves it unchanged (its x86 code, constants),
// paging gives its frame back with nothing written to the swap file and reads it from the file
// again when touched. The caller keeps `file` open and unchanged for as long as the image lives.
// A page shared by two sections, or with a section's zero tail, stays an ordinary copy. NULL:
// wl_xbe_load.
struct WlPlatformFile;
WlResult wl_xbe_load_backed(const WlXbe *xbe, WlXbeRead read, void *user, struct WlPlatformFile *file);

// How an import resolves (the kernel's answer for an ordinal).
typedef struct {
    bool data;            // a data export: the slot gets `data_va`
    uint32_t data_va;
    void (*fn)(void);     // a function: the slot gets 0xFE000000 + slot * 4
    uint32_t arg_bytes;   // the stdcall bytes the function pops
    const char *name;
} WlXbImport;
typedef bool (*WlXbImportResolve)(void *user, uint32_t ordinal, WlXbImport *out);

// Resolves the loaded image's kernel thunk table: each slot 0x80000000|ordinal becomes the import's
// data VA or synthetic code address (a function the resolver does not know is bound to a logged
// stub that returns 0). Ordinals pass through `remap` first when given (a title built with another
// XDK: remap[ordinal] != 0 replaces it). With `look_behind`, a run of further thunks just below
// the declared table (after one NULL word) is resolved too, as some builds have. Returns the count
// of slots resolved in `*count`.
WlResult wl_xbe_resolve_thunks(const WlXbe *xbe, WlXbImportResolve resolve, void *user, const uint16_t *remap,
                               uint32_t remap_count, bool look_behind, uint32_t *count);

#ifdef __cplusplus
}
#endif
