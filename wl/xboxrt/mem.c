// SPDX-License-Identifier: MIT
// The portable half of guest memory (mem.h): physical page bookkeeping, the contiguous allocator
// (top-down, relocating user pages out of a pinned range), user reservations and commits, queries
// and the accessors' helpers. The backends (mem_posix.c, mem_wii.c) only map pages.
#include <stdlib.h>
#include <string.h>

#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/page_census.h"
#include "wl/xboxrt/mem_internal.h"

#define TAG "xbmem"

enum { K_FREE = 0, K_RESERVED, K_CONTIG, K_USER };

#define USER_PAGES (WL_XB_USER_LIMIT >> WL_XB_PAGE_SHIFT)
#define VA_RESERVED 0x8000u // in g_va_prot: the page belongs to a reservation
#define MAX_REGIONS 4096u

typedef struct {
    uint32_t base, size, protect;
} Region;

uint32_t g_xb_ram_pages;
uint16_t *g_xb_va2phys;

static bool g_ready;
static uint8_t *g_kind;   // [ram pages] K_*
static uint32_t *g_run;   // [ram pages] pages of the allocation starting here (0: none)
static uint16_t *g_owner; // [ram pages] the user page a K_USER page backs
static uint16_t *g_locks; // [ram pages] the page's lock count (MmLockUnlock*): a locked page never moves
static uint16_t *g_va_prot; // [user pages] protection (low 15 bits) | VA_RESERVED
static Region *g_regions;   // reservations, sorted by base
static uint32_t g_region_count;
static uint32_t g_top_hint; // no free page above this one
static uint32_t g_used, g_user_pages, g_contig_pages, g_relocations;
static uint32_t g_low_pages; // the reserved low pages (the retail kernel's), lent as a last resort
// User pages page_ok found committed lately, direct-mapped by page number (the page + 1; 0: empty).
// A title project's own code checks every guest read with wl_xb_range_ok (Halo's play harness
// reads object fields this way: 1.3% of a10's time on the Wii), and the 128 KB table's entry is a
// cache miss more often than not; these 64 bytes stay in the cache. Only a decommit makes a
// committed page fail, and it empties them (and they are empty until guest memory is ready).
#define OK_PAGES 16u
static uint32_t g_ok_pages[OK_PAGES];
// The RAM window's bytes (0x80000000 up) once guest memory is ready, 0 before: wl_xb_range_ok's
// second common case. Halo keeps its objects and tags in contiguous memory, read through the window,
// and its level runner's reads of them were 3.5% of a d40 run on the Wii, nearly all in
// wl_xb_range_ok_slow.
static uint32_t g_window_bytes;

void wl_xb_mem_config_default(WlXbMemConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->ram_bytes = 64u << 20;
    cfg->reserved_low_bytes = 0x60000u;
    cfg->gpu_instance_bytes = 0x10000u;
    cfg->htab_bytes = 256u << 10;
    cfg->page_cache_bytes = WL_XB_PAGE_CACHE_AUTO;
}

bool wl_xb_mem_ready(void) {
    return g_ready;
}

uint32_t wl_xb_ram_bytes(void) {
    return g_xb_ram_pages << WL_XB_PAGE_SHIFT;
}

uint32_t wl_xb_ea_to_va(uint32_t ea) {
    ea &= WL_XB_EA_SPAN - 1u;
    if (ea < WL_XB_EA_WINDOW) {
        return ea;
    }
    if (ea >= WL_XB_EA_MMIO) {
        return ea | 0xE0000000u;
    }
    return (ea & 0x0FFFFFFFu) | WL_XB_WINDOW; // 0x1xxxxxxx -> 0x8xxxxxxx
}

static void mark(uint32_t first, uint32_t pages, uint8_t kind) {
    for (uint32_t p = first; p < first + pages; p++) {
        g_kind[p] = kind;
    }
}

// The host memory the guest's memory holds (mem_census.h): the page tables of the core, the paging
// and the backend; and the frames when they come from the heap (the Wii's pools).
static void census_tables(void *user, WlMemCensusReport *out) {
    (void)user;
    size_t tables, frames;
    wl_xb_backend_host_bytes(&tables, &frames);
    out->bytes = (size_t)g_xb_ram_pages * (sizeof *g_kind + sizeof *g_run + sizeof *g_owner + sizeof *g_locks) +
                 (size_t)USER_PAGES * (sizeof *g_va_prot + sizeof *g_xb_va2phys) + MAX_REGIONS * sizeof *g_regions +
                 wl_xb_pg_host_bytes() + tables;
}

static void census_frames(void *user, WlMemCensusReport *out) {
    (void)user;
    size_t tables;
    wl_xb_backend_host_bytes(&tables, &out->bytes);
}

WlResult wl_xb_mem_init(const WlXbMemConfig *cfg) {
    if (g_ready) {
        return WL_ERR_STATE;
    }
    if (cfg == NULL || cfg->ram_bytes < (4u << 20) || cfg->ram_bytes > (128u << 20) || (cfg->ram_bytes & 0x3FFFFFu) != 0 ||
        (cfg->reserved_low_bytes & (WL_XB_PAGE - 1u)) != 0 || (cfg->gpu_instance_bytes & (WL_XB_PAGE - 1u)) != 0 ||
        cfg->reserved_low_bytes + cfg->gpu_instance_bytes >= cfg->ram_bytes) {
        return WL_ERR_ARG;
    }
    uint32_t pages = cfg->ram_bytes >> WL_XB_PAGE_SHIFT;
    g_kind = calloc(pages, 1);
    g_run = calloc(pages, sizeof *g_run);
    g_owner = calloc(pages, sizeof *g_owner);
    g_locks = calloc(pages, sizeof *g_locks);
    g_va_prot = calloc(USER_PAGES, sizeof *g_va_prot);
    g_xb_va2phys = calloc(USER_PAGES, sizeof *g_xb_va2phys);
    g_regions = calloc(MAX_REGIONS, sizeof *g_regions);
    if (g_kind == NULL || g_run == NULL || g_owner == NULL || g_locks == NULL || g_va_prot == NULL ||
        g_xb_va2phys == NULL || g_regions == NULL) {
        return WL_ERR_NOMEM;
    }
    g_xb_ram_pages = pages;
    uint32_t low = cfg->reserved_low_bytes >> WL_XB_PAGE_SHIFT;
    uint32_t top = cfg->gpu_instance_bytes >> WL_XB_PAGE_SHIFT;
    mark(0, low, K_RESERVED);
    mark(pages - top, top, K_RESERVED);
    g_low_pages = low;
    g_used = low + top;
    g_top_hint = pages - top;
    uintptr_t base = 0;
    WlResult r = wl_xb_backend_init(cfg, &base);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "backend init failed: %s", wl_result_name(r));
        return r;
    }
#if !defined(__wii__)
    g_xbox_mem_offset = (ptrdiff_t)base;
#endif
    g_ready = true;
    g_window_bytes = pages << WL_XB_PAGE_SHIFT;
    (void)wl_mem_census_add("guest tables", census_tables, NULL);
    (void)wl_mem_census_add("frames", census_frames, NULL);
    WL_LOG_I(TAG, "guest RAM %u MB (%u pages), host base %p, reserved low 0x%x, GPU instance 0x%x", cfg->ram_bytes >> 20,
             (unsigned)pages, (void *)base, (unsigned)cfg->reserved_low_bytes, (unsigned)cfg->gpu_instance_bytes);
    const char *census = getenv("WL_XB_PAGE_CENSUS"); // the page census (page_census.h): a diagnostic
    if (census != NULL && census[0] != '\0') {
        (void)wl_xb_census_start(census);
    }
    return WL_OK;
}

// ---- Physical pages ----------------------------------------------------------------------------

// One free page, the highest there is (user pages and the kernel's single pages come from the top,
// so the bottom stays whole for the pinned buffers titles ask for).
static bool alloc_page_top(uint8_t kind, uint32_t *out) {
    for (uint32_t p = g_top_hint; p-- > 0;) {
        if (g_kind[p] == K_FREE) {
            g_kind[p] = kind;
            g_top_hint = p + 1u;
            g_used++;
            *out = p;
            return true;
        }
    }
    return false;
}

static void free_pages(uint32_t first, uint32_t pages) {
    wl_xb_backend_release_phys(first, pages);
    mark(first, pages, K_FREE);
    memset(g_locks + first, 0, pages * sizeof *g_locks); // a freed page's locks go with it
    g_used -= pages;
    if (first + pages > g_top_hint) {
        g_top_hint = first + pages;
    }
}

// The run [first, first + pages) can be had: every page free, or (when `move` is set) a user page
// nobody has locked (a title's DMA, or its own physical address bookkeeping, counts on it staying).
static bool run_ok(uint32_t first, uint32_t pages, bool move) {
    for (uint32_t p = first; p < first + pages; p++) {
        uint8_t k = g_kind[p];
        if (k != K_FREE && !(move && k == K_USER && g_locks[p] == 0)) {
            return false;
        }
    }
    return true;
}

static uint32_t count_kind(uint32_t first, uint32_t pages, uint8_t kind) {
    uint32_t n = 0;
    for (uint32_t p = first; p < first + pages; p++) {
        n += g_kind[p] == kind;
    }
    return n;
}

WlResult wl_xb_phys_lock(uint32_t phys, bool lock) {
    uint32_t p = phys >> WL_XB_PAGE_SHIFT;
    if (!g_ready || p >= g_xb_ram_pages) {
        return WL_ERR_RANGE;
    }
    if (lock) {
        if (g_locks[p] == 0xFFFFu) {
            return WL_ERR_LIMIT; // (the count stays: the page stays locked)
        }
        g_locks[p]++;
    } else if (g_locks[p] != 0) {
        g_locks[p]--;
    }
    return WL_OK;
}

uint32_t wl_xb_phys_lock_count(uint32_t phys) {
    uint32_t p = phys >> WL_XB_PAGE_SHIFT;
    return g_ready && p < g_xb_ram_pages ? g_locks[p] : 0u;
}

WlResult wl_xb_phys_alloc(uint32_t bytes, uint32_t lowest, uint32_t highest, uint32_t align, uint32_t *out) {
    if (!g_ready || out == NULL || bytes == 0 || (align & (align - 1u)) != 0) {
        return WL_ERR_ARG;
    }
    uint32_t ram = g_xb_ram_pages << WL_XB_PAGE_SHIFT;
    uint32_t pages = (uint32_t)(((uint64_t)bytes + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT);
    uint32_t align_pages = align <= WL_XB_PAGE ? 1u : align >> WL_XB_PAGE_SHIFT;
    if (highest >= ram) {
        highest = ram - 1u;
    }
    if (pages > g_xb_ram_pages || lowest > highest) {
        return WL_ERR_NOMEM;
    }
    uint32_t lo_page = (lowest + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT;
    uint32_t end_page = (highest + 1u) >> WL_XB_PAGE_SHIFT; // one past the last whole page
    if (end_page < lo_page + pages) {
        return WL_ERR_NOMEM;
    }
    lo_page = (lo_page + align_pages - 1u) / align_pages * align_pages;
    for (int pass = 0; pass < 2; pass++) {
        bool move = pass == 1;
        uint32_t start = (end_page - pages) / align_pages * align_pages;
        for (;;) {
            if (start < lo_page) {
                break;
            }
            if (run_ok(start, pages, move)) {
                uint32_t users = count_kind(start, pages, K_USER);
                uint32_t free_elsewhere = g_xb_ram_pages - g_used - (pages - users);
                if (users > free_elsewhere) {
                    break; // the pages to move cannot go anywhere
                }
                // claim the free pages first, so the moves below cannot land inside the run
                for (uint32_t p = start; p < start + pages; p++) {
                    if (g_kind[p] == K_FREE) {
                        g_kind[p] = K_CONTIG;
                        g_used++;
                    }
                }
                for (uint32_t p = start; p < start + pages; p++) {
                    if (g_kind[p] != K_USER) {
                        continue;
                    }
                    uint32_t to = 0;
                    if (!alloc_page_top(K_USER, &to)) {
                        WL_LOG_E(TAG, "relocation ran out of pages"); // cannot happen: counted above
                        return WL_ERR_NOMEM;
                    }
                    uint32_t vp = g_owner[p];
                    wl_xb_backend_move_phys(p, to, vp << WL_XB_PAGE_SHIFT);
                    g_owner[to] = (uint16_t)vp;
                    g_xb_va2phys[vp] = (uint16_t)(to + 1u);
                    g_kind[p] = K_CONTIG;
                    g_relocations++;
                }
                if (users > 0) {
                    WL_LOG_I(TAG, "moved %u user pages out of physical 0x%08x+0x%x", (unsigned)users,
                             (unsigned)(start << WL_XB_PAGE_SHIFT), (unsigned)(pages << WL_XB_PAGE_SHIFT));
                }
                g_run[start] = pages;
                g_contig_pages += pages;
                *out = start << WL_XB_PAGE_SHIFT;
                return WL_OK;
            }
            if (start < align_pages) {
                break;
            }
            start -= align_pages;
        }
    }
    // The last resort: the low pages the retail kernel's image would hold, which titles never pin
    // (they pin right above them) and nothing here uses. A request that must lie low finds the
    // rest of that range pinned by the title: Halo pins physical 0x61000..0x19A6000, and D3D's
    // handle tables then ask for a page below 16 MB (MmAllocateContiguousMemoryEx(0x1000, 0,
    // 0xFFFFFF) from 0x001810AD) when a10 loads. Page 0 stays out.
    uint32_t low_end = end_page < g_low_pages ? end_page : g_low_pages;
    uint32_t low_lo = lo_page > 1u ? lo_page : (align_pages > 1u ? align_pages : 1u);
    if (low_end >= low_lo + pages) {
        for (uint32_t start = (low_end - pages) / align_pages * align_pages; start >= low_lo; start -= align_pages) {
            bool free_run = true;
            for (uint32_t p = start; p < start + pages && free_run; p++) {
                free_run = g_kind[p] == K_RESERVED && g_run[p] == 0;
            }
            if (free_run) {
                for (uint32_t p = start; p < start + pages; p++) {
                    g_kind[p] = K_CONTIG;
                }
                g_run[start] = pages;
                g_contig_pages += pages;
                static bool logged;
                if (!logged) {
                    logged = true;
                    WL_LOG_I(TAG, "contiguous 0x%x in [0x%08x, 0x%08x]: lent the kernel's reserved low pages (0x%08x)",
                             (unsigned)bytes, (unsigned)lowest, (unsigned)highest, (unsigned)(start << WL_XB_PAGE_SHIFT));
                }
                *out = start << WL_XB_PAGE_SHIFT;
                return WL_OK;
            }
            if (start < align_pages + low_lo) {
                break;
            }
        }
    }
    return WL_ERR_NOMEM;
}

WlResult wl_xb_phys_free(uint32_t phys) {
    uint32_t p = phys >> WL_XB_PAGE_SHIFT;
    if (!g_ready || (phys & (WL_XB_PAGE - 1u)) != 0 || p >= g_xb_ram_pages || g_run[p] == 0) {
        return WL_ERR_ARG;
    }
    uint32_t pages = g_run[p];
    g_run[p] = 0;
    g_contig_pages -= pages;
    free_pages(p, pages);
    return WL_OK;
}

WlResult wl_xb_va_discard(uint32_t va, uint32_t bytes, uint32_t *held) {
    uint32_t n = 0;
    if (held != NULL) {
        *held = 0;
    }
    if (!g_ready || bytes == 0 || (uint64_t)va + bytes > 0x100000000ull) {
        return WL_ERR_RANGE;
    }
    uint32_t first = (va + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT; // whole pages only
    uint32_t end = (uint32_t)(((uint64_t)va + bytes) >> WL_XB_PAGE_SHIFT);
    for (uint32_t vp = first; vp < end; vp++) {
        uint32_t phys = 0;
        if (!wl_xb_va_to_phys(vp << WL_XB_PAGE_SHIFT, &phys)) {
            continue;
        }
        uint32_t p = phys >> WL_XB_PAGE_SHIFT;
        if (g_kind[p] == K_FREE || g_locks[p] != 0 ||
            (g_xb_pg_flags != NULL && (g_xb_pg_flags[p] & WL_XB_PG_PINNED) != 0)) {
            continue; // nobody's, or a device may be using it
        }
        if (g_xb_pg_frame != NULL && (g_xb_pg_frame[p] != 0 || g_xb_pg_slot[p] != 0)) {
            n++;
        }
        wl_xb_backend_release_phys(p, 1);
    }
    if (held != NULL) {
        *held = n;
    }
    return WL_OK;
}

uint32_t wl_xb_phys_size(uint32_t phys) {
    uint32_t p = phys >> WL_XB_PAGE_SHIFT;
    if (!g_ready || (phys & (WL_XB_PAGE - 1u)) != 0 || p >= g_xb_ram_pages) {
        return 0;
    }
    return g_run[p] << WL_XB_PAGE_SHIFT;
}

void wl_xb_phys_usage(uint32_t *used_pages, uint32_t *free_pages_out) {
    if (used_pages != NULL) {
        *used_pages = g_used;
    }
    if (free_pages_out != NULL) {
        *free_pages_out = g_xb_ram_pages - g_used;
    }
}

// ---- User virtual memory -----------------------------------------------------------------------

static bool va_in_user(uint32_t va, uint32_t bytes) {
    return va >= WL_XB_PAGE && bytes > 0 && (uint64_t)va + bytes <= WL_XB_USER_LIMIT;
}

// Index of the first region whose end lies above `va` (where one containing `va` would be).
static uint32_t region_find(uint32_t va) {
    uint32_t lo = 0, hi = g_region_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (g_regions[mid].base + g_regions[mid].size <= va) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static Region *region_containing(uint32_t va) {
    uint32_t i = region_find(va);
    if (i < g_region_count && g_regions[i].base <= va) {
        return &g_regions[i];
    }
    return NULL;
}

static bool range_free(uint32_t va, uint32_t bytes) {
    uint32_t i = region_find(va);
    return i >= g_region_count || g_regions[i].base >= va + bytes;
}

WlResult wl_xb_va_reserve(uint32_t *va, uint32_t bytes, uint32_t lowest_va, uint32_t protect) {
    if (!g_ready || va == NULL || bytes == 0) {
        return WL_ERR_ARG;
    }
    if (g_region_count == MAX_REGIONS) {
        return WL_ERR_NOMEM;
    }
    uint32_t base = *va & ~(WL_XB_PAGE - 1u);
    uint32_t size = (uint32_t)((((uint64_t)*va + bytes + WL_XB_PAGE - 1u) & ~(uint64_t)(WL_XB_PAGE - 1u)) - base);
    if (*va == 0) {
        size = (bytes + WL_XB_PAGE - 1u) & ~(WL_XB_PAGE - 1u);
        uint32_t cand = lowest_va < WL_XB_VA_GRANULE ? WL_XB_VA_GRANULE : lowest_va;
        cand = (cand + WL_XB_VA_GRANULE - 1u) & ~(WL_XB_VA_GRANULE - 1u);
        bool found = false;
        while ((uint64_t)cand + size <= WL_XB_USER_LIMIT) {
            uint32_t i = region_find(cand);
            if (i >= g_region_count || g_regions[i].base >= cand + size) {
                found = true;
                break;
            }
            // skip past the region in the way
            uint32_t next = g_regions[i].base + g_regions[i].size;
            cand = (next + WL_XB_VA_GRANULE - 1u) & ~(WL_XB_VA_GRANULE - 1u);
        }
        if (!found) {
            return WL_ERR_NOMEM;
        }
        base = cand;
    } else if (!va_in_user(base, size) || !range_free(base, size)) {
        return WL_ERR_RANGE;
    }
    uint32_t i = region_find(base);
    memmove(&g_regions[i + 1u], &g_regions[i], (g_region_count - i) * sizeof *g_regions);
    g_regions[i] = (Region){base, size, protect};
    g_region_count++;
    for (uint32_t vp = base >> WL_XB_PAGE_SHIFT; vp < (base + size) >> WL_XB_PAGE_SHIFT; vp++) {
        g_va_prot[vp] = VA_RESERVED;
    }
    *va = base;
    return WL_OK;
}

// A run of user pages [vp0, vp1) mapped through the backend in batches of consecutive physical pages.
static void map_run(uint32_t vp0, uint32_t vp1) {
    uint32_t vp = vp0;
    while (vp < vp1) {
        uint32_t first = g_xb_va2phys[vp] - 1u;
        uint32_t n = 1;
        while (vp + n < vp1 && g_xb_va2phys[vp + n] == first + n + 1u) {
            n++;
        }
        wl_xb_backend_map_user(vp << WL_XB_PAGE_SHIFT, first, n);
        vp += n;
    }
}

WlResult wl_xb_va_commit(uint32_t va, uint32_t bytes, uint32_t protect) {
    if (!g_ready || bytes == 0) {
        return WL_ERR_ARG;
    }
    uint32_t first = va >> WL_XB_PAGE_SHIFT;
    uint64_t end = ((uint64_t)va + bytes + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT;
    if (!va_in_user(va, bytes)) {
        return WL_ERR_RANGE;
    }
    for (uint32_t vp = first; vp < end; vp++) {
        if ((g_va_prot[vp] & VA_RESERVED) == 0) {
            return WL_ERR_RANGE;
        }
    }
    uint32_t pending = first; // first page committed here and not yet mapped
    WlResult r = WL_OK;
    for (uint32_t vp = first; vp < end; vp++) {
        g_va_prot[vp] = (uint16_t)(VA_RESERVED | (protect & 0x7FFFu));
        if (g_xb_va2phys[vp] != 0) {
            if (pending < vp) {
                map_run(pending, vp);
            }
            pending = vp + 1u;
            continue;
        }
        uint32_t p = 0;
        if (!alloc_page_top(K_USER, &p)) {
            r = WL_ERR_NOMEM;
            end = vp;
            break;
        }
        g_owner[p] = (uint16_t)vp;
        g_xb_va2phys[vp] = (uint16_t)(p + 1u);
        g_user_pages++;
    }
    if (pending < end) {
        map_run(pending, (uint32_t)end);
    }
    if (r != WL_OK) {
        WL_LOG_E(TAG, "commit of 0x%08x+0x%x: guest RAM exhausted", (unsigned)va, (unsigned)bytes);
    }
    return r;
}

static void decommit_pages(uint32_t vp0, uint32_t vp1) {
    memset(g_ok_pages, 0, sizeof g_ok_pages); // (page_ok's memo of committed pages)
    for (uint32_t vp = vp0; vp < vp1; vp++) {
        if (g_xb_va2phys[vp] == 0) {
            continue;
        }
        uint32_t n = 1;
        while (vp + n < vp1 && g_xb_va2phys[vp + n] != 0) {
            n++;
        }
        wl_xb_backend_unmap_user(vp << WL_XB_PAGE_SHIFT, n);
        for (uint32_t k = vp; k < vp + n; k++) {
            uint32_t p = g_xb_va2phys[k] - 1u;
            g_xb_va2phys[k] = 0;
            free_pages(p, 1);
            g_user_pages--;
        }
        vp += n - 1u;
    }
}

WlResult wl_xb_va_decommit(uint32_t va, uint32_t bytes) {
    if (!g_ready || !va_in_user(va, bytes)) {
        return WL_ERR_RANGE;
    }
    uint32_t first = va >> WL_XB_PAGE_SHIFT;
    uint32_t end = (uint32_t)(((uint64_t)va + bytes + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT);
    decommit_pages(first, end);
    for (uint32_t vp = first; vp < end; vp++) {
        g_va_prot[vp] &= VA_RESERVED;
    }
    return WL_OK;
}

WlResult wl_xb_va_release(uint32_t va, uint32_t *bytes) {
    Region *reg = g_ready ? region_containing(va) : NULL;
    if (reg == NULL || reg->base != va) {
        return WL_ERR_RANGE;
    }
    uint32_t size = reg->size;
    decommit_pages(va >> WL_XB_PAGE_SHIFT, (va + size) >> WL_XB_PAGE_SHIFT);
    for (uint32_t vp = va >> WL_XB_PAGE_SHIFT; vp < (va + size) >> WL_XB_PAGE_SHIFT; vp++) {
        g_va_prot[vp] = 0;
    }
    uint32_t i = (uint32_t)(reg - g_regions);
    memmove(&g_regions[i], &g_regions[i + 1u], (g_region_count - i - 1u) * sizeof *g_regions);
    g_region_count--;
    if (bytes != NULL) {
        *bytes = size;
    }
    return WL_OK;
}

WlResult wl_xb_va_alloc(uint32_t *va, uint32_t bytes, uint32_t lowest_va, uint32_t protect) {
    WlResult r = wl_xb_va_reserve(va, bytes, lowest_va, protect);
    if (r != WL_OK) {
        return r;
    }
    r = wl_xb_va_commit(*va, bytes, protect);
    if (r != WL_OK) {
        wl_xb_va_release(*va, NULL);
    }
    return r;
}

WlResult wl_xb_va_query(uint32_t va, WlXbVaInfo *out) {
    if (!g_ready || out == NULL || va >= WL_XB_USER_LIMIT) {
        return WL_ERR_RANGE;
    }
    memset(out, 0, sizeof *out);
    uint32_t vp = va >> WL_XB_PAGE_SHIFT;
    Region *reg = region_containing(va);
    if (reg == NULL) {
        uint32_t i = region_find(va);
        uint32_t end = i < g_region_count ? g_regions[i].base : WL_XB_USER_LIMIT;
        out->base = va & ~(WL_XB_PAGE - 1u);
        out->size = end - out->base;
        out->state = 0x10000u; // MEM_FREE
        out->protect = WL_XB_PROT_NOACCESS;
        return WL_OK;
    }
    bool committed = g_xb_va2phys[vp] != 0;
    uint16_t prot = g_va_prot[vp];
    uint32_t last = vp;
    uint32_t reg_end = (reg->base + reg->size) >> WL_XB_PAGE_SHIFT;
    while (last + 1u < reg_end && (g_xb_va2phys[last + 1u] != 0) == committed && g_va_prot[last + 1u] == prot) {
        last++;
    }
    out->base = va & ~(WL_XB_PAGE - 1u); // NT reports from the page asked about to the run's end
    out->size = ((last + 1u) << WL_XB_PAGE_SHIFT) - out->base;
    out->allocation_base = reg->base;
    out->allocation_protect = reg->protect;
    out->state = committed ? 0x1000u : 0x2000u;
    out->protect = committed ? (uint32_t)(prot & 0x7FFFu) : 0;
    return WL_OK;
}

WlResult wl_xb_va_protect(uint32_t va, uint32_t bytes, uint32_t protect, uint32_t *old_protect) {
    if (!g_ready) {
        return WL_ERR_STATE;
    }
    if (va >= WL_XB_WINDOW) {
        if (old_protect != NULL) {
            *old_protect = WL_XB_PROT_READWRITE; // the window: always read-write
        }
        return WL_OK;
    }
    if (!va_in_user(va, bytes)) {
        return WL_ERR_RANGE;
    }
    uint32_t first = va >> WL_XB_PAGE_SHIFT;
    uint32_t end = (uint32_t)(((uint64_t)va + bytes + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT);
    for (uint32_t vp = first; vp < end; vp++) {
        if (g_xb_va2phys[vp] == 0) {
            return WL_ERR_RANGE;
        }
    }
    if (old_protect != NULL) {
        *old_protect = g_va_prot[first] & 0x7FFFu;
    }
    for (uint32_t vp = first; vp < end; vp++) {
        g_va_prot[vp] = (uint16_t)(VA_RESERVED | (protect & 0x7FFFu));
    }
    return WL_OK;
}

bool wl_xb_va_to_phys(uint32_t va, uint32_t *phys) {
    uint32_t p = 0;
    bool ok = false;
    if (g_ready && va < WL_XB_USER_LIMIT) {
        uint32_t entry = g_xb_va2phys[va >> WL_XB_PAGE_SHIFT];
        if (entry != 0) {
            ok = true;
            p = ((entry - 1u) << WL_XB_PAGE_SHIFT) | (va & (WL_XB_PAGE - 1u));
        }
    } else if (g_ready && ((va >> 28) == 0x8u || (va >> 28) == 0xFu) && va < WL_XB_MMIO_BASE) {
        p = va & 0x0FFFFFFFu; // the window (or its tiled alias): the offset is the physical address
        ok = p < (g_xb_ram_pages << WL_XB_PAGE_SHIFT);
    }
    if (phys != NULL) {
        *phys = ok ? p : 0;
    }
    return ok;
}

// ---- Accessor helpers --------------------------------------------------------------------------

static bool page_ok(uint32_t va) {
    if (va < WL_XB_USER_LIMIT) {
        uint32_t vp = va >> WL_XB_PAGE_SHIFT;
        uint32_t *memo = &g_ok_pages[vp & (OK_PAGES - 1u)];
        if (*memo == vp + 1u) {
            return true;
        }
        if (g_xb_va2phys == NULL || g_xb_va2phys[vp] == 0) {
            return false;
        }
        *memo = vp + 1u;
        return true;
    }
    uint32_t top = va >> 28;
    if ((top == 0x8u || top == 0xFu) && va < WL_XB_MMIO_BASE) {
        return (va & 0x0FFFFFFFu) < (g_xb_ram_pages << WL_XB_PAGE_SHIFT);
    }
    return false;
}

// The common cases first: a range inside one user page the memo holds (only pages page_ok found
// committed are there), and a range inside the RAM window. The kernel checks every argument this
// way and a title's own code every field it reads (2.4% of Halo's airlock fight on the Wii before
// the first, 3.5% of d40 before the second); without a frame of its own, the rest is a tail call to
// the whole check.
bool wl_xb_range_ok(uint32_t va, uint32_t n) {
    uint32_t vp = va >> WL_XB_PAGE_SHIFT;
    // n - 1 below the bytes left in va's page: n is not 0, and the range neither wraps nor crosses a page
    if (va < WL_XB_USER_LIMIT && n - 1u < WL_XB_PAGE - (va & (WL_XB_PAGE - 1u)) &&
        g_ok_pages[vp & (OK_PAGES - 1u)] == vp + 1u) {
        return true;
    }
    // va's offset in the window below its RAM's bytes (so va is 0x80000000 up, and guest memory is
    // ready), and n - 1 below the bytes left (n is not 0, and the range ends in that RAM): what
    // page_ok answers for each of its pages
    uint32_t off = va - 0x80000000u;
    if (off < g_window_bytes && n - 1u < g_window_bytes - off) {
        return true;
    }
    return wl_xb_range_ok_slow(va, n);
}

__attribute__((noinline)) bool wl_xb_range_ok_slow(uint32_t va, uint32_t n) {
    if (!g_ready) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    if ((uint64_t)va + n > 0x100000000ull) {
        return false;
    }
    uint32_t last = va + n - 1u;
    // the two halves of a range must not straddle a hole in the EA space
    if ((va < WL_XB_USER_LIMIT) != (last < WL_XB_USER_LIMIT)) {
        return false;
    }
    for (uint32_t p = va & ~(WL_XB_PAGE - 1u);; p += WL_XB_PAGE) {
        if (!page_ok(p)) {
            return false;
        }
        if (p >= (last & ~(WL_XB_PAGE - 1u))) {
            break;
        }
    }
    return true;
}

static uint32_t phys_page_of(uint32_t va);

void wl_xb_copy_in(uint32_t va, const void *src, uint32_t n) {
    if (n == 0) {
        return;
    }
    if (g_ready && g_xb_pg_frames != 0) {
        // the whole pages it covers are overwritten: a swapped one is not read back first (a file
        // read into a buffer that was swapped out)
        uint64_t end = ((uint64_t)va + n) & ~(uint64_t)(WL_XB_PAGE - 1u);
        for (uint64_t a = ((uint64_t)va + WL_XB_PAGE - 1u) & ~(uint64_t)(WL_XB_PAGE - 1u); a < end; a += WL_XB_PAGE) {
            uint32_t p1 = phys_page_of((uint32_t)a);
            if (p1 != 0) {
                wl_xb_pg_discard(p1 - 1u, 1);
            }
        }
    }
    memcpy(wl_xb_ptr(va), src, n);
}

void wl_xb_file_pages(uint32_t va, uint32_t n, struct WlPlatformFile *file, uint64_t offset) {
    if (!g_ready || g_xb_pg_frames == 0 || n < WL_XB_PAGE) {
        return;
    }
    uint32_t source = wl_xb_pg_source(file);
    if (source == 0) {
        return;
    }
    uint64_t end = ((uint64_t)va + n) & ~(uint64_t)(WL_XB_PAGE - 1u);
    for (uint64_t a = ((uint64_t)va + WL_XB_PAGE - 1u) & ~(uint64_t)(WL_XB_PAGE - 1u); a < end; a += WL_XB_PAGE) {
        uint64_t at = offset + (a - va);
        uint32_t p1 = phys_page_of((uint32_t)a);
        if (at + WL_XB_PAGE > 0x100000000ull) {
            break;
        }
        if (p1 != 0) {
            wl_xb_pg_filed(p1 - 1u, source, (uint32_t)at);
        }
    }
}

WlResult wl_xb_file_forget(struct WlPlatformFile *file, uint64_t offset, uint64_t bytes) {
    return wl_xb_pg_forget(file, offset, bytes);
}

void wl_xb_copy_out(void *dst, uint32_t va, uint32_t n) {
    if (n != 0) {
        memcpy(dst, wl_xb_ptr(va), n);
    }
}

WlResult wl_xb_file_write(struct WlPlatformFile *file, const void *data, uint32_t bytes) {
    const uintptr_t lo = WL_XB_HOST_BASE, at = (uintptr_t)data;
    if (at < lo || at - lo >= WL_XB_EA_SPAN || g_xb_pg_frames == 0) {
        return wl_platform_fs_write(file, data, bytes); // host memory, or guest memory never paged
    }
    static uint8_t bounce[2048]; // (the guest lock's holder: one writer)
    const uint8_t *src = data;
    while (bytes > 0) {
        uint32_t n = bytes < sizeof bounce ? bytes : (uint32_t)sizeof bounce;
        memcpy(bounce, src, n); // a page without a frame faults in here
        WlResult r = wl_platform_fs_write(file, bounce, n);
        if (r != WL_OK) {
            return r;
        }
        src += n;
        bytes -= n;
    }
    return WL_OK;
}

void wl_xb_fill(uint32_t va, uint8_t byte, uint32_t n) {
    if (n != 0) {
        memset(wl_xb_ptr(va), byte, n);
    }
}

uint32_t wl_xb_read_cstr(uint32_t va, char *dst, uint32_t cap) {
    uint32_t n = 0;
    if (cap == 0) {
        return 0;
    }
    while (n + 1u < cap) {
        uint32_t a = va + n;
        if ((n == 0 || (a & (WL_XB_PAGE - 1u)) == 0) && !page_ok(a)) {
            break;
        }
        char c = (char)wl_xb_rd8(a);
        if (c == '\0') {
            break;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
    return n;
}

void wl_xb_mem_stats(WlXbMemStats *out) {
    memset(out, 0, sizeof *out);
    out->ram_pages = g_xb_ram_pages;
    out->used_pages = g_used;
    out->user_pages = g_user_pages;
    out->contig_pages = g_contig_pages;
    out->relocations = g_relocations;
    if (g_ready) {
        wl_xb_backend_stats(&out->frames, &out->faults_resolved, &out->pte_evictions);
        wl_xb_pg_stats(out);
        if (out->frames_total != 0) {
            out->frames = out->frames_total - out->frames_free;
        }
    }
}

// ---- Pinned pages ------------------------------------------------------------------------------

// The physical page of a VA (+ 1), 0 when it is not guest RAM.
static uint32_t phys_page_of(uint32_t va) {
    uint32_t phys = 0;
    return wl_xb_va_to_phys(va, &phys) ? (phys >> WL_XB_PAGE_SHIFT) + 1u : 0u;
}

static WlResult pin_range(uint32_t va, uint32_t bytes, bool pin) {
    if (!g_ready) {
        return WL_ERR_STATE;
    }
    if (bytes == 0) {
        return WL_OK;
    }
    uint32_t first = va & ~(WL_XB_PAGE - 1u);
    uint64_t end = (uint64_t)va + bytes;
    for (uint64_t a = first; a < end; a += WL_XB_PAGE) {
        if (phys_page_of((uint32_t)a) == 0) {
            return WL_ERR_RANGE;
        }
    }
    for (uint64_t a = first; a < end; a += WL_XB_PAGE) {
        WlResult r = wl_xb_pg_pin(phys_page_of((uint32_t)a) - 1u, pin);
        if (r != WL_OK) {
            return r;
        }
    }
    return WL_OK;
}

WlResult wl_xb_mem_pin(uint32_t va, uint32_t bytes) {
    return pin_range(va, bytes, true);
}

void wl_xb_mem_unpin(uint32_t va, uint32_t bytes) {
    (void)pin_range(va, bytes, false);
}

bool wl_xb_mem_resident(uint32_t va) {
    uint32_t p1 = g_ready ? phys_page_of(va) : 0u;
    if (p1 == 0) {
        return false;
    }
    return g_xb_pg_frames == 0 || g_xb_pg_frame[p1 - 1u] != 0; // without paging the OS backs every page
}

bool wl_xb_mem_peek(uint32_t va, void *dst, uint32_t bytes) {
    uint32_t p1 = g_ready ? phys_page_of(va) : 0u;
    uint32_t off = va & (WL_XB_PAGE - 1u);
    if (p1 == 0 || bytes == 0 || bytes > WL_XB_PAGE - off) {
        return false;
    }
    if (g_xb_pg_frames == 0) {
        memcpy(dst, wl_xb_ptr(va), bytes); // without paging the OS backs every page
        return true;
    }
    // the frame as the tables say under the MMU lock: none, or in swap I/O (unmapped), is no read
    uint32_t p = p1 - 1u;
    bool ok = false;
    uint32_t level = wl_xb_mmu_lock();
    uint32_t f = g_xb_pg_frame[p];
    if (f != 0 && (g_xb_pg_flags[p] & WL_XB_PG_IO) == 0) {
        memcpy(dst, wl_xb_mmu_frame_ptr(p, f) + off, bytes);
        ok = true;
    }
    wl_xb_mmu_unlock(level);
    return ok;
}

// ---- Write watch -------------------------------------------------------------------------------

// The pages of guest-physical [phys, phys + bytes): false when the range is empty or not RAM.
static bool phys_pages(uint32_t phys, uint32_t bytes, uint32_t *first, uint32_t *pages) {
    uint64_t end = (uint64_t)phys + bytes;
    if (!g_ready || bytes == 0 || end > (uint64_t)g_xb_ram_pages << WL_XB_PAGE_SHIFT) {
        return false;
    }
    *first = phys >> WL_XB_PAGE_SHIFT;
    *pages = (uint32_t)((end - 1u) >> WL_XB_PAGE_SHIFT) - *first + 1u;
    return true;
}

uint32_t wl_xb_phys_watch(uint32_t phys, uint32_t bytes) {
    uint32_t first, pages;
    return phys_pages(phys, bytes, &first, &pages) ? wl_xb_pg_watch(first, pages) : 0u;
}

bool wl_xb_phys_written(uint32_t phys, uint32_t bytes, uint32_t stamp) {
    uint32_t first, pages;
    return !phys_pages(phys, bytes, &first, &pages) || wl_xb_pg_written(first, pages, stamp);
}

void wl_xb_mem_counters_log(void) {
    static int counters = -1;
    if (counters < 0) {
        const char *env = getenv("WL_XB_PAGING_LOG");
        counters = env != NULL && env[0] != '\0' && env[0] != '0';
    }
    if (counters <= 0 || !g_ready || g_xb_pg_frames == 0) {
        return;
    }
    WlXbMemStats st;
    memset(&st, 0, sizeof st);
    wl_xb_pg_stats(&st);
    uint32_t dz, cz;
    wl_xb_pg_zero_census(&dz, &cz);
    WL_LOG_I(TAG, "paging counters: ins %u swap %u file (+%u ahead); outs %u written %u zero %u clean (%u file, %u zero) "
             "in %u fast; refaults %u swap %u file %u zero; returns %u; KB read %u written %u; ahead unused %u; "
             "resident %u dirty %u file; zero %u dirty %u clean; cache %u frames: put %u raw %u hits %u, holds %u (%u KB)",
             (unsigned)st.swap_ins, (unsigned)st.file_ins, (unsigned)st.file_ahead, (unsigned)st.swap_outs,
             (unsigned)st.zero_outs, (unsigned)st.clean_evictions, (unsigned)st.file_evictions,
             (unsigned)st.zero_evictions, (unsigned)st.fast_evictions, (unsigned)st.refaults_swap,
             (unsigned)st.refaults_file, (unsigned)st.refaults_zero, (unsigned)st.returns,
             (unsigned)(st.swap_read_bytes >> 10), (unsigned)(st.swap_write_bytes >> 10), (unsigned)st.ahead_unused,
             (unsigned)st.resident_dirty, (unsigned)st.resident_file, (unsigned)dz, (unsigned)cz,
             (unsigned)st.zcache_frames, (unsigned)st.zcache_puts, (unsigned)st.zcache_raw, (unsigned)st.zcache_hits,
             (unsigned)st.zcache_pages, (unsigned)(st.zcache_bytes >> 10));
}

void wl_xb_mem_watch(uint32_t swap_ins_per_s) {
    static uint64_t last_us, last_log_us;
    static uint32_t last_ins, last_outs, last_evictions;
    if (!g_ready || g_xb_pg_frames == 0) {
        return;
    }
    WlXbMemStats st;
    memset(&st, 0, sizeof st);
    wl_xb_pg_stats(&st);
    uint64_t now = wl_platform_time_us();
    if (last_us == 0) {
        last_us = now;
        last_ins = st.swap_ins;
        last_outs = st.swap_outs;
        last_evictions = st.evictions;
        return;
    }
    uint64_t span = now - last_us;
    if (span < 500000u) {
        return;
    }
    uint32_t ins = st.swap_ins - last_ins, outs = st.swap_outs - last_outs, ev = st.evictions - last_evictions;
    last_us = now;
    last_ins = st.swap_ins;
    last_outs = st.swap_outs;
    last_evictions = st.evictions;
    uint32_t limit = swap_ins_per_s != 0 ? swap_ins_per_s : 128u;
    uint32_t ins_s = (uint32_t)((uint64_t)ins * 1000000u / span);
    if (ins_s > limit && (last_log_us == 0 || now - last_log_us >= 10000000u)) {
        last_log_us = now;
        uint32_t outs_s = (uint32_t)((uint64_t)outs * 1000000u / span);
        uint32_t ev_s = (uint32_t)((uint64_t)ev * 1000000u / span);
        WL_LOG_W(TAG, "thrashing: %u swap-ins/s (%u KB/s read), %u swap-outs/s, %u evictions/s; %u of %u frames free, "
                 "%u pinned; swap I/O %u ms in all",
                 (unsigned)ins_s, (unsigned)(ins_s * 4u), (unsigned)outs_s, (unsigned)ev_s, (unsigned)st.frames_free,
                 (unsigned)st.frames_total, (unsigned)st.pinned_pages, (unsigned)(st.swap_us / 1000u));
    }
}
