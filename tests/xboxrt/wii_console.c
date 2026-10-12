// SPDX-License-Identifier: MIT
// wl/xboxrt's guest memory on the Wii's MMU, as a console program (wl_xboxrt_console.dol, run in
// Dolphin with --mmu): the hashed page table filled by first-touch DSIs, the user and window views
// of one physical page, a pinned contiguous allocation that moves a user page (its frame goes with
// it), PTE groups made to overflow (evictions, and the pages fault back in), and device accesses
// emulated through the DSI trampoline and the PowerPC decoder, with live FP state across a thousand
// of them. Prints "XBRT ok <name>" per check, " XBRT FAIL <name>" on a failure, and
// "XBRT DONE pass=<n> fail=<n>" at the end.
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"

#define TAG "xbrt"
#define DEV_VA 0xFE400000u

static int g_pass, g_fail;

static void check(const char *name, bool ok) {
    if (ok) {
        g_pass++;
        WL_LOG_I(TAG, "XBRT ok %s", name);
    } else {
        g_fail++;
        WL_LOG_E(TAG, " XBRT FAIL %s", name);
    }
}

static uint32_t g_regs[64];
static uint32_t g_reads, g_writes;

static uint32_t dev_read(void *user, uint32_t off, unsigned size) {
    (void)user;
    g_reads++;
    uint32_t w = g_regs[(off >> 2) & 63u];
    uint32_t shift = (off & 3u) * 8u;
    return size == 4 ? w : (w >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
}

static void dev_write(void *user, uint32_t off, uint32_t v, unsigned size) {
    (void)user;
    g_writes++;
    uint32_t *w = &g_regs[(off >> 2) & 63u];
    uint32_t shift = (off & 3u) * 8u;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((size == 2 ? 0xFFFFu : 0xFFu) << shift);
    *w = (*w & ~mask) | ((v << (size == 4 ? 0 : shift)) & mask);
}

int main(void) {
    wl_platform_system_init();
    wl_platform_log_init(NULL);
    WL_LOG_I(TAG, "XBRT START wl/xboxrt on the MMU");
    // no wl_mem_init: its default would claim the regions malloc gives the frame pool from
    const uint32_t pool_bytes = 8u << 20;
    void *pool = memalign(WL_XB_PAGE, pool_bytes);
    check("frame_pool", pool != NULL);
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    cfg.htab_bytes = 64u << 10; // the smallest table: 1024 PTE groups
    cfg.frame_pools[0].base = pool;
    cfg.frame_pools[0].bytes = pool_bytes;
    WlResult r = wl_xb_mem_init(&cfg);
    check("mem_init", r == WL_OK);
    if (r != WL_OK) {
        WL_LOG_I(TAG, "XBRT DONE pass=%d fail=%d", g_pass, g_fail + 1);
        for (;;) {
            wl_platform_thread_sleep_ms(1000);
        }
    }

    // lazy commit: committed pages take no frame until touched
    WlXbMemStats st0, st1;
    uint32_t va = 0;
    check("commit", wl_xb_va_alloc(&va, 64u * WL_XB_PAGE, 0x00400000u, WL_XB_PROT_READWRITE) == WL_OK);
    wl_xb_mem_stats(&st0);
    check("lazy_before_touch", st0.frames == 0);
    for (uint32_t i = 0; i < 64; i++) {
        wl_xb_wr32(va + i * WL_XB_PAGE + 8u, 0xA0000000u | i); // a first touch: a DSI resolved in place
    }
    wl_xb_mem_stats(&st1);
    check("lazy_frames_on_touch", st1.frames == 64u && st1.faults_resolved >= 64u);
    bool same = true;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t phys = 0;
        same &= wl_xb_va_to_phys(va + i * WL_XB_PAGE, &phys);
        same &= wl_xb_rd32(WL_XB_WINDOW | (phys + 8u)) == (0xA0000000u | i); // the window: the same frame
        same &= wl_xb_rd32(va + i * WL_XB_PAGE + 8u) == (0xA0000000u | i);
    }
    check("user_and_window_alias", same);
    uint8_t b[4];
    wl_xb_copy_out(b, va + 8u, 4);
    check("little_endian_bytes", b[0] == 0x00 && b[3] == 0xA0);
    // a misaligned access across two pages
    wl_xb_wr32(va + WL_XB_PAGE - 2u, 0x11223344u);
    check("across_pages", wl_xb_rd32(va + WL_XB_PAGE - 2u) == 0x11223344u);

    // a pinned contiguous allocation over a user page: the page moves, its contents stay
    uint32_t p0 = 0, got = 0, p1 = 0;
    wl_xb_va_to_phys(va, &p0);
    p0 &= ~(WL_XB_PAGE - 1u);
    check("pin_over_user_page", wl_xb_phys_alloc(WL_XB_PAGE, p0, p0 + WL_XB_PAGE - 1u, 0, &got) == WL_OK && got == p0);
    wl_xb_va_to_phys(va, &p1);
    check("relocated_keeps_contents", (p1 & ~(WL_XB_PAGE - 1u)) != p0 && wl_xb_rd32(va + 8u) == 0xA0000000u &&
                                          wl_xb_rd32(WL_XB_WINDOW | ((p1 & ~(WL_XB_PAGE - 1u)) + 8u)) == 0xA0000000u);
    check("pinned_page_clean", wl_xb_rd32(WL_XB_WINDOW | (p0 + 8u)) == 0);

    // 24 pages 4 MB apart share their PTE groups (16 entries): some must be evicted and come back
    uint32_t far = 0x01000000u;
    check("sparse_reserve", wl_xb_va_reserve(&far, 24u * 0x00400000u, 0, WL_XB_PROT_READWRITE) == WL_OK);
    bool ok = true;
    for (uint32_t i = 0; i < 24; i++) {
        ok &= wl_xb_va_commit(far + i * 0x00400000u, WL_XB_PAGE, WL_XB_PROT_READWRITE) == WL_OK;
        wl_xb_wr32(far + i * 0x00400000u, 0xE0000000u | i);
    }
    for (uint32_t round = 0; round < 3; round++) {
        for (uint32_t i = 0; i < 24; i++) {
            ok &= wl_xb_rd32(far + i * 0x00400000u) == (0xE0000000u | i);
        }
    }
    WlXbMemStats st2;
    wl_xb_mem_stats(&st2);
    check("evictions_refault", ok && st2.evictions > 0);

    // devices: the DSI slow path, the trampoline, the decoder
    WlXbDevice dev = {"fake", DEV_VA, 0x1000u, dev_read, dev_write, NULL};
    check("mmio_register", wl_xb_mmio_register(&dev) == WL_OK);
    g_regs[1] = 0x89ABCDEFu;
    check("mmio_read32", wl_xb_rd32(DEV_VA + 4u) == 0x89ABCDEFu);
    check("mmio_read16", wl_xb_rd16(DEV_VA + 4u) == 0xCDEFu);
    check("mmio_read8", wl_xb_rd8(DEV_VA + 6u) == 0xABu);
    wl_xb_wr32(DEV_VA + 8u, 0x01020304u);
    check("mmio_write32", g_regs[2] == 0x01020304u);
    wl_xb_wr16(DEV_VA + 0xEu, 0xAA55u);
    check("mmio_write16", g_regs[3] == 0xAA550000u);
    uint32_t v = 0;
    uint32_t ea = WL_XB_EA(DEV_VA + 4u);
    __asm__ volatile("lwbrx %0,0,%1" : "=r"(v) : "r"(ea) : "memory");
    check("mmio_lwbrx", v == 0x89ABCDEFu);
    __asm__ volatile("lwz %0,0(%1)" : "=r"(v) : "b"(ea) : "memory");
    check("mmio_lwz", v == 0xEFCDAB89u);
    uint32_t upd = WL_XB_EA(DEV_VA);
    __asm__ volatile("lwzu %0,4(%1)" : "=r"(v), "+b"(upd) : : "memory");
    check("mmio_lwzu", v == 0xEFCDAB89u && upd == WL_XB_EA(DEV_VA + 4u));
    __asm__ volatile("stwbrx %0,0,%1" : : "r"(0xCAFEF00Du), "r"(WL_XB_EA(DEV_VA + 0x10u)) : "memory");
    check("mmio_stwbrx", g_regs[4] == 0xCAFEF00Du);
    double acc = 1.0;
    uint32_t before = g_reads;
    for (int i = 0; i < 1000; i++) {
        acc = acc * 1.0001 + (double)(wl_xb_rd32(DEV_VA + 4u) & 1u);
    }
    check("mmio_fp_state", g_reads - before == 1000u && acc > 1000.0 && acc < 1200.0);
    WlXbMmioStats ms;
    wl_xb_mmio_stats(&ms);
    WL_LOG_I(TAG, "XBRT frames %u, faults %u, evictions %u, device reads %u writes %u", (unsigned)st2.frames,
             (unsigned)st2.faults_resolved, (unsigned)st2.evictions, (unsigned)ms.reads, (unsigned)ms.writes);
    WL_LOG_I(TAG, "XBRT DONE pass=%d fail=%d", g_pass, g_fail);
    for (;;) {
        wl_platform_thread_sleep_ms(1000);
    }
}
