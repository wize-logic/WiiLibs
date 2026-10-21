// SPDX-License-Identifier: MIT
// Mirrored device pages (mmio.h): a page of a fake device's registers read as memory (no fault, no
// device call), a write still the device's and the written word read again into the copy, a kept
// store answered without the device (wl_xb_mmio_fast_write, the WBC flush's form), the fault
// handler's answers (wl_xb_mmio_fast_read, wl_xb_ppc_fast for the PowerPC forms), and the check
// mode (the page unmapped, every read compared with the copy).
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/ppc_decode.h"
#include "wl/xboxrt/thread.h"

#define DEV_VA 0xFE400000u
#define WBC_OFF 0x10u
#define WBC_FLUSH 0x10000u

typedef struct { // (volatile: the fault handler writes them behind the compiler's back)
    volatile uint32_t regs[0x2000 / 4];
    volatile uint32_t reads, writes;
} FakeDev;

static FakeDev g_dev;
static bool g_up;

static uint32_t fake_read(void *user, uint32_t off, unsigned size) {
    FakeDev *d = user;
    d->reads++;
    uint32_t w = d->regs[(off & 0x1FFFu) >> 2];
    uint32_t shift = (off & 3u) * 8u;
    return size == 4 ? w : (w >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
}

static void fake_write(void *user, uint32_t off, uint32_t v, unsigned size) {
    FakeDev *d = user;
    d->writes++;
    volatile uint32_t *w = &d->regs[(off & 0x1FFFu) >> 2];
    uint32_t shift = (off & 3u) * 8u;
    uint32_t mask = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << shift;
    *w = (*w & ~mask) | ((v << shift) & mask);
    if ((off & ~3u) == WBC_OFF) {
        *w &= ~WBC_FLUSH; // (the flush done at once)
    }
}

static const WlXbMmioFastStore k_wbc = {WBC_OFF, ~(uint32_t)WBC_FLUSH, (uint32_t *)&g_dev.regs[WBC_OFF / 4]};

static bool up(bool check) {
    if (!g_up) {
        WlXbMemConfig cfg;
        wl_xb_mem_config_default(&cfg);
        g_up = wl_xb_mem_init(&cfg) == WL_OK && wl_xb_fault_install() == WL_OK;
    }
    wl_xb_mmio_reset();
    memset((void *)&g_dev, 0, sizeof g_dev);
    WlXbDevice dev = {"fake", DEV_VA, 0x2000u, fake_read, fake_write, &g_dev};
    wl_xb_mmio_set_mirrors(true, check);
    return g_up && wl_xb_mmio_register(&dev) == WL_OK;
}

TEST reads_are_memory(void) {
    ASSERT(up(false));
    g_dev.regs[1] = 0x89ABCDEFu;
    g_dev.regs[WBC_OFF / 4] = 0x5u;
    ASSERT_EQ(WL_ERR_ARG, wl_xb_mmio_mirror(DEV_VA + 0x10u, NULL, 0)); // not page aligned
    ASSERT_EQ(WL_OK, wl_xb_mmio_mirror(DEV_VA, &k_wbc, 1u));
    ASSERT_EQ(WL_ERR_ARG, wl_xb_mmio_mirror(DEV_VA, NULL, 0)); // twice
    uint32_t reads = g_dev.reads, writes = g_dev.writes;
    WlXbMmioStats before, after;
    wl_xb_mmio_stats(&before);
    ASSERT_EQ_FMT(0x89ABCDEFu, wl_xb_rd32(DEV_VA + 4u), "%08x");
    ASSERT_EQ_FMT(0xCDEFu, wl_xb_rd16(DEV_VA + 4u), "%04x");
    ASSERT_EQ_FMT(0xABu, wl_xb_rd8(DEV_VA + 6u), "%02x");
    ASSERT_EQ_FMT(0x5u, wl_xb_rd32(DEV_VA + WBC_OFF), "%08x");
    wl_xb_mmio_stats(&after);
    ASSERT_EQ(reads, g_dev.reads); // no fault, no device call
    ASSERT_EQ(before.reads, after.reads);
    // a write is the device's (or, for the kept store, answered without it); the copy follows
    wl_xb_wr32(DEV_VA + 8u, 0x01020304u);
    ASSERT_EQ_FMT(0x01020304u, g_dev.regs[2], "%08x");
    ASSERT_EQ(writes + 1u, g_dev.writes);
    ASSERT_EQ_FMT(0x01020304u, wl_xb_rd32(DEV_VA + 8u), "%08x");
    wl_xb_wr16(DEV_VA + 0xEu, 0xAA55u);
    ASSERT_EQ_FMT(0xAA550000u, wl_xb_rd32(DEV_VA + 0xCu), "%08x");
    // the kick's flush: read, or-in the flush bit, poll until it clears
    volatile uint32_t *wbc = wl_xb_ptr(DEV_VA + WBC_OFF);
    *wbc = *wbc | WL_XB_LE32(WBC_FLUSH);
    ASSERT_EQ_FMT(0x5u, g_dev.regs[WBC_OFF / 4], "%08x");
    ASSERT_EQ_FMT(0x5u, WL_XB_LE32(*wbc), "%08x");
    // (an x86 host's or to memory reads through the device: its store faults; a PowerPC's load does not)
    reads = g_dev.reads;
    // the next page is not mirrored: a read faults to the device
    g_dev.regs[0x1000 / 4] = 77u;
    ASSERT_EQ(77u, wl_xb_rd32(DEV_VA + 0x1000u));
    ASSERT_EQ(reads + 1u, g_dev.reads);
    // a change the device made by itself: the word read again on request
    g_dev.regs[3] = 0x33u;
    wl_xb_mmio_mirror_refresh(DEV_VA + 0xCu);
    ASSERT_EQ_FMT(0x33u, wl_xb_rd32(DEV_VA + 0xCu), "%08x");
    PASS();
}

TEST fast_answers(void) {
    ASSERT(up(false));
    g_dev.regs[2] = 0x11223344u;
    uint32_t v = 0;
    ASSERT(!wl_xb_mmio_fast_read(DEV_VA + 8u, 4, &v)); // nothing mirrored yet
    ASSERT_EQ(WL_OK, wl_xb_mmio_mirror(DEV_VA, &k_wbc, 1u));
    ASSERT(wl_xb_mmio_fast_read(DEV_VA + 8u, 4, &v));
    ASSERT_EQ_FMT(0x11223344u, v, "%08x");
    ASSERT(wl_xb_mmio_fast_read(DEV_VA + 0xAu, 2, &v));
    ASSERT_EQ_FMT(0x1122u, v, "%04x");
    ASSERT(!wl_xb_mmio_fast_read(DEV_VA + 9u, 2, &v)); // misaligned: the device's
    ASSERT(!wl_xb_mmio_fast_read(DEV_VA + 0x1008u, 4, &v));
    ASSERT(wl_xb_mmio_fast_write(DEV_VA + WBC_OFF, 0x10007u, 4));
    ASSERT_EQ_FMT(0x7u, g_dev.regs[WBC_OFF / 4], "%08x");
    ASSERT(wl_xb_mmio_fast_read(DEV_VA + WBC_OFF, 4, &v));
    ASSERT_EQ_FMT(0x7u, v, "%08x");
    ASSERT(!wl_xb_mmio_fast_write(DEV_VA + 8u, 1u, 4)); // not a kept store: the device's
    ASSERT(!wl_xb_mmio_fast_write(DEV_VA + WBC_OFF, 1u, 2));
    g_xb_attention = 1; // a safe point's work is due: the device's path
    ASSERT(!wl_xb_mmio_fast_read(DEV_VA + 8u, 4, &v));
    g_xb_attention = 0;
    // the PowerPC forms, as the Wii's handler answers them: lwbrx (the generated code's), lwz, lhbrx,
    // lwzu, stwbrx and stw of the kept store
    uint32_t gpr[32] = {0};
    WlXbPpcAccess a;
    gpr[4] = 0x1000u;
    gpr[5] = 8u;
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(31u << 26 | 3u << 21 | 4u << 16 | 5u << 11 | 534u << 1, &a));
    ASSERT(wl_xb_ppc_fast(&a, gpr, 0x1008u, DEV_VA + 8u));
    ASSERT_EQ_FMT(0x11223344u, gpr[3], "%08x");
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(32u << 26 | 6u << 21 | 4u << 16 | 8u, &a));
    ASSERT(wl_xb_ppc_fast(&a, gpr, 0x1008u, DEV_VA + 8u));
    ASSERT_EQ_FMT(0x44332211u, gpr[6], "%08x");
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(31u << 26 | 7u << 21 | 4u << 16 | 5u << 11 | 790u << 1, &a));
    ASSERT(wl_xb_ppc_fast(&a, gpr, 0x1008u, DEV_VA + 8u));
    ASSERT_EQ_FMT(0x3344u, gpr[7], "%08x");
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(33u << 26 | 8u << 21 | 4u << 16 | 8u, &a));
    ASSERT(wl_xb_ppc_fast(&a, gpr, 0x1008u, DEV_VA + 8u));
    ASSERT_EQ_FMT(0x1008u, gpr[4], "%08x"); // the update form's base
    gpr[4] = 0x1000u;
    gpr[9] = 0x10009u;
    gpr[10] = WBC_OFF;
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(31u << 26 | 9u << 21 | 4u << 16 | 10u << 11 | 662u << 1, &a));
    ASSERT(wl_xb_ppc_fast(&a, gpr, 0x1010u, DEV_VA + WBC_OFF));
    ASSERT_EQ_FMT(0x9u, g_dev.regs[WBC_OFF / 4], "%08x");
    gpr[9] = 0x0A000100u; // stw: the big-endian register's bytes are the little-endian 0x0001000A
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(36u << 26 | 9u << 21 | 4u << 16 | WBC_OFF, &a));
    ASSERT(wl_xb_ppc_fast(&a, gpr, 0x1010u, DEV_VA + WBC_OFF));
    ASSERT_EQ_FMT(0xAu, g_dev.regs[WBC_OFF / 4], "%08x");
    gpr[9] = 1u;
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(36u << 26 | 9u << 21 | 4u << 16 | 8u, &a));
    ASSERT(!wl_xb_ppc_fast(&a, gpr, 0x1008u, DEV_VA + 8u)); // a store the device must see
    WlXbMmioStats st;
    wl_xb_mmio_stats(&st);
    ASSERT(st.fast_reads >= 6u && st.fast_writes >= 3u);
    PASS();
}

TEST check_mode(void) {
    ASSERT(up(true));
    g_dev.regs[1] = 0x42u;
    ASSERT_EQ(WL_OK, wl_xb_mmio_mirror(DEV_VA, &k_wbc, 1u));
    WlXbMmioStats a, b;
    wl_xb_mmio_stats(&a);
    uint32_t reads = g_dev.reads;
    ASSERT_EQ(0x42u, wl_xb_rd32(DEV_VA + 4u)); // the device's (a fault), compared with the copy
    ASSERT_EQ(reads + 1u, g_dev.reads);
    uint32_t v;
    ASSERT(!wl_xb_mmio_fast_read(DEV_VA + 4u, 4, &v)); // no answers in the check
    wl_xb_wr32(DEV_VA + 4u, 0x43u);
    ASSERT_EQ(0x43u, wl_xb_rd32(DEV_VA + 4u));
    wl_xb_mmio_stats(&b);
    ASSERT_EQ(a.check_reads + 2u, b.check_reads);
    ASSERT_EQ(a.check_misses, b.check_misses);
    g_dev.regs[1] = 0x44u; // behind the copy's back
    ASSERT_EQ(0x44u, wl_xb_rd32(DEV_VA + 4u));
    wl_xb_mmio_stats(&b);
    ASSERT_EQ(a.check_misses + 1u, b.check_misses);
    wl_xb_mmio_set_mirrors(true, false);
    PASS();
}

TEST off_mirrors_nothing(void) {
    ASSERT(up(false));
    wl_xb_mmio_set_mirrors(false, false);
    ASSERT_EQ(WL_OK, wl_xb_mmio_mirror(DEV_VA + 0x1000u, NULL, 0));
    uint32_t v;
    ASSERT(!wl_xb_mmio_fast_read(DEV_VA + 0x1000u, 4, &v));
    uint32_t reads = g_dev.reads;
    (void)wl_xb_rd32(DEV_VA + 0x1004u);
    ASSERT_EQ(reads + 1u, g_dev.reads);
    wl_xb_mmio_set_mirrors(true, false);
    PASS();
}

SUITE(mirror_suite) {
    RUN_TEST(reads_are_memory);
    RUN_TEST(fast_answers);
    RUN_TEST(check_mode);
    RUN_TEST(off_mirrors_nothing);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mirror_suite);
    GREATEST_MAIN_END();
}
