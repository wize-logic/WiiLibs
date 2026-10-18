// SPDX-License-Identifier: MIT
// The Broadway block operations (wl/platform/memfast.h) against the C library: every length
// 0..300 at every destination offset in a cache line (0..31) and every source offset in a double
// word (0..7), the whole buffer compared, so a byte written outside the range (a head or tail
// store, a dcbz of a line not wholly inside) shows. Copies also over overlapping ranges at every
// distance up to 40 either way; compares with the first difference at every place and of both
// signs. Built twice: plain, and (WL_MEMFAST_WRAP, linked with --wrap) as the program's memcpy &
// co., with the switch off and on.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/memfast.h"

#define MAXN 300u
#define BUF (MAXN + 128u)

#if defined(WL_MEMFAST_WRAP)
void *__real_memcpy(void *dst, const void *src, size_t n);
void *__real_memmove(void *dst, const void *src, size_t n);
void *__real_memset(void *dst, int c, size_t n);
int __real_memcmp(const void *a, const void *b, size_t n);
#define REF_MOVE __real_memmove
#define REF_SET __real_memset
#define REF_CMP __real_memcmp
#else
#define REF_MOVE memmove
#define REF_SET memset
#define REF_CMP memcmp
#endif

typedef void *(*CopyFn)(void *, const void *, size_t);
typedef void *(*FillFn)(void *, int, size_t);
typedef int (*CmpFn)(const void *, const void *, size_t);

static _Alignas(32) uint8_t g_src[BUF], g_dst[BUF], g_ref[BUF];
static _Alignas(32) uint8_t g_ovl[BUF], g_ovl_ref[BUF];

static void pattern(uint8_t *p, size_t n, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1u;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t)(x >> 16);
    }
}

static int copies_exactly(CopyFn fn) {
    pattern(g_src, BUF, 1u);
    for (size_t doff = 0; doff < 32u; doff++) {
        for (size_t soff = 0; soff < 8u; soff++) {
            for (size_t n = 0; n <= MAXN; n++) {
                pattern(g_dst, BUF, (uint32_t)(n + doff * 7u + soff * 131u));
                REF_MOVE(g_ref, g_dst, BUF);
                REF_MOVE(g_ref + 32 + doff, g_src + 32 + soff, n);
                void *r = fn(g_dst + 32 + doff, g_src + 32 + soff, n);
                if (r != g_dst + 32 + doff || REF_CMP(g_dst, g_ref, BUF) != 0) {
                    fprintf(stderr, "copy n=%zu doff=%zu soff=%zu\n", n, doff, soff);
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int moves_exactly(CopyFn fn) {
    for (size_t off = 0; off < 8u; off++) {
        for (int dist = -40; dist <= 40; dist++) {
            for (size_t n = 0; n <= MAXN; n++) {
                size_t s = 48u + off, d = (size_t)((int)s + dist);
                pattern(g_ovl, BUF, (uint32_t)(n * 3u + off + (unsigned)(dist + 40) * 1000u));
                REF_MOVE(g_ovl_ref, g_ovl, BUF);
                REF_MOVE(g_ovl_ref + d, g_ovl_ref + s, n);
                void *r = fn(g_ovl + d, g_ovl + s, n);
                if (r != g_ovl + d || REF_CMP(g_ovl, g_ovl_ref, BUF) != 0) {
                    fprintf(stderr, "move n=%zu off=%zu dist=%d\n", n, off, dist);
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int fills_exactly(FillFn fn) {
    static const int values[] = {0, 0xA5, -1, 0x1FF};
    for (size_t v = 0; v < sizeof values / sizeof values[0]; v++) {
        for (size_t doff = 0; doff < 32u; doff++) {
            for (size_t n = 0; n <= MAXN; n++) {
                pattern(g_dst, BUF, (uint32_t)(n + doff * 5u + v));
                REF_MOVE(g_ref, g_dst, BUF);
                REF_SET(g_ref + 32 + doff, values[v], n);
                void *r = fn(g_dst + 32 + doff, values[v], n);
                if (r != g_dst + 32 + doff || REF_CMP(g_dst, g_ref, BUF) != 0) {
                    fprintf(stderr, "fill n=%zu doff=%zu v=%d\n", n, doff, values[v]);
                    return 0;
                }
            }
        }
    }
    return 1;
}

// whether the compare under test must give the byte difference (newlib's and ours do; a C
// library may give any value of the right sign, glibc on PowerPC gives -1 and 1)
static int g_exact = 1;

static int sign(int x) {
    return (x > 0) - (x < 0);
}

static int compares_exactly(CmpFn fn) {
    pattern(g_src, BUF, 7u);
    for (size_t aoff = 0; aoff < 8u; aoff++) {
        for (size_t boff = 0; boff < 8u; boff++) {
            uint8_t *a = g_src + 16 + aoff, *b = g_dst + 16 + boff;
            for (size_t n = 0; n <= MAXN; n++) {
                // equal, then one difference at each place (the last 40 places of long ones), both ways
                REF_MOVE(b, a, n);
                if (fn(a, b, n) != 0) {
                    fprintf(stderr, "compare equal n=%zu\n", n);
                    return 0;
                }
                for (size_t at = n > 40u ? n - 40u : 0u; at < n; at++) {
                    for (int up = 0; up < 2; up++) {
                        uint8_t keep = b[at];
                        b[at] = (uint8_t)(up ? a[at] + 1u + (unsigned)(at & 0x3Fu) : a[at] - 1u - (unsigned)(at & 0x3Fu));
                        int want = (int)a[at] - (int)b[at];
                        int got = fn(a, b, n), lib = REF_CMP(a, b, n);
                        b[at] = keep;
                        if ((g_exact ? got != want : sign(got) != sign(want)) || sign(got) != sign(lib)) {
                            fprintf(stderr, "compare n=%zu at=%zu got=%d want=%d\n", n, at, got, want);
                            return 0;
                        }
                    }
                }
            }
        }
    }
    return 1;
}

TEST copy_is_byte_exact_at_every_length_and_alignment(void) {
    ASSERT(copies_exactly(wl_memfast_copy));
    ASSERT(copies_exactly(wl_memfast_move));
    PASS();
}

TEST overlapping_copies_are_memmove(void) {
    ASSERT(moves_exactly(wl_memfast_move));
    ASSERT(moves_exactly(wl_memfast_copy));
    PASS();
}

TEST fill_is_byte_exact_at_every_length_and_alignment(void) {
    ASSERT(fills_exactly(wl_memfast_fill));
    PASS();
}

TEST compare_gives_the_first_difference(void) {
    ASSERT(compares_exactly(wl_memfast_compare));
    PASS();
}

TEST large_blocks(void) {
    // whole lines in the middle and the dcbz paths, at sizes the loop above does not reach
    static _Alignas(32) uint8_t a[9000], b[9000], r[9000];
    pattern(a, sizeof a, 3u);
    for (size_t n = 4000u; n < 4200u; n += 37u) {
        for (size_t off = 0; off < 32u; off += 5u) {
            pattern(b, sizeof b, (uint32_t)n);
            REF_MOVE(r, b, sizeof r);
            REF_MOVE(r + off, a + 3, n);
            wl_memfast_copy(b + off, a + 3, n);
            ASSERT_EQ(0, REF_CMP(b, r, sizeof b));
            REF_SET(r + off + 1, 0, n);
            wl_memfast_fill(b + off + 1, 0, n);
            ASSERT_EQ(0, REF_CMP(b, r, sizeof b));
            ASSERT_EQ(0, wl_memfast_compare(b + off + 1, r + off + 1, n));
            REF_MOVE(r + off + 100, r + off, n);
            wl_memfast_move(b + off + 100, b + off, n);
            ASSERT_EQ(0, REF_CMP(b, r, sizeof b));
        }
    }
    PASS();
}

#if defined(WL_MEMFAST_WRAP)
// Through the wrapped names, called through pointers so GCC cannot expand them inline
static void *(*volatile p_memcpy)(void *, const void *, size_t) = memcpy;
static void *(*volatile p_memmove)(void *, const void *, size_t) = memmove;
static void *(*volatile p_memset)(void *, int, size_t) = memset;
static int (*volatile p_memcmp)(const void *, const void *, size_t) = memcmp;

static void *call_memcpy(void *d, const void *s, size_t n) {
    // memcpy's ranges must not overlap: the overlapping cases go to memmove
    uintptr_t a = (uintptr_t)d, b = (uintptr_t)s;
    return (a < b + n && b < a + n) ? p_memmove(d, s, n) : p_memcpy(d, s, n);
}

TEST the_wrapped_names_are_exact_with_the_switch_on_and_off(void) {
    ASSERT(wl_memfast_wrapped());
    for (int on = 1; on >= 0; on--) {
        wl_memfast_set_enabled(on != 0);
        ASSERT_EQ(on != 0, wl_memfast_enabled());
        g_exact = on;
        ASSERT(copies_exactly(call_memcpy));
        ASSERT(moves_exactly(p_memmove));
        ASSERT(fills_exactly(p_memset));
        ASSERT(compares_exactly(p_memcmp));
    }
    wl_memfast_set_enabled(true);
    g_exact = 1;
#if defined(WL_MEMFAST_HIST)
    const WlMemfastHist *h = wl_memfast_hist();
    ASSERT(h->calls[0] > 0u && h->calls[1] > 0u && h->calls[2] > 0u && h->calls[3] > 0u);
    ASSERT(h->exact[2][17] > 0u && h->misaligned[0] > 0u);
#endif
    PASS();
}
#endif

SUITE(memfast_suite) {
    RUN_TEST(copy_is_byte_exact_at_every_length_and_alignment);
    RUN_TEST(overlapping_copies_are_memmove);
    RUN_TEST(fill_is_byte_exact_at_every_length_and_alignment);
    RUN_TEST(compare_gives_the_first_difference);
    RUN_TEST(large_blocks);
#if defined(WL_MEMFAST_WRAP)
    RUN_TEST(the_wrapped_names_are_exact_with_the_switch_on_and_off);
#endif
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(memfast_suite);
    GREATEST_MAIN_END();
}
