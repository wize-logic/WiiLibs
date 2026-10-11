// SPDX-License-Identifier: MIT
// The sampling profiler's interrupt on the Wii (profile.h). libogc's decrementer interrupt runs
// its tick tasks (tuxedo/tick.h) after saving the interrupted thread's registers into that
// thread's context (KThread::ctx: srr0, the link register, r1 and the rest), with interrupts off
// and on an exception stack of its own. A periodic tick task therefore sees exactly where the
// interrupted code was, and walks that thread's stack from the saved r1; the samples cost no
// system call and no allocation.
//
// What the samples can and cannot show:
// - Code that runs with interrupts masked (libogc's critical sections, IOS calls) is not
//   interrupted: the sample is taken when the interrupts come on again, so its time is credited to
//   the instruction after that.
// - The PC is exact on the console. Dolphin delivers the decrementer at the end of a translated
//   block of code, so there the PC is always at a block's edge (a branch target, usually).
#include <stdbool.h>
#include <stdio.h>
#include <tuxedo/ppc/clock.h>
#include <tuxedo/ppc/intrinsics.h>
#include <tuxedo/thread.h>
#include <tuxedo/tick.h>

#include <ogc/system.h>

#include "wl/platform/profile.h"

static KTickTask g_task;
static WlProfileRing *g_ring;
static uint32_t g_base_period;
static uint32_t g_random;
static volatile bool g_running;

// (in the decrementer interrupt: the interrupted thread's context is its KThread's)
static void sample(KTickTask *task) {
    const KThread *thread = KThreadGetSelf();

    wl_profile_ring_record(g_ring, PPCMftb(), (uint32_t)(uintptr_t)thread, thread->ctx.pc, thread->ctx.lr,
                           thread->ctx.gpr[1]);
    // (the next-but-one period: this one was scheduled before the call)
    task->period = wl_profile_jitter(&g_random, g_base_period);
}

uint32_t wl_profile_timebase_hz(void) {
    return PPC_TIMER_CLOCK;
}

WlResult wl_profile_start(WlProfileRing *ring, uint32_t hz) {
    if (ring == NULL || hz == 0 || hz > 10000u) {
        return WL_ERR_ARG;
    }
    if (g_running) {
        return WL_ERR_STATE;
    }
    g_ring = ring;
    g_base_period = PPC_TIMER_CLOCK / hz;
    g_random = PPCMftb() | 1u;
    g_running = true;
    KTickTaskStart(&g_task, sample, g_base_period, g_base_period);
    return WL_OK;
}

void wl_profile_stop(void) {
    if (!g_running) {
        return;
    }
    KTickTaskStop(&g_task); // (interrupts off while it takes the task off the list)
    g_running = false;
}

bool wl_profile_running(void) {
    return g_running;
}

static void report_sink(void *user, const char *line) {
    (void)user;
    SYS_Report("%s\n", line);
}

uint32_t wl_profile_dump_log(const WlProfileRing *ring, const WlProfileDumpInfo *info) {
    return wl_profile_dump(ring, info, report_sink, NULL);
}
