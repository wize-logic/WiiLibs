// SPDX-License-Identifier: MIT
// The host's heap figures and its cap (mem_census.h).
//
// On glibc this file replaces malloc and its relatives for the whole process (glibc supports it:
// a program's own malloc interposes the C library's, whose __libc_* entry points do the work), so
// that it can count the bytes live and refuse what would pass a cap. It is in the program only
// when something calls wl_platform_heap_info or wl_platform_heap_cap (a static library's object is
// linked only when one of its symbols is needed), and it counts from the process's start. The
// counts are the blocks' usable sizes (malloc_usable_size), kept with atomic additions; a block
// the C library made by a path of its own (none is known) and freed here would only lower them.
// A static link (WL_HEAP_HOST_STATIC, which CMake defines when the executables are linked with
// -static: the big-endian tests under qemu) cannot replace the C library's malloc: there the
// figures come from mallinfo2 alone and the cap is not offered. Elsewhere (no glibc) the figures
// are not known and the cap is not offered.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/mem_census.h"

#if defined(__GLIBC__) && !defined(WL_HEAP_HOST_STATIC)
#include <dlfcn.h>
#include <execinfo.h>
#include <malloc.h>

#include "wl/platform/heap_sites.h"

void *__libc_malloc(size_t size);
void *__libc_calloc(size_t count, size_t size);
void *__libc_realloc(void *ptr, size_t size);
void __libc_free(void *ptr);
void *__libc_memalign(size_t align, size_t size);
void *__libc_valloc(size_t size);
void *__libc_pvalloc(size_t size);

static long g_live;        // usable bytes of the live blocks counted
static long g_base;        // g_live when the cap was set
static long g_cap;         // 0: none
static long g_cap_peak;    // the most g_live - g_base reached under the cap
static uint32_t g_refused;    // requests the cap refused
static size_t g_largest_refused;
static long g_mark;           // g_live at wl_platform_heap_mark (0: the process's start)
static long g_peak;           // the most g_live reached since the mark

// ---- who holds it (wl_platform_heap_track_sites): the blocks by call site ----
static WlHeapSites g_site_table;
static WlHeapSites *g_sites;
static uintptr_t g_program_base; // the callers are kept from here: offsets for addr2line
static __thread int t_in_sites;  // (backtrace may allocate: not counted by site)

// The block `p` made, by the two callers above the allocator's entry (counted's caller's).
static __attribute__((noinline)) void sites_alloc(void *p) {
    WlHeapSites *s = __atomic_load_n(&g_sites, __ATOMIC_ACQUIRE);
    if (s == NULL || t_in_sites != 0) {
        return;
    }
    size_t size = malloc_usable_size(p);
    uintptr_t c0 = 0, c1 = 0;
    if (size >= s->min_tracked) {
        // frames: this function, counted (which the entry point, malloc or another, tail-calls), the
        // entry point's caller, the next
        void *frames[4];
        t_in_sites = 1;
        int n = backtrace(frames, 4);
        t_in_sites = 0;
        c0 = n > 2 ? (uintptr_t)frames[2] - g_program_base : 0u;
        c1 = n > 3 ? (uintptr_t)frames[3] - g_program_base : 0u;
    }
    wl_heap_sites_alloc(s, p, size, c0, c1);
}

static void sites_free(void *p) {
    WlHeapSites *s = __atomic_load_n(&g_sites, __ATOMIC_ACQUIRE);
    if (s != NULL) {
        wl_heap_sites_free(s, p, malloc_usable_size(p));
    }
}

static long live_add(long delta) {
    return __atomic_add_fetch(&g_live, delta, __ATOMIC_RELAXED);
}

// ---- the exempt blocks: a set of their addresses (open addressing, backward-shift deletion) ----

#define EXEMPT_SLOTS 8192u // a power of two
static __thread int t_exempt;
static uintptr_t g_exempt[EXEMPT_SLOTS];
static uint32_t g_exempt_count; // read without the lock as a hint (0: no lookup)
static int g_exempt_lock;

static void exempt_lock(void) {
    while (__atomic_exchange_n(&g_exempt_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

static void exempt_unlock(void) {
    __atomic_store_n(&g_exempt_lock, 0, __ATOMIC_RELEASE);
}

static uint32_t exempt_slot(uintptr_t p) {
    return (uint32_t)((p >> 4) * 2654435761u) & (EXEMPT_SLOTS - 1u);
}

// false when the set is full (the block is counted then, like any other)
static int exempt_add(void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    int added = 0;
    exempt_lock();
    if (g_exempt_count < EXEMPT_SLOTS * 3u / 4u) {
        uint32_t i = exempt_slot(p);
        while (g_exempt[i] != 0) {
            i = (i + 1u) & (EXEMPT_SLOTS - 1u);
        }
        g_exempt[i] = p;
        __atomic_store_n(&g_exempt_count, g_exempt_count + 1u, __ATOMIC_RELAXED);
        added = 1;
    }
    exempt_unlock();
    return added;
}

// true when `ptr` was an exempt block (taken out of the set)
static int exempt_take(void *ptr) {
    if (__atomic_load_n(&g_exempt_count, __ATOMIC_RELAXED) == 0) {
        return 0;
    }
    uintptr_t p = (uintptr_t)ptr;
    int found = 0;
    exempt_lock();
    uint32_t i = exempt_slot(p);
    while (g_exempt[i] != 0 && g_exempt[i] != p) {
        i = (i + 1u) & (EXEMPT_SLOTS - 1u);
    }
    if (g_exempt[i] == p) {
        found = 1;
        g_exempt[i] = 0;
        __atomic_store_n(&g_exempt_count, g_exempt_count - 1u, __ATOMIC_RELAXED);
        // the entries after it that it may have pushed along move back
        for (uint32_t j = (i + 1u) & (EXEMPT_SLOTS - 1u); g_exempt[j] != 0; j = (j + 1u) & (EXEMPT_SLOTS - 1u)) {
            uint32_t home = exempt_slot(g_exempt[j]);
            if (((j - home) & (EXEMPT_SLOTS - 1u)) >= ((j - i) & (EXEMPT_SLOTS - 1u))) {
                g_exempt[i] = g_exempt[j];
                g_exempt[j] = 0;
                i = j;
            }
        }
    }
    exempt_unlock();
    return found;
}

void wl_platform_heap_exempt(bool on) {
    t_exempt += on ? 1 : -1;
    if (t_exempt < 0) {
        t_exempt = 0;
    }
}

// Whether `more` bytes may be had under the cap (counting the refusal if not).
static int admit(size_t more) {
    long cap = __atomic_load_n(&g_cap, __ATOMIC_RELAXED);
    if (cap == 0) {
        return 1;
    }
    for (;;) {
        long used = __atomic_load_n(&g_live, __ATOMIC_RELAXED) - __atomic_load_n(&g_base, __ATOMIC_RELAXED);
        cap = __atomic_load_n(&g_cap, __ATOMIC_RELAXED);
        if (more <= (size_t)LONG_MAX && used + (long)more <= cap) {
            return 1;
        }
        // memory the program keeps for itself given back (mem_census.h), as the Wii's heap is
        // given it: the cap grows or blocks are freed
        if (wl_platform_heap_reclaim(more) == 0) {
            break;
        }
    }
    __atomic_add_fetch(&g_refused, 1u, __ATOMIC_RELAXED);
    size_t seen = __atomic_load_n(&g_largest_refused, __ATOMIC_RELAXED);
    while (more > seen &&
           !__atomic_compare_exchange_n(&g_largest_refused, &seen, more, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    errno = ENOMEM;
    return 0;
}

static __attribute__((noinline)) void *counted(void *p) {
    if (p != NULL && t_exempt > 0 && exempt_add(p)) {
        return p;
    }
    if (p != NULL) {
        sites_alloc(p);
        long now = live_add((long)malloc_usable_size(p));
        long high = __atomic_load_n(&g_peak, __ATOMIC_RELAXED);
        while (now > high &&
               !__atomic_compare_exchange_n(&g_peak, &high, now, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
        if (__atomic_load_n(&g_cap, __ATOMIC_RELAXED) != 0) {
            long used = now - __atomic_load_n(&g_base, __ATOMIC_RELAXED);
            long peak = __atomic_load_n(&g_cap_peak, __ATOMIC_RELAXED);
            while (used > peak &&
                   !__atomic_compare_exchange_n(&g_cap_peak, &peak, used, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            }
        }
    }
    return p;
}

// (an exempt thread's request is not held to the cap)
#define ADMIT(n) (t_exempt > 0 || admit(n))

void *malloc(size_t size) {
    return ADMIT(size) ? counted(__libc_malloc(size)) : NULL;
}

void *calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    return ADMIT(total) ? counted(__libc_calloc(count, size)) : NULL;
}

void free(void *ptr) {
    if (ptr != NULL) {
        if (!exempt_take(ptr)) {
            sites_free(ptr);
            live_add(-(long)malloc_usable_size(ptr));
        }
        __libc_free(ptr);
    }
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    size_t old = malloc_usable_size(ptr);
    int was_exempt = exempt_take(ptr);
    if (!was_exempt && size > old && !ADMIT(size - old)) {
        return NULL;
    }
    void *q = __libc_realloc(ptr, size);
    if (q == NULL) {
        if (was_exempt) {
            exempt_add(ptr); // (the block stays as it was)
        }
        return NULL;
    }
    if (was_exempt) {
        if (exempt_add(q)) {
            return q;
        }
        // (no room in the set: counted from now on)
    } else {
        live_add(-(long)old);
        WlHeapSites *sites = __atomic_load_n(&g_sites, __ATOMIC_ACQUIRE);
        if (sites != NULL) {
            // (the old block, moved or kept: off its site by the size it was counted for)
            wl_heap_sites_free(sites, ptr, old);
        }
    }
    return counted(q);
}

void *memalign(size_t align, size_t size) {
    return ADMIT(size) ? counted(__libc_memalign(align, size)) : NULL;
}

void *aligned_alloc(size_t align, size_t size) {
    return memalign(align, size);
}

int posix_memalign(void **out, size_t align, size_t size) {
    if (align < sizeof(void *) || (align & (align - 1u)) != 0) {
        return EINVAL;
    }
    void *p = memalign(align, size);
    if (p == NULL && size != 0) {
        return ENOMEM;
    }
    *out = p;
    return 0;
}

void *valloc(size_t size) {
    return ADMIT(size) ? counted(__libc_valloc(size)) : NULL;
}

void *pvalloc(size_t size) {
    return ADMIT(size) ? counted(__libc_pvalloc(size)) : NULL;
}

void wl_platform_heap_info(WlPlatformHeapInfo *out) {
    memset(out, 0, sizeof *out);
    struct mallinfo2 mi = mallinfo2();
    out->known = true;
    out->arena = mi.arena + mi.hblkhd;
    out->in_use = mi.uordblks + mi.hblkhd;
    out->free_inside = mi.fordblks;
    out->top = SIZE_MAX;
    out->free_total = SIZE_MAX;
    long peak = __atomic_load_n(&g_peak, __ATOMIC_RELAXED) - __atomic_load_n(&g_mark, __ATOMIC_RELAXED);
    out->peak_known = true;
    out->peak = peak > 0 ? (size_t)peak : 0;
    out->low_top = SIZE_MAX;
    long cap = __atomic_load_n(&g_cap, __ATOMIC_RELAXED);
    if (cap != 0) {
        long used = __atomic_load_n(&g_live, __ATOMIC_RELAXED) - __atomic_load_n(&g_base, __ATOMIC_RELAXED);
        out->cap = (size_t)cap;
        out->cap_used = used > 0 ? (size_t)used : 0;
        out->cap_peak = (size_t)__atomic_load_n(&g_cap_peak, __ATOMIC_RELAXED);
        out->cap_refused = __atomic_load_n(&g_refused, __ATOMIC_RELAXED);
        out->cap_largest_refused = (size_t)__atomic_load_n(&g_largest_refused, __ATOMIC_RELAXED);
        out->top = out->cap > out->cap_used ? out->cap - out->cap_used : 0;
        out->free_total = out->free_inside + out->top;
        out->low_top = out->cap > out->cap_peak ? out->cap - out->cap_peak : 0;
    }
}

void wl_platform_heap_mark(void) {
    long now = __atomic_load_n(&g_live, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mark, now, __ATOMIC_RELAXED);
    __atomic_store_n(&g_peak, now, __ATOMIC_RELAXED);
}

WlResult wl_platform_heap_cap_add(size_t bytes) {
    if (__atomic_load_n(&g_cap, __ATOMIC_RELAXED) == 0) {
        return WL_ERR_STATE;
    }
    if (bytes > (size_t)LONG_MAX / 2) {
        return WL_ERR_ARG;
    }
    __atomic_add_fetch(&g_cap, (long)bytes, __ATOMIC_RELAXED);
    return WL_OK;
}

bool wl_platform_heap_reclaims(void) {
    return true;
}

WlResult wl_platform_heap_track_sites(void *storage, size_t bytes, uint32_t sites, size_t min_tracked) {
    if (g_sites != NULL) {
        return WL_ERR_STATE;
    }
    Dl_info info;
    if (dladdr((void *)&wl_platform_heap_track_sites, &info) != 0) {
        g_program_base = (uintptr_t)info.dli_fbase;
    }
    void *frames[2];
    (void)backtrace(frames, 2); // (its first call loads the unwinder, which allocates)
    WlResult r = wl_heap_sites_init(&g_site_table, storage, bytes, sites, min_tracked);
    if (r == WL_OK) {
        __atomic_store_n(&g_sites, &g_site_table, __ATOMIC_RELEASE);
    }
    return r;
}

void wl_platform_heap_log_sites(size_t count) {
    WlHeapSites *s = __atomic_load_n(&g_sites, __ATOMIC_ACQUIRE);
    if (s != NULL) {
        wl_heap_sites_log(s, count);
    }
}

WlResult wl_platform_heap_cap(size_t bytes) {
    if (bytes > (size_t)LONG_MAX) {
        return WL_ERR_ARG;
    }
    __atomic_store_n(&g_cap, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_base, __atomic_load_n(&g_live, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&g_cap_peak, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_refused, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_largest_refused, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cap, (long)bytes, __ATOMIC_RELAXED);
    return WL_OK;
}

#else

void wl_platform_heap_exempt(bool on) {
    (void)on;
}

void wl_platform_heap_mark(void) {
}

#if defined(__GLIBC__)
#include <malloc.h>

void wl_platform_heap_info(WlPlatformHeapInfo *out) {
    memset(out, 0, sizeof *out);
    struct mallinfo2 mi = mallinfo2();
    out->known = true;
    out->arena = mi.arena + mi.hblkhd;
    out->in_use = mi.uordblks + mi.hblkhd;
    out->free_inside = mi.fordblks;
    out->top = SIZE_MAX;
    out->free_total = SIZE_MAX;
}
#else
void wl_platform_heap_info(WlPlatformHeapInfo *out) {
    memset(out, 0, sizeof *out);
}
#endif

WlResult wl_platform_heap_cap_add(size_t bytes) {
    (void)bytes;
    return WL_ERR_UNSUPPORTED;
}

bool wl_platform_heap_reclaims(void) {
    return false;
}

WlResult wl_platform_heap_track_sites(void *storage, size_t bytes, uint32_t sites, size_t min_tracked) {
    (void)storage;
    (void)bytes;
    (void)sites;
    (void)min_tracked;
    return WL_ERR_UNSUPPORTED;
}

void wl_platform_heap_log_sites(size_t count) {
    (void)count;
}

WlResult wl_platform_heap_cap(size_t bytes) {
    (void)bytes;
    return WL_ERR_UNSUPPORTED;
}

#endif
