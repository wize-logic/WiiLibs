// SPDX-License-Identifier: MIT
// newlib's heap with an overflow (heap_overflow.h): the linker's --wrap sends malloc, calloc,
// realloc, free, memalign, aligned_alloc, malloc_usable_size and newlib's reentrant forms of
// them here; the allocator newlib really has (__real_*) is the primary, and what it cannot hold
// comes from a wl_mem heap (a MEM2 tag) instead. Opt in from CMake with
// wl_platform_heap_overflow(<executable>); wl_heap_overflow_enable(tag) arms it after wl_mem_init.
//
// Why both the plain and the reentrant names: devkitPPC's newlib keeps malloc, free, ... in
// objects of their own that call _malloc_r, _free_r, ... (so wrapping the reentrant forms
// catches what libc itself allocates: fopen, strdup, ...), while a nano-malloc build defines
// both in one object (so the plain names must be wrapped too). Either way the work is one
// code path, in the core.
//
// The one hazard: newlib's own calloc, realloc and memalign call _malloc_r, which would now
// reach this file and could hand them a block of another heap, to be cut up as if it were
// theirs. The primary's operations therefore run with malloc's lock held and `inside_newlib`
// raised, and a _malloc_r that finds it raised goes straight to newlib. (The lock is
// libsysbase's recursive __malloc_lock, which newlib takes itself.)
//
// This file includes no libogc header; the host tests compile it against a fake __real_* set.
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/heap_overflow.h"
#include "wl/platform/heap_sites.h"

#if defined(__has_include)
#if __has_include(<reent.h>)
#include <reent.h>
#define WL_HAVE_REENT 1
#endif
#endif
#ifndef WL_HAVE_REENT
struct _reent;
#define _REENT ((struct _reent *)0)
#endif

#define LOG_TAG "heap"

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__real_memalign(size_t alignment, size_t size);
size_t __real_malloc_usable_size(void *pointer);
void *__real__malloc_r(struct _reent *reent, size_t size);
void *__real__calloc_r(struct _reent *reent, size_t count, size_t size);
void *__real__realloc_r(struct _reent *reent, void *pointer, size_t size);
void __real__free_r(struct _reent *reent, void *pointer);
void *__real__memalign_r(struct _reent *reent, size_t alignment, size_t size);
size_t __real__malloc_usable_size_r(struct _reent *reent, void *pointer);

void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);
void __wrap_free(void *pointer);
void *__wrap_memalign(size_t alignment, size_t size);
void *__wrap_aligned_alloc(size_t alignment, size_t size);
size_t __wrap_malloc_usable_size(void *pointer);
void *__wrap__malloc_r(struct _reent *reent, size_t size);
void *__wrap__calloc_r(struct _reent *reent, size_t count, size_t size);
void *__wrap__realloc_r(struct _reent *reent, void *pointer, size_t size);
void __wrap__free_r(struct _reent *reent, void *pointer);
void *__wrap__memalign_r(struct _reent *reent, size_t alignment, size_t size);
size_t __wrap__malloc_usable_size_r(struct _reent *reent, void *pointer);

void __malloc_lock(struct _reent *reent);
void __malloc_unlock(struct _reent *reent);

static WlOverflow g_overflow;
// The accounting by call site, once wl_heap_overflow_track_sites has turned it on.
static WlHeapSites g_site_table;
static WlHeapSites *g_sites;
// Raised (under the malloc lock) while newlib's own function runs for us.
static int inside_newlib;

static struct _reent *reent_of(void *ctx) {
    return ctx != NULL ? (struct _reent *)ctx : _REENT;
}

static void enter(void *ctx) {
    __malloc_lock(reent_of(ctx));
    inside_newlib++;
}

static void leave(void *ctx) {
    inside_newlib--;
    __malloc_unlock(reent_of(ctx));
}

// ---- the primary through the plain names (ctx unused) -----------------------------------------

static void *plain_alloc(void *ctx, size_t align, size_t size) {
    enter(ctx);
    void *p = align != 0u ? __real_memalign(align, size) : __real_malloc(size);
    leave(ctx);
    return p;
}

static void *plain_calloc(void *ctx, size_t count, size_t size) {
    enter(ctx);
    void *p = __real_calloc(count, size);
    leave(ctx);
    return p;
}

static void *plain_realloc(void *ctx, void *ptr, size_t size) {
    enter(ctx);
    void *p = __real_realloc(ptr, size);
    leave(ctx);
    return p;
}

static void plain_free(void *ctx, void *ptr) {
    enter(ctx);
    __real_free(ptr);
    leave(ctx);
}

static size_t plain_usable(void *ctx, void *ptr) {
    enter(ctx);
    size_t n = __real_malloc_usable_size(ptr);
    leave(ctx);
    return n;
}

static void primary_usage(void *ctx, size_t *used, size_t *free_bytes) {
    (void)ctx;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    struct mallinfo info = mallinfo();
#pragma GCC diagnostic pop
    *used = (size_t)info.uordblks;
    *free_bytes = (size_t)info.fordblks;
}

static const WlOverflowPrimary k_plain = {NULL,       plain_alloc,  plain_calloc, plain_realloc,
                                          plain_free, plain_usable, primary_usage};

// ---- the primary through the reentrant names (ctx = the reent) --------------------------------

static void *reent_alloc(void *ctx, size_t align, size_t size) {
    enter(ctx);
    void *p = align != 0u ? __real__memalign_r(reent_of(ctx), align, size)
                          : __real__malloc_r(reent_of(ctx), size);
    leave(ctx);
    return p;
}

static void *reent_calloc(void *ctx, size_t count, size_t size) {
    enter(ctx);
    void *p = __real__calloc_r(reent_of(ctx), count, size);
    leave(ctx);
    return p;
}

static void *reent_realloc(void *ctx, void *ptr, size_t size) {
    enter(ctx);
    void *p = __real__realloc_r(reent_of(ctx), ptr, size);
    leave(ctx);
    return p;
}

static void reent_free(void *ctx, void *ptr) {
    enter(ctx);
    __real__free_r(reent_of(ctx), ptr);
    leave(ctx);
}

static size_t reent_usable(void *ctx, void *ptr) {
    enter(ctx);
    size_t n = __real__malloc_usable_size_r(reent_of(ctx), ptr);
    leave(ctx);
    return n;
}

static WlOverflowPrimary reent_primary(struct _reent *reent) {
    WlOverflowPrimary p = {reent_of(reent), reent_alloc,  reent_calloc, reent_realloc,
                           reent_free,      reent_usable, primary_usage};
    return p;
}

// True when newlib's own function (running for us) is calling: the caller holds the lock.
static bool nested(struct _reent *reent) {
    __malloc_lock(reent);
    bool inside = inside_newlib != 0;
    __malloc_unlock(reent);
    return inside;
}

// ---- who asked, when both heaps are full ------------------------------------------------------

#if defined(__powerpc__) && !defined(__linux__)
#define CALLERS 8
static void on_fail(void *user, const char *what, size_t size, size_t align) {
    (void)user;
    // The return addresses up the frame chain, for addr2line on the program's ELF (the first
    // ones are in this file and the core). The chain lives in MEM1/MEM2; stop outside them.
    unsigned long *frame = __builtin_frame_address(0);
    unsigned long callers[CALLERS] = {0};
    for (int depth = 0; depth < CALLERS && frame != NULL && (unsigned long)frame >= 0x80000000UL &&
                        (unsigned long)frame < 0x94000000UL;
         depth++) {
        frame = (unsigned long *)frame[0];
        if (frame == NULL || (unsigned long)frame < 0x80000000UL ||
            (unsigned long)frame >= 0x94000000UL) {
            break;
        }
        callers[depth] = frame[1];
    }
    WL_LOG_E(LOG_TAG, "%s(%u%s) failed, from %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", what,
             (unsigned)size, align != 0u ? ", aligned" : "", callers[0], callers[1], callers[2],
             callers[3], callers[4], callers[5], callers[6], callers[7]);
}
#else
#define on_fail NULL
#endif

// ---- the accounting by call site --------------------------------------------------------------

// The return addresses into the wrapper's caller and into that one's caller. On the Wii, from the
// EABI frames: the wrapper's back chain is its caller's frame, whose LR word holds the return
// into the caller; the caller's back chain leads to the frame holding its own return. A macro, so
// that the frame read is the wrapper's. Elsewhere only the first (the host's frames may have no
// chain).
#if defined(__powerpc__) && !defined(__linux__)
#define IN_RAM(p) ((unsigned long)(p) >= 0x80000000UL && (unsigned long)(p) < 0x94000000UL)
#define CALLERS(c0, c1)                                                                            \
    do {                                                                                           \
        unsigned long *caller_frame_ = *(unsigned long **)__builtin_frame_address(0);              \
        (c0) = (uintptr_t)__builtin_return_address(0);                                             \
        (c1) = 0u;                                                                                 \
        if (IN_RAM(caller_frame_) && IN_RAM(caller_frame_[0])) {                                   \
            (c1) = (uintptr_t)((unsigned long *)caller_frame_[0])[1];                              \
        }                                                                                          \
    } while (0)
#else
#define CALLERS(c0, c1)                                                                            \
    do {                                                                                           \
        (c0) = (uintptr_t)__builtin_return_address(0);                                             \
        (c1) = 0u;                                                                                 \
    } while (0)
#endif

static void note_alloc(const WlOverflowPrimary *p, void *ptr, uintptr_t c0, uintptr_t c1) {
    if (g_sites != NULL && ptr != NULL) {
        wl_heap_sites_alloc(g_sites, ptr, wl_overflow_usable_size(&g_overflow, p, ptr), c0, c1);
    }
}

// (before the block goes: once freed, its address may be another thread's new block)
static size_t note_free(const WlOverflowPrimary *p, void *ptr) {
    size_t size = 0u;
    if (g_sites != NULL && ptr != NULL) {
        size = wl_overflow_usable_size(&g_overflow, p, ptr);
        wl_heap_sites_free(g_sites, ptr, size);
    }
    return size;
}

// realloc: the old block taken off first, and put back if it stays (the call failed)
static void *tracked_realloc(const WlOverflowPrimary *p, void *ptr, size_t size, uintptr_t c0,
                             uintptr_t c1) {
    size_t old = note_free(p, ptr);
    void *q = wl_overflow_realloc(&g_overflow, p, ptr, size);
    if (q != NULL) {
        note_alloc(p, q, c0, c1);
    } else if (ptr != NULL && size != 0u && g_sites != NULL) {
        wl_heap_sites_alloc(g_sites, ptr, old, c0, c1);
    }
    return q;
}

// ---- the wrapped names ------------------------------------------------------------------------

void *__wrap_malloc(size_t size) {
    void *q = wl_overflow_malloc(&g_overflow, &k_plain, size);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        note_alloc(&k_plain, q, c0, c1);
    }
    return q;
}

void *__wrap_calloc(size_t count, size_t size) {
    void *q = wl_overflow_calloc(&g_overflow, &k_plain, count, size);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        note_alloc(&k_plain, q, c0, c1);
    }
    return q;
}

void *__wrap_realloc(void *pointer, size_t size) {
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        return tracked_realloc(&k_plain, pointer, size, c0, c1);
    }
    return wl_overflow_realloc(&g_overflow, &k_plain, pointer, size);
}

void __wrap_free(void *pointer) {
    note_free(&k_plain, pointer);
    wl_overflow_free(&g_overflow, &k_plain, pointer);
}

void *__wrap_memalign(size_t alignment, size_t size) {
    void *q = wl_overflow_memalign(&g_overflow, &k_plain, alignment, size);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        note_alloc(&k_plain, q, c0, c1);
    }
    return q;
}

void *__wrap_aligned_alloc(size_t alignment, size_t size) {
    void *q = wl_overflow_memalign(&g_overflow, &k_plain, alignment, size);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        note_alloc(&k_plain, q, c0, c1);
    }
    return q;
}

size_t __wrap_malloc_usable_size(void *pointer) {
    return wl_overflow_usable_size(&g_overflow, &k_plain, pointer);
}

void *__wrap__malloc_r(struct _reent *reent, size_t size) {
    if (nested(reent)) {
        return __real__malloc_r(reent, size);
    }
    WlOverflowPrimary p = reent_primary(reent);
    void *q = wl_overflow_malloc(&g_overflow, &p, size);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        note_alloc(&p, q, c0, c1);
    }
    return q;
}

void *__wrap__calloc_r(struct _reent *reent, size_t count, size_t size) {
    if (nested(reent)) {
        return __real__calloc_r(reent, count, size);
    }
    WlOverflowPrimary p = reent_primary(reent);
    void *q = wl_overflow_calloc(&g_overflow, &p, count, size);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        note_alloc(&p, q, c0, c1);
    }
    return q;
}

void *__wrap__realloc_r(struct _reent *reent, void *pointer, size_t size) {
    if (nested(reent)) {
        return __real__realloc_r(reent, pointer, size);
    }
    WlOverflowPrimary p = reent_primary(reent);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        return tracked_realloc(&p, pointer, size, c0, c1);
    }
    return wl_overflow_realloc(&g_overflow, &p, pointer, size);
}

void __wrap__free_r(struct _reent *reent, void *pointer) {
    WlOverflowPrimary p = reent_primary(reent);
    note_free(&p, pointer);
    wl_overflow_free(&g_overflow, &p, pointer);
}

void *__wrap__memalign_r(struct _reent *reent, size_t alignment, size_t size) {
    if (nested(reent)) {
        return __real__memalign_r(reent, alignment, size);
    }
    WlOverflowPrimary p = reent_primary(reent);
    void *q = wl_overflow_memalign(&g_overflow, &p, alignment, size);
    if (g_sites != NULL) {
        uintptr_t c0, c1;
        CALLERS(c0, c1);
        note_alloc(&p, q, c0, c1);
    }
    return q;
}

size_t __wrap__malloc_usable_size_r(struct _reent *reent, void *pointer) {
    if (wl_overflow_owns(&g_overflow, pointer)) {
        return wl_overflow_usable_size(&g_overflow, NULL, pointer);
    }
    return __real__malloc_usable_size_r(reent, pointer);
}

// ---- the public part --------------------------------------------------------------------------

WlResult wl_heap_overflow_enable(WlMemTag tag) {
    WlResult r = wl_overflow_init(&g_overflow, tag);
    if (r == WL_OK) {
        wl_overflow_set_fail_hook(&g_overflow, on_fail, NULL);
    }
    return r;
}

void wl_heap_overflow_disable(void) {
    wl_overflow_stop(&g_overflow);
}

void wl_heap_overflow_stats(WlOverflowStats *out) {
    wl_overflow_stats(&g_overflow, out);
}

void wl_heap_overflow_usage(WlOverflowUsage *out) {
    wl_overflow_usage(&g_overflow, &k_plain, out);
}

void wl_heap_overflow_log_usage(void) {
    wl_overflow_log_usage(&g_overflow, &k_plain);
}

bool wl_heap_overflow_owns(const void *ptr) {
    return wl_overflow_owns(&g_overflow, ptr);
}

WlResult wl_heap_overflow_track_sites(void *storage, size_t bytes, uint32_t sites,
                                      size_t min_tracked) {
    if (g_sites != NULL) {
        return WL_ERR_STATE;
    }
    WlResult r = wl_heap_sites_init(&g_site_table, storage, bytes, sites, min_tracked);
    if (r == WL_OK) {
        g_sites = &g_site_table;
    }
    return r;
}

void wl_heap_overflow_log_sites(size_t count) {
    wl_heap_sites_log(g_sites, count);
}

void wl_heap_overflow_site_totals(WlHeapSitesTotals *out) {
    wl_heap_sites_totals(g_sites, out);
}
