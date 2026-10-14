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
#include <malloc.h>

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
    long used = __atomic_load_n(&g_live, __ATOMIC_RELAXED) - __atomic_load_n(&g_base, __ATOMIC_RELAXED);
    if (more <= (size_t)LONG_MAX && used + (long)more <= cap) {
        return 1;
    }
    __atomic_add_fetch(&g_refused, 1u, __ATOMIC_RELAXED);
    size_t seen = __atomic_load_n(&g_largest_refused, __ATOMIC_RELAXED);
    while (more > seen &&
           !__atomic_compare_exchange_n(&g_largest_refused, &seen, more, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    errno = ENOMEM;
    return 0;
}

static void *counted(void *p) {
    if (p != NULL && t_exempt > 0 && exempt_add(p)) {
        return p;
    }
    if (p != NULL) {
        long now = live_add((long)malloc_usable_size(p));
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

WlResult wl_platform_heap_cap(size_t bytes) {
    (void)bytes;
    return WL_ERR_UNSUPPORTED;
}

#endif
