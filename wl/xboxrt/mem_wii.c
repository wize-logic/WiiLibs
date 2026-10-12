// SPDX-License-Identifier: MIT
// Guest memory on the Wii (mem_internal.h): the Broadway's MMU translates EAs 0x00000000..
// 0x1FFFFFFF (segment registers 0 and 1; libogc keeps 0x80000000 and up for itself, in BATs)
// through a hashed page table that this file fills on demand. A first touch of a guest page is a
// DSI (dsi_wii.S); wl_xb_dsi_fast, in exception context, finds the page's physical page (user
// pages through the core's g_xb_va2phys, the window directly), gives it a frame from the pools
// (zeroed with dcbz) if it has none, and inserts the PTE. The page table is only a cache of those
// tables: a full PTE group evicts an entry (tlbie), which faults back in on its next use.
//
// Device addresses and pages that are not committed are not mapped; their DSI goes to
// wl_xb_dsi_slow in normal context: the PowerPC decoder and the device, or the crash report.
//
// PTE layout and hashing: PowerPC Operating Environment Architecture (Book III), "Hashed Page
// Tables", 32-bit implementations: PTEG = 8 PTEs of two words; word 0 V|VSID|H|API, word 1
// RPN|R|C|WIMG|PP; primary hash = VSID[5:23] ^ page index, secondary = its complement.
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

void wl_xb_dsi_entry(void);
uint32_t wl_xb_dsi_fast(uint32_t dar, uint32_t dsisr);
void wl_xb_dsi_slow(void *frame);
PPCExcptHandlerFn g_xb_prev_dsi; // read by dsi_wii.S

typedef struct {
    uint32_t first; // physical page number
    uint32_t pages;
    uint32_t next;
} Pool;

static volatile uint32_t *g_htab;
static uint32_t g_pteg_mask;
static uint32_t *g_phys2frame; // [ram pages] frame's physical page number (0: none)
static Pool g_pools[WL_XB_MAX_FRAME_POOLS];
static uint32_t g_frames, g_faults, g_evictions, g_evict_rr;

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

// The PTE word 0 slot mapping `ea`, or NULL.
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

// The EA a valid PTE in PTEG `index` maps (for its tlbie when it is evicted).
static uint32_t pte_ea(uint32_t w0, uint32_t index) {
    uint32_t vsid = (w0 >> 7) & 0xFFFFFFu;
    uint32_t h = (w0 >> 6) & 1u;
    uint32_t low = ((h ? ~index : index) ^ vsid) & 0x3FFu;
    uint32_t pidx = ((w0 & 0x3Fu) << 10) | low;
    return ((vsid - VSID_BASE) << 28) | (pidx << 12);
}

static void pte_invalidate(volatile uint32_t *w0p, uint32_t ea) {
    *w0p = 0;
    __asm__ volatile("sync" ::: "memory");
    tlbie(ea);
}

static void pte_insert(uint32_t ea, uint32_t frame) {
    uint32_t hash = primary_hash(ea);
    uint32_t w1 = (frame << 12) | PTE_R | PTE_C | PTE_PP_RW;
    for (uint32_t h = 0; h < 2; h++) {
        volatile uint32_t *g = pteg(h ? ~hash : hash);
        for (uint32_t i = 0; i < 8; i++) {
            if ((g[2 * i] & 0x80000000u) == 0) {
                g[2 * i + 1] = w1;
                __asm__ volatile("eieio" ::: "memory");
                g[2 * i] = pte_w0(ea, h);
                __asm__ volatile("sync" ::: "memory");
                return;
            }
        }
    }
    // both groups full: reuse a primary slot
    uint32_t i = g_evict_rr++ & 7u;
    volatile uint32_t *g = pteg(hash);
    uint32_t old = g[2 * i];
    pte_invalidate(&g[2 * i], pte_ea(old, hash & g_pteg_mask));
    g_evictions++;
    g[2 * i + 1] = w1;
    __asm__ volatile("eieio" ::: "memory");
    g[2 * i] = pte_w0(ea, 0);
    __asm__ volatile("sync" ::: "memory");
}

static void zero_frame(uint32_t frame) {
    uint8_t *p = (uint8_t *)(0x80000000u + (frame << 12));
    for (uint32_t off = 0; off < WL_XB_PAGE; off += 32u) {
        __asm__ volatile("dcbz 0,%0" ::"r"(p + off) : "memory");
    }
}

static uint32_t frame_alloc(void) {
    for (uint32_t i = 0; i < WL_XB_MAX_FRAME_POOLS; i++) {
        Pool *p = &g_pools[i];
        if (p->next < p->pages) {
            return p->first + p->next++;
        }
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

// 1: mapped now, 2: already mapped, 0: cannot be.
static uint32_t map_page(uint32_t ea) {
    uint32_t p1 = phys_of(ea);
    if (p1 == 0) {
        return 0;
    }
    if (pte_find(ea) != NULL) {
        return 2;
    }
    uint32_t p = p1 - 1u;
    uint32_t frame = g_phys2frame[p];
    if (frame == 0) {
        frame = frame_alloc();
        if (frame == 0) {
            return 0; // out of frames: the slow path reports it
        }
        zero_frame(frame);
        g_phys2frame[p] = frame;
        g_frames++;
    }
    pte_insert(ea & ~(WL_XB_PAGE - 1u), frame);
    g_faults++;
    return 1;
}

// Exception context: no locks, no FP, no guest memory.
uint32_t wl_xb_dsi_fast(uint32_t dar, uint32_t dsisr) {
    if (dar >= WL_XB_EA_SPAN) {
        return 2;
    }
    if ((dsisr & DSISR_NO_PTE) == 0) {
        return 0;
    }
    uint32_t a = map_page(dar);
    uint32_t last = dar + 7u; // a misaligned access may run into the next page
    if ((last >> 12) != (dar >> 12) && last < WL_XB_EA_SPAN) {
        uint32_t b = map_page(last);
        if (a != 0 && b == 1) {
            return 1;
        }
        if (a == 2 && b == 0) {
            return 0; // the second page is a device or a hole
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

void wl_xb_dsi_slow(void *frame) {
    DsiFrame *f = frame;
    uint32_t ea = f->dar;
    uint32_t va = wl_xb_ea_to_va(ea);
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
        WlXbCrash c = {"device access the decoder cannot emulate", what, va, ea, f->pc, (f->dsisr & 0x02000000u) != 0};
        wl_xb_crash(&c);
    }
    const char *reason = va >= WL_XB_MMIO_BASE ? "access to an address no device claims"
                         : phys_of(ea) != 0    ? "guest RAM: no frame left (the pools are exhausted)"
                                               : "access to guest memory that is not there";
    WlXbCrash c = {reason, NULL, va, ea, f->pc, (f->dsisr & 0x02000000u) != 0};
    wl_xb_crash(&c);
}

WlResult wl_xb_backend_init(const WlXbMemConfig *cfg, uintptr_t *base_out) {
    uint32_t htab = cfg->htab_bytes;
    if (htab < (64u << 10) || htab > (1u << 20) || (htab & (htab - 1u)) != 0) {
        return WL_ERR_ARG;
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < WL_XB_MAX_FRAME_POOLS; i++) {
        const WlXbFramePool *p = &cfg->frame_pools[i];
        if (p->bytes == 0) {
            continue;
        }
        uintptr_t b = (uintptr_t)p->base;
        if ((b & (WL_XB_PAGE - 1u)) != 0 || (p->bytes & (WL_XB_PAGE - 1u)) != 0 || b < 0x80000000u || b >= 0xC0000000u) {
            return WL_ERR_ARG; // cached MEM1 or MEM2, page aligned
        }
        g_pools[i].first = (uint32_t)(b - 0x80000000u) >> 12;
        g_pools[i].pages = p->bytes >> 12;
        total += p->bytes;
    }
    if (total == 0) {
        return WL_ERR_ARG;
    }
    g_phys2frame = calloc(cfg->ram_bytes >> 12, sizeof *g_phys2frame);
    void *t = memalign(htab, htab);
    if (g_phys2frame == NULL || t == NULL) {
        return WL_ERR_NOMEM;
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
            pte_invalidate(w0, ea);
        }
    }
    IRQ_Restore(level);
}

void wl_xb_backend_release_phys(uint32_t phys, uint32_t pages) {
    uint32_t level = IRQ_Disable();
    for (uint32_t k = 0; k < pages; k++) {
        uint32_t frame = g_phys2frame[phys + k];
        if (frame != 0) {
            zero_frame(frame); // keeps its frame and its window mapping, now zero
        }
    }
    IRQ_Restore(level);
}

void wl_xb_backend_move_phys(uint32_t from, uint32_t to, uint32_t va) {
    (void)va; // the user page's PTE maps the frame, which moves with it: still right
    uint32_t level = IRQ_Disable();
    uint32_t f = g_phys2frame[from], t = g_phys2frame[to];
    g_phys2frame[to] = f;
    g_phys2frame[from] = t; // `to` was free: its frame (if any) is zero
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t ea = WL_XB_EA_WINDOW + ((k == 0 ? from : to) << 12);
        volatile uint32_t *w0 = pte_find(ea);
        if (w0 != NULL) {
            pte_invalidate(w0, ea); // the window maps another frame now
        }
    }
    IRQ_Restore(level);
}

void wl_xb_backend_stats(uint32_t *frames, uint32_t *faults, uint32_t *evictions) {
    *frames = g_frames;
    *faults = g_faults;
    *evictions = g_evictions;
}

WlResult wl_xb_fault_install(void) {
    return WL_OK; // the DSI handler is part of wl_xb_backend_init
}
