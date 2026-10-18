// SPDX-License-Identifier: MIT
// The card's read speed through libfat and straight from the card, on one big file (fs.h).
#include <stdlib.h>
#include <string.h>

#include "wl/platform/fs.h"

#define SEQ_CHUNK (256u << 10)
#define SEQ_BYTES (16u << 20)
#define RND_BIG (64u << 10)
#define RND_BIG_COUNT 100u
#define RND_PAGE 4096u
#define RND_PAGE_COUNT 200u

static uint32_t lcg(uint32_t *s) {
    *s = *s * 1103515245u + 12345u;
    return *s >> 1;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

// One way's pass (`fast`: straight from the card) of `count` reads of `len` bytes: at `seq` steps from
// 0, else at offsets from `seed` (multiples of `align`), into `buf` at the offset's alignment (as the
// kernel reads). Its time in *us and a checksum of the bytes in *sum.
static WlResult pass(WlPlatformFile *f, bool fast, uint8_t *buf, uint32_t count, uint32_t len, bool seq, uint32_t seed,
                     uint32_t align, uint64_t *us, uint32_t *sum) {
    uint64_t size = wl_platform_fs_size(f);
    uint64_t span = size > len ? size - len : 0u;
    wl_platform_fs_fast_set(fast);
    *sum = 2166136261u;
    uint64_t t0 = wl_platform_fs_clock_us();
    for (uint32_t i = 0; i < count; i++) {
        uint64_t at = seq ? (uint64_t)i * len : (span != 0u ? (uint64_t)lcg(&seed) * align % span : 0u);
        at -= at % align;
        uint32_t n = at + len <= size ? len : (uint32_t)(size - at);
        uint8_t *to = buf + (at & 31u);
        WlResult r = wl_platform_fs_read_at(f, at, to, n);
        if (r != WL_OK) {
            return r;
        }
        *sum = fnv(*sum, to, n);
    }
    *us = wl_platform_fs_clock_us() - t0;
    return WL_OK;
}

static uint32_t kib_s(uint64_t bytes, uint64_t us) {
    return us != 0u ? (uint32_t)(bytes * 1000000u / (us * 1024u)) : 0u;
}

// Both ways of one kind of read, logged: MB/s and ms a read, and the transfers a read straight
// from the card made.
static WlResult compare(WlPlatformFile *f, const char *what, uint8_t *buf, uint32_t count, uint32_t len, bool seq,
                        uint32_t align, uint32_t *out, bool *same) {
    uint64_t us[2];
    uint32_t sum[2];
    WlPlatformFsReads a, b;
    WlResult r = pass(f, false, buf, count, len, seq, 77u, align, &us[0], &sum[0]);
    wl_platform_fs_reads(&a);
    if (r == WL_OK) {
        r = pass(f, true, buf, count, len, seq, 77u, align, &us[1], &sum[1]);
    }
    wl_platform_fs_reads(&b);
    if (r != WL_OK) {
        WL_LOG_E("FS", "speed test, %s: %s", what, wl_result_name(r));
        return r;
    }
    uint64_t bytes = (uint64_t)count * len;
    uint32_t fast_calls = b.fast_calls - a.fast_calls;
    for (int k = 0; k < 2; k++) {
        out[k] = kib_s(bytes, us[k]);
    }
    *same = *same && sum[0] == sum[1];
    WL_LOG_I("FS",
             "speed test, %s: libfat %u.%02u MB/s (%u.%u ms a read), straight from the card %u.%02u MB/s (%u.%u ms a "
             "read, %u of %u reads straight, %u transfers)%s",
             what, (unsigned)(out[0] / 1024u), (unsigned)(out[0] % 1024u * 100u / 1024u),
             (unsigned)(us[0] / count / 1000u), (unsigned)(us[0] / count / 100u % 10u), (unsigned)(out[1] / 1024u),
             (unsigned)(out[1] % 1024u * 100u / 1024u), (unsigned)(us[1] / count / 1000u),
             (unsigned)(us[1] / count / 100u % 10u), (unsigned)fast_calls, (unsigned)count,
             (unsigned)(b.transfers - a.transfers), sum[0] == sum[1] ? "" : "; THE BYTES DIFFER");
    return WL_OK;
}

WlResult wl_fs_speed_compare(const char *path, WlFsSpeedCompare *out) {
    if (path == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    out->same = true;
    WlPlatformFile *f = NULL;
    WlResult r = wl_platform_fs_open(path, WL_FILE_READ, &f);
    if (r != WL_OK) {
        WL_LOG_E("FS", "speed test: %s: %s", path, wl_result_name(r));
        return r;
    }
    bool was = wl_platform_fs_fast();
    uint8_t *buf = aligned_alloc(32, SEQ_CHUNK + 32u);
    if (buf == NULL) {
        wl_platform_fs_close(f);
        return WL_ERR_NOMEM;
    }
    uint64_t size = wl_platform_fs_size(f);
    WL_LOG_I("FS", "speed test of %s (%u KB)", path, (unsigned)(size >> 10));
    // the file's map made before the timing (the first read straight from the card makes it)
    wl_platform_fs_fast_set(true);
    uint64_t t0 = wl_platform_fs_clock_us();
    WlPlatformFsReads a, b;
    wl_platform_fs_reads(&a);
    r = wl_platform_fs_read_at(f, size > 2u * RND_BIG ? size - 2u * RND_BIG : 0u, buf,
                               size > 2u * RND_BIG ? RND_BIG : (uint32_t)size);
    wl_platform_fs_reads(&b);
    out->map_us = (uint32_t)(wl_platform_fs_clock_us() - t0);
    out->direct = b.fast_calls != a.fast_calls;
    WL_LOG_I("FS", "speed test: the first read %s in %u ms (the file's map made)",
             out->direct ? "straight from the card" : "through libfat (none straight from the card)",
             (unsigned)(out->map_us / 1000u));
    uint32_t seq_count = size >= SEQ_BYTES ? SEQ_BYTES / SEQ_CHUNK : (uint32_t)(size / SEQ_CHUNK);
    if (r == WL_OK && seq_count > 0u) {
        r = compare(f, "the first 16 MB (or all) in 256 KB reads", buf, seq_count, SEQ_CHUNK, true, 1u, out->seq_kib_s,
                    &out->same);
    }
    if (r == WL_OK && size > RND_BIG) {
        r = compare(f, "100 reads of 64 KB anywhere (the textures')", buf, RND_BIG_COUNT, RND_BIG, false, 1u,
                    out->big_kib_s, &out->same);
    }
    if (r == WL_OK && size > RND_PAGE) {
        r = compare(f, "200 reads of a 4 KB page anywhere (the paging's)", buf, RND_PAGE_COUNT, RND_PAGE, false,
                    RND_PAGE, out->page_kib_s, &out->same);
    }
    wl_platform_fs_fast_set(was);
    free(buf);
    wl_platform_fs_close(f);
    if (r == WL_OK) {
        WL_LOG_I("FS", "speed test: %s", out->same ? "the bytes read both ways are the same" : "THE BYTES DIFFER");
    }
    return r;
}
