// SPDX-License-Identifier: MIT
// Wii memory backend: claims the MEM1/MEM2 arenas for wl/platform/mem.c, the
// data-cache helpers for GPU/DMA buffers, and the heap lock. Only this directory may include
// <ogc/*.h>.
//
// MEM1: libogc's malloc (newlib sbrk), the XFBs and the libraries' own allocations grow from the
// arena lo; the budgeted heaps take the top of the arena, so the part below stays libogc's.
// MEM1_SYSTEM_LEAVE is the least it keeps (about 1.5 MiB of XFBs plus libc, libfat and wiiuse).
// MEM2: libogc already took its IOS heap below the arena lo and above the hi before main, so the
// whole remaining range is ours; the budgets leave the reserve free inside it.
// A limit (wl_platform_mem_region_limit) claims only the top of either arena: malloc keeps the
// rest, and spills from MEM1 into the bottom of MEM2 as it does without wl_mem.
// Claiming = shrinking the libogc arena (SYS_SetArena1Hi, and SYS_SetArena2Lo for all of MEM2 or
// SYS_SetArena2Hi for its top) so nothing else allocates there afterwards. A fixed claim (wl_platform_mem_claim_fixed) moves the arena's end
// past itself before the region is claimed: MEM2's lo up, MEM1's hi down.
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/system.h>

#include <malloc.h>
#include <reent.h>
#include <stddef.h>
#include <string.h>

#include "wl/platform/mem.h"
#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"

#define MEM1_SYSTEM_LEAVE (4u * 1024u * 1024u)
#define ALIGN 32u

static struct {
    bool claimed[2];
    size_t limit[2]; // 0 = the whole arena
    void *base[2];
    size_t size[2];
    mutex_t lock;
    bool lock_ready;
} g;

static void lock_init(void) {
    if (!g.lock_ready) {
        LWP_MutexInit(&g.lock, true); // recursive
        g.lock_ready = true;
    }
}

WlResult wl_platform_mem_region(WlMemRegion region, void **out_base, size_t *out_size) {
    if ((unsigned)region > WL_MEM2 || out_base == NULL || out_size == NULL) {
        return WL_ERR_ARG;
    }
    lock_init();
    if (!g.claimed[region]) {
        uintptr_t lo, hi;
        if (region == WL_MEM1) {
            lo = (uintptr_t)SYS_GetArena1Lo();
            hi = (uintptr_t)SYS_GetArena1Hi();
            lo += MEM1_SYSTEM_LEAVE;
        } else {
            lo = (uintptr_t)SYS_GetArena2Lo();
            hi = (uintptr_t)SYS_GetArena2Hi();
        }
        lo = (lo + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1);
        hi &= ~(uintptr_t)(ALIGN - 1);
        if (lo >= hi) {
            return WL_ERR_NOMEM;
        }
        bool limited = g.limit[region] != 0u && g.limit[region] < hi - lo;
        if (limited) {
            lo = (hi - g.limit[region]) & ~(uintptr_t)(ALIGN - 1);
        }
        if (region == WL_MEM1) {
            SYS_SetArena1Hi((void *)lo); // libogc keeps [arena lo, lo)
        } else if (limited) {
            SYS_SetArena2Hi((void *)lo); // libogc keeps [arena lo, lo) for malloc's spill
        } else {
            SYS_SetArena2Lo((void *)hi); // all of it is ours
        }
        g.base[region] = (void *)lo;
        g.size[region] = hi - lo;
        g.claimed[region] = true;
    }
    *out_base = g.base[region];
    *out_size = g.size[region];
    return WL_OK;
}

WlResult wl_platform_mem_region_limit(WlMemRegion region, size_t max_size) {
    if ((unsigned)region > WL_MEM2 || max_size == 0u) {
        return WL_ERR_ARG;
    }
    lock_init();
    if (g.claimed[region]) {
        return WL_ERR_STATE;
    }
    g.limit[region] = (max_size + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
    return WL_OK;
}

WlResult wl_platform_mem_claim_fixed(WlMemRegion region, void *address, size_t size) {
    if ((unsigned)region > WL_MEM2 || size == 0u || ((uintptr_t)address & (ALIGN - 1)) != 0u ||
        (size & (ALIGN - 1)) != 0u) {
        return WL_ERR_ARG;
    }
    lock_init();
    if (g.claimed[region]) {
        return WL_ERR_STATE;
    }
    uintptr_t lo = (uintptr_t)(region == WL_MEM1 ? SYS_GetArena1Lo() : SYS_GetArena2Lo());
    uintptr_t hi = (uintptr_t)(region == WL_MEM1 ? SYS_GetArena1Hi() : SYS_GetArena2Hi());
    uintptr_t start = (uintptr_t)address;
    if (start < lo || size > hi - start) {
        return WL_ERR_RANGE;
    }
    if (region == WL_MEM1) {
        if (start + size < hi) {
            WL_LOG_W("MEM", "fixed MEM1 claim at %p leaves %u bytes above it unused", address,
                     (unsigned)(hi - start - size));
        }
        SYS_SetArena1Hi((void *)start); // the region now ends below the claim
    } else {
        if (start > lo) {
            WL_LOG_W("MEM", "fixed MEM2 claim at %p leaves %u bytes below it unused", address,
                     (unsigned)(start - lo));
        }
        SYS_SetArena2Lo((void *)(start + size)); // the region now starts above the claim
    }
    return WL_OK;
}

void wl_platform_mem_lock(void) {
    if (g.lock_ready) {
        LWP_MutexLock(g.lock);
    }
}

void wl_platform_mem_unlock(void) {
    if (g.lock_ready) {
        LWP_MutexUnlock(g.lock);
    }
}

void wl_gpu_buffer_commit(void *ptr, size_t size) {
    WL_ASSERT(((uintptr_t)ptr & (WL_ARENA_ALIGN_GPU - 1)) == 0 &&
              (size & (WL_ARENA_ALIGN_GPU - 1)) == 0);
    DCFlushRange(ptr, (u32)size);
}

void wl_gpu_buffer_invalidate(void *ptr, size_t size) {
    WL_ASSERT(((uintptr_t)ptr & (WL_ARENA_ALIGN_GPU - 1)) == 0 &&
              (size & (WL_ARENA_ALIGN_GPU - 1)) == 0);
    DCInvalidateRange(ptr, (u32)size);
}

// ---- the heap's figures (mem_census.h) --------------------------------------------------------
//
// newlib's mallinfo, and the arena libogc's sbrk has not handed out yet: it grows from MEM1's
// arena lo and, once MEM1 cannot hold a request, from MEM2's (for good, until it shrinks back to
// MEM2's start), and newlib trims the top back to it whenever more than 128 KB lies free there.

void *_sbrk_r(struct _reent *reent, ptrdiff_t incr);
void __malloc_lock(struct _reent *reent);
void __malloc_unlock(struct _reent *reent);
// newlib's own high-water mark of its arena (mallocr.c's max_sbrked_mem, a global of libc.a's
// mallocr.o): _malloc_r raises it each time it takes more from sbrk, and mallinfo's arena is
// the arena now (sbrked_mem). The marks below are read from it: no cost on malloc's path.
extern unsigned long __malloc_max_sbrked_mem;

static size_t g_mark_arena;   // the arena at wl_platform_heap_mark (0: the program's start)
static size_t g_mark_top;     // `top` then
static bool g_marked;

void wl_platform_heap_info(WlPlatformHeapInfo *out) {
    memset(out, 0, sizeof *out);
    __malloc_lock(_REENT);
    struct mallinfo mi = mallinfo();
    uintptr_t top = (uintptr_t)_sbrk_r(_REENT, 0); // (where the next sbrk would start)
    size_t max_arena = (size_t)__malloc_max_sbrked_mem;
    __malloc_unlock(_REENT);
    uintptr_t mem2_lo = (uintptr_t)SYS_GetArena2Lo(), mem2_hi = (uintptr_t)SYS_GetArena2Hi();
    if (top == (uintptr_t)-1) {
        top = 0;
    }
    out->known = true;
    out->arena = (size_t)mi.arena;
    out->in_use = (size_t)mi.uordblks;
    out->free_inside = (size_t)mi.fordblks;
    if (top >= 0x90000000u) {
        out->top = mem2_hi > top ? mem2_hi - top : 0;
        // newlib counts the hole between sbrk's last end in MEM1 (MEM1's arena lo, which stays
        // there) and its start in MEM2 as a block in use: taken back out (to MEM2's first byte, so
        // the figure is a few KB high, not exact). The part of it inside MEM1's arena, up to its hi,
        // is memory malloc can no longer reach: stranded.
        uintptr_t mem1_lo = (uintptr_t)SYS_GetArena1Lo(), mem1_hi = (uintptr_t)SYS_GetArena1Hi();
        uintptr_t hole = 0x90000000u - mem1_lo;
        out->in_use = out->in_use > hole ? out->in_use - hole : out->in_use;
        out->stranded = mem1_hi > mem1_lo ? mem1_hi - mem1_lo : 0;
    } else {
        uintptr_t mem1_hi = (uintptr_t)SYS_GetArena1Hi();
        out->top = mem1_hi > top ? mem1_hi - top : 0;
        out->elsewhere = mem2_hi > mem2_lo ? mem2_hi - mem2_lo : 0;
    }
    out->free_total = out->free_inside + out->top + out->elsewhere;
    // the arena grows only at its top, so what it grew past the mark's is what it took of the
    // mark's `top` (sbrk moving on to MEM2 after the mark would count MEM1's hole too: boot marks
    // after its probe has moved it)
    out->peak_known = true;
    out->peak = max_arena > g_mark_arena ? max_arena - g_mark_arena : 0;
    out->low_top = !g_marked ? SIZE_MAX : g_mark_top > out->peak ? g_mark_top - out->peak : 0;
}

void wl_platform_heap_mark(void) {
    WlPlatformHeapInfo h;
    wl_platform_heap_info(&h);
    __malloc_lock(_REENT);
    struct mallinfo mi = mallinfo();
    __malloc_max_sbrked_mem = (unsigned long)mi.arena;
    g_mark_arena = (size_t)mi.arena;
    __malloc_unlock(_REENT);
    g_mark_top = h.top;
    g_marked = true;
}

WlResult wl_platform_heap_cap(size_t bytes) {
    (void)bytes;
    return WL_ERR_UNSUPPORTED;
}

WlResult wl_platform_heap_cap_add(size_t bytes) {
    (void)bytes;
    return WL_ERR_UNSUPPORTED;
}

// The reclaimer is called by the wrap (heap_wrap_wii.c), when it is linked in: its malloc, weak
// here so that asking does not pull it in
void *__wrap_malloc(size_t size) __attribute__((weak));

bool wl_platform_heap_reclaims(void) {
    void *(*wrap)(size_t) = __wrap_malloc;
    __asm__("" : "+r"(wrap)); // not folded: weak, so it may be null
    return wrap != NULL;
}

void wl_platform_heap_exempt(bool on) {
    (void)on;
}
