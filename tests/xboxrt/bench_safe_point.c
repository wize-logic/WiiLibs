// SPDX-License-Identifier: MIT
// A benchmark of a safe point (wl_xb_safe_point, a recompiled backward jump's call while
// g_xb_attention is set) when nothing is due: another thread waits in the ready queue below the
// holder (which keeps the attention up, so every backward jump calls in), its quantum and its
// starvation far off, and the level sources a title has (the USB host controllers, the GPU, the
// APU and the AC'97) none asserted. Prints the safe points and the yields (none).
//
//     bench_safe_point [points] [full]
//
// "full" runs every safe point's whole look (the switch "fullsafe": wl_xb_thread_set_quiet_points
// off); otherwise the first safe point arms a quiet window (no service thread here ends it) and the
// rest cost the backward jump's load and compare. Count instructions with tools/qemu_icount.py on a ppc-be build (--base with fewer points for
// the cost of one).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/thread.h"

static uint32_t g_reg[5][4]; // a device's enable and status words (each line's source looks at two)
static volatile uint32_t g_ran;

static bool level(void *user) {
    const uint32_t *r = user;
    return (r[0] & r[1]) != 0;
}

static void waiter(WlXbThread *self, void *user) {
    (void)self;
    (void)user;
    g_ran = 1;
}

int main(int argc, char **argv) {
    uint32_t points = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 100000u;
    bool full = argc > 2 && strcmp(argv[2], "full") == 0;
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    WlXbThreadConfig tcfg;
    wl_xb_thread_config_default(&tcfg);
    tcfg.quantum_ms = 600000u; // (the waiter's turn never comes in the run)
    tcfg.starve_ms = 600000u;
    WlXbThread *boot = NULL, *low = NULL;
    uint32_t va = 0x00400000u;
    if (wl_xb_mem_init(&cfg) != WL_OK || wl_xb_va_reserve(&va, 0x10000u, 0, WL_XB_PROT_READWRITE) != WL_OK ||
        wl_xb_va_commit(va, WL_XB_PAGE, WL_XB_PROT_READWRITE) != WL_OK || wl_xb_threads_init(&tcfg, &boot) != WL_OK ||
        wl_xb_thread_create_host("waiter", waiter, NULL, 0, WL_XB_PRIORITY_NORMAL - 1, &low) != WL_OK) {
        fprintf(stderr, "bench_safe_point: no guest memory or threads\n");
        return 2;
    }
#ifdef WL_XB_HAS_QUIET_POINTS
    wl_xb_thread_set_quiet_points(!full);
#else
    (void)full;
#endif
    static const uint32_t lines[5] = {WL_XB_IRQ_USB0, WL_XB_IRQ_GPU, WL_XB_IRQ_APU, WL_XB_IRQ_AC97, WL_XB_IRQ_USB1};
    for (unsigned i = 0; i < 5; i++) {
        g_reg[i][0] = 0xFFu; // enabled, nothing raised
        wl_xb_irq_set_level(lines[i], level, g_reg[i]);
    }
    wl_xb_service_set_tick_va(va);
    wl_platform_thread_sleep_us(50000); // (the waiter's host thread queues itself)
#ifdef WL_XB_HAS_QUIET_POINTS
    wl_xb_thread_quiet_due(wl_xb_time_us()); // (a service thread's round: a window may be armed)
#endif
    g_xb_attention = 1;
    for (uint32_t i = 0; i < points; i++) {
        if (g_xb_attention != 0) { // (RECOMP_BACKEDGE)
            wl_xb_safe_point();
        }
    }
    WlXbThreadStats st;
    wl_xb_thread_stats(&st);
    printf("bench_safe_point: %u points, %u looked, %u yields, waiter %s\n", (unsigned)points, (unsigned)st.safe_points,
           (unsigned)st.safe_yields, g_ran ? "ran" : "waits");
#ifdef WL_XB_HAS_QUIET_POINTS
    bool wanted = wl_xb_lock_wanted(); // (a quiet window leaves the attention down)
#else
    bool wanted = g_xb_attention != 0;
#endif
    return st.safe_yields == 0 && g_ran == 0 && wanted ? 0 : 1;
}
