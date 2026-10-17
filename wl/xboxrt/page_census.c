// SPDX-License-Identifier: MIT
// The page census (page_census.h).
#include "wl/xboxrt/page_census.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/thread.h"

#define TAG "xbcensus"
#define VBLANKS 60u     // a second of guest time
#define DUMP_EVERY 30u  // seconds between dumps of the pages
#define WINDOWS 3u
static const uint32_t k_window[WINDOWS] = {5, 10, 30}; // the working sets over the last 5, 10, 30 s

// the classes a second's line counts pages by (a page's class: the first that fits)
enum { C_CODE, C_TEX, C_VTX, C_APU, C_GPU, C_XDATA, C_STACK, C_KERNEL, C_CMAP, C_CONTIG, C_VMAP, C_VIRT, C_OTHER, C_COUNT };
static const char *const k_class[C_COUNT] = {"code", "tex", "vtx", "apu", "gpu", "xdata", "stack", "kernel",
                                             "cmap", "contig", "vmap", "virt", "other"};
static const char *const k_alloc[WL_XB_CA_COUNT] = {"none", "xbe_code", "xbe_data", "contig", "system",
                                                    "virtual", "pool", "stack"};

typedef struct {
    uint8_t kind;
    uint32_t va, bytes, caller, second;
} Alloc;

uint8_t *g_xb_census_use;
static uint8_t *g_now;      // [ram pages] touched in this second
static uint16_t *g_first;   // [ram pages] the first second it was touched in (+1; 0: never)
static uint16_t *g_last;
static uint16_t *g_seconds; // how many
static uint32_t *g_alloc_of; // [ram pages] its allocation (+1; 0: none recorded)
static Alloc *g_allocs;
static uint32_t g_alloc_n, g_alloc_cap;
static uint32_t g_pages, g_second, g_vblanks;
static FILE *g_out;
static char g_path[512];

bool wl_xb_census_on(void) {
    return g_xb_census_use != NULL;
}

void wl_xb_census_seen_slow(uint32_t p, bool write) {
    if (p >= g_pages) {
        return;
    }
    g_now[p] = 1;
    if (write) {
        g_xb_census_use[p] |= WL_XB_CU_WRITE;
    }
}

void wl_xb_census_phys_slow(uint32_t phys, uint32_t bytes, uint8_t use) {
    if (bytes == 0) {
        return;
    }
    uint64_t end = ((uint64_t)phys + bytes + WL_XB_PAGE - 1u) >> WL_XB_PAGE_SHIFT;
    for (uint64_t p = phys >> WL_XB_PAGE_SHIFT; p < end && p < g_pages; p++) {
        g_xb_census_use[p] |= use;
    }
}

void wl_xb_census_va_slow(uint32_t va, uint32_t bytes, uint8_t use) {
    uint64_t end = (uint64_t)va + bytes;
    for (uint64_t a = va & ~(uint64_t)(WL_XB_PAGE - 1u); a < end; a += WL_XB_PAGE) {
        uint32_t phys = 0;
        if (wl_xb_va_to_phys((uint32_t)a, &phys)) {
            wl_xb_census_phys_slow(phys, 1, use);
        }
    }
}

void wl_xb_census_alloc_slow(WlXbCensusAlloc kind, uint32_t va, uint32_t bytes, uint32_t caller) {
    if (bytes == 0) {
        return;
    }
    if (g_alloc_n == g_alloc_cap) {
        uint32_t cap = g_alloc_cap != 0 ? g_alloc_cap * 2u : 256u;
        Alloc *a = realloc(g_allocs, cap * sizeof *a);
        if (a == NULL) {
            return;
        }
        g_allocs = a;
        g_alloc_cap = cap;
    }
    g_allocs[g_alloc_n] = (Alloc){(uint8_t)kind, va, bytes, caller, g_second};
    g_alloc_n++;
    uint64_t end = (uint64_t)va + bytes;
    for (uint64_t a = va & ~(uint64_t)(WL_XB_PAGE - 1u); a < end; a += WL_XB_PAGE) {
        uint32_t phys = 0;
        if (wl_xb_va_to_phys((uint32_t)a, &phys) && (phys >> WL_XB_PAGE_SHIFT) < g_pages) {
            g_alloc_of[phys >> WL_XB_PAGE_SHIFT] = g_alloc_n;
        }
    }
}

static uint8_t kind_of(uint32_t p) {
    return g_alloc_of[p] != 0 ? g_allocs[g_alloc_of[p] - 1u].kind : (uint8_t)WL_XB_CA_NONE;
}

static int class_of(uint32_t p) {
    uint8_t u = g_xb_census_use[p];
    uint8_t k = kind_of(p);
    if (k == WL_XB_CA_XBE_CODE) {
        return C_CODE;
    }
    if (u & WL_XB_CU_TEX) {
        return C_TEX;
    }
    if (u & WL_XB_CU_VTX) {
        return C_VTX;
    }
    if (u & WL_XB_CU_APU) {
        return C_APU;
    }
    if (u & WL_XB_CU_GPU) {
        return C_GPU;
    }
    switch (k) {
    case WL_XB_CA_XBE_DATA:
        return C_XDATA;
    case WL_XB_CA_STACK:
        return C_STACK;
    case WL_XB_CA_POOL:
    case WL_XB_CA_SYSTEM:
        return C_KERNEL;
    case WL_XB_CA_CONTIG:
        return (u & WL_XB_CU_MAP) ? C_CMAP : C_CONTIG;
    case WL_XB_CA_VIRTUAL:
        return (u & WL_XB_CU_MAP) ? C_VMAP : C_VIRT;
    default:
        return C_OTHER;
    }
}

uint32_t wl_xb_census_interval(void) {
    if (g_xb_census_use == NULL) {
        return 0;
    }
    g_second++;
    uint32_t n = 0, fresh = 0, resident = 0, by[C_COUNT] = {0}, win[WINDOWS] = {0};
    for (uint32_t p = 0; p < g_pages; p++) {
        uint8_t fl = g_xb_pg_flags[p];
        if (g_xb_pg_frame[p] != 0) {
            resident++;
        }
        // a pinned page is open for good: counted in every second it has its frame
        bool pinned = (fl & WL_XB_PG_PINNED) != 0 && g_xb_pg_frame[p] != 0;
        if (g_now[p] || pinned) {
            g_now[p] = 0;
            n++;
            by[class_of(p)]++;
            if (g_first[p] == 0) {
                g_first[p] = (uint16_t)g_second;
                fresh++;
            }
            g_last[p] = (uint16_t)g_second;
            g_seconds[p]++;
            if (!pinned && g_xb_pg_frame[p] != 0 && (fl & WL_XB_PG_IO) == 0) {
                wl_xb_mmu_census_close(p); // its next touch faults again
            }
        }
        if (g_last[p] != 0) {
            uint32_t age = g_second - g_last[p]; // (seconds since its last touch)
            for (uint32_t w = 0; w < WINDOWS; w++) {
                win[w] += age < k_window[w];
            }
        }
    }
    if (g_out != NULL) {
        fprintf(g_out, "second %u ws %u new %u resident %u", (unsigned)g_second, (unsigned)n, (unsigned)fresh,
                (unsigned)resident);
        for (uint32_t w = 0; w < WINDOWS; w++) {
            fprintf(g_out, " ws%u %u", (unsigned)k_window[w], (unsigned)win[w]);
        }
        for (int c = 0; c < C_COUNT; c++) {
            fprintf(g_out, " %s %u", k_class[c], (unsigned)by[c]);
        }
        fputc('\n', g_out);
        fflush(g_out);
    }
    if (g_second % DUMP_EVERY == 0) {
        wl_xb_census_dump();
    }
    return n;
}

void wl_xb_census_dump(void) {
    if (g_xb_census_use == NULL || g_path[0] == '\0') {
        return;
    }
    char path[sizeof g_path + 8];
    snprintf(path, sizeof path, "%s.pages", g_path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "census seconds %u pages %u\n", (unsigned)g_second, (unsigned)g_pages);
    for (uint32_t i = 0; i < g_alloc_n; i++) {
        const Alloc *a = &g_allocs[i];
        fprintf(f, "alloc %u %s %08x %x %08x %u\n", (unsigned)(i + 1u), k_alloc[a->kind], (unsigned)a->va,
                (unsigned)a->bytes, (unsigned)a->caller, (unsigned)a->second);
    }
    // page, allocation, use bits, first and last second, seconds, has a frame now
    for (uint32_t p = 0; p < g_pages; p++) {
        if (g_alloc_of[p] == 0 && g_xb_census_use[p] == 0 && g_first[p] == 0) {
            continue;
        }
        fprintf(f, "page %x %u %x %u %u %u %u\n", (unsigned)p, (unsigned)g_alloc_of[p], (unsigned)g_xb_census_use[p],
                (unsigned)g_first[p], (unsigned)g_last[p], (unsigned)g_seconds[p], g_xb_pg_frame[p] != 0 ? 1u : 0u);
    }
    fclose(f);
}

void wl_xb_census_page(uint32_t p, uint8_t *use, uint32_t *first, uint32_t *last, uint32_t *seconds,
                       WlXbCensusAlloc *kind) {
    bool ok = g_xb_census_use != NULL && p < g_pages;
    *use = ok ? g_xb_census_use[p] : 0u;
    *first = ok ? g_first[p] : 0u;
    *last = ok ? g_last[p] : 0u;
    *seconds = ok ? g_seconds[p] : 0u;
    *kind = ok ? (WlXbCensusAlloc)kind_of(p) : WL_XB_CA_NONE;
}

static void census_vblank(void *user) {
    (void)user;
    if (++g_vblanks % VBLANKS == 0) {
        wl_xb_census_interval();
    }
}

static void census_exit(void) {
    wl_xb_census_dump();
}

WlResult wl_xb_census_start(const char *path) {
    if (g_xb_census_use != NULL) {
        return WL_ERR_STATE;
    }
    if (!wl_xb_mem_ready() || g_xb_pg_frames == 0) {
        WL_LOG_W(TAG, "the census needs guest memory under a frame limit: off");
        return WL_ERR_STATE;
    }
    uint32_t pages = g_xb_ram_pages;
    g_now = calloc(pages, 1);
    g_first = calloc(pages, sizeof *g_first);
    g_last = calloc(pages, sizeof *g_last);
    g_seconds = calloc(pages, sizeof *g_seconds);
    g_alloc_of = calloc(pages, sizeof *g_alloc_of);
    uint8_t *use = calloc(pages, 1);
    if (g_now == NULL || g_first == NULL || g_last == NULL || g_seconds == NULL || g_alloc_of == NULL || use == NULL) {
        return WL_ERR_NOMEM;
    }
    g_pages = pages;
    if (path != NULL && path[0] != '\0') {
        snprintf(g_path, sizeof g_path, "%s", path);
        g_out = fopen(path, "w");
        if (g_out == NULL) {
            WL_LOG_W(TAG, "cannot write %s", path);
        }
        atexit(census_exit);
    }
    (void)wl_xb_service_add_vblank_hook(census_vblank, NULL);
    g_xb_census_use = use; // (last: the hooks run from here on)
    WL_LOG_I(TAG, "the page census writes %s (a line a second, the pages to %s.pages)", path, path);
    return WL_OK;
}
