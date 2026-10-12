// SPDX-License-Identifier: MIT
// Guest memory on the host / ppc-be backend: the EA transform, user and window views of the same
// physical page, lazy backing, accesses across pages, the contiguous allocator (top-down, pinned
// ranges, relocation of user pages), reservations and queries, and the fault path: a device access
// emulated through the host's decoder, an access to nothing reported as a guest crash.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <string.h>

#include "greatest.h"
#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"

static bool g_up;

static void up(void) {
    if (!g_up) {
        WlXbMemConfig cfg;
        wl_xb_mem_config_default(&cfg);
        if (wl_xb_mem_init(&cfg) == WL_OK && wl_xb_fault_install() == WL_OK) {
            g_up = true;
        }
    }
}

TEST ea_transform(void) {
    ASSERT_EQ_FMT(0x00012345u, WL_XB_EA(0x00012345u), "%08x");
    ASSERT_EQ_FMT(0x10061000u, WL_XB_EA(0x80061000u), "%08x");
    ASSERT_EQ_FMT(0x10061000u, WL_XB_EA(0xF0061000u), "%08x");
    ASSERT_EQ_FMT(0x1D800040u, WL_XB_EA(0xFD800040u), "%08x");
    ASSERT_EQ_FMT(0x80061000u, wl_xb_ea_to_va(0x10061000u), "%08x");
    ASSERT_EQ_FMT(0xFD800040u, wl_xb_ea_to_va(0x1D800040u), "%08x");
    ASSERT_EQ_FMT(0x00400000u, wl_xb_ea_to_va(0x00400000u), "%08x");
    PASS();
}

TEST user_and_window_alias(void) {
    up();
    ASSERT(g_up);
    uint32_t va = 0;
    ASSERT_EQ(WL_OK, wl_xb_va_alloc(&va, 3u * WL_XB_PAGE, 0x00400000u, WL_XB_PROT_READWRITE));
    ASSERT(va >= 0x00400000u && (va & 0xFFFFu) == 0);
    uint32_t phys = 0;
    ASSERT(wl_xb_va_to_phys(va + 0x123u, &phys));
    ASSERT_EQ_FMT(0x123u, phys & 0xFFFu, "%x");
    ASSERT_EQ_FMT(0u, wl_xb_rd32(va), "%08x"); // new pages are zero
    wl_xb_wr32(va + 0x10u, 0xA1B2C3D4u);
    uint32_t pbase = phys & ~0xFFFu;
    ASSERT_EQ_FMT(0xA1B2C3D4u, wl_xb_rd32(WL_XB_WINDOW | (pbase + 0x10u)), "%08x");
    ASSERT_EQ_FMT(0xA1B2C3D4u, wl_xb_rd32(WL_XB_TILED | (pbase + 0x10u)), "%08x");
    // little-endian bytes in memory, whatever the host
    uint8_t b[4];
    wl_xb_copy_out(b, va + 0x10u, 4);
    ASSERT_EQ(0xD4, b[0]);
    ASSERT_EQ(0xA1, b[3]);
    wl_xb_wr16(WL_XB_WINDOW | (pbase + 0x20u), 0xBEEFu);
    ASSERT_EQ_FMT(0xBEEFu, wl_xb_rd16(va + 0x20u), "%04x");
    // an access across the boundary of two pages whose physical pages are not neighbours
    wl_xb_wr32(va + WL_XB_PAGE - 2u, 0x11223344u);
    ASSERT_EQ_FMT(0x11223344u, wl_xb_rd32(va + WL_XB_PAGE - 2u), "%08x");
    ASSERT_EQ(0x22, wl_xb_rd8(va + WL_XB_PAGE));
    wl_xb_wr64(va + 2u * WL_XB_PAGE - 4u, 0x0102030405060708ull);
    ASSERT_EQ(0x0102030405060708ull, wl_xb_rd64(va + 2u * WL_XB_PAGE - 4u));
    wl_xb_wrf(va + 0x40u, 1.5f);
    ASSERT_EQ(0x3FC00000u, wl_xb_rd32(va + 0x40u));
    PASS();
}

TEST lazy_backing(void) {
    up();
    WlXbMemStats before, after;
    wl_xb_mem_stats(&before);
    uint32_t phys = 0;
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(8u << 20, 0, 0xFFFFFFFFu, 0, &phys)); // 8 MB, untouched
    wl_xb_mem_stats(&after);
    ASSERT(after.frames < before.frames + 64u); // nothing backs it yet
    for (uint32_t off = 0; off < (1u << 20); off += WL_XB_PAGE) {
        wl_xb_wr8(WL_XB_WINDOW | (phys + off), 1);
    }
    wl_xb_mem_stats(&after);
#if defined(__linux__) && !defined(__powerpc__)
    ASSERT(after.frames >= before.frames + 200u); // 256 pages touched (qemu-user does not report)
#endif
    ASSERT_EQ(WL_OK, wl_xb_phys_free(phys));
    ASSERT_EQ(0, wl_xb_rd8(WL_XB_WINDOW | phys)); // freed pages read as zero
    PASS();
}

TEST contiguous_allocator(void) {
    up();
    uint32_t a = 0, b = 0, c = 0;
    // top-down below a ceiling, aligned
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(0x82000u, 0, 0x3FFB000u, 0x4000u, &a));
    ASSERT(a + 0x82000u <= 0x3FFB000u && (a & 0x3FFFu) == 0);
    ASSERT_EQ_FMT(0x82000u, wl_xb_phys_size(a), "%x");
    // a pinned range: lowest = the address, highest = address + size - 1
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(0x345000u, 0x61000u, 0x61000u + 0x345000u - 1u, 0, &b));
    ASSERT_EQ_FMT(0x61000u, b, "%x");
    ASSERT_EQ(WL_ERR_NOMEM, wl_xb_phys_alloc(0x1000u, 0x61000u, 0x61FFFu, 0, &c)); // taken
    ASSERT_EQ(WL_ERR_NOMEM, wl_xb_phys_alloc(0x1000u, 0, 0x5FFFFu, 0, &c));          // the reserved low pages
    ASSERT_EQ(WL_OK, wl_xb_phys_free(b));
    ASSERT_EQ(WL_ERR_ARG, wl_xb_phys_free(b));
    ASSERT_EQ(WL_OK, wl_xb_phys_free(a));
    PASS();
}

TEST pinned_range_moves_user_pages(void) {
    up();
    uint32_t used = 0, free_pages = 0;
    wl_xb_phys_usage(&used, &free_pages);
    // fill the top of the RAM with user pages until one lands at a known physical page
    uint32_t va = 0;
    ASSERT_EQ(WL_OK, wl_xb_va_alloc(&va, WL_XB_PAGE, 0x00800000u, WL_XB_PROT_READWRITE));
    uint32_t p0 = 0;
    ASSERT(wl_xb_va_to_phys(va, &p0));
    wl_xb_wr32(va + 0x100u, 0xCAFEF00Du);
    // pin exactly that physical page
    uint32_t got = 0;
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(WL_XB_PAGE, p0, p0 + WL_XB_PAGE - 1u, 0, &got));
    ASSERT_EQ_FMT(p0, got, "%x");
    uint32_t p1 = 0;
    ASSERT(wl_xb_va_to_phys(va, &p1));
    ASSERT(p1 != p0);
    ASSERT_EQ_FMT(0xCAFEF00Du, wl_xb_rd32(va + 0x100u), "%08x");            // the user page kept its bytes
    ASSERT_EQ_FMT(0xCAFEF00Du, wl_xb_rd32(WL_XB_WINDOW | (p1 + 0x100u)), "%08x"); // at its new place
    ASSERT_EQ_FMT(0u, wl_xb_rd32(WL_XB_WINDOW | (p0 + 0x100u)), "%08x");      // the pinned page is clean
    WlXbMemStats st;
    wl_xb_mem_stats(&st);
    ASSERT(st.relocations >= 1u);
    ASSERT_EQ(WL_OK, wl_xb_phys_free(got));
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

TEST reservations_and_queries(void) {
    up();
    uint32_t va = 0x02000000u;
    ASSERT_EQ(WL_OK, wl_xb_va_reserve(&va, 0x40000u, 0, WL_XB_PROT_READWRITE));
    ASSERT_EQ_FMT(0x02000000u, va, "%08x");
    uint32_t again = 0x02010000u;
    ASSERT_EQ(WL_ERR_RANGE, wl_xb_va_reserve(&again, 0x1000u, 0, WL_XB_PROT_READWRITE));
    ASSERT(!wl_xb_range_ok(va, 4)); // reserved, not committed
    ASSERT_EQ(WL_OK, wl_xb_va_commit(va + 0x10000u, 0x2000u, WL_XB_PROT_READWRITE));
    ASSERT(wl_xb_range_ok(va + 0x10000u, 0x2000u));
    ASSERT(!wl_xb_range_ok(va + 0x10000u, 0x2001u));
    WlXbVaInfo info;
    ASSERT_EQ(WL_OK, wl_xb_va_query(va + 0x10800u, &info));
    ASSERT_EQ_FMT(0x1000u, info.state, "%x");
    ASSERT_EQ_FMT(va, info.allocation_base, "%08x");
    ASSERT_EQ_FMT(va + 0x10000u, info.base, "%08x");
    ASSERT_EQ_FMT(0x2000u, info.size, "%x");
    ASSERT_EQ(WL_OK, wl_xb_va_query(va, &info));
    ASSERT_EQ_FMT(0x2000u, info.state, "%x"); // reserved
    ASSERT_EQ_FMT(0x10000u, info.size, "%x");
    ASSERT_EQ(WL_OK, wl_xb_va_query(0x0F000000u, &info));
    ASSERT_EQ_FMT(0x10000u, info.state, "%x"); // free
    uint32_t old = 0;
    ASSERT_EQ(WL_OK, wl_xb_va_protect(va + 0x10000u, 0x1000u, WL_XB_PROT_READONLY, &old));
    ASSERT_EQ_FMT(WL_XB_PROT_READWRITE, old, "%x");
    ASSERT_EQ(WL_OK, wl_xb_va_decommit(va + 0x10000u, 0x1000u));
    ASSERT(!wl_xb_range_ok(va + 0x10000u, 4));
    ASSERT(wl_xb_range_ok(va + 0x11000u, 4));
    uint32_t size = 0;
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, &size));
    ASSERT_EQ_FMT(0x40000u, size, "%x");
    ASSERT(!wl_xb_range_ok(va + 0x11000u, 4));
    // the window: always guest memory inside the RAM, never past it or at a device
    ASSERT(wl_xb_range_ok(0x80000000u, 16));
    ASSERT(!wl_xb_range_ok(0x84000000u, 4));
    ASSERT(!wl_xb_range_ok(0xFD000000u, 4));
    PASS();
}

// ---- Device faults through the host's decoder ---------------------------------------------------

typedef struct {
    uint32_t regs[64];
    uint32_t reads, writes;
    uint32_t last_off, last_size;
} FakeDev;

static FakeDev g_dev;

static uint32_t fake_read(void *user, uint32_t off, unsigned size) {
    FakeDev *d = user;
    d->reads++;
    d->last_off = off;
    d->last_size = size;
    uint32_t w = d->regs[(off >> 2) & 63u];
    uint32_t shift = (off & 3u) * 8u;
    return size == 4 ? w : (w >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
}

static void fake_write(void *user, uint32_t off, uint32_t v, unsigned size) {
    FakeDev *d = user;
    d->writes++;
    d->last_off = off;
    d->last_size = size;
    uint32_t *w = &d->regs[(off >> 2) & 63u];
    uint32_t shift = (off & 3u) * 8u;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((size == 2 ? 0xFFFFu : 0xFFu) << shift);
    *w = (*w & ~mask) | ((v << (size == 4 ? 0 : shift)) & mask);
}

#define DEV_VA 0xFE400000u

TEST device_faults(void) {
    up();
    wl_xb_mmio_reset();
    memset(&g_dev, 0, sizeof g_dev);
    WlXbDevice dev = {"fake", DEV_VA, 0x1000u, fake_read, fake_write, &g_dev};
    ASSERT_EQ(WL_OK, wl_xb_mmio_register(&dev));
    g_dev.regs[1] = 0x89ABCDEFu;
    // through the accessors (the compiler's own loads and stores on the faulting address)
    ASSERT_EQ_FMT(0x89ABCDEFu, wl_xb_rd32(DEV_VA + 4u), "%08x");
    ASSERT_EQ_FMT(0xCDEFu, wl_xb_rd16(DEV_VA + 4u), "%04x");
    ASSERT_EQ_FMT(0xABu, wl_xb_rd8(DEV_VA + 6u), "%02x");
    wl_xb_wr32(DEV_VA + 8u, 0x01020304u);
    ASSERT_EQ_FMT(0x01020304u, g_dev.regs[2], "%08x");
    wl_xb_wr16(DEV_VA + 0xEu, 0xAA55u);
    ASSERT_EQ_FMT(0xAA550000u, g_dev.regs[3], "%08x");
    wl_xb_wr8(DEV_VA + 0x10u, 0x7Fu);
    ASSERT_EQ_FMT(0x7Fu, g_dev.regs[4], "%08x");
    // a read-modify-write through a volatile pointer (or-in a bit, as a kick-off loop does)
    volatile uint32_t *p = wl_xb_ptr(DEV_VA + 0x20u);
    g_dev.regs[8] = 0x1u;
    *p |= WL_XB_LE32(0x10000u);
    ASSERT_EQ_FMT(0x10001u, g_dev.regs[8], "%08x");
    // a compare against the register
    g_dev.regs[9] = 7;
    volatile uint32_t *q = wl_xb_ptr(DEV_VA + 0x24u);
    ASSERT(WL_XB_LE32(*q) == 7u);
    ASSERT(g_dev.reads >= 5u && g_dev.writes >= 4u);
    WlXbMmioStats st;
    wl_xb_mmio_stats(&st);
    ASSERT(st.reads >= 5u);
    PASS();
}

static sigjmp_buf g_crash_jmp;
static WlXbCrash g_crash;

static void crash_stop(const WlXbCrash *c) {
    g_crash = *c;
    siglongjmp(g_crash_jmp, 1);
}

TEST crash_on_missing_memory(void) {
    up();
    wl_xb_crash_set_stop(crash_stop);
    memset(&g_crash, 0, sizeof g_crash);
    if (sigsetjmp(g_crash_jmp, 1) == 0) {
        (void)wl_xb_rd32(0x03FF0000u); // never reserved
        FAILm("the access returned");
    }
    ASSERT_EQ_FMT(0x03FF0000u, g_crash.guest_va, "%08x");
    ASSERT(g_crash.reason != NULL && strstr(g_crash.reason, "not there") != NULL);
    memset(&g_crash, 0, sizeof g_crash);
    if (sigsetjmp(g_crash_jmp, 1) == 0) {
        (void)wl_xb_rd32(0xFE7F0000u); // a device address nobody claims
        FAILm("the access returned");
    }
    ASSERT_EQ_FMT(0xFE7F0000u, g_crash.guest_va, "%08x");
    wl_xb_crash_set_stop(NULL);
    PASS();
}

SUITE(mem_suite) {
    RUN_TEST(ea_transform);
    RUN_TEST(user_and_window_alias);
    RUN_TEST(lazy_backing);
    RUN_TEST(contiguous_allocator);
    RUN_TEST(pinned_range_moves_user_pages);
    RUN_TEST(reservations_and_queries);
    RUN_TEST(device_faults);
    RUN_TEST(crash_on_missing_memory);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mem_suite);
    GREATEST_MAIN_END();
}
