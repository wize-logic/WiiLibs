// SPDX-License-Identifier: MIT
// Memory exports: contiguous and system memory from the guest-physical allocator (window VAs
// 0x80000000|P), user virtual memory (reservations and commits), queries, protection (recorded;
// the mapping is read-write), the GPU's instance memory, and the pools (Ex).
#include <stdlib.h>

#include "wl/xboxkrnl/kernel_internal.h"
#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/page_census.h"

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_RESET 0x80000u
#define MEM_PRIVATE 0x20000u

// ---- Allocation census (diagnostics) --------------------------------------------------------------
// Off until armed (WL_XB_ALLOCS=<KB> on the host, or wl_xk_alloc_census): then every contiguous,
// system or virtual allocation of at least that size is logged with its guest callers, and kept
// until freed for wl_xk_alloc_census_report. Off, it costs one compare per allocation.

#define CENSUS_MAX 256u
#define CENSUS_CALLERS 4u

typedef struct {
    uint32_t va, bytes;
    char kind; // 'C' contiguous, 'S' system, 'V' virtual
    uint32_t callers[CENSUS_CALLERS];
} XkCensusEntry;

static XkCensusEntry s_census[CENSUS_MAX];
static uint32_t s_census_count, s_census_min;

void wl_xk_alloc_census(uint32_t min_bytes) {
    s_census_min = min_bytes;
}

// The kernel call's return address, then the next return addresses on the guest stack.
static void census_callers(uint32_t *out) {
    memset(out, 0, CENSUS_CALLERS * sizeof *out);
    out[0] = wl_xb_kernel_caller();
    uint32_t n = 1;
    for (uint32_t i = 0; i < 96u && n < CENSUS_CALLERS; i++) {
        uint32_t slot = g_esp + 4u * i;
        if (!wl_xb_range_ok(slot, 4)) {
            break;
        }
        uint32_t w = wl_xb_rd32(slot);
        if (w != out[n - 1u] && wl_xb_guest_return_address(w)) {
            out[n++] = w;
        }
    }
}

static void census_add(char kind, uint32_t va, uint32_t bytes) {
    if (s_census_min == 0 || bytes < s_census_min) {
        return;
    }
    XkCensusEntry e = {.va = va, .bytes = bytes, .kind = kind};
    census_callers(e.callers);
    WL_LOG_I(XK_TAG, "alloc %c 0x%08x %u KB from %08x %08x %08x %08x", kind, (unsigned)va, (unsigned)(bytes >> 10),
             (unsigned)e.callers[0], (unsigned)e.callers[1], (unsigned)e.callers[2], (unsigned)e.callers[3]);
    if (s_census_count < CENSUS_MAX) {
        s_census[s_census_count++] = e;
    }
}

static void census_remove(uint32_t va) {
    if (s_census_min == 0) {
        return;
    }
    for (uint32_t i = 0; i < s_census_count; i++) {
        if (s_census[i].va == va) {
            WL_LOG_I(XK_TAG, "free %c 0x%08x %u KB", s_census[i].kind, (unsigned)va, (unsigned)(s_census[i].bytes >> 10));
            s_census[i] = s_census[--s_census_count];
            return;
        }
    }
}

// Each allocation kept: its pages with a frame now, and of those the ones not all zero (a page never
// touched is zero; one swapped out is not seen). A map of the allocation, one character a 256 KB:
// ' ' nothing nonzero seen, '.' under a quarter, '+' under three quarters, '#' more. Lock held.
void wl_xk_alloc_census_report(void) {
    static uint8_t page[WL_XB_PAGE];
    uint32_t total = 0, total_resident = 0, total_used = 0;
    WL_LOG_I(XK_TAG, "allocs: %u kept of at least %u KB (KB: asked, resident, nonzero)", (unsigned)s_census_count,
             (unsigned)(s_census_min >> 10));
    for (uint32_t i = 0; i < s_census_count; i++) {
        const XkCensusEntry *e = &s_census[i];
        uint32_t pages = (e->bytes + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT, resident = 0, used = 0, chunk_used = 0;
        char map[129];
        uint32_t m = 0;
        for (uint32_t p = 0; p < pages; p++) {
            uint32_t va = e->va + (p << WL_XB_PAGE_SHIFT);
            if (wl_xb_mem_peek(va, page, WL_XB_PAGE)) {
                resident++;
                for (uint32_t b = 0; b < WL_XB_PAGE; b++) {
                    if (page[b] != 0) {
                        used++;
                        chunk_used++;
                        break;
                    }
                }
            }
            if ((p & 63u) == 63u || p + 1u == pages) {
                uint32_t of = (p & 63u) + 1u;
                if (m < sizeof map - 1u) {
                    map[m++] = chunk_used == 0 ? ' ' : chunk_used * 4u < of ? '.' : chunk_used * 4u < of * 3u ? '+' : '#';
                }
                chunk_used = 0;
            }
        }
        map[m] = '\0';
        WL_LOG_I(XK_TAG, "allocs: %c 0x%08x %6u %6u %6u from %08x %08x %08x %08x |%s|", e->kind, (unsigned)e->va,
                 (unsigned)(e->bytes >> 10), (unsigned)(resident << 2), (unsigned)(used << 2), (unsigned)e->callers[0],
                 (unsigned)e->callers[1], (unsigned)e->callers[2], (unsigned)e->callers[3], map);
        total += e->bytes >> 10;
        total_resident += resident << 2;
        total_used += used << 2;
    }
    WL_LOG_I(XK_TAG, "allocs: total %u KB asked, %u resident, %u nonzero", (unsigned)total, (unsigned)total_resident,
             (unsigned)total_used);
}

static uint32_t window_va(uint32_t phys) {
    return WL_XB_WINDOW | phys;
}

static uint32_t contiguous(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t align, WlXbCensusAlloc kind) {
    uint32_t phys = 0;
    if (bytes == 0 || wl_xb_phys_alloc(bytes, lowest, highest, align < WL_XB_PAGE ? WL_XB_PAGE : align, &phys) != WL_OK) {
        WL_LOG_W(XK_TAG, "contiguous 0x%x in [0x%08x, 0x%08x] align 0x%x: no memory (from 0x%08x)", (unsigned)bytes,
                 (unsigned)lowest, (unsigned)highest, (unsigned)align, (unsigned)wl_xb_kernel_caller());
        return 0;
    }
    census_add(kind == WL_XB_CA_SYSTEM ? 'S' : 'C', window_va(phys), bytes);
    wl_xb_census_alloc(kind, window_va(phys), bytes, wl_xb_kernel_caller());
    return window_va(phys);
}

// PVOID MmAllocateContiguousMemory(SIZE_T NumberOfBytes)
void k_MmAllocateContiguousMemory(void) {
    RET(contiguous(ARG(0), 0, 0xFFFFFFFFu, WL_XB_PAGE, WL_XB_CA_CONTIG));
}

// PVOID MmAllocateContiguousMemoryEx(SIZE_T NumberOfBytes, ULONG_PTR Lowest, ULONG_PTR Highest,
//                                    ULONG_PTR Alignment, ULONG Protect)
void k_MmAllocateContiguousMemoryEx(void) {
    uint32_t va = contiguous(ARG(0), ARG(1), ARG(2), ARG(3), WL_XB_CA_CONTIG);
    if (va != 0 && ARG(1) != 0 && ARG(2) - ARG(1) + 1u <= ARG(0) + WL_XB_PAGE) {
        WL_LOG_I(XK_TAG, "pinned contiguous 0x%x at physical 0x%08x -> 0x%08x", (unsigned)ARG(0), (unsigned)ARG(1),
                 (unsigned)va);
    }
    RET(va);
}

static uint32_t phys_of_window(uint32_t va) {
    uint32_t phys = 0;
    return wl_xb_va_to_phys(va, &phys) && va >= WL_XB_WINDOW ? phys : 0xFFFFFFFFu;
}

// VOID MmFreeContiguousMemory(PVOID BaseAddress)
void k_MmFreeContiguousMemory(void) {
    uint32_t phys = phys_of_window(ARG(0));
    census_remove(ARG(0));
    if (phys == 0xFFFFFFFFu || wl_xb_phys_free(phys) != WL_OK) {
        WL_LOG_W(XK_TAG, "MmFreeContiguousMemory(0x%08x): not an allocation", (unsigned)ARG(0));
    }
}

// PVOID MmAllocateSystemMemory(SIZE_T NumberOfBytes, ULONG Protect)
void k_MmAllocateSystemMemory(void) {
    RET(contiguous(ARG(0), 0, 0xFFFFFFFFu, WL_XB_PAGE, WL_XB_CA_SYSTEM));
}

// ULONG MmFreeSystemMemory(PVOID BaseAddress, SIZE_T NumberOfBytes)
void k_MmFreeSystemMemory(void) {
    uint32_t phys = phys_of_window(ARG(0));
    uint32_t size = phys != 0xFFFFFFFFu ? wl_xb_phys_size(phys) : 0;
    if (size != 0) {
        census_remove(ARG(0));
        wl_xb_phys_free(phys);
    }
    RET(size >> WL_XB_PAGE_SHIFT);
}

// PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *NumberOfPaddingBytes): the instance
// memory is the top of the RAM (kept free from boot); the console returns the address the region
// ends at (gpu_instance_end) and D3D works down from it
void k_MmClaimGpuInstanceMemory(void) {
    uint32_t bytes = ARG(0), pad = ARG(1);
    if (pad != 0) {
        wl_xb_wr32(pad, 0);
    }
    uint32_t end = WL_XB_WINDOW + wl_xb_ram_bytes();
    if (bytes == 0xFFFFFFFFu) {
        bytes = 0x10000u;
    }
    RET(g_xk_cfg.gpu_instance_end ? end : end - bytes);
    WL_LOG_I(XK_TAG, "MmClaimGpuInstanceMemory(0x%x) -> 0x%08x", (unsigned)bytes, (unsigned)g_eax);
}

// ULONG_PTR MmGetPhysicalAddress(PVOID BaseAddress)
void k_MmGetPhysicalAddress(void) {
    uint32_t phys = 0;
    if (!wl_xb_va_to_phys(ARG(0), &phys)) {
        WL_LOG_W(XK_TAG, "MmGetPhysicalAddress(0x%08x): no page behind it", (unsigned)ARG(0));
    }
    RET(phys);
}

// VOID MmLockUnlockBufferPages(PVOID BaseAddress, SIZE_T NumberOfBytes, BOOLEAN UnlockPages): the
// pages behind the buffer keep their physical addresses while locked (a contiguous allocation does
// not move them).
void k_MmLockUnlockBufferPages(void) {
    uint32_t va = ARG(0), bytes = ARG(1);
    bool lock = (ARG(2) & 0xFFu) == 0;
    if (bytes == 0) {
        return;
    }
    uint32_t first = va & ~(WL_XB_PAGE - 1u), last = (va + bytes - 1u) & ~(WL_XB_PAGE - 1u);
    for (uint32_t page = first;; page += WL_XB_PAGE) {
        uint32_t phys = 0;
        if (wl_xb_va_to_phys(page, &phys)) {
            (void)wl_xb_phys_lock(phys, lock);
        }
        if (page == last) {
            break;
        }
    }
}

// VOID MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage)
void k_MmLockUnlockPhysicalPage(void) {
    (void)wl_xb_phys_lock(ARG(0), (ARG(1) & 0xFFu) == 0);
}

// Persistence across a title's reboot has nothing to do here.
void k_MmPersistContiguousMemory(void) {
}

// ULONG MmQueryAddressProtect(PVOID VirtualAddress)
void k_MmQueryAddressProtect(void) {
    uint32_t va = ARG(0);
    if (va >= WL_XB_WINDOW) {
        RET(wl_xb_range_ok(va, 1) ? WL_XB_PROT_READWRITE : 0u);
        return;
    }
    WlXbVaInfo info;
    RET(wl_xb_va_query(va, &info) == WL_OK && info.state == MEM_COMMIT ? info.protect : 0u);
}

// NTSTATUS MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect): VOID on Xbox
void k_MmSetAddressProtect(void) {
    wl_xb_va_protect(ARG(0), ARG(1), ARG(2), NULL);
}

// SIZE_T MmQueryAllocationSize(PVOID BaseAddress)
void k_MmQueryAllocationSize(void) {
    uint32_t phys = phys_of_window(ARG(0));
    RET(phys != 0xFFFFFFFFu ? wl_xb_phys_size(phys) : 0u);
}

// NTSTATUS MmQueryStatistics(PMM_STATISTICS MemoryStatistics): Length, then eight page counts
void k_MmQueryStatistics(void) {
    uint32_t s = ARG(0);
    if (!wl_xb_range_ok(s, 36) || wl_xb_rd32(s) != 36u) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    WlXbMemStats st;
    wl_xb_mem_stats(&st);
    wl_xb_wr32(s + 4u, st.ram_pages);                 // TotalPhysicalPages
    wl_xb_wr32(s + 8u, st.ram_pages - st.used_pages); // AvailablePages
    wl_xb_wr32(s + 12u, st.user_pages << WL_XB_PAGE_SHIFT); // VirtualMemoryBytesCommitted
    wl_xb_wr32(s + 16u, st.user_pages << WL_XB_PAGE_SHIFT); // VirtualMemoryBytesReserved
    wl_xb_wr32(s + 20u, 0);                           // CachePagesCommitted
    wl_xb_wr32(s + 24u, g_xk_stats.pool_bytes >> WL_XB_PAGE_SHIFT);
    wl_xb_wr32(s + 28u, 0);
    wl_xb_wr32(s + 32u, 0);
    RET(STATUS_SUCCESS);
}

void k_MmIsAddressValid(void) {
    RET(wl_xb_range_ok(ARG(0), 1) ? 1u : 0u);
}

// PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, SIZE_T NumberOfBytes, ULONG Protect): device space
// keeps its addresses; RAM is seen through the window
void k_MmMapIoSpace(void) {
    uint32_t phys = ARG(0);
    RET(phys >= WL_XB_MMIO_BASE ? phys : window_va(phys));
}

void k_MmUnmapIoSpace(void) {
}

// ---- Nt virtual memory -----------------------------------------------------------------------------

// NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize,
//                                  ULONG AllocationType, ULONG Protect)
void k_NtAllocateVirtualMemory(void) {
    uint32_t base_p = ARG(0), size_p = ARG(2), type = ARG(3), protect = ARG(4);
    if (!wl_xb_range_ok(base_p, 4) || !wl_xb_range_ok(size_p, 4)) {
        RET(STATUS_ACCESS_VIOLATION);
        return;
    }
    uint32_t base = wl_xb_rd32(base_p), size = wl_xb_rd32(size_p);
    if (size == 0 || (type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) == 0) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    if (type & MEM_RESET) {
        RET(STATUS_SUCCESS); // "the contents may go": they stay
        return;
    }
    WlResult r = WL_OK;
    if (base == 0 || (type & MEM_RESERVE)) { // MEM_COMMIT alone with no address reserves too, as NT does
        uint32_t va = base != 0 ? (base & ~(WL_XB_VA_GRANULE - 1u)) : 0u;
        uint32_t span = base != 0 ? (uint32_t)(((uint64_t)base + size + WL_XB_PAGE - 1u) & ~(uint64_t)(WL_XB_PAGE - 1u)) - va
                                  : size;
        r = wl_xb_va_reserve(&va, span, 0x00010000u, protect);
        if (r == WL_OK && (type & MEM_COMMIT)) {
            r = wl_xb_va_commit(va, span, protect);
            if (r != WL_OK) {
                wl_xb_va_release(va, NULL);
            }
        }
        if (r == WL_OK) {
            base = va;
            size = (span + WL_XB_PAGE - 1u) & ~(WL_XB_PAGE - 1u);
            census_add('V', base, size);
        }
    } else {
        uint32_t first = base & ~(WL_XB_PAGE - 1u);
        uint32_t span = (uint32_t)(((uint64_t)base + size + WL_XB_PAGE - 1u) & ~(uint64_t)(WL_XB_PAGE - 1u)) - first;
        r = wl_xb_va_commit(first, span, protect);
        base = first;
        size = span;
    }
    if (r != WL_OK) {
        RET(r == WL_ERR_RANGE ? STATUS_CONFLICTING_ADDRESSES : STATUS_NO_MEMORY);
        return;
    }
    wl_xb_wr32(base_p, base);
    wl_xb_wr32(size_p, size);
    if (type & MEM_COMMIT) {
        wl_xb_census_alloc(WL_XB_CA_VIRTUAL, base, size, wl_xb_kernel_caller());
    }
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType)
void k_NtFreeVirtualMemory(void) {
    uint32_t base_p = ARG(0), size_p = ARG(1), type = ARG(2);
    if (!wl_xb_range_ok(base_p, 4) || !wl_xb_range_ok(size_p, 4)) {
        RET(STATUS_ACCESS_VIOLATION);
        return;
    }
    uint32_t base = wl_xb_rd32(base_p), size = wl_xb_rd32(size_p);
    WlResult r;
    if (type & MEM_RELEASE) {
        uint32_t released = 0;
        census_remove(base & ~(WL_XB_PAGE - 1u));
        r = wl_xb_va_release(base & ~(WL_XB_PAGE - 1u), &released);
        size = released;
    } else if (type & MEM_DECOMMIT) {
        // the pages the range touches; size 0: from base to the end of its allocation
        uint32_t first = base & ~(WL_XB_PAGE - 1u);
        if (size == 0) {
            WlXbVaInfo info;
            uint32_t at = first, alloc = 0;
            while (at < WL_XB_USER_LIMIT && wl_xb_va_query(at, &info) == WL_OK && info.state != 0x10000u &&
                   (alloc == 0 || info.allocation_base == alloc) && info.size > 0) {
                alloc = info.allocation_base;
                at = info.base + info.size;
            }
            size = at - first;
        } else {
            size = (uint32_t)(((uint64_t)base + size + WL_XB_PAGE - 1u) & ~(uint64_t)(WL_XB_PAGE - 1u)) - first;
        }
        r = size > 0 ? wl_xb_va_decommit(first, size) : WL_ERR_RANGE;
    } else {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    if (r != WL_OK) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    wl_xb_wr32(base_p, base & ~(WL_XB_PAGE - 1u));
    wl_xb_wr32(size_p, size);
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION MemoryInformation)
void k_NtQueryVirtualMemory(void) {
    uint32_t va = ARG(0), out = ARG(1);
    WlXbVaInfo info;
    if (!wl_xb_range_ok(out, 28) || wl_xb_va_query(va, &info) != WL_OK) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    wl_xb_wr32(out + 0u, info.base);
    wl_xb_wr32(out + 4u, info.allocation_base);
    wl_xb_wr32(out + 8u, info.allocation_protect);
    wl_xb_wr32(out + 12u, info.size);
    wl_xb_wr32(out + 16u, info.state);
    wl_xb_wr32(out + 20u, info.protect);
    wl_xb_wr32(out + 24u, info.state == 0x10000u ? 0u : MEM_PRIVATE);
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect)
void k_NtProtectVirtualMemory(void) {
    uint32_t base_p = ARG(0), size_p = ARG(1), old_p = ARG(3);
    if (!wl_xb_range_ok(base_p, 4) || !wl_xb_range_ok(size_p, 4)) {
        RET(STATUS_ACCESS_VIOLATION);
        return;
    }
    uint32_t old = 0;
    if (wl_xb_va_protect(wl_xb_rd32(base_p), wl_xb_rd32(size_p), ARG(2), &old) != WL_OK) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    if (old_p != 0 && wl_xb_range_ok(old_p, 4)) {
        wl_xb_wr32(old_p, old);
    }
    RET(STATUS_SUCCESS);
}

// ---- Pools -----------------------------------------------------------------------------------------

void k_ExAllocatePool(void) {
    RET(xk_pool_alloc(ARG(0)));
}

void k_ExAllocatePoolWithTag(void) {
    RET(xk_pool_alloc(ARG(0)));
}

void k_ExFreePool(void) {
    xk_pool_free(ARG(0));
}

void k_ExQueryPoolBlockSize(void) {
    RET(xk_pool_size(ARG(0)));
}

void xk_link_mm(void) {
    const char *kb = getenv("WL_XB_ALLOCS");
    if (kb != NULL && s_census_min == 0) {
        long v = strtol(kb, NULL, 10);
        s_census_min = (v > 0 ? (uint32_t)v : 64u) << 10;
    }
}
