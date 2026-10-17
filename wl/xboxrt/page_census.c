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

#if defined(__GLIBC__) && defined(__x86_64__)
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#define CENSUS_STACKS 1
#else
#define CENSUS_STACKS 0
#endif

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

// The card's traffic in the current second (page_census.h): by kind, its ops, pages, pages a map read
// filled, pages still fresh from a file read (no device use since), and pages by class
static uint8_t *g_fresh; // [ram pages] filled by a file read, no GPU/APU use since
#define IO_ALLOCS 8u
typedef struct {
    uint32_t ops, pages, map, fresh, by[C_COUNT];
    uint32_t al_id[IO_ALLOCS], al_pages[IO_ALLOCS], al_fresh[IO_ALLOCS]; // by allocation (+1; 0: free)
    uint32_t al_rest;                                                    // pages of the allocations past those
} IoCount;
static IoCount g_io[WL_XB_CIO_COUNT];
static uint32_t g_card_ops[WL_XB_CCARD_COUNT][2];   // [kind][map]
static uint32_t g_card_bytes[WL_XB_CCARD_COUNT][2]; // (32 bits: no 64-bit atomics on 32-bit PowerPC; a second's)
static const char *const k_io[WL_XB_CIO_COUNT] = {"swap_in", "swap_out", "file_in", "ahead", "zhit",
                                                  "evict", "ahead_unused", "forget"};
static const char *const k_card[WL_XB_CCARD_COUNT] = {"read", "read_held", "ahead", "write"};

// ---- Who touched (WL_XB_PAGE_CENSUS_STACKS=1, x86-64 glibc hosts) ----------------------------------
// (not under qemu-ppc: a backtrace from its fault handler crashes)
// Each touch the census sees (a page's first in a second: a fault) has its host call stack taken;
// the distinct stacks go to `<file>.stacks` ("stack <id> <frames...>", after a line "anchor <address
// of wl_xb_census_start>" that gives the program's load bias) and the touches to `<file>.touch`, three
// 32-bit words each in the host's order: the census second, the page, the stack's id (0: no room).
// tools/xb_page_census.py --who symbolizes them (guest functions, our texture cache, the kernel...).
#if CENSUS_STACKS
#define ST_DEPTH 16u
#define ST_SLOTS 32768u
typedef struct {
    uint32_t hash, n;
    void *pc[ST_DEPTH];
} Stack;
static Stack *g_stacks; // [ST_SLOTS] open addressing; n == 0: free
static int g_touch_fd = -1, g_stack_fd = -1;
static uint32_t g_touch_buf[3u * 4096u], g_touch_n;
static uint32_t g_st_lock;
static uint32_t g_st_ids;

static void st_flush(void) {
    if (g_touch_n != 0 && g_touch_fd >= 0) {
        ssize_t w = write(g_touch_fd, g_touch_buf, g_touch_n * sizeof g_touch_buf[0]);
        (void)w;
    }
    g_touch_n = 0;
}

// the stack's id (1..; 0: the table is full), written out the first time it is seen
static uint32_t st_intern(void *const *pc, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ (uint32_t)((uintptr_t)pc[i] >> 2)) * 16777619u;
    }
    for (uint32_t k = 0; k < ST_SLOTS; k++) {
        Stack *s = &g_stacks[(h + k) & (ST_SLOTS - 1u)];
        if (s->n == 0) {
            if (g_st_ids + 1u >= ST_SLOTS * 3u / 4u) {
                return 0;
            }
            s->hash = h;
            s->n = n;
            memcpy(s->pc, pc, n * sizeof pc[0]);
            uint32_t id = (uint32_t)(s - g_stacks) + 1u;
            g_st_ids++;
            char line[32 + ST_DEPTH * 20u];
            int at = snprintf(line, sizeof line, "stack %u", (unsigned)id);
            for (uint32_t i = 0; i < n && at < (int)sizeof line - 24; i++) {
                at += snprintf(line + at, sizeof line - (size_t)at, " %lx", (unsigned long)(uintptr_t)pc[i]);
            }
            line[at++] = '\n';
            ssize_t w = write(g_stack_fd, line, (size_t)at);
            (void)w;
            return id;
        }
        if (s->hash == h && s->n == n && memcmp(s->pc, pc, n * sizeof pc[0]) == 0) {
            return (uint32_t)(s - g_stacks) + 1u;
        }
    }
    return 0;
}

// (`fill`: no stack; the page was filled by a file read, written as the id ST_FILL)
#define ST_FILL 0xFFFFFFFFu
static void st_record(uint32_t p, bool fill) {
    void *pc[ST_DEPTH];
    int n = fill ? 0 : backtrace(pc, (int)ST_DEPTH);
    // (the census's own frames and the fault handler's are left in: the tool knows their names)
    while (__atomic_exchange_n(&g_st_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
    uint32_t id = fill ? ST_FILL : n > 0 ? st_intern(pc, (uint32_t)n) : 0u;
    if (g_touch_n + 3u > sizeof g_touch_buf / sizeof g_touch_buf[0]) {
        st_flush();
    }
    g_touch_buf[g_touch_n++] = g_second + 1u; // (the second the touch is counted in)
    g_touch_buf[g_touch_n++] = p;
    g_touch_buf[g_touch_n++] = id;
    __atomic_store_n(&g_st_lock, 0u, __ATOMIC_RELEASE);
}

static void st_start(const char *path) {
    const char *on = getenv("WL_XB_PAGE_CENSUS_STACKS");
    if (on == NULL || on[0] == '\0' || on[0] == '0') {
        return;
    }
    char name[sizeof g_path + 8];
    Stack *stacks = calloc(ST_SLOTS, sizeof *stacks);
    snprintf(name, sizeof name, "%s.stacks", path);
    g_stack_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    snprintf(name, sizeof name, "%s.touch", path);
    g_touch_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (stacks == NULL || g_stack_fd < 0 || g_touch_fd < 0) {
        WL_LOG_W(TAG, "the census's stacks: cannot write %s", name);
        free(stacks);
        return;
    }
    void *prime[2];
    (void)backtrace(prime, 2); // (its first call loads the unwinder: not in a fault handler)
    char line[64];
    int at = snprintf(line, sizeof line, "anchor %lx\n", (unsigned long)(uintptr_t)&wl_xb_census_start);
    ssize_t w = write(g_stack_fd, line, (size_t)at);
    (void)w;
    g_stacks = stacks;
    WL_LOG_I(TAG, "the census takes each touch's host stack (%s.stacks, %s.touch)", path, path);
}
#endif

bool wl_xb_census_on(void) {
    return g_xb_census_use != NULL;
}

void wl_xb_census_seen_slow(uint32_t p, bool write) {
    if (p >= g_pages) {
        return;
    }
#if CENSUS_STACKS
    if (g_stacks != NULL && g_now[p] == 0) {
        st_record(p, false);
    }
#endif
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
        if (use & (WL_XB_CU_MAP | WL_XB_CU_FILE)) {
            g_fresh[p] = 1;
#if CENSUS_STACKS
            if (g_stacks != NULL) {
                st_record((uint32_t)p, true);
            }
#endif
        } else if (use & (WL_XB_CU_GPU | WL_XB_CU_TEX | WL_XB_CU_VTX | WL_XB_CU_APU)) {
            g_fresh[p] = 0;
        }
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

void wl_xb_census_io_slow(WlXbCensusIo kind, uint32_t p, bool op) {
    if ((unsigned)kind >= WL_XB_CIO_COUNT || p >= g_pages) {
        return;
    }
    IoCount *c = &g_io[kind];
    c->ops += op;
    c->pages++;
    c->map += (g_xb_census_use[p] & WL_XB_CU_MAP) != 0;
    c->fresh += g_fresh[p];
    c->by[class_of(p)]++;
    uint32_t id = g_alloc_of[p] + 1u, i = 0; // (1: no allocation)
    while (i < IO_ALLOCS && c->al_id[i] != 0 && c->al_id[i] != id) {
        i++;
    }
    if (i == IO_ALLOCS) {
        c->al_rest++;
        return;
    }
    c->al_id[i] = id;
    c->al_pages[i]++;
    c->al_fresh[i] += g_fresh[p];
}

void wl_xb_census_card_slow(WlXbCensusCard kind, bool map, uint32_t bytes) {
    if ((unsigned)kind >= WL_XB_CCARD_COUNT) {
        return;
    }
    // (the read-ahead's thread counts without the guest lock)
    __atomic_fetch_add(&g_card_ops[kind][map], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_card_bytes[kind][map], bytes, __ATOMIC_RELAXED);
}

void wl_xb_census_io_now(WlXbCensusIo kind, uint32_t *ops, uint32_t *pages, uint32_t *fresh) {
    bool ok = g_xb_census_use != NULL && (unsigned)kind < WL_XB_CIO_COUNT;
    *ops = ok ? g_io[kind].ops : 0u;
    *pages = ok ? g_io[kind].pages : 0u;
    *fresh = ok ? g_io[kind].fresh : 0u;
}

void wl_xb_census_card_now(WlXbCensusCard kind, uint32_t *ops, uint64_t *bytes) {
    bool ok = g_xb_census_use != NULL && (unsigned)kind < WL_XB_CCARD_COUNT;
    *ops = ok ? __atomic_load_n(&g_card_ops[kind][0], __ATOMIC_RELAXED) +
                    __atomic_load_n(&g_card_ops[kind][1], __ATOMIC_RELAXED)
              : 0u;
    *bytes = ok ? (uint64_t)__atomic_load_n(&g_card_bytes[kind][0], __ATOMIC_RELAXED) +
                      __atomic_load_n(&g_card_bytes[kind][1], __ATOMIC_RELAXED)
                : 0u;
}

// The second's traffic lines (written when there was any), the counts started again.
static void io_lines(void) {
    for (int k = 0; k < WL_XB_CIO_COUNT; k++) {
        IoCount *c = &g_io[k];
        if (c->pages != 0 && g_out != NULL) {
            fprintf(g_out, "io %u %s ops %u pages %u map %u fresh %u", (unsigned)g_second, k_io[k], (unsigned)c->ops,
                    (unsigned)c->pages, (unsigned)c->map, (unsigned)c->fresh);
            for (int i = 0; i < C_COUNT; i++) {
                if (c->by[i] != 0) {
                    fprintf(g_out, " %s %u", k_class[i], (unsigned)c->by[i]);
                }
            }
            // by allocation (its number in <file>.pages; 0: none): pages/fresh
            for (uint32_t i = 0; i < IO_ALLOCS && c->al_id[i] != 0; i++) {
                fprintf(g_out, " @%u %u/%u", (unsigned)(c->al_id[i] - 1u), (unsigned)c->al_pages[i],
                        (unsigned)c->al_fresh[i]);
            }
            if (c->al_rest != 0) {
                fprintf(g_out, " @rest %u", (unsigned)c->al_rest);
            }
            fputc('\n', g_out);
        }
        memset(c, 0, sizeof *c);
    }
    uint32_t ops[WL_XB_CCARD_COUNT][2];
    uint32_t bytes[WL_XB_CCARD_COUNT][2];
    uint32_t any = 0;
    for (int k = 0; k < WL_XB_CCARD_COUNT; k++) {
        for (int m = 0; m < 2; m++) {
            ops[k][m] = __atomic_exchange_n(&g_card_ops[k][m], 0u, __ATOMIC_RELAXED);
            bytes[k][m] = __atomic_exchange_n(&g_card_bytes[k][m], 0u, __ATOMIC_RELAXED);
            any |= ops[k][m];
        }
    }
    if (any != 0 && g_out != NULL) {
        // per kind: ops and KB of maps, then of other files
        fprintf(g_out, "card %u", (unsigned)g_second);
        for (int k = 0; k < WL_XB_CCARD_COUNT; k++) {
            fprintf(g_out, " %s %u %u %u %u", k_card[k], (unsigned)ops[k][1], (unsigned)(bytes[k][1] >> 10),
                    (unsigned)ops[k][0], (unsigned)(bytes[k][0] >> 10));
        }
        fputc('\n', g_out);
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
    }
    io_lines();
    if (g_out != NULL) {
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
#if CENSUS_STACKS
    if (g_stacks != NULL) {
        while (__atomic_exchange_n(&g_st_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
        }
        st_flush();
        __atomic_store_n(&g_st_lock, 0u, __ATOMIC_RELEASE);
    }
#endif
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
    g_fresh = calloc(pages, 1);
    if (g_fresh == NULL || g_now == NULL || g_first == NULL || g_last == NULL || g_seconds == NULL || g_alloc_of == NULL || use == NULL) {
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
#if CENSUS_STACKS
        st_start(path);
#endif
    }
    (void)wl_xb_service_add_vblank_hook(census_vblank, NULL);
    g_xb_census_use = use; // (last: the hooks run from here on)
    WL_LOG_I(TAG, "the page census writes %s (a line a second, the pages to %s.pages)", path, path);
    return WL_OK;
}
