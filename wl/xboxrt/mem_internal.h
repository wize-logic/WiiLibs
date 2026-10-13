// SPDX-License-Identifier: MIT
// Between the portable guest memory core (mem.c), the demand paging core (paging.c) and the
// backends: mem_posix.c (host and ppc-be: a shared memory object mapped twice into a PROT_NONE
// reservation, paged by hand through mprotect when a frame limit is set) and mem_wii.c (the
// Broadway's hashed page table, filled on first touch by the DSI handler).
#pragma once

#include "wl/xboxrt/mem.h"

// The page tables the core keeps (and the Wii's DSI handler reads in exception context).
extern uint32_t g_xb_ram_pages;      // physical pages
extern uint16_t *g_xb_va2phys;      // [WL_XB_USER_LIMIT / 4 KB]: user page -> physical page + 1 (0: none)

// Backend operations. Pages are 4 KB; `phys` arguments are page numbers.
// Reserves the EA span and maps the window over all of the RAM; writes the host base. A backend
// that pages (the Wii; the host with a frame limit) starts the paging core (wl_xb_pg_init) first.
WlResult wl_xb_backend_init(const WlXbMemConfig *cfg, uintptr_t *base_out);
// Maps `pages` user pages from `va` to the consecutive physical pages from `phys`.
void wl_xb_backend_map_user(uint32_t va, uint32_t phys, uint32_t pages);
// Takes user pages out of the address space (accesses fault again).
void wl_xb_backend_unmap_user(uint32_t va, uint32_t pages);
// The contents of physical pages are no longer wanted: they read as zero from now on.
void wl_xb_backend_release_phys(uint32_t phys, uint32_t pages);
// The user page at `va` moves from physical page `from` to `to` (contents kept).
void wl_xb_backend_move_phys(uint32_t from, uint32_t to, uint32_t va);
// Frames (host memory behind physical pages), faults resolved, page table entries reused.
void wl_xb_backend_stats(uint32_t *frames, uint32_t *faults, uint32_t *pte_evictions);
// The calling host thread as the fault backend names it (the host: its pthread_t; the Wii: 0).
uintptr_t wl_xb_backend_thread_handle(void);
// Logs where the host thread `handle` is running: on the host a signal makes it log its PC and the
// translated function there; nothing on the Wii. A diagnostic for a thread that keeps the guest lock.
void wl_xb_backend_probe(uintptr_t handle);
#if !defined(__wii__)
// The host's fault on guest RAM under a frame limit (fault_posix.c asks first): WL_OK when the page
// is open now (the access runs again); WL_ERR_UNSUPPORTED when the EA is not paged guest RAM (no
// frame limit, a hole, a device: the handler's other cases); WL_ERR_DENIED for a thread without
// the guest lock that would have to wait for the swap file; else what wl_xb_pg_fault failed with.
WlResult wl_xb_backend_page_fault(uint32_t ea);
#endif

// ---- Demand paging (paging.c) ------------------------------------------------------------------
// Per physical page the core keeps its frame (an index into the pool, 1..frames; 0: none), its
// swap slot and its flags; the backend's MMU keeps the mappings and knows where a frame's bytes
// are. A page with neither a frame nor a slot is zero.

#define WL_XB_PG_REF 0x01u    // used since the clock last passed it (the MMU's R bits harvested in)
#define WL_XB_PG_DIRTY 0x02u  // changed since its zero fill or its last copy in the swap file (C bits)
#define WL_XB_PG_PINNED 0x04u // never gives its frame back
#define WL_XB_PG_IO 0x08u     // its frame is being filled or written out: neither mapped nor taken

extern uint8_t *g_xb_pg_flags;  // [ram pages]
extern uint16_t *g_xb_pg_frame; // [ram pages] 1..frames, 0: none
extern uint16_t *g_xb_pg_slot;  // [ram pages] swap slot + 1, 0: none
extern uint32_t g_xb_pg_frames; // frames in the pool; 0: paging is off (the OS backs every page)

// Starts the core over `frames` frames (at most 65535) and opens the swap file the configuration
// names. Once, from the backend's init.
WlResult wl_xb_pg_init(const WlXbMemConfig *cfg, uint32_t frames);
// Exception-safe (the Wii's DSI handler: no locks, no I/O, no FP): a frame for page `p`, which has
// none and no slot: a free one, or one a clean page not used lately gives back (a short look of the
// clock). Its bytes are not set. `keep` (a page + 1, or 0) is not taken. 0 when neither is at hand.
uint32_t wl_xb_pg_frame_fast(uint32_t p, uint32_t keep);
// Normal context, the guest lock held: page `p` gets a frame with its contents (zero, or read back
// from the swap file), writing changed pages out first when it must; not mapped. WL_OK, WL_ERR_NOMEM
// (every frame is pinned, or changed with no swap file to write it to), WL_ERR_IO (the swap file
// failed), WL_ERR_STATE (the page is in I/O already: a fault inside the paging code).
WlResult wl_xb_pg_fault(uint32_t p, uint32_t keep);
// The pages' contents are no longer wanted: unmapped, frames and slots back, flags cleared.
void wl_xb_pg_release(uint32_t p, uint32_t pages);
// The user page in physical page `from` moves to physical page `to` (free): its frame, slot and
// flags go with it. Both are unmapped.
void wl_xb_pg_move(uint32_t from, uint32_t to);
// Pins (with a frame now: normal context, lock held) or unpins page `p`.
WlResult wl_xb_pg_pin(uint32_t p, bool pin);
// The paging fields of the statistics.
void wl_xb_pg_stats(WlXbMemStats *out);

// The backend's MMU, for the core. On the Wii they run in exception context too: no locks, no I/O.
// The R and C bits of `p`'s live mappings into its flags (WL_XB_PG_REF, WL_XB_PG_DIRTY); with
// `clear_ref`, the mappings' R bits cleared too, so that the next use sets them again.
void wl_xb_mmu_harvest(uint32_t p, bool clear_ref);
// Every mapping of `p` gone (both EAs), its R and C bits harvested into its flags first.
void wl_xb_mmu_unmap(uint32_t p);
// The host pointer of frame `f`, which page `p` holds.
uint8_t *wl_xb_mmu_frame_ptr(uint32_t p, uint32_t f);
// Frame `f` was given to page `p` with nothing in it yet: zero it.
void wl_xb_mmu_frame_zero(uint32_t p, uint32_t f);
// Page `p` gives frame `f` back (unmapped): the host memory behind it may go.
void wl_xb_mmu_frame_drop(uint32_t p, uint32_t f);
// Frame `f` goes from page `from` to page `to`, both unmapped (wl_xb_pg_move): a backend whose
// frames are tied to pages (the host's) moves the bytes.
void wl_xb_mmu_frame_move(uint32_t from, uint32_t to, uint32_t f);
// Page `p` was pinned and has its frame: map it wherever a fault must not be needed (the host maps
// both EAs read-write; the Wii's fast path maps a resident page without I/O, so nothing).
void wl_xb_mmu_pinned(uint32_t p);
// Keeps the fault path of another thread out of the core's tables for a few lines (the Wii:
// interrupts off; the host: nothing, only the guest lock's holder faults on pages that are not
// pinned, and pinned pages stay mapped).
uint32_t wl_xb_mmu_lock(void);
void wl_xb_mmu_unlock(uint32_t level);
