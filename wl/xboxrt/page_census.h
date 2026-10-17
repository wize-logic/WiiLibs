// SPDX-License-Identifier: MIT
// The page census: a diagnostic of what a title's guest pages are and how many it uses each second
// (its working set). Off unless WL_XB_PAGE_CENSUS names a file when guest memory starts, and then
// only under a frame limit on the host (the host's backend sees every touch as a fault); off, each
// hook is a load and a branch.
//
// Each physical page gets
//   - the allocation it belongs to (wl_xb_census_alloc: the XBE's sections, contiguous and system
//     memory, NtAllocateVirtualMemory, the pool, thread stacks), with the guest's call site;
//   - what used it, sticky: written, read by the GPU (a texture it converted, a vertex array it
//     cached, anything else it read), by the APU, filled by a file read (a map or another file);
//   - the seconds it was touched in: the first, the last and how many.
// Every second of guest time (60 vertical blanks) the pages touched in it are counted by class and
// a line goes to the file, with the pages touched in the last 5, 10 and 30 s (the working set over
// a window); the touched pages' guest EAs are closed again, so a touch in the next second faults
// once more (the census leaves the paging's own R bits alone: the clock decides as without it).
// `<file>.pages` gets the allocations and every page's record every 30 s and at exit;
// tools/xb_page_census.py makes the tables. With WL_XB_PAGE_CENSUS_STACKS=1 too (x86-64 glibc hosts), each
// touch's host call stack is kept (`<file>.stacks`, `<file>.touch`): tools/xb_page_who.py says who
// touched (a guest function, the texture cache hashing or converting, the kernel reading a file).
#ifndef WL_XBOXRT_PAGE_CENSUS_H
#define WL_XBOXRT_PAGE_CENSUS_H

#include <stdbool.h>
#include <stdint.h>

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

// what used a page (sticky)
#define WL_XB_CU_WRITE 0x01u // written by anything
#define WL_XB_CU_GPU 0x02u   // read by the GPU
#define WL_XB_CU_TEX 0x04u   // a texture (or a resolved target) the GPU converted: its GX copy exists
#define WL_XB_CU_VTX 0x08u   // a vertex array the GPU cached converted
#define WL_XB_CU_APU 0x10u   // read by the APU (voices, sample buffers)
#define WL_XB_CU_MAP 0x20u   // filled by a read of a map file (a name with ".map")
#define WL_XB_CU_FILE 0x40u  // filled by a read of any other file

// what an allocation was
typedef enum {
    WL_XB_CA_NONE = 0,
    WL_XB_CA_XBE_CODE, // an executable section of the XBE (x86 code: the recompiled code never runs it)
    WL_XB_CA_XBE_DATA, // the XBE's headers and its other sections
    WL_XB_CA_CONTIG,   // MmAllocateContiguousMemory(Ex)
    WL_XB_CA_SYSTEM,   // MmAllocateSystemMemory
    WL_XB_CA_VIRTUAL,  // NtAllocateVirtualMemory (committed)
    WL_XB_CA_POOL,     // ExAllocatePool's chunks
    WL_XB_CA_STACK,    // a guest thread's stack
    WL_XB_CA_COUNT
} WlXbCensusAlloc;

extern uint8_t *g_xb_census_use; // [ram pages] NULL while the census is off

// Starts it, writing to `path` (guest memory ready). WL_ERR_STATE without a frame limit.
WlResult wl_xb_census_start(const char *path);
bool wl_xb_census_on(void);

void wl_xb_census_seen_slow(uint32_t p, bool write);
void wl_xb_census_phys_slow(uint32_t phys, uint32_t bytes, uint8_t use);
void wl_xb_census_va_slow(uint32_t va, uint32_t bytes, uint8_t use);
void wl_xb_census_alloc_slow(WlXbCensusAlloc kind, uint32_t va, uint32_t bytes, uint32_t caller);

// A touch of physical page `p` (the backend's fault).
static inline void wl_xb_census_seen(uint32_t p, bool write) {
    if (g_xb_census_use != NULL) {
        wl_xb_census_seen_slow(p, write);
    }
}

// A device used the physical bytes [phys, phys + bytes) as `use`.
static inline void wl_xb_census_phys(uint32_t phys, uint32_t bytes, uint8_t use) {
    if (g_xb_census_use != NULL) {
        wl_xb_census_phys_slow(phys, bytes, use);
    }
}

// The same through a guest VA (a file read's buffer).
static inline void wl_xb_census_va(uint32_t va, uint32_t bytes, uint8_t use) {
    if (g_xb_census_use != NULL) {
        wl_xb_census_va_slow(va, bytes, use);
    }
}

// The VAs [va, va + bytes) (committed: they have pages) were allocated as `kind` from `caller` (a
// guest return address, a section's index, or 0).
static inline void wl_xb_census_alloc(WlXbCensusAlloc kind, uint32_t va, uint32_t bytes, uint32_t caller) {
    if (g_xb_census_use != NULL) {
        wl_xb_census_alloc_slow(kind, va, bytes, caller);
    }
}

// ---- The card's traffic (where SD reads and writes come from), counted by second ------------------
// Each second with traffic gets `io <second> <kind> ops <n> pages <n> map <n> fresh <n> <class> <n>...
// @<allocation> <pages>/<fresh>...` lines (a page's class as in the second's line, its allocation's number
// in `<file>.pages`; `map`: pages a map read filled; `fresh`: pages a file
// read filled and no device (GPU, APU) used since: a texture evicted or read back before its conversion)
// and a `card <second> ...` line for the kernel's own file I/O. tools/xb_paging_io.py makes the tables.
typedef enum {
    WL_XB_CIO_SWAP_IN,      // a page read back from the swap file (an op each)
    WL_XB_CIO_SWAP_OUT,     // a page written to the swap file (an op for each run of slots)
    WL_XB_CIO_FILE_IN,      // a file copy read back from its file (an op each, its read-ahead with it)
    WL_XB_CIO_AHEAD,        // a file copy that came in that read after it (no op of its own)
    WL_XB_CIO_ZHIT,         // a page unpacked from the compressed page cache (no op)
    WL_XB_CIO_EVICT,        // a page losing its frame (any way)
    WL_XB_CIO_AHEAD_UNUSED, // of those, a read-ahead page never used
    WL_XB_CIO_FORGET,       // a file copy forgotten (its file changed or closed): a swap write later
    WL_XB_CIO_COUNT
} WlXbCensusIo;

// The kernel's own card I/O: a read with the guest lock given up, one with it kept (a read under
// XK_BLOCK_MIN: the other threads wait, as for the pager's), a read by the file read-ahead's thread, or
// a write, of `bytes` of a map file (`map`) or another, as one op.
typedef enum {
    WL_XB_CCARD_READ,
    WL_XB_CCARD_READ_HELD,
    WL_XB_CCARD_AHEAD,
    WL_XB_CCARD_WRITE,
    WL_XB_CCARD_COUNT
} WlXbCensusCard;

void wl_xb_census_io_slow(WlXbCensusIo kind, uint32_t p, bool op);
void wl_xb_census_card_slow(WlXbCensusCard kind, bool map, uint32_t bytes);

// Page `p` went through `kind`; `op`: it started a card transfer.
static inline void wl_xb_census_io(WlXbCensusIo kind, uint32_t p, bool op) {
    if (g_xb_census_use != NULL) {
        wl_xb_census_io_slow(kind, p, op);
    }
}

static inline void wl_xb_census_card(WlXbCensusCard kind, bool map, uint32_t bytes) {
    if (g_xb_census_use != NULL) {
        wl_xb_census_card_slow(kind, map, bytes);
    }
}

// The counts of the current second so far, for the tests: `kind`'s ops and pages, and of the pages
// those of class "fresh" (filled from a file, no device use since).
void wl_xb_census_io_now(WlXbCensusIo kind, uint32_t *ops, uint32_t *pages, uint32_t *fresh);
void wl_xb_census_card_now(WlXbCensusCard kind, uint32_t *ops, uint64_t *bytes);

// Ends the current second now (the vertical blank's hook does it every 60): its line written, the
// touched pages' EAs closed. Returns the pages touched in it.
uint32_t wl_xb_census_interval(void);
// Writes `<file>.pages`.
void wl_xb_census_dump(void);

// Page `p`'s record, for the tests: its use bits, the seconds it was touched in (first and last, 1
// for the first second; 0: never) and how many, its allocation's kind.
void wl_xb_census_page(uint32_t p, uint8_t *use, uint32_t *first, uint32_t *last, uint32_t *seconds,
                       WlXbCensusAlloc *kind);

#ifdef __cplusplus
}
#endif

#endif // WL_XBOXRT_PAGE_CENSUS_H
