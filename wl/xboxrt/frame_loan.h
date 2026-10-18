// SPDX-License-Identifier: MIT
// Guest frames lent by the host's heap: memory the host side keeps free for its peaks (boot.c's
// keep_free_bytes) serves as guest frames while it is idle, and goes back to the heap, a piece at
// a time, when an allocation would fail (the platform's reclaimer, wl/platform/mem_census.h).
//
// The loan is a run of the paging's frames, [first, first + frames), backed by one block of the
// heap's (the Wii: a pool of its own, wl_xb_mem_init's WlXbMemConfig.frame_pools; the host: the top
// of the frame limit, the heap's cap standing for the block). Its frames are given back from the
// run's end: the pages in them go to other frames (a changed page is copied into one the paging
// frees as it would for a fault, writing to the swap file if it must) or give their frames up as
// the clock would have them, and `give` then hands the run's tail to the heap (the Wii: the block
// shrunk in place, its tail freed; the host: the cap grown). Pieces of LOAN_STEP frames, enough for
// the request.
//
// The reclaimer gives back only on a thread that holds the guest lock (the paging's tables are
// changed under it), and not inside the paging's own I/O; a failure elsewhere fails as it did.
// Frames given back stay the heap's for the rest of the run.
#pragma once

#include "wl/base/base.h"

#define WL_XB_LOAN_STEP 64u        // frames given back at least (256 KB)
#define WL_XB_RETIRE_MAX 256u      // frames wl_xb_pg_retire takes at most at once

// The loan's run now holds its first `frames_left` frames: the rest goes to the heap.
typedef void (*WlXbLoanGiveFn)(void *user, uint32_t frames_left);

// Starts the loan (after wl_xb_mem_init, with paging) and registers the reclaimer; `first` 0: the
// paging's last `frames` frames (the host, whose frames are a count). WL_ERR_ARG when
// the run is not all paging frames (the page cache's are not), WL_ERR_UNSUPPORTED when the
// platform's allocator does not call a reclaimer (nothing registered: the caller frees the block).
WlResult wl_xb_loan_start(uint32_t first, uint32_t frames, WlXbLoanGiveFn give, void *user);
// The reclaimer itself (what wl_xb_loan_start registers; for a test to call).
size_t wl_xb_loan_reclaim(void *user, size_t want);

typedef struct {
    uint32_t lent;      // frames lent at the start
    uint32_t left;      // frames still lent
    uint32_t gives;     // pieces given back
    uint32_t refusals;  // reclaims that gave nothing (no guest lock, the paging busy, a page held)
    uint32_t moved;     // changed pages copied to other frames
} WlXbLoanStats;
void wl_xb_loan_stats(WlXbLoanStats *out);

// The paging's part (paging.c; the guest lock held): frames [first, first + count) leave the
// paging for good, count <= WL_XB_RETIRE_MAX. All or nothing: false, with none retired, when a page
// in them is pinned or in I/O, a frame is in a fault's hands, the paging is in the middle of a swap
// write, or no other frame can be had. `moved` (may be NULL) counts the changed pages copied.
bool wl_xb_pg_retire(uint32_t first, uint32_t count, uint32_t *moved);

#if defined(__wii__)
// The backend's part (mem_wii.c): `bytes` of its pools went back to the heap (the census's frames).
void wl_xb_backend_frames_given(uint32_t bytes);
#endif
