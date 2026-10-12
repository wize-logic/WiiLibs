// SPDX-License-Identifier: MIT
// The display queue (present.h): a portable state machine over two or three XFBs.
#include "wl/platform/present.h"

#include <string.h>

#define MAX_INTERVAL 15u

WlResult wl_present_init(WlPresentQueue *q, uint32_t count, uint32_t interval, int shown) {
    if (q == NULL || count < 2u || count > WL_PRESENT_MAX_BUFFERS || shown < -1 || shown >= (int)count) {
        return WL_ERR_ARG;
    }
    memset(q, 0, sizeof *q);
    q->count = (uint8_t)count;
    q->back = -1;
    q->shown = (int8_t)shown;
    q->committed = -1;
    q->scanned = (int8_t)shown;
    if (shown >= 0) {
        q->state[shown] = WL_XFB_SHOWN;
    }
    wl_present_set_interval(q, interval);
    return WL_OK;
}

void wl_present_set_interval(WlPresentQueue *q, uint32_t interval) {
    wl_present_set_ratio(q, interval, 1u);
}

void wl_present_set_ratio(WlPresentQueue *q, uint32_t num, uint32_t den) {
    den = den < 1u ? 1u : den > MAX_INTERVAL ? MAX_INTERVAL : den;
    // (at least one retrace a frame, at most MAX_INTERVAL)
    num = num < den ? den : num > MAX_INTERVAL * den ? MAX_INTERVAL * den : num;
    uint32_t a = num, b = den;
    while (b != 0u) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    q->rate_num = (uint8_t)(num / a);
    q->rate_den = (uint8_t)(den / a);
    q->interval = (uint8_t)((q->rate_num + q->rate_den - 1u) / q->rate_den);
    // (counted from the frame on the screen, as a change of interval always was)
    q->due = (uint64_t)q->last_latch * q->rate_den + q->rate_num;
}

int wl_present_acquire(WlPresentQueue *q) {
    if (q->back >= 0) {
        return q->back;
    }
    for (uint32_t i = 0; i < q->count; i++) {
        // (a buffer the video interface scans is never drawn, even one shown behind the queue's back)
        if (q->state[i] == WL_XFB_FREE && (int)i != q->scanned) {
            q->state[i] = WL_XFB_BACK;
            q->back = (int8_t)i;
            return (int)i;
        }
    }
    return -1;
}

bool wl_present_submit(WlPresentQueue *q, WlPresentDone done, uint32_t done_arg) {
    if (q->back < 0) {
        return false;
    }
    uint32_t i = (uint32_t)q->back;
    q->state[i] = WL_XFB_PENDING;
    q->done[i] = done;
    q->done_arg[i] = done_arg;
    q->queue[q->queued++] = (uint8_t)i;
    q->back = -1;
    q->presented++;
    return true;
}

static uint32_t pop_oldest(WlPresentQueue *q) {
    uint32_t i = q->queue[0];
    q->queued--;
    memmove(q->queue, q->queue + 1, q->queued);
    return i;
}

int wl_present_reclaim(WlPresentQueue *q) {
    if (q->queued != 0) {
        uint32_t i = pop_oldest(q);
        q->state[i] = WL_XFB_FREE;
        q->reclaimed++;
        return (int)i;
    }
    // None pending: a free buffer held back only because the video interface scans it (given to it
    // behind the queue's back, as a text console's) is drawn over after all.
    for (uint32_t i = 0; i < q->count; i++) {
        if (q->state[i] == WL_XFB_FREE) {
            q->scanned = -1;
            return (int)i;
        }
    }
    return -1;
}

bool wl_present_idle(const WlPresentQueue *q) {
    return q->queued == 0 && q->committed < 0;
}

int wl_present_retrace_begin(WlPresentQueue *q) {
    q->retraces++;
    // (one in flight at a time: libogc may defer the registers to a later retrace)
    if (q->committed >= 0 || q->queued == 0) {
        return -1;
    }
    if (q->latched_any && (uint64_t)q->retraces * q->rate_den < q->due) {
        return -1;
    }
    uint32_t i = q->queue[0];
    if (q->done[i] != NULL && !q->done[i](q->done_arg[i])) {
        return -1;
    }
    (void)pop_oldest(q);
    q->state[i] = WL_XFB_COMMITTED;
    q->committed = (int8_t)i;
    return (int)i;
}

void wl_present_retrace_end(WlPresentQueue *q, int current) {
    q->scanned = (int8_t)(current >= 0 && current < (int)q->count ? current : -1);
    if (q->committed < 0 || current != q->committed) {
        return;
    }
    if (q->shown >= 0 && q->shown != q->committed) {
        q->state[q->shown] = WL_XFB_FREE;
    }
    if (q->latched_any) {
        q->last_gap = q->retraces - q->last_latch;
        if (q->last_gap > q->interval) {
            q->late++;
        }
    }
    q->state[q->committed] = WL_XFB_SHOWN;
    q->shown = q->committed;
    q->committed = -1;
    q->last_latch = q->retraces;
    // (the next frame is due `rate_num / rate_den` retraces after this one's due time; a frame a whole
    // retrace or more late starts from its own retrace instead, so that lateness is not made up)
    uint64_t now = (uint64_t)q->retraces * q->rate_den;
    if (!q->latched_any || now >= q->due + q->rate_den) {
        q->due = now;
    }
    q->due += q->rate_num;
    q->latched_any = true;
    q->latched++;
}
