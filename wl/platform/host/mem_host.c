// SPDX-License-Identifier: MIT
// Host stub of the memory part of platform.h: one malloc'd block per region, sized
// like the Wii's usable arenas, allocated once and kept for the process (the
// pages are only touched when used), or as large as the region's limit. Fixed MEM2 claims are anonymous mappings at their exact
// address, and come out of the MEM2 block's size as they do out of the Wii's arena. The cache helpers only check alignment. The heap lock is a
// recursive pthread mutex so threaded tests can contend on it.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

#define HOST_MEM1 (20u * 1024u * 1024u)
#define HOST_MEM2 (52u * 1024u * 1024u)

static void *g_base[2];
static size_t g_limit[2];      // 0 = the whole region
static size_t g_claimed[2];    // bytes of fixed claims per region
static uintptr_t g_claim_end;  // MEM2: end of the last claim (they ascend)
static uintptr_t g_claim_start = UINTPTR_MAX; // MEM1: start of the last claim (they descend)

WlResult wl_platform_mem_region(WlMemRegion region, void **out_base, size_t *out_size) {
    if ((unsigned)region > WL_MEM2 || out_base == NULL || out_size == NULL) {
        return WL_ERR_ARG;
    }
    size_t size = (region == WL_MEM1 ? HOST_MEM1 : HOST_MEM2) - g_claimed[region];
    if (g_limit[region] != 0u && g_limit[region] < size) {
        size = g_limit[region];
    }
    if (g_base[region] == NULL) {
        g_base[region] = aligned_alloc(32u, size); // 32-byte aligned, as the Wii's arenas are
        if (g_base[region] == NULL) {
            return WL_ERR_NOMEM;
        }
    }
    *out_base = g_base[region];
    *out_size = size;
    return WL_OK;
}

WlResult wl_platform_mem_region_limit(WlMemRegion region, size_t max_size) {
    if ((unsigned)region > WL_MEM2 || max_size == 0u) {
        return WL_ERR_ARG;
    }
    if (g_base[region] != NULL) {
        return WL_ERR_STATE;
    }
    g_limit[region] = (max_size + 31u) & ~(size_t)31u;
    return WL_OK;
}

WlResult wl_platform_mem_claim_fixed(WlMemRegion region, void *address, size_t size) {
    if ((unsigned)region > WL_MEM2 || size == 0u || ((uintptr_t)address & 31u) != 0u || (size & 31u) != 0u) {
        return WL_ERR_ARG;
    }
    if (g_base[region] != NULL) {
        return WL_ERR_STATE;
    }
    size_t total = region == WL_MEM1 ? HOST_MEM1 : HOST_MEM2;
    bool in_order = region == WL_MEM1 ? (uintptr_t)address + size <= g_claim_start
                                      : (uintptr_t)address >= g_claim_end;
    if (!in_order || size > total - g_claimed[region]) {
        return WL_ERR_RANGE;
    }
    // (whole pages: the claim may share its first and last page with nothing else of ours)
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t first = (uintptr_t)address & ~(page - 1u);
    uintptr_t last = ((uintptr_t)address + size + page - 1u) & ~(page - 1u);
    void *mapped = mmap((void *)first, last - first, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped == MAP_FAILED) {
        return WL_ERR_NOMEM;
    }
    if (mapped != (void *)first) { // (kernels before 4.17 take the flag as a hint)
        munmap(mapped, last - first);
        return WL_ERR_NOMEM;
    }
    g_claimed[region] += size;
    if (region == WL_MEM1) {
        g_claim_start = (uintptr_t)address;
    } else {
        g_claim_end = (uintptr_t)address + size;
    }
    return WL_OK;
}

static pthread_mutex_t g_lock;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;

static void lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
    pthread_mutexattr_destroy(&a);
}

void wl_platform_mem_lock(void) {
    pthread_once(&g_lock_once, lock_init);
    pthread_mutex_lock(&g_lock);
}

void wl_platform_mem_unlock(void) {
    pthread_mutex_unlock(&g_lock);
}

void wl_gpu_buffer_commit(void *ptr, size_t size) {
    WL_ASSERT(((uintptr_t)ptr & (WL_ARENA_ALIGN_GPU - 1)) == 0 &&
              (size & (WL_ARENA_ALIGN_GPU - 1)) == 0);
}

void wl_gpu_buffer_invalidate(void *ptr, size_t size) {
    WL_ASSERT(((uintptr_t)ptr & (WL_ARENA_ALIGN_GPU - 1)) == 0 &&
              (size & (WL_ARENA_ALIGN_GPU - 1)) == 0);
}
