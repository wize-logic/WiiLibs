// SPDX-License-Identifier: MIT
// Block copies, fills and compares for Broadway (memfast.h), and the --wrap entries that make
// them the program's memcpy, memmove, memset and memcmp.
//
// Plain C over word loads and stores: GCC schedules the 8 loads of a line ahead of its 8 stores.
// It must not turn a loop here back into a call of memcpy or memset (that call would be wrapped
// to land here again): the file is compiled with -fno-tree-loop-distribute-patterns and
// -fno-builtin (wl/platform/CMakeLists.txt), and the attribute below says so again.
//
// Word access at any address goes through WlMfWord (aligned(1)): one lwz/stw on PowerPC, where
// the 750 does a misaligned integer access in hardware (an alignment exception is only for
// floating-point, lmw/stmw, lwarx/stwcx., dcbz on uncached memory and eciwx/ecowx: 750CL user's
// manual, "Alignment Exception"); a guest page boundary inside a misaligned word is what the Xbox
// runtime's DSI path already maps both pages for.
#include "wl/platform/memfast.h"

#include <string.h>

#include <stdlib.h>

#include "wl/platform/platform.h"

#if defined(__GNUC__) && !defined(__clang__)
#define WL_MF_FN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define WL_MF_FN
#endif

typedef uint32_t WlMfWord __attribute__((aligned(1), may_alias));

#define LD(p) (*(const WlMfWord *)(const void *)(p))
#define ST(p, v) (*(WlMfWord *)(void *)(p) = (v))

#if defined(__powerpc__) || defined(__PPC__)
#define WL_MF_PPC 1
// 32-byte data cache lines (the 750 family, and qemu-ppc's 750)
static inline void line_zero(uint8_t *p) {
    __asm__ volatile("dcbz 0,%0" ::"r"(p) : "memory");
}
static inline void line_touch(const uint8_t *p) {
    __asm__ volatile("dcbt 0,%0" ::"r"(p));
}
#else
#define WL_MF_PPC 0
static inline void line_zero(uint8_t *p) {
    (void)p;
}
static inline void line_touch(const uint8_t *p) {
    (void)p;
}
#endif

// Whether the lines of [d, d + n) may be zeroed with dcbz: cached RAM only (memfast.h). The Linux
// test build's memory is all cached; elsewhere there is no dcbz.
static inline bool zero_ok(const uint8_t *d, size_t n) {
#if defined(__wii__)
    uintptr_t a = (uintptr_t)d, e = a + n - 1u;
    if (a < 0x1C000000u) {
        return e < 0x1C000000u && e >= a;
    }
    if (a - 0x80000000u < 0x01800000u) {
        return e - 0x80000000u < 0x01800000u;
    }
    return a - 0x90000000u < 0x04000000u && e - 0x90000000u < 0x04000000u;
#else
    (void)d;
    (void)n;
    return WL_MF_PPC != 0;
#endif
}

// 0..16 bytes: every load before any store, so any overlap is fine. The short ones first: in Halo
// 45% of the copies are of 0 bytes and 20% of 1..3 (the remainder of MSVC's `rep movsd; rep movsb`,
// lifted as two copies), 82% have a pointer that is not word aligned.
static inline void copy_small(uint8_t *d, const uint8_t *s, size_t n) {
    if (n < 4u) {
        if (n != 0u) {
            uint8_t a = s[0], m = s[n >> 1], z = s[n - 1u];
            d[0] = a;
            d[n >> 1] = m;
            d[n - 1u] = z;
        }
    } else if (n < 8u) {
        uint32_t a = LD(s), z = LD(s + n - 4u);
        ST(d, a);
        ST(d + n - 4u, z);
    } else {
        uint32_t a = LD(s), b = LD(s + 4), y = LD(s + n - 8u), z = LD(s + n - 4u);
        ST(d, a);
        ST(d + 4, b);
        ST(d + n - 8u, y);
        ST(d + n - 4u, z);
    }
}

#define LINE_LOAD(s)                                                                                \
    uint32_t w0 = LD((s)), w1 = LD((s) + 4), w2 = LD((s) + 8), w3 = LD((s) + 12), w4 = LD((s) + 16), \
             w5 = LD((s) + 20), w6 = LD((s) + 24), w7 = LD((s) + 28)
#define LINE_STORE(d)                                                                               \
    do {                                                                                            \
        ST((d), w0);                                                                                \
        ST((d) + 4, w1);                                                                            \
        ST((d) + 8, w2);                                                                            \
        ST((d) + 12, w3);                                                                           \
        ST((d) + 16, w4);                                                                           \
        ST((d) + 20, w5);                                                                           \
        ST((d) + 24, w6);                                                                           \
        ST((d) + 28, w7);                                                                           \
    } while (0)

// Whole lines to a line-aligned destination: touch the source two lines ahead, its 8 words into
// registers, dcbz the destination line (when `zero`), the 8 words out. On PowerPC the loop is
// written out, 21 instructions a line with the counter in CTR and in 11 registers (the count
// comes in in w0), so a copy saves one register at most (GCC's own loop needed 12 besides the
// destination: 4 saved and restored a call).
#if WL_MF_PPC
#define COPY_LINE_LOOP(ZERO) \
    __asm__ volatile( \
                     "mtctr %[w0]\n" \
                     "1:\n" \
                     "dcbt %[s],%[ahead]\n" \
                     "lwz %[w0],0(%[s])\n" \
                     "lwz %[w1],4(%[s])\n" \
                     "lwz %[w2],8(%[s])\n" \
                     "lwz %[w3],12(%[s])\n" \
                     "lwz %[w4],16(%[s])\n" \
                     "lwz %[w5],20(%[s])\n" \
                     "lwz %[w6],24(%[s])\n" \
                     "lwz %[w7],28(%[s])\n" \
                     "addi %[s],%[s],32\n" \
                     ZERO \
                     "stw %[w0],0(%[d])\n" \
                     "stw %[w1],4(%[d])\n" \
                     "stw %[w2],8(%[d])\n" \
                     "stw %[w3],12(%[d])\n" \
                     "stw %[w4],16(%[d])\n" \
                     "stw %[w5],20(%[d])\n" \
                     "stw %[w6],24(%[d])\n" \
                     "stw %[w7],28(%[d])\n" \
                     "addi %[d],%[d],32\n" \
                     "bdnz 1b\n" \
                     : [d] "+b"(d), [s] "+b"(s), [w0] "+&r"(w0), [w1] "=&r"(w1), [w2] "=&r"(w2), \
                       [w3] "=&r"(w3), [w4] "=&r"(w4), [w5] "=&r"(w5), [w6] "=&r"(w6), [w7] "=&r"(w7) \
                     : [ahead] "r"(64) \
                     : "ctr", "memory")

static inline void copy_lines(uint8_t *d, const uint8_t *s, size_t lines, bool zero) {
    uint32_t w0 = (uint32_t)lines, w1, w2, w3, w4, w5, w6, w7; // w0: the count, then a word
    if (zero) {
        COPY_LINE_LOOP("dcbz 0,%[d]\n");
    } else {
        COPY_LINE_LOOP("");
    }
}
#else
static inline void copy_lines(uint8_t *d, const uint8_t *s, size_t lines, bool zero) {
    (void)zero;
    do {
        LINE_LOAD(s);
        LINE_STORE(d);
        d += 32;
        s += 32;
    } while (--lines != 0u);
}
#endif

// 16 bytes from s to d, the loads before the stores
static inline void copy16(uint8_t *d, const uint8_t *s) {
    uint32_t a = LD(s), b = LD(s + 4), c = LD(s + 8), e = LD(s + 12);
    ST(d, a);
    ST(d + 4, b);
    ST(d + 8, c);
    ST(d + 12, e);
}

// More than 16 bytes, the ranges disjoint (or the destination at least a line below the source):
// the first 32 bytes stored whole, then whole destination lines (dcbz'd first where the RAM is
// cached), then the last 16 bytes stored whole; the stores over each other write the same bytes,
// so head and tail are exact without a byte or word loop.
WL_MF_FN static void copy_disjoint(uint8_t *d, const uint8_t *s, size_t n) {
    if (n <= 32u) {
        copy16(d, s);
        copy16(d + n - 16u, s + n - 16u);
        return;
    }
    copy16(d, s);
    copy16(d + 16, s + 16);
    if (n <= 64u) {
        copy16(d + n - 32u, s + n - 32u);
        copy16(d + n - 16u, s + n - 16u);
        return;
    }
    size_t a = 32u - ((uintptr_t)d & 31u);
    d += a;
    s += a;
    n -= a;
    size_t lines = n >> 5;
    if (lines != 0u) {
        copy_lines(d, s, lines, zero_ok(d, lines << 5));
        d += lines << 5;
        s += lines << 5;
    }
    n &= 31u;
    if (n != 0u) {
        if (n > 16u) {
            copy16(d, s);
        }
        copy16(d + n - 16u, s + n - 16u);
    }
}

// Overlapping, the destination below the source (by less than a line): forward, each group's
// loads before its stores, no word stored twice
WL_MF_FN static void copy_forward(uint8_t *d, const uint8_t *s, size_t n) {
    for (; n != 0u && ((uintptr_t)d & 3u) != 0u; n--) {
        *d++ = *s++;
    }
    for (; n >= 32u; n -= 32u, d += 32, s += 32) {
        LINE_LOAD(s);
        LINE_STORE(d);
    }
    for (; n >= 4u; n -= 4u, d += 4, s += 4) {
        ST(d, LD(s));
    }
    for (; n != 0u; n--) {
        *d++ = *s++;
    }
}

// Overlapping, the destination above the source: from the end down
WL_MF_FN static void copy_backward(uint8_t *d, const uint8_t *s, size_t n) {
    d += n;
    s += n;
    for (; n != 0u && ((uintptr_t)d & 3u) != 0u; n--) {
        *--d = *--s;
    }
    for (; n >= 32u; n -= 32u) {
        d -= 32;
        s -= 32;
        LINE_LOAD(s);
        LINE_STORE(d);
    }
    for (; n >= 4u; n -= 4u) {
        d -= 4;
        s -= 4;
        ST(d, LD(s));
    }
    for (; n != 0u; n--) {
        *--d = *--s;
    }
}

WL_MF_FN void *wl_memfast_move(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (n <= 16u) {
        copy_small(d, s, n);
        return dst;
    }
    uintptr_t up = (uintptr_t)d - (uintptr_t)s, down = (uintptr_t)s - (uintptr_t)d;
    if (up < n) {
        if (up != 0u) {
            copy_backward(d, s, n);
        }
    } else if (down < n && down < 32u) {
        copy_forward(d, s, n);
    } else {
        copy_disjoint(d, s, n);
    }
    return dst;
}

WL_MF_FN void *wl_memfast_copy(void *dst, const void *src, size_t n) {
    return wl_memfast_move(dst, src, n);
}

WL_MF_FN void *wl_memfast_fill(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    uint8_t b = (uint8_t)c;
    uint32_t w = b * 0x01010101u;
    if (n <= 16u) {
        if (n >= 8u) {
            ST(d, w);
            ST(d + 4, w);
            ST(d + n - 8u, w);
            ST(d + n - 4u, w);
        } else if (n >= 4u) {
            ST(d, w);
            ST(d + n - 4u, w);
        } else if (n != 0u) {
            d[0] = b;
            d[n >> 1] = b;
            d[n - 1u] = b;
        }
        return dst;
    }
    if (n <= 32u) {
        ST(d, w);
        ST(d + 4, w);
        ST(d + 8, w);
        ST(d + 12, w);
        ST(d + n - 16u, w);
        ST(d + n - 12u, w);
        ST(d + n - 8u, w);
        ST(d + n - 4u, w);
        return dst;
    }
    ST(d, w);
    ST(d + 4, w);
    ST(d + 8, w);
    ST(d + 12, w);
    ST(d + 16, w);
    ST(d + 20, w);
    ST(d + 24, w);
    ST(d + 28, w);
    if (n <= 64u) {
        ST(d + n - 32u, w);
        ST(d + n - 28u, w);
        ST(d + n - 24u, w);
        ST(d + n - 20u, w);
        ST(d + n - 16u, w);
        ST(d + n - 12u, w);
        ST(d + n - 8u, w);
        ST(d + n - 4u, w);
        return dst;
    }
    size_t a = 32u - ((uintptr_t)d & 31u);
    d += a;
    n -= a;
    size_t lines = n >> 5;
    if (lines != 0u) {
        if (!zero_ok(d, lines << 5)) {
            do {
                ST(d, w);
                ST(d + 4, w);
                ST(d + 8, w);
                ST(d + 12, w);
                ST(d + 16, w);
                ST(d + 20, w);
                ST(d + 24, w);
                ST(d + 28, w);
                d += 32;
            } while (--lines != 0u);
        } else if (w == 0u) {
            do {
                line_zero(d);
                d += 32;
            } while (--lines != 0u);
        } else {
            do {
                line_zero(d);
                ST(d, w);
                ST(d + 4, w);
                ST(d + 8, w);
                ST(d + 12, w);
                ST(d + 16, w);
                ST(d + 20, w);
                ST(d + 24, w);
                ST(d + 28, w);
                d += 32;
            } while (--lines != 0u);
        }
    }
    n &= 31u;
    if (n != 0u) {
        if (n > 16u) {
            ST(d, w);
            ST(d + 4, w);
            ST(d + 8, w);
            ST(d + 12, w);
        }
        ST(d + n - 16u, w);
        ST(d + n - 12u, w);
        ST(d + n - 8u, w);
        ST(d + n - 4u, w);
    }
    return dst;
}

WL_MF_FN int wl_memfast_compare(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n >= 32u; n -= 32u, p += 32, q += 32) {
        uint32_t x = (LD(p) ^ LD(q)) | (LD(p + 4) ^ LD(q + 4)) | (LD(p + 8) ^ LD(q + 8)) | (LD(p + 12) ^ LD(q + 12));
        uint32_t y = (LD(p + 16) ^ LD(q + 16)) | (LD(p + 20) ^ LD(q + 20)) | (LD(p + 24) ^ LD(q + 24)) |
                     (LD(p + 28) ^ LD(q + 28));
        if ((x | y) != 0u) {
            break;
        }
    }
    for (; n >= 16u; n -= 16u, p += 16, q += 16) {
        uint32_t x = (LD(p) ^ LD(q)) | (LD(p + 4) ^ LD(q + 4));
        uint32_t y = (LD(p + 8) ^ LD(q + 8)) | (LD(p + 12) ^ LD(q + 12));
        if ((x | y) != 0u) {
            break;
        }
    }
    for (; n >= 4u && LD(p) == LD(q); n -= 4u, p += 4, q += 4) {
    }
    for (; n != 0u; n--, p++, q++) {
        if (*p != *q) {
            return (int)*p - (int)*q;
        }
    }
    return 0;
}

// ---- The switch, the counts and the wrap ---------------------------------------------------------

static volatile bool g_off;

void wl_memfast_set_enabled(bool on) {
    g_off = !on;
}

bool wl_memfast_enabled(void) {
    return !g_off;
}

void wl_memfast_options(const char *dir) {
    char flag[WL_FS_PATH_MAX];
    snprintf(flag, sizeof flag, "%s/oldcopy", dir);
    bool old = wl_platform_fs_exists(flag);
#if !defined(__wii__)
    old = old || getenv("WL_XB_OLDCOPY") != NULL;
#endif
    wl_memfast_set_enabled(!old);
    if (wl_memfast_wrapped()) {
        WL_LOG_I("memfast", "memcpy, memmove, memset and memcmp: %s",
                 old ? "the C library's (\"oldcopy\")" : "Broadway's line copies (\"oldcopy\": the C library's)");
    }
}

bool wl_memfast_wrapped(void) {
#if defined(WL_MEMFAST_WRAP)
    return true;
#else
    return false;
#endif
}

#if defined(WL_MEMFAST_HIST)
static WlMemfastHist g_hist;

static void count(unsigned op, const void *x, const void *y, size_t n) {
    g_hist.calls[op]++;
    g_hist.bytes[op] += n;
    if ((((uintptr_t)x | (uintptr_t)y) & 3u) != 0u) {
        g_hist.misaligned[op]++;
    }
    if (n < WL_MEMFAST_EXACT) {
        g_hist.exact[op][n]++;
        return;
    }
    unsigned k = 0;
    for (size_t top = 128u; n > top && k + 1u < WL_MEMFAST_BUCKETS; top <<= 1) {
        k++;
    }
    g_hist.bucket[op][k]++;
}
#define COUNT(op, x, y, n) count((op), (x), (y), (n))
#else
#define COUNT(op, x, y, n) ((void)0)
#endif

bool wl_memfast_hist_enabled(void) {
#if defined(WL_MEMFAST_HIST)
    return true;
#else
    return false;
#endif
}

const WlMemfastHist *wl_memfast_hist(void) {
#if defined(WL_MEMFAST_HIST)
    return &g_hist;
#else
    static const WlMemfastHist none;
    return &none;
#endif
}

void wl_memfast_hist_write(FILE *out) {
#if defined(WL_MEMFAST_HIST)
    static const char *const names[WL_MEMFAST_OPS] = {"copy", "move", "fill", "compare"};
    for (unsigned op = 0; op < WL_MEMFAST_OPS; op++) {
        uint64_t calls = g_hist.calls[op];
        fprintf(out, "memfast %s: %llu calls, %llu bytes, %u misaligned\n", names[op], (unsigned long long)calls,
                (unsigned long long)g_hist.bytes[op], (unsigned)g_hist.misaligned[op]);
        if (calls == 0u) {
            continue;
        }
        for (unsigned n = 0; n < WL_MEMFAST_EXACT; n++) {
            uint32_t c = g_hist.exact[op][n];
            if ((uint64_t)c * 1000u >= calls) {
                fprintf(out, "memfast %s %u: %u (%.1f%%)\n", names[op], n, (unsigned)c, 100.0 * c / (double)calls);
            }
        }
        size_t lo = 65u, hi = 128u;
        for (unsigned k = 0; k < WL_MEMFAST_BUCKETS; k++, lo = hi + 1u, hi <<= 1) {
            uint32_t c = g_hist.bucket[op][k];
            if ((uint64_t)c * 1000u >= calls) {
                if (k + 1u == WL_MEMFAST_BUCKETS) {
                    fprintf(out, "memfast %s %zu+: %u (%.1f%%)\n", names[op], lo, (unsigned)c, 100.0 * c / (double)calls);
                } else {
                    fprintf(out, "memfast %s %zu..%zu: %u (%.1f%%)\n", names[op], lo, hi, (unsigned)c,
                            100.0 * c / (double)calls);
                }
            }
        }
    }
#else
    (void)out;
#endif
}

#if defined(WL_MEMFAST_HIST) && !defined(__wii__)
// A host run with WL_MEMFAST_HIST_OUT=<file> writes the counts there when the program exits
static void hist_at_exit(void) {
    const char *path = getenv("WL_MEMFAST_HIST_OUT");
    FILE *f = path != NULL ? fopen(path, "w") : NULL;
    if (f != NULL) {
        wl_memfast_hist_write(f);
        fclose(f);
    }
}

__attribute__((constructor)) static void hist_arm(void) {
    if (getenv("WL_MEMFAST_HIST_OUT") != NULL) {
        atexit(hist_at_exit);
    }
}
#endif

#if defined(WL_MEMFAST_WRAP)
void *__real_memcpy(void *dst, const void *src, size_t n);
void *__real_memmove(void *dst, const void *src, size_t n);
void *__real_memset(void *dst, int c, size_t n);
int __real_memcmp(const void *a, const void *b, size_t n);
void *__wrap_memcpy(void *dst, const void *src, size_t n);
void *__wrap_memmove(void *dst, const void *src, size_t n);
void *__wrap_memset(void *dst, int c, size_t n);
int __wrap_memcmp(const void *a, const void *b, size_t n);

void *__wrap_memcpy(void *dst, const void *src, size_t n) {
    COUNT(0u, dst, src, n);
    if (n == 0u) {
        return dst; // (the most common length in Halo: see copy_small)
    }
    if (g_off) {
        return __real_memcpy(dst, src, n);
    }
    return wl_memfast_move(dst, src, n);
}

void *__wrap_memmove(void *dst, const void *src, size_t n) {
    COUNT(1u, dst, src, n);
    if (g_off) {
        return __real_memmove(dst, src, n);
    }
    return wl_memfast_move(dst, src, n);
}

void *__wrap_memset(void *dst, int c, size_t n) {
    COUNT(2u, dst, dst, n);
    if (g_off) {
        return __real_memset(dst, c, n);
    }
    return wl_memfast_fill(dst, c, n);
}

int __wrap_memcmp(const void *a, const void *b, size_t n) {
    COUNT(3u, a, b, n);
    if (g_off) {
        return __real_memcmp(a, b, n);
    }
    return wl_memfast_compare(a, b, n);
}
#endif
