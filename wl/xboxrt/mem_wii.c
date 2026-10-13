// SPDX-License-Identifier: MIT
// Guest memory on the Wii (mem_internal.h): the Broadway's MMU translates EAs 0x00000000..
// 0x1FFFFFFF (segment registers 0 and 1; libogc keeps 0x80000000 and up for itself, in BATs)
// through a hashed page table that this file fills on demand. A touch of a guest page that has no
// PTE is a DSI (dsi_wii.c); wl_xb_dsi_fast, in exception context, finds the page's physical page
// (user pages through the core's g_xb_va2phys, the window directly), gives it a frame if it has
// none and needs no I/O (a free frame, or one the paging core's clock takes from a clean page;
// zeroed with dcbz), and inserts the PTE. The page table is only a cache of those tables: a full
// PTE group evicts an entry (tlbie), which faults back in on its next use.
//
// Demand paging (paging.c) runs over this MMU: the hardware keeps each PTE's R (referenced) and C
// (changed) bits, which this file harvests into the core's flags whenever a PTE goes (evicted from
// its group, unmapped) and when the clock asks; the clock clears R (and the TLB entry, so the next
// use sets it again). A clean page's PTE is inserted with C clear, so its first store marks it.
// Where a page sits in the table is remembered per page and EA (its PTE group), so the clock finds
// a page's PTEs without hashing.
//
// Device addresses, pages that are not committed, and touches that need the swap file (a page to
// read back, a batch of changed pages to write before a frame is free) go to wl_xb_dsi_slow in
// normal context, through the trampoline: the PowerPC decoder and the device, the paging core's
// slow path (the faulting thread holds the guest lock: it may wait for the SD card), or the crash
// report.
//
// PTE layout and hashing: PowerPC Operating Environment Architecture (Book III), "Hashed Page
// Tables", 32-bit implementations: PTEG = 8 PTEs of two words; word 0 V|VSID|H|API, word 1
// RPN|R|C|WIMG|PP; primary hash = VSID[5:23] ^ page index, secondary = its complement. R and C:
// "Page History Recording" there, and the 750CL user's manual (hardware R/C updates; a store that
// hits a TLB entry with C clear searches the table to set it).
#include <malloc.h>
#include <ogc/cache.h>
#include <ogc/irq.h>
#include <stdlib.h>
#include <string.h>
#include <tuxedo/ppc/exception.h>

#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/ppc_decode.h"
#include "wl/xboxrt/thread.h"

#define TAG "xbmmu"
#define VSID_BASE 0x00051000u // segment n's VSID is VSID_BASE + n
#define PTE_R 0x100u
#define PTE_C 0x080u
#define PTE_PP_RW 0x002u
#define DSISR_NO_PTE 0x40000000u
#define DSISR_STORE 0x02000000u
#define XB_MSR_EE 0x8000u

void wl_xb_dsi_entry(void);
uint32_t wl_xb_dsi_fast(uint32_t dar, uint32_t dsisr);
void wl_xb_dsi_slow(void *frame);
PPCExcptHandlerFn g_xb_prev_dsi; // read by dsi_wii.S

typedef struct {
    uint32_t first; // the pool's first real page number
    uint32_t pages;
} Pool;

static volatile uint32_t *g_htab;
static uint32_t g_pteg_mask;
static Pool g_pools[WL_XB_MAX_FRAME_POOLS];
static uint16_t *g_hint[2]; // [ram pages] the PTE group (+ 1) of the page's user / window PTE
static uint32_t g_faults, g_pte_evictions, g_evict_rr;
// Whether the MMU records R and C in the table (the 750CL does; an emulator might not): measured
// once at init (mmu_selftest). Without R, "used" is a fault: clearing it drops the page's PTEs.
// Without C, every page that gets a PTE counts as changed.
static bool g_hw_ref = true, g_hw_changed = true;

static inline void tlbie(uint32_t ea) {
    __asm__ volatile("tlbie %0; sync" ::"r"(ea) : "memory");
}

static inline uint32_t vsid_of(uint32_t ea) {
    return VSID_BASE + (ea >> 28);
}

static inline volatile uint32_t *pteg(uint32_t hash) {
    return g_htab + ((hash & g_pteg_mask) << 4);
}

static inline uint32_t pte_w0(uint32_t ea, uint32_t h) {
    uint32_t pidx = (ea >> 12) & 0xFFFFu;
    return 0x80000000u | (vsid_of(ea) << 7) | (h << 6) | (pidx >> 10);
}

static inline uint32_t primary_hash(uint32_t ea) {
    return (vsid_of(ea) & 0x7FFFFu) ^ ((ea >> 12) & 0xFFFFu);
}

// The real page number of frame f (1..frames): the pools in order.
static uint32_t rpn_of(uint32_t f) {
    uint32_t i = f - 1u;
    for (uint32_t k = 0; k < WL_XB_MAX_FRAME_POOLS; k++) {
        if (i < g_pools[k].pages) {
            return g_pools[k].first + i;
        }
        i -= g_pools[k].pages;
    }
    return 0;
}

// The physical page behind `ea` (+1), 0 when the EA is not guest RAM a touch may map.
static uint32_t phys_of(uint32_t ea) {
    if (ea < WL_XB_EA_WINDOW) {
        return g_xb_va2phys[ea >> 12];
    }
    uint32_t off = ea - WL_XB_EA_WINDOW;
    return off < (g_xb_ram_pages << 12) ? (off >> 12) + 1u : 0;
}

// The PTE word 0 slot mapping `ea`, or NULL (both groups searched).
static volatile uint32_t *pte_find(uint32_t ea) {
    uint32_t hash = primary_hash(ea);
    for (uint32_t h = 0; h < 2; h++) {
        volatile uint32_t *g = pteg(h ? ~hash : hash);
        uint32_t w0 = pte_w0(ea, h);
        for (uint32_t i = 0; i < 8; i++) {
            if (g[2 * i] == w0) {
                return &g[2 * i];
            }
        }
    }
    return NULL;
}

// The EA a valid PTE in PTEG `index` maps (for its tlbie when it goes).
static uint32_t pte_ea(uint32_t w0, uint32_t index) {
    uint32_t vsid = (w0 >> 7) & 0xFFFFFFu;
    uint32_t h = (w0 >> 6) & 1u;
    uint32_t low = ((h ? ~index : index) ^ vsid) & 0x3FFu;
    uint32_t pidx = ((w0 & 0x3Fu) << 10) | low;
    return ((vsid - VSID_BASE) << 28) | (pidx << 12);
}

static inline uint32_t pteg_index(volatile uint32_t *w0p) {
    return (uint32_t)(w0p - g_htab) >> 4;
}

// Page `p`'s PTE for its user (a = 0) or window (a = 1) EA, or NULL: a valid entry of the group its
// hint names, in that segment, mapping p's frame. (A page has at most one PTE per EA; the hint is
// set at each insert, so a PTE elsewhere cannot exist, and a stale hint finds nothing.)
static volatile uint32_t *pte_of(uint32_t p, uint32_t a) {
    uint32_t h = g_hint[a][p];
    uint32_t f = g_xb_pg_frame[p];
    if (h == 0 || f == 0) {
        return NULL;
    }
    uint32_t rpn = rpn_of(f) << 12;
    uint32_t vsid = VSID_BASE + a;
    volatile uint32_t *g = g_htab + ((h - 1u) << 4);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t w0 = g[2 * i];
        if ((w0 & 0x80000000u) != 0 && ((w0 >> 7) & 0xFFFFFFu) == vsid && (g[2 * i + 1] & 0xFFFFF000u) == rpn) {
            return &g[2 * i];
        }
    }
    return NULL;
}

// A valid PTE's R and C bits as page flags (R only when the hardware records it).
static inline uint8_t bits_flags(uint32_t w1) {
    return (uint8_t)(((w1 & PTE_R) != 0 && g_hw_ref ? WL_XB_PG_REF : 0u) | ((w1 & PTE_C) != 0 ? WL_XB_PG_DIRTY : 0u));
}

// A valid PTE's R and C bits into its page's flags.
static void harvest_bits(uint32_t ea, uint32_t w1) {
    uint32_t p1 = phys_of(ea);
    if (p1 != 0) {
        g_xb_pg_flags[p1 - 1u] |= bits_flags(w1);
    }
}

// The PTE at `w0p` (mapping `ea`) goes: its bits harvested, the entry invalid, the TLB told.
static void pte_drop(volatile uint32_t *w0p, uint32_t ea) {
    harvest_bits(ea, w0p[1]);
    *w0p = 0;
    __asm__ volatile("sync" ::: "memory");
    tlbie(ea);
}

// A PTE for `ea` (page `p`) mapping `frame`, with R and C as `rc` has them (PTE_R, PTE_C).
static void pte_insert(uint32_t ea, uint32_t frame, uint32_t p, uint32_t rc) {
    uint32_t hash = primary_hash(ea);
    uint32_t w1 = (rpn_of(frame) << 12) | rc | PTE_PP_RW;
    uint32_t a = ea >> 28;
    for (uint32_t h = 0; h < 2; h++) {
        uint32_t index = (h ? ~hash : hash) & g_pteg_mask;
        volatile uint32_t *g = pteg(index);
        for (uint32_t i = 0; i < 8; i++) {
            if ((g[2 * i] & 0x80000000u) == 0) {
                g[2 * i + 1] = w1;
                __asm__ volatile("eieio" ::: "memory");
                g[2 * i] = pte_w0(ea, h);
                __asm__ volatile("sync" ::: "memory");
                g_hint[a][p] = (uint16_t)(index + 1u);
                return;
            }
        }
    }
    // both groups full: reuse a primary slot
    uint32_t i = g_evict_rr++ & 7u;
    uint32_t index = hash & g_pteg_mask;
    volatile uint32_t *g = pteg(index);
    pte_drop(&g[2 * i], pte_ea(g[2 * i], index));
    g_pte_evictions++;
    g[2 * i + 1] = w1;
    __asm__ volatile("eieio" ::: "memory");
    g[2 * i] = pte_w0(ea, 0);
    __asm__ volatile("sync" ::: "memory");
    g_hint[a][p] = (uint16_t)(index + 1u);
}

// The PTE for a touch of page `p` at `ea`: R set (it is being used now), C when the page is known
// changed (no table search for its first store) or the hardware does not record C (the page then
// counts as changed from now on).
static void pte_map(uint32_t ea, uint32_t frame, uint32_t p) {
    if (!g_hw_changed) {
        g_xb_pg_flags[p] |= WL_XB_PG_DIRTY;
    }
    pte_insert(ea & ~(WL_XB_PAGE - 1u), frame, p, PTE_R | ((g_xb_pg_flags[p] & WL_XB_PG_DIRTY) != 0 ? PTE_C : 0u));
    g_xb_pg_flags[p] |= WL_XB_PG_REF;
}

static void zero_frame(uint32_t frame) {
    uint8_t *p = (uint8_t *)(0x80000000u + (rpn_of(frame) << 12));
    for (uint32_t off = 0; off < WL_XB_PAGE; off += 32u) {
        __asm__ volatile("dcbz 0,%0" ::"r"(p + off) : "memory");
    }
}

// ---- The paging core's MMU (exception-safe) ------------------------------------------------------

void wl_xb_mmu_harvest(uint32_t p, bool clear_ref) {
    bool drop = clear_ref && !g_hw_ref && (g_xb_pg_flags[p] & WL_XB_PG_REF) != 0;
    for (uint32_t a = 0; a < 2u; a++) {
        volatile uint32_t *w0p = pte_of(p, a);
        if (w0p == NULL) {
            continue;
        }
        uint32_t w1 = w0p[1];
        g_xb_pg_flags[p] |= bits_flags(w1);
        if (drop) {
            // no R from the hardware: "used" is a fault since the last pass, so the PTE goes
            pte_drop(w0p, pte_ea(*w0p, pteg_index(w0p)));
        } else if (clear_ref && g_hw_ref && (w1 & PTE_R) != 0) {
            // R cleared in the table and the TLB entry dropped, so the next use searches the table
            // and sets R again (the hardware only records R on a table search)
            w0p[1] = w1 & ~PTE_R;
            __asm__ volatile("sync" ::: "memory");
            tlbie(pte_ea(*w0p, pteg_index(w0p)));
        }
    }
}

void wl_xb_mmu_unmap(uint32_t p) {
    for (uint32_t a = 0; a < 2u; a++) {
        volatile uint32_t *w0p = pte_of(p, a);
        if (w0p != NULL) {
            pte_drop(w0p, pte_ea(*w0p, pteg_index(w0p)));
        }
        g_hint[a][p] = 0;
    }
}

uint8_t *wl_xb_mmu_frame_ptr(uint32_t p, uint32_t f) {
    (void)p;
    return (uint8_t *)(0x80000000u + (rpn_of(f) << 12)); // the cached BAT view: the same cache lines
}

void wl_xb_mmu_frame_zero(uint32_t p, uint32_t f) {
    (void)p;
    zero_frame(f);
}

void wl_xb_mmu_frame_drop(uint32_t p, uint32_t f) {
    (void)p;
    (void)f; // the frame stays in the pool
}

void wl_xb_mmu_frame_move(uint32_t from, uint32_t to, uint32_t f) {
    (void)from;
    (void)to;
    (void)f; // frames are not tied to pages here
}

void wl_xb_mmu_pinned(uint32_t p) {
    (void)p; // a resident page's PTE is put back by the fast path, without I/O
}

uint32_t wl_xb_mmu_lock(void) {
    return IRQ_Disable();
}

void wl_xb_mmu_unlock(uint32_t level) {
    IRQ_Restore(level);
}

// ---- The DSI handler -----------------------------------------------------------------------------

// 1: mapped now, 2: already mapped, 0: the slow path (not guest RAM, or it needs I/O). `keep` (a
// page + 1) is not to lose its frame.
static uint32_t map_page(uint32_t ea, uint32_t keep) {
    uint32_t p1 = phys_of(ea);
    if (p1 == 0) {
        return 0;
    }
    uint32_t p = p1 - 1u;
    uint32_t a = ea >> 28;
    if ((g_xb_pg_flags[p] & WL_XB_PG_IO) != 0) {
        return 0;
    }
    uint32_t frame = g_xb_pg_frame[p];
    if (frame != 0 && pte_of(p, a) != NULL) {
        return 2;
    }
    if (frame == 0) {
        if (g_xb_pg_slot[p] != 0) {
            return 0; // in the swap file
        }
        frame = wl_xb_pg_frame_fast(p, keep);
        if (frame == 0) {
            return 0; // nothing free and no clean page at hand: the slow path writes a batch
        }
        zero_frame(frame);
    }
    pte_map(ea, frame, p);
    g_faults++;
    return 1;
}

// Exception context: no locks, no FP, no guest memory, no I/O.
uint32_t wl_xb_dsi_fast(uint32_t dar, uint32_t dsisr) {
    if (dar >= WL_XB_EA_SPAN) {
        return 2;
    }
    if ((dsisr & DSISR_NO_PTE) == 0) {
        return 0;
    }
    uint32_t last = dar + 7u; // a misaligned access may run into the next page
    bool cross = (last >> 12) != (dar >> 12) && last < WL_XB_EA_SPAN;
    uint32_t a = map_page(dar, cross ? phys_of(last) : 0u);
    if (cross) {
        uint32_t b = map_page(last, phys_of(dar));
        if (a != 0 && b == 1) {
            return 1;
        }
        if (a == 2 && b == 0) {
            return 0; // the second page is a device, a hole, or needs I/O
        }
    }
    return a == 1 ? 1u : 0u;
}

typedef struct {
    uint32_t back, pad[3];
    uint32_t gpr[32];
    uint32_t pc, msr, cr, lr, ctr, xer, dar, dsisr;
    uint64_t fpscr;
    double fpr[14];
} DsiFrame;

// The slow path for guest RAM: page `ea`'s contents into a frame (paging.c, which may wait for the
// swap file), then its PTE. Normal context, the guest lock held.
static WlResult page_in(uint32_t ea, uint32_t keep) {
    uint32_t p1 = phys_of(ea);
    if (p1 == 0) {
        return WL_ERR_RANGE;
    }
    uint32_t p = p1 - 1u;
    WlResult r = wl_xb_pg_fault(p, keep);
    if (r != WL_OK) {
        return r;
    }
    uint32_t level = IRQ_Disable();
    if (g_xb_pg_frame[p] != 0 && pte_of(p, ea >> 28) == NULL) {
        pte_map(ea, g_xb_pg_frame[p], p);
        g_faults++;
    }
    g_xb_pg_flags[p] |= WL_XB_PG_REF;
    IRQ_Restore(level);
    return WL_OK;
}

static const char *paging_failure(WlResult r) {
    switch (r) {
    case WL_ERR_NOMEM: return "guest RAM: no frame left (every frame pinned or changed, and no swap file to write to)";
    case WL_ERR_IO: return "guest RAM: the swap file failed";
    case WL_ERR_STATE: return "guest RAM: a fault inside the paging code (a page in I/O touched)";
    default: return "access to guest memory that is not there";
    }
}

void wl_xb_dsi_slow(void *frame) {
    DsiFrame *f = frame;
    uint32_t ea = f->dar;
    uint32_t va = wl_xb_ea_to_va(ea);
    bool store = (f->dsisr & DSISR_STORE) != 0;
    const char *what = NULL;
    if (va >= WL_XB_MMIO_BASE && wl_xb_mmio_find(va) != NULL) {
        uint32_t insn = *(const uint32_t *)f->pc;
        WlXbPpcAccess a;
        WlXbPpcDecode d = wl_xb_ppc_decode(insn, &a);
        if (d == WL_XB_PPC_OK) {
            uint32_t acc = wl_xb_ppc_ea(&a, f->gpr);
            if (wl_xb_ppc_emulate(&a, f->gpr, acc, va)) {
                f->pc += 4u;
                wl_xb_safe_point(); // normal context (the trampoline): interrupt work, a waiting thread
                return;
            }
            what = "no device at the address";
        } else {
            what = d == WL_XB_PPC_REFUSED ? a.name : "not a load or store";
        }
        WlXbCrash c = {"device access the decoder cannot emulate", what, va, ea, f->pc, store};
        wl_xb_crash(&c);
    }
    if (va < WL_XB_MMIO_BASE && phys_of(ea) != 0) {
        if ((f->dsisr & DSISR_NO_PTE) == 0) {
            WlXbCrash c = {"guest RAM: a DSI that is not a missing PTE", NULL, va, ea, f->pc, store};
            wl_xb_crash(&c);
        }
        if (!wl_xb_lock_held()) {
            WlXbCrash c = {"guest RAM touched without the guest lock needs the swap file (pin it: wl_xb_mem_pin)", NULL,
                           va, ea, f->pc, store};
            wl_xb_crash(&c);
        }
        if ((f->msr & XB_MSR_EE) == 0) {
            WlXbCrash c = {"guest RAM touched with interrupts off needs the swap file", NULL, va, ea, f->pc, store};
            wl_xb_crash(&c);
        }
        // (as the fast path: the access may run into the next page; when that one is not guest RAM,
        // the access only ran into it if the first page was mapped already)
        uint32_t last = ea + 7u;
        bool cross = (last >> 12) != (ea >> 12) && last < WL_XB_EA_SPAN;
        uint32_t pa = phys_of(ea), pb = cross ? phys_of(last) : 0u;
        bool was_mapped = g_xb_pg_frame[pa - 1u] != 0 && pte_of(pa - 1u, ea >> 28) != NULL;
        WlResult r = page_in(ea, pb);
        if (r == WL_OK && cross) {
            r = pb != 0 ? page_in(last, pa) : was_mapped ? WL_ERR_RANGE : WL_OK;
        }
        if (r == WL_OK) {
            return; // the access runs again
        }
        WlXbCrash c = {paging_failure(r), NULL, va, ea, f->pc, store};
        wl_xb_crash(&c);
    }
    const char *reason = va >= WL_XB_MMIO_BASE ? "access to an address no device claims" : "access to guest memory that is not there";
    WlXbCrash c = {reason, NULL, va, ea, f->pc, store};
    wl_xb_crash(&c);
}

// ---- Set-up and the core's backend operations ----------------------------------------------------

// Whether the MMU records R and C in the page table: physical page 0 (the retail kernel's reserved
// low pages, which nothing here uses) gets a frame and a PTE with both clear, is read, then
// written, and the PTE looked at after each. The 750CL records both; an MMU that does not (an
// emulator's) gets the fallbacks (g_hw_ref, g_hw_changed) rather than pages lost as clean.
static void mmu_selftest(void) {
    const uint32_t ea = WL_XB_EA_WINDOW;
    uint32_t level = IRQ_Disable();
    uint32_t f = wl_xb_pg_frame_fast(0, 0);
    if (f == 0) {
        IRQ_Restore(level);
        return;
    }
    zero_frame(f);
    pte_insert(ea, f, 0, 0);
    tlbie(ea);
    volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)ea;
    (void)*word;
    __asm__ volatile("sync" ::: "memory");
    volatile uint32_t *w0p = pte_of(0, 1);
    bool r = w0p != NULL && (w0p[1] & PTE_R) != 0;
    *word = 0;
    __asm__ volatile("sync" ::: "memory");
    bool c = w0p != NULL && (w0p[1] & PTE_C) != 0;
    IRQ_Restore(level);
    wl_xb_pg_release(0, 1);
    g_hw_ref = r;
    g_hw_changed = c;
    if (r && c) {
        WL_LOG_I(TAG, "the MMU records R and C in the page table: the clock uses them");
    } else {
        WL_LOG_W(TAG, "the MMU records R: %s, C: %s; %s%s", r ? "yes" : "no", c ? "yes" : "no",
                 r ? "" : "a page's use is its fault (the clock drops its PTEs); ",
                 c ? "" : "every page mapped counts as changed (written out when it goes)");
    }
}

WlResult wl_xb_backend_init(const WlXbMemConfig *cfg, uintptr_t *base_out) {
    uint32_t htab = cfg->htab_bytes;
    if (htab < (64u << 10) || htab > (1u << 20) || (htab & (htab - 1u)) != 0) {
        return WL_ERR_ARG;
    }
    uint32_t total = 0;
    for (uint32_t i = 0, n = 0; i < WL_XB_MAX_FRAME_POOLS; i++) {
        const WlXbFramePool *p = &cfg->frame_pools[i];
        if (p->bytes == 0) {
            continue;
        }
        uintptr_t b = (uintptr_t)p->base;
        if ((b & (WL_XB_PAGE - 1u)) != 0 || (p->bytes & (WL_XB_PAGE - 1u)) != 0 || b < 0x80000000u || b >= 0xC0000000u) {
            return WL_ERR_ARG; // cached MEM1 or MEM2, page aligned
        }
        g_pools[n].first = (uint32_t)(b - 0x80000000u) >> 12;
        g_pools[n].pages = p->bytes >> 12;
        n++;
        total += p->bytes;
    }
    if (total == 0 || (total >> 12) > 0xFFFFu) {
        return WL_ERR_ARG;
    }
    uint32_t pages = cfg->ram_bytes >> 12;
    g_hint[0] = calloc(pages, sizeof *g_hint[0]);
    g_hint[1] = calloc(pages, sizeof *g_hint[1]);
    void *t = memalign(htab, htab);
    if (g_hint[0] == NULL || g_hint[1] == NULL || t == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_xb_pg_init(cfg, total >> 12);
    if (r != WL_OK) {
        return r;
    }
    memset(t, 0, htab);
    DCFlushRange(t, htab);
    g_htab = t;
    g_pteg_mask = (htab >> 6) - 1u; // PTEGs of 64 bytes
    uint32_t sdr1 = ((uint32_t)(uintptr_t)t - 0x80000000u) | ((htab >> 16) - 1u);

    uint32_t level = IRQ_Disable();
    for (uint32_t sr = 0; sr < 2; sr++) {
        // T=0, Ks=Kp=1 (PP=2 lets either key read and write)
        __asm__ volatile("mtsrin %0,%1" ::"r"(0x60000000u | (VSID_BASE + sr)), "r"(sr << 28));
    }
    __asm__ volatile("mtsdr1 %0; isync" ::"r"(sdr1) : "memory");
    for (uint32_t i = 0; i < 64; i++) {
        tlbie(i << 12); // the TLB's 64 sets
    }
    g_xb_prev_dsi = PPCExcptGetHandler(PPC_EXCPT_DSI);
    PPCExcptSetHandler(PPC_EXCPT_DSI, wl_xb_dsi_entry);
    IRQ_Restore(level);
    WL_LOG_I(TAG, "page table %u KB at %p, frames %u KB in pools", (unsigned)(htab >> 10), t, (unsigned)(total >> 10));
    mmu_selftest();
    *base_out = 0;
    return WL_OK;
}

void wl_xb_backend_map_user(uint32_t va, uint32_t phys, uint32_t pages) {
    (void)va;
    (void)phys;
    (void)pages; // the first touch maps
}

void wl_xb_backend_unmap_user(uint32_t va, uint32_t pages) {
    uint32_t level = IRQ_Disable();
    for (uint32_t k = 0; k < pages; k++) {
        uint32_t ea = va + (k << 12);
        volatile uint32_t *w0 = pte_find(ea);
        if (w0 != NULL) {
            pte_drop(w0, ea);
        }
    }
    IRQ_Restore(level);
}

void wl_xb_backend_release_phys(uint32_t phys, uint32_t pages) {
    wl_xb_pg_release(phys, pages); // frames back to the pool: the pages fault in zero again
}

void wl_xb_backend_move_phys(uint32_t from, uint32_t to, uint32_t va) {
    (void)va; // the user page's PTE is dropped with the rest and faults back in through its new page
    wl_xb_pg_move(from, to);
}

void wl_xb_backend_stats(uint32_t *frames, uint32_t *faults, uint32_t *pte_evictions) {
    *frames = 0; // the paging core's count (mem.c)
    *faults = g_faults;
    *pte_evictions = g_pte_evictions;
}

uintptr_t wl_xb_backend_thread_handle(void) {
    return 0;
}

void wl_xb_backend_probe(uintptr_t handle) {
    (void)handle; // another LWP's PC is not looked at here
}

WlResult wl_xb_fault_install(void) {
    return WL_OK; // the DSI handler is part of wl_xb_backend_init
}
