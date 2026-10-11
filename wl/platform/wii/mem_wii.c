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

#include "wl/platform/mem.h"
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
