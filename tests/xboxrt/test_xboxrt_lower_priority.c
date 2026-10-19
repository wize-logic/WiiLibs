// SPDX-License-Identifier: MIT
// wl/xboxrt/thread: a lock holder that lowers its own priority (wl_xb_thread_set_priority on the
// running thread) below a ready thread hands the lock over at its next safe point: the attention is
// raised by the lowering itself, as no enqueue says so. With quiet safe points (the default) the
// holder's earlier safe points armed a window that left the attention down, so without it the ready
// thread waited for the window's end (or forever, with no service thread to end it); with the switch
// "fullsafe" (quiet points off) the attention stays up and the handover is the same. The waiters'
// quantum and starvation are far off, so only the priorities decide.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/thread.h"

// (the back edges of a try: few, so that a try on a loaded machine is seldom cut by other work that
// raises the attention, the platform's ticks among it; the tries many)
enum { EDGES = 200, TRIES = 2000 };

static volatile uint32_t g_ran[2];
static WlXbThread *g_boot;

static void waiter(WlXbThread *self, void *user) {
    (void)self;
    __atomic_store_n(&g_ran[(uintptr_t)user], 1u, __ATOMIC_SEQ_CST);
}

// `t` in the ready queue (its host thread started and queued itself), waited for a few seconds at most
static bool queued(WlXbThread *t) {
    for (unsigned ms = 0; ms < 10000u; ms++) {
        if (__atomic_load_n(&t->ready, __ATOMIC_SEQ_CST)) {
            return true;
        }
        wl_platform_thread_sleep_us(1000);
    }
    return false;
}

// backward jumps (RECOMP_BACKEDGE's test and call) until `which` has run, `most` at most: the safe
// points it took
static uint32_t back_edges_until_ran(unsigned which, uint32_t most) {
    uint32_t i, points = 0;

    for (i = 0; i < most && __atomic_load_n(&g_ran[which], __ATOMIC_SEQ_CST) == 0; i++) {
        if (g_xb_attention != 0) {
            wl_xb_safe_point();
            points++;
        }
    }
    return points;
}

// one round: a waiter below the holder queued, safe points that keep the lock (and with quiet points
// arm a window, the attention then down), the holder lowered below the waiter, which then has the
// lock at the next safe point
static enum greatest_test_res lowered_holder_hands_over(unsigned which, bool quiet) {
    WlXbThread *low = NULL;
    WlXbThreadStats before, start, after;
    uint32_t points = 0, tries, attention = 0;

    wl_xb_thread_set_quiet_points(quiet);
    wl_xb_thread_set_priority(g_boot, WL_XB_PRIORITY_NORMAL);
    ASSERT_EQ(WL_OK, wl_xb_thread_create_host(which ? "waiter1" : "waiter0", waiter, (void *)(uintptr_t)which, 0,
                                              WL_XB_PRIORITY_NORMAL - 1, &low));
    ASSERT(queued(low));
    // (below the holder: its turn does not come, the attention up or, quiet, a window armed and down;
    // a window only within 50 ms of the service thread's round, which this stands for, and held over
    // the back edges only when no round ended it among them, so again for a machine slow or loaded
    // enough to take that long: the round each try's own)
    wl_xb_thread_stats(&before);
    for (tries = 0; tries < TRIES; tries++) {
        wl_xb_thread_stats(&start);
        wl_xb_thread_quiet_due(wl_xb_time_us());
        g_xb_attention = 1;
        points = back_edges_until_ran(which, EDGES);
        attention = g_xb_attention;
        wl_xb_thread_stats(&after);
        if (!quiet || (after.quiet_windows == start.quiet_windows + 1u && points == 1u && attention == 0u)) {
            break;
        }
    }
    ASSERT(tries < TRIES);
    ASSERT_EQ(0u, g_ran[which]);
    // (quiet: the first safe point armed the window and the rest of the back edges took none)
    ASSERT_EQ(quiet ? 1u : (uint32_t)EDGES, points);
    ASSERT_EQ(before.safe_yields, after.safe_yields);
    if (quiet) {
        ASSERT(after.quiet_windows > start.quiet_windows);
        ASSERT_EQ(0u, attention);
    } else {
        ASSERT_EQ(after.quiet_windows, before.quiet_windows);
        ASSERT(attention != 0u);
    }
    ASSERT(wl_xb_lock_wanted());
    // the holder lowers itself below the waiter: the attention up at once, and the next safe point
    // gives the waiter the lock (it runs and ends before the holder has it back)
    wl_xb_thread_set_priority(g_boot, WL_XB_PRIORITY_NORMAL - 2);
    ASSERT(g_xb_attention != 0);
    ASSERT_EQ(1u, back_edges_until_ran(which, 1000u));
    ASSERT_EQ(1u, g_ran[which]);
    wl_xb_thread_stats(&after);
    ASSERT_EQ(before.safe_yields + 1u, after.safe_yields);
    PASS();
}

TEST a_holder_that_lowers_itself_hands_the_lock_over_at_its_next_safe_point(void) {
    WlXbMemConfig cfg;
    WlXbThreadConfig tcfg;

    wl_xb_mem_config_default(&cfg);
    wl_xb_thread_config_default(&tcfg);
    tcfg.quantum_ms = 600000u; // (no waiter's turn comes by its quantum or its starvation)
    tcfg.starve_ms = 600000u;
    ASSERT_EQ(WL_OK, wl_xb_mem_init(&cfg));
    ASSERT_EQ(WL_OK, wl_xb_threads_init(&tcfg, &g_boot));
    // quiet safe points (the default), then the switch fullsafe
    CHECK_CALL(lowered_holder_hands_over(0, true));
    CHECK_CALL(lowered_holder_hands_over(1, false));
    wl_xb_thread_set_quiet_points(true);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_holder_that_lowers_itself_hands_the_lock_over_at_its_next_safe_point);
    GREATEST_MAIN_END();
}
