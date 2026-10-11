// SPDX-License-Identifier: MIT
// The portable half of profile.h: the sample ring, the jitter and the text dumps.
#include <stdio.h>
#include <string.h>

#include "wl/platform/profile.h"

WlResult wl_profile_ring_init(WlProfileRing *ring, uint32_t *storage, uint32_t words, uint32_t depth,
                              WlProfileMode mode) {
    if (ring == NULL || storage == NULL || depth > WL_PROFILE_MAX_DEPTH ||
        (mode != WL_PROFILE_STOP_WHEN_FULL && mode != WL_PROFILE_OVERWRITE)) {
        return WL_ERR_ARG;
    }
    uint32_t stride = WL_PROFILE_HEADER_WORDS + depth;
    if (words < stride) {
        return WL_ERR_ARG;
    }
    ring->words = storage;
    ring->capacity = words / stride;
    ring->depth = depth;
    ring->stride = stride;
    ring->mode = (uint32_t)mode;
    wl_profile_ring_reset(ring);
    return WL_OK;
}

void wl_profile_ring_reset(WlProfileRing *ring) {
    ring->head = 0;
    ring->count = 0;
    ring->total = 0;
    ring->dropped = 0;
}

// (platform: walk with wl_platform_backtrace_from, the other with `read`)
static bool record(WlProfileRing *ring, uint32_t tick, uint32_t thread, uint32_t pc, uint32_t lr, uint32_t sp,
                   WlBacktraceRead read, void *user) {
    uint32_t head = ring->head;
    uint32_t *slot;
    uint32_t found = 0;

    if (ring->count == ring->capacity && ring->mode == (uint32_t)WL_PROFILE_STOP_WHEN_FULL) {
        ring->dropped++;
        ring->total++;
        return false;
    }
    slot = ring->words + (size_t)head * ring->stride;
    slot[0] = tick;
    slot[1] = thread;
    slot[2] = pc;
    slot[3] = lr;
    if (ring->depth != 0) {
        found = read != NULL ? wl_backtrace_walk(read, user, sp, 0, 0, slot + WL_PROFILE_HEADER_WORDS, ring->depth)
                             : wl_platform_backtrace_from(sp, 0, slot + WL_PROFILE_HEADER_WORDS, ring->depth);
        for (uint32_t i = found; i < ring->depth; i++) {
            slot[WL_PROFILE_HEADER_WORDS + i] = 0;
        }
    }
    head++;
    ring->head = head == ring->capacity ? 0 : head;
    if (ring->count < ring->capacity) {
        ring->count++;
    }
    ring->total++;
    return true;
}

bool wl_profile_ring_record(WlProfileRing *ring, uint32_t tick, uint32_t thread, uint32_t pc, uint32_t lr,
                            uint32_t sp) {
    return record(ring, tick, thread, pc, lr, sp, NULL, NULL);
}

bool wl_profile_ring_record_with(WlProfileRing *ring, uint32_t tick, uint32_t thread, uint32_t pc, uint32_t lr,
                                 uint32_t sp, WlBacktraceRead read, void *user) {
    return record(ring, tick, thread, pc, lr, sp, read, user);
}

const uint32_t *wl_profile_ring_get(const WlProfileRing *ring, uint32_t index) {
    uint32_t slot;

    if (index >= ring->count) {
        return NULL;
    }
    slot = ring->head + ring->capacity - ring->count + index;
    if (slot >= ring->capacity) {
        slot -= ring->capacity;
    }
    return ring->words + (size_t)slot * ring->stride;
}

uint32_t wl_profile_jitter(uint32_t *state, uint32_t base) {
    uint32_t x = *state != 0 ? *state : 0x9e3779b9u;
    uint32_t span = base >> 3; // (the width of the range: +-base/16)
    uint32_t period;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    period = base - (span >> 1) + (uint32_t)(((uint64_t)(x & 0xffffu) * span) >> 16);
    return period != 0 ? period : 1;
}

uint32_t wl_profile_dump(const WlProfileRing *ring, const WlProfileDumpInfo *info, WlProfileSink sink, void *user) {
    char line[256];
    char note[80];
    uint32_t sent = 0;
    uint32_t timebase = info != NULL && info->timebase_hz != 0 ? info->timebase_hz : wl_profile_timebase_hz();
    size_t n = 0;

    if (info != NULL && info->note != NULL) {
        for (const char *c = info->note; *c != '\0' && n < sizeof note - 1u; c++) {
            note[n++] = (unsigned char)*c >= 0x20u ? *c : ' ';
        }
    }
    note[n] = '\0';
    snprintf(line, sizeof line, "WLPROF 1 hz=%u tbhz=%u depth=%u samples=%u total=%u dropped=%u thread=%x note=%s",
             (unsigned)(info != NULL ? info->hz : 0u), (unsigned)timebase, (unsigned)ring->depth,
             (unsigned)ring->count, (unsigned)ring->total, (unsigned)ring->dropped,
             (unsigned)(info != NULL ? info->main_thread : 0u), note);
    sink(user, line);
    sent++;
    for (uint32_t i = 0; i < ring->count; i++) {
        const uint32_t *s = wl_profile_ring_get(ring, i);
        uint32_t last = ring->depth;
        int length;

        while (last != 0 && s[WL_PROFILE_HEADER_WORDS + last - 1u] == 0) {
            last--;
        }
        length = snprintf(line, sizeof line, "WLPS %x %x %x %x", (unsigned)s[0], (unsigned)s[1], (unsigned)s[2],
                          (unsigned)s[3]);
        for (uint32_t c = 0; c < last && length > 0 && (size_t)length + 10u < sizeof line; c++) {
            length += snprintf(line + length, sizeof line - (size_t)length, " %x",
                               (unsigned)s[WL_PROFILE_HEADER_WORDS + c]);
        }
        sink(user, line);
        sent++;
    }
    return sent;
}

static void file_sink(void *user, const char *line) {
    FILE *file = user;

    fputs(line, file);
    fputc('\n', file);
}

WlResult wl_profile_dump_file(const WlProfileRing *ring, const WlProfileDumpInfo *info, const char *path) {
    FILE *file;
    WlResult result = WL_OK;

    if (ring == NULL || path == NULL) {
        return WL_ERR_ARG;
    }
    file = fopen(path, "w");
    if (file == NULL) {
        return WL_ERR_IO;
    }
    wl_profile_dump(ring, info, file_sink, file);
    if (ferror(file) != 0) {
        result = WL_ERR_IO;
    }
    if (fclose(file) != 0) {
        result = WL_ERR_IO;
    }
    return result;
}
