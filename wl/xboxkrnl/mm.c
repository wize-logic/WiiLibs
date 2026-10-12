// SPDX-License-Identifier: MIT
// Memory exports: contiguous and system memory from the guest-physical allocator (window VAs
// 0x80000000|P), user virtual memory (reservations and commits), queries, protection (recorded;
// the mapping is read-write), the GPU's instance memory, and the pools (Ex).
#include "wl/xboxkrnl/kernel_internal.h"

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_RESET 0x80000u
#define MEM_PRIVATE 0x20000u

static uint32_t window_va(uint32_t phys) {
    return WL_XB_WINDOW | phys;
}

static uint32_t contiguous(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t align) {
    uint32_t phys = 0;
    if (bytes == 0 || wl_xb_phys_alloc(bytes, lowest, highest, align < WL_XB_PAGE ? WL_XB_PAGE : align, &phys) != WL_OK) {
        WL_LOG_W(XK_TAG, "contiguous 0x%x in [0x%08x, 0x%08x] align 0x%x: no memory (from 0x%08x)", (unsigned)bytes,
                 (unsigned)lowest, (unsigned)highest, (unsigned)align, (unsigned)wl_xb_kernel_caller());
        return 0;
    }
    return window_va(phys);
}

// PVOID MmAllocateContiguousMemory(SIZE_T NumberOfBytes)
void k_MmAllocateContiguousMemory(void) {
    RET(contiguous(ARG(0), 0, 0xFFFFFFFFu, WL_XB_PAGE));
}

// PVOID MmAllocateContiguousMemoryEx(SIZE_T NumberOfBytes, ULONG_PTR Lowest, ULONG_PTR Highest,
//                                    ULONG_PTR Alignment, ULONG Protect)
void k_MmAllocateContiguousMemoryEx(void) {
    uint32_t va = contiguous(ARG(0), ARG(1), ARG(2), ARG(3));
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
    if (phys == 0xFFFFFFFFu || wl_xb_phys_free(phys) != WL_OK) {
        WL_LOG_W(XK_TAG, "MmFreeContiguousMemory(0x%08x): not an allocation", (unsigned)ARG(0));
    }
}

// PVOID MmAllocateSystemMemory(SIZE_T NumberOfBytes, ULONG Protect)
void k_MmAllocateSystemMemory(void) {
    RET(contiguous(ARG(0), 0, 0xFFFFFFFFu, WL_XB_PAGE));
}

// ULONG MmFreeSystemMemory(PVOID BaseAddress, SIZE_T NumberOfBytes)
void k_MmFreeSystemMemory(void) {
    uint32_t phys = phys_of_window(ARG(0));
    uint32_t size = phys != 0xFFFFFFFFu ? wl_xb_phys_size(phys) : 0;
    if (size != 0) {
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

// The physical page lock and persistence calls have nothing to do here.
void k_MmLockUnlockBufferPages(void) {
}

void k_MmLockUnlockPhysicalPage(void) {
}

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
    if (type & MEM_RESERVE) {
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
        r = wl_xb_va_release(base & ~(WL_XB_PAGE - 1u), &released);
        size = released;
    } else if (type & MEM_DECOMMIT) {
        if (size == 0) {
            WlXbVaInfo info;
            if (wl_xb_va_query(base, &info) == WL_OK) {
                size = info.size;
            }
        }
        r = wl_xb_va_decommit(base & ~(WL_XB_PAGE - 1u), size);
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
}
