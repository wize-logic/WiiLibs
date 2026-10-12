// SPDX-License-Identifier: MIT
#include <string.h>

#include "wl/platform/input_gesture.h"

// The reading interval assumed with nothing to measure it by (the first feed, or after a gap), and
// the bounds of a believable one
#define DEFAULT_STEP_US 10000u
#define MIN_STEP_US 1000u
#define MAX_STEP_US 20000u

void wl_thrust_reset(WlThrust *t) {
    t->first = 0;
    t->count = 0;
    t->last_feed_us = 0;
    t->fired = false;
    t->fired_us = 0;
}

void wl_thrust_init(WlThrust *t) {
    memset(t, 0, sizeof *t);
    t->params.rise = 100;
    t->params.rise_us = 130000;
    t->params.rest_us = 150000;
    t->params.rest_range = 35;
    t->params.side_range = 60;
    t->params.holdoff_us = 600000;
    t->params.forward_axis = 1;
    t->params.forward_sign = 1;
}

static uint32_t slot(const WlThrust *t, uint32_t index) {
    return (t->first + index) % WL_THRUST_HISTORY;
}

static void push(WlThrust *t, uint64_t at_us, const int16_t a[3]) {
    if (t->count == WL_THRUST_HISTORY) {
        t->first = (t->first + 1) % WL_THRUST_HISTORY;
        t->count--;
    }
    uint32_t s = slot(t, t->count);
    t->at_us[s] = at_us;
    memcpy(t->value[s], a, sizeof t->value[s]);
    t->count++;
}

// the range of each axis over the readings from index `from` to `to`
static void ranges(const WlThrust *t, uint32_t from, uint32_t to, int32_t lo[3], int32_t hi[3]) {
    for (int ax = 0; ax < 3; ax++) {
        lo[ax] = 32767;
        hi[ax] = -32768;
    }
    for (uint32_t i = from; i <= to; i++) {
        const int16_t *v = t->value[slot(t, i)];
        for (int ax = 0; ax < 3; ax++) {
            lo[ax] = v[ax] < lo[ax] ? v[ax] : lo[ax];
            hi[ax] = v[ax] > hi[ax] ? v[ax] : hi[ax];
        }
    }
}

// whether the newest reading ends a thrust
static bool thrust_found(const WlThrust *t) {
    const WlThrustParams *p = &t->params;
    uint32_t last = t->count - 1;
    uint64_t now = t->at_us[slot(t, last)];
    int f = p->forward_axis;
    int sign = p->forward_sign < 0 ? -1 : 1;
    int a1 = (f + 1) % 3;
    int a2 = (f + 2) % 3;
    int32_t current = sign * t->value[slot(t, last)][f];
    int32_t lowest = current;
    uint32_t low_index = last;
    int32_t lo[3];
    int32_t hi[3];

    if (t->fired && now - t->fired_us < p->holdoff_us) {
        return false;
    }
    // the lowest the forward axis was within the rise's time (the latest, if it was that low more than once)
    for (uint32_t i = last; i-- > 0;) {
        uint32_t s = slot(t, i);
        if (now - t->at_us[s] > p->rise_us) {
            break;
        }
        int32_t v = sign * t->value[s][f];
        if (v < lowest) {
            lowest = v;
            low_index = i;
        }
    }
    if (current - lowest < (int32_t)p->rise) {
        return false;
    }
    // the sideways axes stay quiet while it rises
    ranges(t, low_index, last, lo, hi);
    if (hi[a1] - lo[a1] > (int32_t)p->side_range || hi[a2] - lo[a2] > (int32_t)p->side_range) {
        return false;
    }
    // and before it, a rest: the readings kept must reach back over most of the rest's time (three
    // quarters: the first of a rest has no earlier reading to show) and none of its axes moves much
    uint64_t rise_start = t->at_us[slot(t, low_index)];
    uint64_t oldest = t->at_us[slot(t, 0)];
    if (rise_start < oldest || rise_start - oldest < (uint64_t)p->rest_us / 4u * 3u) {
        return false;
    }
    uint32_t rest_from = low_index;
    while (rest_from > 0 && rise_start - t->at_us[slot(t, rest_from - 1)] <= p->rest_us) {
        rest_from--;
    }
    ranges(t, rest_from, low_index, lo, hi);
    for (int ax = 0; ax < 3; ax++) {
        if (hi[ax] - lo[ax] > (int32_t)p->rest_range) {
            return false;
        }
    }
    return true;
}

bool wl_thrust_feed(WlThrust *t, const int16_t (*samples)[3], unsigned n, unsigned reports, uint64_t now_us) {
    uint64_t step = DEFAULT_STEP_US;
    bool found = false;

    if (n == 0) {
        return false;
    }
    if (t->last_feed_us != 0 && now_us > t->last_feed_us) {
        // (an evenly spaced guess at the times of the reports: the frame's time over their number)
        step = (now_us - t->last_feed_us) / (reports > n ? reports : n);
        step = step < MIN_STEP_US ? MIN_STEP_US : step > MAX_STEP_US ? MAX_STEP_US : step;
    }
    t->last_feed_us = now_us;
    for (unsigned i = 0; i < n; i++) {
        uint64_t behind = (uint64_t)(n - 1 - i) * step;
        push(t, behind < now_us ? now_us - behind : 0, samples[i]);
        if (thrust_found(t)) {
            t->fired = true;
            t->fired_us = t->at_us[slot(t, t->count - 1)];
            found = true;
        }
    }
    return found;
}
