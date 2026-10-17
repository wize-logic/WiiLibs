// SPDX-License-Identifier: MIT
// Guest memory of a recompiled Xbox title: the x86's little-endian bytes behind the generated
// code's XBOX_PTR, the guest-physical page model, the allocators the kernel builds on, and the
// accessors runtime code reads guest memory with.
//
// Addresses. A guest virtual address (VA, what the title's pointers hold) becomes a host
// effective address (EA) by WL_XB_EA: user space 0x0xxxxxxx keeps its value, the contiguous
// window 0x8xxxxxxx and its tiled alias 0xFxxxxxxx (with the devices above 0xFC000000) land at
// 0x1xxxxxxx. The host pointer is the EA plus g_xbox_mem_offset (the base of a 512 MB
// reservation on the host and ppc-be; 0 on the Wii, where the EA is the address the MMU
// translates through the page table).
//
// Pages. Guest RAM is ram_bytes of guest-physical pages (64 MB retail). Every committed user page
// (the XBE image, stacks, NtAllocateVirtualMemory) is backed by a physical page from the same
// allocator the contiguous allocations use, and the window VA 0x80000000|P shows physical page P
// whether it is allocated or not (as the console's RAM does). A host frame backs a physical page
// on its first touch, zero-filled, and both EAs of the page reach the same frame. A user VA that is
// not committed, or a window address past the RAM, faults: a guest crash. Device addresses fault
// too, and are emulated (mmio.h).
//
// Demand paging. Where the frames are fewer than the pages a title touches (the Wii: ~33 MB of
// frames for 64 MB of guest RAM), a page's frame is taken back when none is free: a clock over the
// frames, with the MMU's referenced bits, picks a page not used lately, preferring one with
// nothing to write; a changed page goes to the swap file first, in batches, and comes back from it
// on its next touch. Pinned pages (wl_xb_mem_pin) keep their frames. Any access may fault and wait
// for the swap file, so guest memory is touched only with the guest lock held, pinned pages aside
// (wl/xboxrt/README.md, "Demand paging").
//
// All functions here run with the guest lock held (thread.h) unless they say otherwise.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Address transform -------------------------------------------------------------------------

#define WL_XB_EA(va) ((((uint32_t)(va)) & 0x0FFFFFFFu) | ((((uint32_t)(va)) >> 3) & 0x10000000u))
#define WL_XB_EA_SPAN 0x20000000u     // EAs 0..0x1FFFFFFF: the host reservation
#define WL_XB_PAGE 4096u
#define WL_XB_PAGE_SHIFT 12u
#define WL_XB_USER_LIMIT 0x10000000u  // user VAs live below this (above it they alias low EAs)
#define WL_XB_WINDOW 0x80000000u      // VA of physical address 0 (the contiguous window)
#define WL_XB_TILED 0xF0000000u       // the write-combined alias of the window: the same EAs
#define WL_XB_EA_WINDOW 0x10000000u   // EA of physical address 0
#define WL_XB_MMIO_BASE 0xFC000000u   // guest VAs from here up are devices (EA 0x1C000000 up)
#define WL_XB_EA_MMIO 0x1C000000u

#if defined(__wii__)
#define WL_XB_HOST_BASE ((uintptr_t)0)
#else
extern ptrdiff_t g_xbox_mem_offset; // the generated code's name for the host base (regs.c)
#define WL_XB_HOST_BASE ((uintptr_t)g_xbox_mem_offset)
#endif

// The host pointer of a guest VA (no check: the access faults where the guest's would).
static inline void *wl_xb_ptr(uint32_t va) {
    return (void *)(WL_XB_HOST_BASE + (uintptr_t)WL_XB_EA(va));
}
// The host pointer of guest-physical bytes (through the window).
static inline void *wl_xb_phys_ptr(uint32_t phys) {
    return (void *)(WL_XB_HOST_BASE + (uintptr_t)WL_XB_EA_WINDOW + (uintptr_t)phys);
}
// The guest VA an EA came from: user space as it is, the window as 0x80000000|P, devices as
// 0xFC000000 up. (0xF0000000|P aliases the window and is reported as the window.)
uint32_t wl_xb_ea_to_va(uint32_t ea);

// ---- Little-endian accessors for runtime code ------------------------------------------------
// Byte order is the guest's on every host. Unaligned addresses are fine. Never cast guest bytes
// to a host struct: read fields with these.

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WL_XB_LE16(v) __builtin_bswap16(v)
#define WL_XB_LE32(v) __builtin_bswap32(v)
#define WL_XB_LE64(v) __builtin_bswap64(v)
#else
#define WL_XB_LE16(v) (v)
#define WL_XB_LE32(v) (v)
#define WL_XB_LE64(v) (v)
#endif

static inline uint8_t wl_xb_rd8(uint32_t va) {
    return *(const volatile uint8_t *)wl_xb_ptr(va);
}
// An aligned access is one volatile load or store of its width: a device's register (the MMIO
// window faults and is emulated) must see one access, which a memcpy does not promise (the
// compiler may split it into bytes, or newlib's memcpy use string moves the fault decoder refuses).
static inline uint16_t wl_xb_rd16(uint32_t va) {
    uint16_t v;
    if ((va & 1u) == 0) {
        v = *(const volatile uint16_t *)wl_xb_ptr(va);
    } else {
        __builtin_memcpy(&v, wl_xb_ptr(va), 2);
    }
    return WL_XB_LE16(v);
}
static inline uint32_t wl_xb_rd32(uint32_t va) {
    uint32_t v;
    if ((va & 3u) == 0) {
        v = *(const volatile uint32_t *)wl_xb_ptr(va);
    } else {
        __builtin_memcpy(&v, wl_xb_ptr(va), 4);
    }
    return WL_XB_LE32(v);
}
static inline uint64_t wl_xb_rd64(uint32_t va) {
    return (uint64_t)wl_xb_rd32(va) | ((uint64_t)wl_xb_rd32(va + 4u) << 32);
}
static inline void wl_xb_wr8(uint32_t va, uint8_t v) {
    *(volatile uint8_t *)wl_xb_ptr(va) = v;
}
static inline void wl_xb_wr16(uint32_t va, uint16_t v) {
    v = WL_XB_LE16(v);
    if ((va & 1u) == 0) {
        *(volatile uint16_t *)wl_xb_ptr(va) = v;
    } else {
        __builtin_memcpy(wl_xb_ptr(va), &v, 2);
    }
}
static inline void wl_xb_wr32(uint32_t va, uint32_t v) {
    v = WL_XB_LE32(v);
    if ((va & 3u) == 0) {
        *(volatile uint32_t *)wl_xb_ptr(va) = v;
    } else {
        __builtin_memcpy(wl_xb_ptr(va), &v, 4);
    }
}
static inline void wl_xb_wr64(uint32_t va, uint64_t v) {
    wl_xb_wr32(va, (uint32_t)v);
    wl_xb_wr32(va + 4u, (uint32_t)(v >> 32));
}
static inline float wl_xb_rdf(uint32_t va) {
    uint32_t b = wl_xb_rd32(va);
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}
static inline void wl_xb_wrf(uint32_t va, float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    wl_xb_wr32(va, b);
}
// An aligned 32-bit store that another thread may read at the same time (KeTickCount).
static inline void wl_xb_wr32_atomic(uint32_t va, uint32_t v) {
    *(volatile uint32_t *)wl_xb_ptr(va) = WL_XB_LE32(v);
}

// Bytes between host memory and guest memory (memcpy semantics; the ranges may cross pages).
void wl_xb_copy_in(uint32_t va, const void *src, uint32_t n);
void wl_xb_copy_out(void *dst, uint32_t va, uint32_t n);
void wl_xb_fill(uint32_t va, uint8_t byte, uint32_t n);
// Host bytes that may lie in guest memory (a host pointer into it, as a device reads guest memory)
// written to a file through a bounce buffer: with paging, the OS's write of a page without a frame
// fails (EFAULT, or a DMA of the wrong bytes on the Wii) where a load by code faults it in. The
// platform's write status (WL_OK when every byte went).
struct WlPlatformFile;
WlResult wl_xb_file_write(struct WlPlatformFile *file, const void *data, uint32_t bytes);
// File-backed pages. A file read put the bytes of `file` from `offset` on at [va, va + n) (with
// wl_xb_copy_in, just before): each whole page among them remembers its file and offset, so that
// while it stays unchanged its frame goes back with nothing written, and a touch reads it from the
// file again. The caller keeps the file open and its bytes as they are while pages remember them:
// before a write to it, a change of its size or its close (by any handle), wl_xb_file_forget. Only
// with paging and a swap file; bytes past 4 GB are not remembered.
void wl_xb_file_pages(uint32_t va, uint32_t n, struct WlPlatformFile *file, uint64_t offset);
// The pages remembering bytes of `file` in [offset, offset + bytes) forget them (UINT64_MAX: to its
// end): those without a frame are read back from the file first. WL_OK, or the read's failure (the
// page's bytes are lost: logged).
WlResult wl_xb_file_forget(struct WlPlatformFile *file, uint64_t offset, uint64_t bytes);
// A guest NUL-terminated string into `dst` (at most cap - 1 bytes, always terminated); the length
// copied. Stops at the first byte of an address that is not valid guest memory.
uint32_t wl_xb_read_cstr(uint32_t va, char *dst, uint32_t cap);

// Whether [va, va + n) is guest memory a kernel call may touch: committed user pages, or the
// window inside the RAM. (A device range is not.) The kernel checks the buffers it is handed with
// it, so a bad guest pointer is a status code, not a host crash. The common case (a range inside
// one user page found committed lately) is a few instructions; the rest goes to
// wl_xb_range_ok_slow, the whole check, which answers every range alike.
bool wl_xb_range_ok(uint32_t va, uint32_t n);
bool wl_xb_range_ok_slow(uint32_t va, uint32_t n);

// ---- Set-up ----------------------------------------------------------------------------------

typedef struct {
    void *base;     // 4 KB aligned host memory the Wii's frames come from (cached MEM1 or MEM2)
    uint32_t bytes; // a multiple of 4 KB
} WlXbFramePool;

#define WL_XB_MAX_FRAME_POOLS 4u

typedef struct {
    uint32_t ram_bytes;          // guest-physical RAM: 64 MB (retail), 128 MB (a debug kit); 4 MB steps
    uint32_t reserved_low_bytes; // physical bytes from 0 never handed out (the retail kernel's own
                                 // pages; titles pin buffers right above it): 0x60000 by default
    uint32_t gpu_instance_bytes; // physical bytes at the top kept for MmClaimGpuInstanceMemory: 64 KB
    // Wii only: where the frames come from, and the hashed page table's size (64 KB..1 MB, a power
    // of two; the table is a cache of the page tables here, so a smaller one only costs reloads).
    WlXbFramePool frame_pools[WL_XB_MAX_FRAME_POOLS];
    uint32_t htab_bytes;
    // The host and ppc-be: 0 lets the OS back every page (no paging); otherwise only this many
    // bytes of frames, paged as the Wii pages (the same clock and swap file, through SIGSEGV and
    // mprotect), to run a title or a test at the Wii's size. A multiple of 4 KB, 64 KB at least.
    uint32_t frame_limit_bytes;
    // The swap file changed pages go to when the frames run out: a wl/platform path
    // ("sd:/xbox/halo/hdd/pagefile.sys"), grown to `swap_bytes` (0: the RAM's size) when it is
    // smaller, and reused. NULL or "": none; then only pages with nothing to write can give their
    // frames back, and when none is left a touch that needs a frame is a guest crash, as before.
    const char *swap_path;
    uint32_t swap_bytes;
} WlXbMemConfig;

// The defaults above (no frame pools: the Wii's caller adds them).
void wl_xb_mem_config_default(WlXbMemConfig *cfg);

// Reserves the address space (host, ppc-be) or programs the MMU (the Wii: page table, segment
// registers 0 and 1, the DSI handler), and sets g_xbox_mem_offset. Once per process.
// WL_ERR_STATE when already up, WL_ERR_ARG for a bad configuration, WL_ERR_NOMEM when the
// reservation or the table cannot be had, WL_ERR_UNSUPPORTED where neither backend exists.
WlResult wl_xb_mem_init(const WlXbMemConfig *cfg);
bool wl_xb_mem_ready(void);
uint32_t wl_xb_ram_bytes(void);

// ---- Physical pages --------------------------------------------------------------------------

// Contiguous physical bytes inside [lowest, highest] (inclusive, as MmAllocateContiguousMemoryEx
// takes them), aligned to `align` (0 or a power of two, at least a page), searched from the top
// down as the console's allocator does. A range that only user pages occupy is had anyway: the
// user pages move elsewhere (their contents and VAs stay; MmGetPhysicalAddress changes), which
// is what lets a title pin a buffer at a fixed physical address after its image is loaded. The
// memory is zero. `*out` is the physical address. WL_ERR_NOMEM when it cannot be had.
WlResult wl_xb_phys_alloc(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t align, uint32_t *out);
// Frees an allocation by its first physical address (the size is remembered). WL_ERR_ARG when
// `phys` does not start one.
WlResult wl_xb_phys_free(uint32_t phys);
// Locks (or unlocks) the physical page at `phys` (MmLockUnlockBufferPages, MmLockUnlockPhysicalPage):
// a page with locks is never moved to make room for a contiguous allocation. Counted, up to 65535
// (DirectSound locks its buffers' pages often); a freed page loses its locks.
WlResult wl_xb_phys_lock(uint32_t phys, bool lock);
uint32_t wl_xb_phys_lock_count(uint32_t phys);
// The size of the allocation starting at `phys`, 0 when none does.
uint32_t wl_xb_phys_size(uint32_t phys);
// Physical pages in use (all kinds) and free.
void wl_xb_phys_usage(uint32_t *used_pages, uint32_t *free_pages);

// ---- User virtual memory (below WL_XB_USER_LIMIT) --------------------------------------------

#define WL_XB_PROT_NOACCESS 0x01u
#define WL_XB_PROT_READONLY 0x02u
#define WL_XB_PROT_READWRITE 0x04u
#define WL_XB_PROT_EXECUTE_READWRITE 0x40u // PAGE_* values; the mapping is read-write whatever they say
#define WL_XB_VA_GRANULE 0x10000u          // reservations start on 64 KB

// Reserves [*va, *va + bytes) (page granular; `*va` 0: the lowest free 64 KB-aligned range from
// `lowest_va` up, written back). WL_ERR_RANGE when a fixed range is taken or out of user space,
// WL_ERR_NOMEM when no free range is big enough.
WlResult wl_xb_va_reserve(uint32_t *va, uint32_t bytes, uint32_t lowest_va, uint32_t protect);
// Commits the pages of [va, va + bytes) inside a reservation (each gets a physical page; already
// committed ones keep theirs). New pages are zero. WL_ERR_RANGE outside a reservation,
// WL_ERR_NOMEM when the RAM is exhausted (what was committed stays).
WlResult wl_xb_va_commit(uint32_t va, uint32_t bytes, uint32_t protect);
// Decommits pages (their physical pages are freed; the range stays reserved).
WlResult wl_xb_va_decommit(uint32_t va, uint32_t bytes);
// Releases the reservation that starts at `va` (decommitting what is committed). `*bytes` gets
// its size. WL_ERR_RANGE when no reservation starts there.
WlResult wl_xb_va_release(uint32_t va, uint32_t *bytes);
// Reserve and commit in one: what loaders and stacks want. `*va` as in wl_xb_va_reserve.
WlResult wl_xb_va_alloc(uint32_t *va, uint32_t bytes, uint32_t lowest_va, uint32_t protect);

typedef struct {
    uint32_t base;            // the page's region: a run of pages in the same state
    uint32_t size;
    uint32_t allocation_base; // the reservation's start (0 when free)
    uint32_t allocation_protect;
    uint32_t protect;         // of the committed pages; 0 when not committed
    uint32_t state;           // 0x1000 committed, 0x2000 reserved, 0x10000 free (MEM_*)
} WlXbVaInfo;
// What NtQueryVirtualMemory reports for `va` (user space; window and device VAs too, as committed
// or free). WL_ERR_RANGE above the user limit when it is not the window.
WlResult wl_xb_va_query(uint32_t va, WlXbVaInfo *out);
// Records a protection for committed pages (MmSetAddressProtect; the mapping stays read-write).
WlResult wl_xb_va_protect(uint32_t va, uint32_t bytes, uint32_t protect, uint32_t *old_protect);

// The physical address behind a VA (MmGetPhysicalAddress): a window or tiled VA by its offset, a
// committed user VA through its page. false (and 0) when it has none.
bool wl_xb_va_to_phys(uint32_t va, uint32_t *phys);

// The title says the bytes of [va, va + bytes) are dead (a buffer it will write again before it reads
// it, or never read): each whole page in it gives back its frame and its swap slot or file copy and
// reads zero from now on (a page that only zero is in costs nothing until it is written), so an
// eviction never writes it to the swap file. Window and user VAs; pinned and locked pages are left
// alone. `*held` (may be NULL) gets the pages that had a frame or a copy. WL_ERR_RANGE when the
// range wraps.
WlResult wl_xb_va_discard(uint32_t va, uint32_t bytes, uint32_t *held);

// ---- Pinned pages ------------------------------------------------------------------------------
// A pinned page gets its frame now and keeps it: the clock passes it by. For guest memory touched
// where a fault could not wait for the swap file: by a host thread without the guest lock (the
// service thread's KeTickCount), by a DMA, by code that must not fault. Pins do not nest; freeing
// the memory unpins it. WL_ERR_RANGE when part of the range is not guest RAM, WL_ERR_NOMEM when no
// frame can be had for it (a pin may not take more than half the frames).
WlResult wl_xb_mem_pin(uint32_t va, uint32_t bytes);
void wl_xb_mem_unpin(uint32_t va, uint32_t bytes);
// Whether the page of `va` has a frame now (diagnostics and tests; false for anything else).
bool wl_xb_mem_resident(uint32_t va);
// Copies `bytes` guest bytes at `va` (within one page) when its page is resident and not in swap
// I/O, without faulting: false otherwise. Diagnostics may call it without the guest lock (the
// freeze watch on the service thread); the bytes are the guest's (little-endian).
bool wl_xb_mem_peek(uint32_t va, void *dst, uint32_t bytes);

// ---- Write watch ---------------------------------------------------------------------------------
// Whether guest memory was written since a moment, from the MMU's changed bits rather than its
// bytes: for a cache of things made from guest memory (the GPU's converted textures), which then
// need not read or hash memory that did not change. Page granular (a store anywhere in a page
// counts), and a page freed or moved counts as written. Only where the guest is paged (the Wii's
// MMU with C bits; the host and ppc-be with a frame limit): elsewhere there is no stamp, and the
// caller compares the bytes.
// Arms the watch over guest-physical [phys, phys + bytes): a stamp (nonzero) for
// wl_xb_phys_written, or 0 when the stores to part of it cannot be seen. Touches no guest memory.
uint32_t wl_xb_phys_watch(uint32_t phys, uint32_t bytes);
// Whether part of [phys, phys + bytes) may have been written since `stamp` was had (true for 0).
bool wl_xb_phys_written(uint32_t phys, uint32_t bytes, uint32_t stamp);

// ---- Diagnostics -------------------------------------------------------------------------------

typedef struct {
    uint32_t ram_pages, used_pages;
    uint32_t user_pages, contig_pages, frames; // frames: physical pages with host memory behind them
    uint32_t relocations;                      // user pages moved for a pinned contiguous allocation
    uint32_t faults_resolved;                  // first touches (the Wii, or paging) / 0 (the OS does it)
    uint32_t pte_evictions;                    // page table entries reused (the Wii's hashed table)
    // Demand paging (0 where the OS backs every page)
    uint32_t frames_total, frames_free;        // the pool
    uint32_t pinned_pages;
    uint32_t evictions;                        // frames taken back from a page for another
    uint32_t clean_evictions;                  // of those, with nothing to write (zero, or in the swap file)
    uint32_t fast_evictions;                   // of those, in the fault's exception context (the Wii)
    uint32_t swap_ins, swap_outs, swap_writes; // pages read back, pages written, write calls (batches)
    uint32_t zero_outs;                        // evictions of changed pages found zero: nothing written
    uint32_t swap_ins_spared;                  // swapped pages a file read overwrote whole: not read back
    uint32_t file_copies;                      // pages a file read filled whole, kept as the file's copy
    uint32_t file_evictions;                   // of the clean evictions, file copies: nothing written
    uint32_t file_ins;                         // file copies read back from their file (reads)
    uint32_t file_ahead;                       // copies after them that came in the same read
    uint32_t file_forgets, file_forget_ins;    // copies forgotten (the file changed or closed); read back first
    uint64_t swap_read_bytes, swap_write_bytes;
    uint64_t swap_us;                          // time spent in paging's I/O (swap file and file copies)
    uint64_t io_held_us;                       // of it, with the guest lock held (no other thread ran)
    uint32_t io_held_max_us;                   // the longest such stretch
    uint32_t io_unlocked;                      // transfers made with the guest lock given up
    uint32_t transit_waits;                    // of those, pages another thread met and waited for
    uint32_t swap_slots, swap_slots_used;      // the swap file's pages
    uint32_t zero_evictions;                   // of the clean evictions, pages with no copy: zero
    // Pages given a frame again after their frame was taken back (a diagnostic: how much of the
    // paging is the pool's size): in all, and those within one pool's worth of evictions of losing it
    // (refaults), by what their touch read: the swap file, their file, nothing (zero)
    uint32_t returns;
    uint32_t refaults_swap, refaults_file, refaults_zero;
    uint32_t ahead_unused;                     // pages read ahead that went again without being used
    uint32_t resident_dirty, resident_file;    // pages with frames now: changed; clean file copies
} WlXbMemStats;
void wl_xb_mem_stats(WlXbMemStats *out);

// Watches the paging rate; the service thread calls it about once a second (lock not held). A
// swap-in rate above `swap_ins_per_s` (0: 128 a second, half a megabyte) is logged as thrashing,
// at most every 10 s.
void wl_xb_mem_watch(uint32_t swap_ins_per_s);
// WL_XB_PAGING_LOG set (a measuring aid): the paging counters in one line (the play harness, at each step).
void wl_xb_mem_counters_log(void);

#ifdef __cplusplus
}
#endif
