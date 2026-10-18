// SPDX-License-Identifier: MIT
// Guest frames lent by the host's heap (frame_loan.h).
#include "wl/xboxrt/frame_loan.h"

#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/thread.h"

#define TAG "xbloan"

static struct {
    uint32_t first;  // the run's first frame
    uint32_t left;   // frames still lent: [first, first + left)
    WlXbLoanGiveFn give;
    void *user;
    WlXbLoanStats st;
} g_loan;

WlResult wl_xb_loan_start(uint32_t first, uint32_t frames, WlXbLoanGiveFn give, void *user) {
    if (first == 0 && frames <= g_xb_pg_frames) {
        first = g_xb_pg_frames - frames + 1u;
    }
    if (frames == 0 || give == NULL || first == 0 || first > g_xb_pg_frames || frames > g_xb_pg_frames - first + 1u) {
        return WL_ERR_ARG;
    }
    if (!wl_platform_heap_reclaims()) {
        return WL_ERR_UNSUPPORTED;
    }
    g_loan.first = first;
    g_loan.left = frames;
    g_loan.give = give;
    g_loan.user = user;
    g_loan.st.lent = g_loan.st.left = frames;
    (void)wl_platform_heap_set_reclaim(wl_xb_loan_reclaim, NULL);
    return WL_OK;
}

size_t wl_xb_loan_reclaim(void *user, size_t want) {
    (void)user;
    if (g_loan.left == 0) {
        return 0;
    }
    if (!wl_xb_lock_held()) {
        g_loan.st.refusals++;
        return 0;
    }
    // what was asked and a little more (the allocator's headers, an alignment), in whole steps
    size_t pages = (want + (64u << 10) + WL_XB_PAGE - 1u) / WL_XB_PAGE;
    uint32_t need = pages > g_loan.left ? g_loan.left : (uint32_t)pages;
    need = (need + WL_XB_LOAN_STEP - 1u) / WL_XB_LOAN_STEP * WL_XB_LOAN_STEP;
    need = need < g_loan.left ? need : g_loan.left;
    uint32_t done = 0;
    while (done < need) {
        uint32_t k = need - done < WL_XB_RETIRE_MAX ? need - done : WL_XB_RETIRE_MAX;
        uint32_t end = g_loan.left - done; // the run's tail first: [end - k, end)
        if (!wl_xb_pg_retire(g_loan.first + end - k, k, &g_loan.st.moved)) {
            break;
        }
        done += k;
    }
    if (done == 0) {
        g_loan.st.refusals++;
        return 0;
    }
    g_loan.left -= done;
    g_loan.st.left = g_loan.left;
    g_loan.st.gives++;
    g_loan.give(g_loan.user, g_loan.left);
    return (size_t)done * WL_XB_PAGE;
}

void wl_xb_loan_stats(WlXbLoanStats *out) {
    *out = g_loan.st;
}
