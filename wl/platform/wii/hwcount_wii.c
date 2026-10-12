// SPDX-License-Identifier: MIT
// The Wii's machine for hwcount.h: the Broadway's performance monitor and GX's counters.
//
// The performance monitor (IBM's PowerPC 750CL user manual, performance monitor chapter): MMCR0
// (SPR 952) and MMCR1 (956) select the events, PMC1 to PMC4 (953, 954, 957, 958) count them.
// Libogc runs in supervisor mode, so the supervisor SPRs are used. Writing MMCR0 with DIS (bit 0)
// freezes the counters; they are zeroed and MMCR1 set while frozen, then MMCR0 with the events
// starts them, with no interrupt (ENINT 0). Dolphin keeps the SPRs as plain registers, so there
// they read back what was written and count nothing; the engine's self-test sees that.
//
// The GPU's counters: libogc queues the selections through the FIFO (GX_SetGPMetric,
// GX_InitXfRasMetric, GX_ClearVCacheMetric) and clears them by a register write
// (GX_ClearGPMetric); its readers are not all usable (GX_ReadGPMetric never stores through its
// second pointer: its first instruction after the loads overwrites r4, as the disassembly of
// libogc.a shows), so the values are read from the command processor's registers. The addresses
// are the ones libogc's own GX_ReadXfRasMetric and GX_ReadVCacheMetric read, and match Dolphin's
// CommandProcessor register map:
//   0xCC000040/42  XF_RASBUSY    (low half, high half)   GP counter 0 in the metric groups
//   0xCC000044/46  XF_CLKS
//   0xCC000048/4A  XF_WAIT_IN                            GP counter 1, by libogc's pairing
//   0xCC00004C/4E  XF_WAIT_OUT
//   0xCC000050/52  vertex cache checks, 54/56 misses, 58/5A stalls
//   0xCC000030/32  FIFO read/write distance (bytes the GP has yet to read)
// Which slot holds each metric of a pair is checked by the self-test and logged.
#include <ogc/gx.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <tuxedo/ppc/clock.h>
#include <tuxedo/ppc/intrinsics.h>
#include <tuxedo/ppc/spr.h>
#include <tuxedo/tick.h>

#include "gx_wii_internal.h"
#include "wl/platform/hwcount.h"

// The metric numbers hwcount.c uses are libogc's.
_Static_assert(GX_PERF0_XF_WAIT_IN == 3 && GX_PERF0_XF_XFRM_CLKS == 5 && GX_PERF0_XF_LIT_CLKS == 6 &&
                   GX_PERF0_XF_BOT_CLKS == 7 && GX_PERF0_TRIANGLES_PASSED == 13 && GX_PERF0_QUAD_NON0CVG == 28 &&
                   GX_PERF0_NONE == 35,
               "hwcount.c's GX_PERF0 numbers");
_Static_assert(GX_PERF1_TEXELS == 0 && GX_PERF1_TX_IDLE == 1 && GX_PERF1_TX_MEMSTALL == 3 && GX_PERF1_TC_MISS == 8 &&
                   GX_PERF1_VC_ALL_STALLS == 15 && GX_PERF1_VERTICES == 16 && GX_PERF1_NONE == 22,
               "hwcount.c's GX_PERF1 numbers");

#define CP_BASE 0xCC000000u
#define CP_SLOT0 0x40u
#define CP_VCACHE 0x50u
#define CP_FIFO_DISTANCE 0x30u

static inline uint16_t cp16(uint32_t offset) {
    return *(volatile uint16_t *)(CP_BASE + offset);
}

// A counter of two 16-bit halves, read again if the high half moved between the reads.
static uint32_t cp32(uint32_t lo_offset) {
    uint16_t hi = cp16(lo_offset + 2u);
    uint16_t lo = cp16(lo_offset);
    uint16_t hi2 = cp16(lo_offset + 2u);

    if (hi != hi2) {
        lo = cp16(lo_offset);
        hi = cp16(lo_offset + 2u);
    }
    return ((uint32_t)hi << 16) | lo;
}

static uint64_t wii_ticks(void *user) {
    (void)user;
    return gettime();
}

static void wii_cpu_start(void *user, const uint8_t sel[4], uint32_t threshold) {
    (void)user;
    PPCMtspr(MMCR0, WL_HWCOUNT_MMCR0_DIS); // frozen
    PPCMtspr(PMC1, 0);
    PPCMtspr(PMC2, 0);
    PPCMtspr(PMC3, 0);
    PPCMtspr(PMC4, 0);
    PPCMtspr(MMCR1, wl_hwcount_mmcr1(sel));
    PPCMtspr(MMCR0, wl_hwcount_mmcr0(sel, threshold)); // counting
    __asm__ __volatile__("isync" ::: "memory");
}

static void wii_cpu_read(void *user, uint32_t pmc[4]) {
    (void)user;
    pmc[0] = PPCMfspr(PMC1);
    pmc[1] = PPCMfspr(PMC2);
    pmc[2] = PPCMfspr(PMC3);
    pmc[3] = PPCMfspr(PMC4);
}

static void wii_cpu_stop(void *user) {
    (void)user;
    PPCMtspr(MMCR0, WL_HWCOUNT_MMCR0_DIS);
    PPCMtspr(MMCR1, 0);
}

static void wii_gp_start(void *user, const WlHwgpGroup *group, bool vcache) {
    (void)user;
    if (group->kind == WL_HWGP_XFRAS) {
        GX_InitXfRasMetric();
    } else {
        GX_SetGPMetric(group->perf0, group->perf1);
    }
    GX_ClearGPMetric();
    if (vcache) {
        GX_ClearVCacheMetric();
    }
    GX_Flush();
}

static void wii_gp_read(void *user, uint32_t slot[4], uint32_t vcache[3]) {
    uint32_t i;

    (void)user;
    for (i = 0; i < 4; i++) {
        slot[i] = cp32(CP_SLOT0 + 4u * i);
    }
    for (i = 0; i < 3; i++) {
        vcache[i] = cp32(CP_VCACHE + 4u * i);
    }
}

static void wii_gp_stop(void *user) {
    (void)user;
    GX_SetGPMetric(GX_PERF0_NONE, GX_PERF1_NONE);
    GX_ClearGPMetric();
    GX_Flush();
}

static uint32_t wii_fifo_fill(void *user) {
    (void)user;
    return cp32(CP_FIFO_DISTANCE);
}

static uint32_t wii_mark_token(void *user) {
    (void)user;
    return wl_gx_wii_mark_token();
}

static WlHwcountTokenFn g_token_fn;
static void *g_token_arg;
static GXDrawSyncCallback g_token_prev;

static void token_callback(u16 token) {
    if (g_token_fn != NULL) {
        g_token_fn(g_token_arg, token);
    }
    if (g_token_prev != NULL) {
        g_token_prev(token);
    }
}

static void wii_set_token_hook(void *user, WlHwcountTokenFn fn, void *arg) {
    (void)user;
    if (fn != NULL) {
        g_token_fn = fn;
        g_token_arg = arg;
        g_token_prev = GX_SetDrawSyncCallback(token_callback);
        if (g_token_prev == token_callback) {
            g_token_prev = NULL;
        }
    } else if (g_token_fn != NULL) {
        (void)GX_SetDrawSyncCallback(g_token_prev);
        g_token_fn = NULL;
        g_token_prev = NULL;
    }
}

static KTickTask g_fifo_task;
static WlHwcountTickFn g_fifo_fn;
static void *g_fifo_arg;
static bool g_fifo_running;

static void fifo_task(KTickTask *task) {
    (void)task;
    g_fifo_fn(g_fifo_arg);
}

static void wii_fifo_sampler(void *user, uint32_t hz, WlHwcountTickFn fn, void *arg) {
    (void)user;
    if (hz != 0 && fn != NULL) {
        if (g_fifo_running) {
            return;
        }
        g_fifo_fn = fn;
        g_fifo_arg = arg;
        g_fifo_running = true;
        KTickTaskStart(&g_fifo_task, fifo_task, PPC_TIMER_CLOCK / hz, PPC_TIMER_CLOCK / hz);
    } else if (g_fifo_running) {
        KTickTaskStop(&g_fifo_task);
        g_fifo_running = false;
    }
}

static void wii_log(void *user, const char *line) {
    (void)user;
    WL_LOG_I("hw", "%s", line);
}

static const WlHwcountHw g_hw = {
    .tb_hz = PPC_TIMER_CLOCK,
    .cpu_clock_per_tick = 12, // Broadway 729 MHz over the 60.75 MHz time base
    .gp_clock_per_tick = 4,   // Hollywood's 243 MHz bus over the time base (a quarter of the bus clock)
    .ticks = wii_ticks,
    .cpu_start = wii_cpu_start,
    .cpu_read = wii_cpu_read,
    .cpu_stop = wii_cpu_stop,
    .gp_start = wii_gp_start,
    .gp_read = wii_gp_read,
    .gp_stop = wii_gp_stop,
    .fifo_fill = wii_fifo_fill,
    .mark_token = wii_mark_token,
    .set_token_hook = wii_set_token_hook,
    .fifo_sampler = wii_fifo_sampler,
    .log = wii_log,
};

const WlHwcountHw *wl_hwcount_platform_hw(void) {
    return &g_hw;
}
