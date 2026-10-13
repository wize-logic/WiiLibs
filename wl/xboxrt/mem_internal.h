// SPDX-License-Identifier: MIT
// Between the portable guest memory core (mem.c) and its backends: mem_posix.c (host and ppc-be:
// a shared memory object mapped twice into a PROT_NONE reservation) and mem_wii.c (the Broadway's
// hashed page table, filled on first touch by the DSI handler).
#pragma once

#include "wl/xboxrt/mem.h"

// The page tables the core keeps (and the Wii's DSI handler reads in exception context).
extern uint32_t g_xb_ram_pages;      // physical pages
extern uint16_t *g_xb_va2phys;      // [WL_XB_USER_LIMIT / 4 KB]: user page -> physical page + 1 (0: none)

// Backend operations. Pages are 4 KB; `phys` arguments are page numbers.
// Reserves the EA span and maps the window over all of the RAM; writes the host base.
WlResult wl_xb_backend_init(const WlXbMemConfig *cfg, uintptr_t *base_out);
// Maps `pages` user pages from `va` to the consecutive physical pages from `phys`.
void wl_xb_backend_map_user(uint32_t va, uint32_t phys, uint32_t pages);
// Takes user pages out of the address space (accesses fault again).
void wl_xb_backend_unmap_user(uint32_t va, uint32_t pages);
// The contents of physical pages are no longer wanted: they read as zero from now on.
void wl_xb_backend_release_phys(uint32_t phys, uint32_t pages);
// The user page at `va` moves from physical page `from` to `to` (contents kept).
void wl_xb_backend_move_phys(uint32_t from, uint32_t to, uint32_t va);
// Frames (host memory behind physical pages), first-touch faults resolved, table evictions.
void wl_xb_backend_stats(uint32_t *frames, uint32_t *faults, uint32_t *evictions);
// The calling host thread as the fault backend names it (the host: its pthread_t; the Wii: 0).
uintptr_t wl_xb_backend_thread_handle(void);
// Logs where the host thread `handle` is running: on the host a signal makes it log its PC and the
// translated function there; nothing on the Wii. A diagnostic for a thread that keeps the guest lock.
void wl_xb_backend_probe(uintptr_t handle);
