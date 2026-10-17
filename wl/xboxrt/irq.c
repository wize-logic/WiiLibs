// SPDX-License-Identifier: MIT
// Interrupt lines, the service thread, KeTickCount and the vertical blank (irq.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/mem.h"
#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"

#include "wl/platform/profile.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/thread.h"

#define TAG "xbirq"
#define MAX_ROUNDS 8u // handler runs per service call before it gives the lock back

static WlXbServiceFn g_handler;
static void *g_handler_user;
static WlXbVblankFn g_vblank_fn;
static void *g_vblank_user;
static struct {
    WlXbTickFn fn;
    void *user;
    uint64_t period_us, next_us; // next_us: the service thread's
    volatile uint32_t due;       // set by the service thread, taken by the work
} g_tick[WL_XB_MAX_TICKS];
static uint32_t g_tick_count;
static WlXbServiceConfig g_cfg;
#define MAX_VBLANK_HOOKS 8u
#define SVC_STACK (256u << 10)  // the service thread's host stack
#define RELAY_STACK (16u << 10) // the retrace relay's (the Wii)
static struct {
    WlXbVblankFn fn;
    void *user;
} g_vblank_hooks[MAX_VBLANK_HOOKS];
static uint32_t g_vblank_hook_count;
static volatile uint32_t g_vblank_count;
// threads in wl_xb_vblank_wait (the guest lock guards it)
#define MAX_VBLANK_WAITERS 4u
static WlXbThread *g_vblank_waiters[MAX_VBLANK_WAITERS];
static uint32_t g_vblank_waits, g_vblank_wait_us; // since the last status line

static WlPlatformMutex g_sm; // g_pending, g_deadline, the service thread's sleep
static WlPlatformCond g_sc;
static volatile uint32_t g_pending;
static volatile uint32_t g_lines;
static volatile uint64_t g_deadline;
// The service thread waits for the guest lock: its loop, which marks the ticks due, does not run,
// so the holder's polls mark them (one of the two at a time: ticks_due is not shared).
static volatile uint32_t g_svc_waiting;
static struct {
    WlXbIrqLevelFn fn;
    void *user;
} g_level[WL_XB_IRQ_LINES];
// the lines with a level source, in line order: wl_xb_irq_lines runs at every device access, and
// looks at these (four or five) instead of all 32
static uint8_t g_level_line[WL_XB_IRQ_LINES];
static uint32_t g_level_n;
static WlXbIrqMaskFn g_mask_fn;
static void *g_mask_user;
static bool g_in_service;
static volatile bool g_running;
static bool g_started;
static WlXbThread *g_svc;
static WlPlatformThread g_svc_host, g_relay_host;
static void *g_svc_stack, *g_relay_stack;
static WlPlatformSemaphore g_retrace_sem;
static volatile uint32_t g_tick_va;
static uint64_t g_tick_start_us;
static uint64_t g_wall_start_us;
static WlXbServiceStats g_stats;
static uint32_t g_work_esp; // the top of the service thread's guest stack: where the work runs
static uint64_t g_det_vblank_us; // the deterministic mode: the next vertical blank (virtual clock)
static uint64_t g_vblank_period_us;
static WlXbStatusFn g_status_hook;
// Vertical blanks raised and not yet run: each is an interrupt of its own (Halo's game clock counts
// its vblank callback's calls), however late the service gets to them; at most a second's worth.
#define VBLANKS_OWED_MAX 60u
static volatile uint32_t g_vblanks_owed;

void wl_xb_service_config_default(WlXbServiceConfig *cfg) {
    cfg->vblank_hz_milli = 59940;
#if defined(__wii__)
    cfg->tick_ms = 2;
#else
    cfg->tick_ms = 1;
#endif
    cfg->vblank = true;
}

void wl_xb_service_set_handler(WlXbServiceFn fn, void *user) {
    g_handler_user = user;
    g_handler = fn;
}

void wl_xb_service_set_vblank(WlXbVblankFn fn, void *user) {
    g_vblank_user = user;
    g_vblank_fn = fn;
}

WlResult wl_xb_service_add_vblank_hook(WlXbVblankFn fn, void *user) {
    if (fn == NULL) {
        return WL_ERR_ARG;
    }
    if (g_vblank_hook_count == MAX_VBLANK_HOOKS) {
        return WL_ERR_LIMIT;
    }
    g_vblank_hooks[g_vblank_hook_count].user = user;
    g_vblank_hooks[g_vblank_hook_count].fn = fn;
    g_vblank_hook_count++;
    return WL_OK;
}

uint32_t wl_xb_vblank_count(void) {
    return g_vblank_count;
}

bool wl_xb_vblank_wait(uint32_t timeout_ms) {
    WlXbThread *self = wl_xb_current();
    if (self == NULL || self->is_service || g_in_service) {
        return false;
    }
    uint32_t slot = 0;
    while (slot < MAX_VBLANK_WAITERS && g_vblank_waiters[slot] != NULL) {
        slot++;
    }
    if (slot == MAX_VBLANK_WAITERS) {
        return false;
    }
    g_vblank_waiters[slot] = self;
    uint32_t count = g_vblank_count;
    uint64_t t0 = wl_xb_time_us();
    // (a vertical blank between here and the wait leaves the thread woken: the wait returns at once)
    bool woke = wl_xb_wait(self, timeout_ms);
    if (g_vblank_waiters[slot] == self) {
        g_vblank_waiters[slot] = NULL;
    }
    g_vblank_waits++;
    g_vblank_wait_us += (uint32_t)(wl_xb_time_us() - t0);
    return woke && g_vblank_count != count;
}

// The vertical blank's round, after the hooks: the threads waiting for it go on.
static void vblank_wake_waiters(void) {
    for (uint32_t i = 0; i < MAX_VBLANK_WAITERS; i++) {
        WlXbThread *t = g_vblank_waiters[i];
        if (t != NULL) {
            g_vblank_waiters[i] = NULL;
            wl_xb_wake(t);
        }
    }
}

WlResult wl_xb_service_add_tick(WlXbTickFn fn, void *user, uint32_t period_us) {
    if (fn == NULL || period_us == 0) {
        return WL_ERR_ARG;
    }
    if (g_tick_count >= WL_XB_MAX_TICKS) {
        return WL_ERR_LIMIT;
    }
    g_tick[g_tick_count].fn = fn;
    g_tick[g_tick_count].user = user;
    g_tick[g_tick_count].period_us = period_us;
    g_tick[g_tick_count].next_us = wl_xb_time_us() + period_us;
    __atomic_store_n(&g_tick_count, g_tick_count + 1u, __ATOMIC_RELEASE);
    return WL_OK;
}

// The service thread: marks the ticks whose time came; returns the earliest next one (or `wake`).
static uint64_t ticks_due(uint64_t now, uint64_t wake) {
    uint32_t n = __atomic_load_n(&g_tick_count, __ATOMIC_ACQUIRE);
    n = n < WL_XB_MAX_TICKS ? n : WL_XB_MAX_TICKS; // (never more; it tells the compiler so)
    for (uint32_t i = 0; i < n; i++) {
        if (now >= g_tick[i].next_us) {
            g_tick[i].next_us += g_tick[i].period_us;
            if (g_tick[i].next_us < now) {
                g_tick[i].next_us = now + g_tick[i].period_us; // fell behind: no burst
            }
            __atomic_store_n(&g_tick[i].due, 1u, __ATOMIC_RELEASE);
            __atomic_fetch_or(&g_pending, WL_XB_SVC_DEVICE, __ATOMIC_SEQ_CST);
            g_xb_attention = 1;
            g_xb_det_work = 1;
        }
        if (g_tick[i].next_us < wake) {
            wake = g_tick[i].next_us;
        }
    }
    return wake;
}

// The work: the ticks marked due, lock held.
static void run_ticks(void) {
    uint32_t n = __atomic_load_n(&g_tick_count, __ATOMIC_ACQUIRE);
    n = n < WL_XB_MAX_TICKS ? n : WL_XB_MAX_TICKS; // (never more; it tells the compiler so)
    for (uint32_t i = 0; i < n; i++) {
        if (__atomic_exchange_n(&g_tick[i].due, 0u, __ATOMIC_ACQ_REL) != 0) {
            g_tick[i].fn(g_tick[i].user);
        }
    }
}

bool wl_xb_in_service(void) {
    return g_in_service;
}

// KeTickCount at now (us of wl_xb_time_us)
static uint32_t tick_ms_at(uint64_t now) {
    uint64_t us = now - g_tick_start_us;
    // a 32-bit division for the first 71 minutes: on the Wii this runs at every poll (update_tick),
    // and libgcc's 64-bit __udivdi3 took 0.4% of a10's time there
    if ((us >> 32) == 0) {
        return (uint32_t)us / 1000u;
    }
    return (uint32_t)(us / 1000u);
}

uint32_t wl_xb_tick_ms(void) {
    return tick_ms_at(wl_xb_time_us());
}

// written at every poll, not only when its millisecond moves on: a checkpoint snapshot's load puts
// back the guest RAM it is in
static void update_tick_at(uint64_t now) {
    uint32_t va = g_tick_va;
    if (va != 0) {
        wl_xb_wr32_atomic(va, tick_ms_at(now));
    }
}

static void update_tick(void) {
    update_tick_at(wl_xb_time_us());
}

void wl_xb_service_set_tick_va(uint32_t va) {
    if (g_tick_start_us == 0) {
        g_tick_start_us = wl_xb_time_us();
    }
    // the service thread writes it without the guest lock (update_tick): its page must never wait
    // for the swap file
    if (va != 0 && wl_xb_mem_pin(va, 4) != WL_OK) {
        WL_LOG_E(TAG, "cannot pin KeTickCount's page (0x%08x)", (unsigned)va);
    }
    g_tick_va = va;
    update_tick();
}

static void wake_service(void) {
    if (g_started) {
        wl_platform_mutex_lock(g_sm);
        wl_platform_cond_signal(g_sc);
        wl_platform_mutex_unlock(g_sm);
    }
}

// A vertical blank is raised, after `skipped` periods a late service thread did not raise: all are
// owed (the caller sets WL_XB_SVC_VBLANK).
static void note_vblank(uint32_t skipped) {
    uint32_t n = 1u + skipped;
    g_stats.vblanks_raised += n;
    g_stats.vblanks_skipped += skipped;
    if (g_pending & WL_XB_SVC_VBLANK) {
        g_stats.vblanks_merged++;
    }
    uint32_t owed = __atomic_load_n(&g_vblanks_owed, __ATOMIC_SEQ_CST);
    if (owed + n > VBLANKS_OWED_MAX) {
        g_stats.vblanks_dropped += owed + n - VBLANKS_OWED_MAX;
        n = owed < VBLANKS_OWED_MAX ? VBLANKS_OWED_MAX - owed : 0u;
    }
    __atomic_fetch_add(&g_vblanks_owed, n, __ATOMIC_SEQ_CST);
}

void wl_xb_service_set_status_hook(WlXbStatusFn fn) {
    g_status_hook = fn;
}

void wl_xb_service_raise(uint32_t reasons) {
    if (reasons & WL_XB_SVC_VBLANK) {
        note_vblank(0);
    }
    g_xb_attention = 1;
    g_xb_det_work = 1;
    if (g_started) {
        wl_platform_mutex_lock(g_sm);
        g_pending |= reasons;
        wl_platform_cond_signal(g_sc);
        wl_platform_mutex_unlock(g_sm);
    } else {
        __atomic_fetch_or(&g_pending, reasons, __ATOMIC_SEQ_CST);
    }
}

void wl_xb_irq_assert(uint32_t line) {
    if (line < WL_XB_IRQ_LINES) {
        __atomic_fetch_or(&g_lines, 1u << line, __ATOMIC_SEQ_CST);
        g_xb_attention = 1;
        g_xb_det_work = 1;
        wl_xb_service_raise(WL_XB_SVC_IRQ);
    }
}

void wl_xb_irq_deassert(uint32_t line) {
    if (line < WL_XB_IRQ_LINES) {
        __atomic_fetch_and(&g_lines, ~(1u << line), __ATOMIC_SEQ_CST);
    }
}

void wl_xb_irq_set_level(uint32_t line, WlXbIrqLevelFn pending, void *user) {
    if (line < WL_XB_IRQ_LINES) {
        g_level[line].user = user;
        g_level[line].fn = pending;
        uint32_t n = 0;
        for (uint32_t i = 0; i < WL_XB_IRQ_LINES; i++) {
            if (g_level[i].fn != NULL) {
                g_level_line[n++] = (uint8_t)i;
            }
        }
        g_level_n = n;
    }
}

uint32_t wl_xb_irq_lines(void) {
    uint32_t lines = g_lines;
    for (uint32_t k = 0, n = g_level_n; k < n; k++) {
        const uint32_t i = g_level_line[k];
        const WlXbIrqLevelFn fn = g_level[i].fn; // (NULL: being taken away)
        if (fn != NULL && fn(g_level[i].user)) {
            lines |= 1u << i;
        }
    }
    return lines;
}

void wl_xb_irq_set_mask(WlXbIrqMaskFn fn, void *user) {
    g_mask_user = user;
    g_mask_fn = fn;
}

uint32_t wl_xb_irq_deliverable(void) {
    uint32_t lines = wl_xb_irq_lines();
    return lines != 0 && g_mask_fn != NULL ? lines & g_mask_fn(g_mask_user) : lines;
}

void wl_xb_service_set_deadline(uint64_t when_us) {
    g_deadline = when_us;
    wl_xb_det_note_deadline(when_us);
    wake_service();
}

// The work itself: the lock held, not nested.
static void run_service(void) {
    if (g_in_service || g_handler == NULL) {
        return;
    }
    g_in_service = true;
    WlXbCpu saved;
    wl_xb_cpu_save(&saved);
    // the work's guest stack: the service thread's, free whenever someone else runs the work (the
    // service thread runs guest code only in here, and this never nests)
    uint32_t work_esp = g_work_esp;
    for (uint32_t round = 0; round < MAX_ROUNDS; round++) {
        uint32_t reasons = __atomic_exchange_n(&g_pending, 0u, __ATOMIC_SEQ_CST);
        uint64_t deadline = g_deadline;
        if (deadline != 0 && wl_xb_time_us() >= deadline) {
            g_deadline = 0;
            reasons |= WL_XB_SVC_TIMER;
        }
        if (reasons & WL_XB_SVC_VBLANK) {
            // one a round: the next round (or the next service) runs the others, each its interrupt
            if (__atomic_load_n(&g_vblanks_owed, __ATOMIC_SEQ_CST) != 0u &&
                __atomic_sub_fetch(&g_vblanks_owed, 1u, __ATOMIC_SEQ_CST) != 0u) {
                __atomic_fetch_or(&g_pending, WL_XB_SVC_VBLANK, __ATOMIC_SEQ_CST);
            }
            g_stats.vblanks++;
            g_vblank_count++;
            if (g_vblank_fn != NULL) {
                g_vblank_fn(g_vblank_user);
            }
            for (uint32_t i = 0; i < g_vblank_hook_count; i++) {
                g_vblank_hooks[i].fn(g_vblank_hooks[i].user);
            }
            vblank_wake_waiters();
        }
        if (reasons & WL_XB_SVC_DEVICE) {
            run_ticks();
        }
        if (wl_xb_irq_deliverable() != 0) {
            reasons |= WL_XB_SVC_IRQ; // (a line the holder's IRQL masks waits for it to come down)
        }
        if (reasons == 0) {
            break;
        }
        g_stats.handler_runs++;
        if (work_esp != 0) {
            g_esp = work_esp;
        }
        g_handler(g_handler_user, reasons);
        wl_xb_cpu_load(&saved);
        if ((reasons & WL_XB_SVC_IRQ) && round + 1u == MAX_ROUNDS) {
            WL_LOG_W(TAG, "interrupt lines 0x%08x stay asserted after %u rounds", (unsigned)wl_xb_irq_deliverable(),
                     MAX_ROUNDS);
        }
    }
    g_in_service = false;
}

static void poll(bool levels) {
    if (g_in_service) {
        return;
    }
    const uint64_t now = wl_xb_time_us();
    update_tick_at(now);
    if (__atomic_load_n(&g_svc_waiting, __ATOMIC_ACQUIRE) != 0) {
        // a holder that keeps the lock for long (at a raised IRQL, where it does not give it up)
        // still has the devices' ticks: the audio processor's frames go on
        (void)ticks_due(now, now);
    }
    uint64_t deadline = g_deadline;
    bool due = g_pending != 0 || (deadline != 0 && now >= deadline);
    if (!due && (levels || g_lines != 0)) {
        // the lines the holder's IRQL lets in (a kernel call's poll leaves the level sources out)
        uint32_t lines = levels ? wl_xb_irq_lines() : g_lines;
        due = lines != 0 && (g_mask_fn == NULL || (lines & g_mask_fn(g_mask_user)) != 0);
    }
    if (!due) {
        // a kernel call's poll leaves the level sources to the service thread and the device safe points
        return;
    }
    g_stats.polls_with_work++;
    run_service();
}

void wl_xb_service_poll(void) {
    poll(false);
}

void wl_xb_service_poll_levels(void) {
    poll(true);
}

uint64_t wl_xb_det_service_due(uint64_t now) {
    if (!g_started) {
        return UINT64_MAX;
    }
    uint64_t next = UINT64_MAX;
    if (g_cfg.vblank) {
        if (now >= g_det_vblank_us) {
            g_det_vblank_us += g_vblank_period_us;
            uint32_t skipped = 0;
            if (g_det_vblank_us <= now) {
                skipped = (uint32_t)((now - g_det_vblank_us) / g_vblank_period_us + 1u);
                g_det_vblank_us = now + g_vblank_period_us; // fell behind: the missed ones are owed
            }
            note_vblank(skipped);
            __atomic_fetch_or(&g_pending, WL_XB_SVC_VBLANK, __ATOMIC_SEQ_CST);
            g_xb_det_work = 1;
        }
        next = g_det_vblank_us;
    }
    next = ticks_due(now, next);
    uint64_t deadline = g_deadline;
    if (deadline != 0 && deadline < next) {
        next = deadline;
    }
    return next;
}

void wl_xb_det_service_run(void) {
    poll(true);
}

// wl_mem's tagged heaps (outside malloc's: carved from MEM1's and MEM2's tops at start), used of
// budget in KB, on one line: a budget much larger than its use is memory malloc could have had.
static void mem_tags_log(void) {
    if (!wl_mem_ready()) {
        return;
    }
    WlMemRow rows[WL_MEM_MAX_ROWS];
    uint32_t n = wl_mem_rows(rows, WL_MEM_MAX_ROWS);
    char line[200];
    size_t used = 0;
    line[0] = '\0';
    for (uint32_t i = 0; i < n && used + 32u < sizeof line; i++) {
        if (rows[i].budget != 0) {
            used += (size_t)snprintf(line + used, sizeof line - used, "%s%s %u/%u", used != 0 ? ", " : "", rows[i].name,
                                     (unsigned)((rows[i].peak + 1023u) >> 10), (unsigned)(rows[i].budget >> 10));
        }
    }
    WL_LOG_I(TAG, "census: wl_mem heaps beside malloc's, peak/budget KB: %s", line);
}

static void census_service(void *user, WlMemCensusReport *out) {
    (void)user;
    out->bytes = sizeof *g_svc + SVC_STACK + (g_relay_stack != NULL ? RELAY_STACK : 0u);
    size_t deepest = wl_platform_stack_used(g_svc_stack, SVC_STACK);
    if (deepest != 0) {
        snprintf(out->note, sizeof out->note, "deepest %u of %u KB", (unsigned)(deepest >> 10),
                 (unsigned)(SVC_STACK >> 10));
    } else {
        snprintf(out->note, sizeof out->note, "stack not used here"); // (the host's threads have their own)
    }
}

// The freeze watch: a title that shows no new frame for 30 s while it runs logs where its threads
// are and, on the Wii, profiles itself for 10 s into the log (tools/wl_profile.py reads it). Nothing
// runs until then, so it cannot move the timing of what it watches. Once per freeze.
static void freeze_check(void) {
    static uint32_t last_flips, quiet;
    static bool reported;
    static WlProfileRing ring;
    static uint32_t *storage;
    if (storage != NULL && wl_profile_running() && ++quiet > 4u) {
        wl_profile_stop();
        WlProfileDumpInfo info = {500u, 0u, 0u, "a frozen title"};
        wl_profile_dump_log(&ring, &info);
        free(storage);
        storage = NULL;
        return;
    }
    uint32_t flips = wl_xb_gpu_flips();
    if (flips != last_flips || flips == 0) {
        last_flips = flips;
        quiet = 0;
        reported = false;
        return;
    }
    if (reported || ++quiet < 3u) {
        return;
    }
    reported = true;
    WL_LOG_W(TAG, "freeze: no new frame for %u s (flip %u)", (unsigned)(quiet * 10u), (unsigned)flips);
    wl_xb_thread_log_guest_stacks();
    enum { WORDS = 16u * 5000u }; // 10 s at 500 Hz, 12 callers
    storage = malloc(WORDS * sizeof(uint32_t));
    if (storage != NULL && wl_profile_ring_init(&ring, storage, WORDS, 12u, WL_PROFILE_STOP_WHEN_FULL) == WL_OK &&
        wl_profile_start(&ring, 500u) == WL_OK) {
        WL_LOG_W(TAG, "freeze: profiling 10 s");
    } else {
        free(storage);
        storage = NULL;
    }
}

// Every 10 s: the guest's memory (pages committed, frames behind them) and the host's free memory,
// the numbers a title's memory knobs (WlXbTitleConfig) are set from.
static void status_report(void) {
    WlXbMemStats m;
    wl_xb_mem_stats(&m);
    if (g_tick_va != 0) {
        // what the guest's clock reads against the host's (a title times its frames by it)
        if (wl_xb_deterministic()) {
            WL_LOG_I(TAG, "status: KeTickCount %u ms, virtual clock %u ms, host clock %u ms",
                     (unsigned)wl_xb_rd32(g_tick_va), (unsigned)wl_xb_tick_ms(),
                     (unsigned)((wl_platform_time_us() - g_wall_start_us) / 1000u));
        } else {
            WL_LOG_I(TAG, "status: KeTickCount %u ms, host clock %u ms", (unsigned)wl_xb_rd32(g_tick_va),
                     (unsigned)wl_xb_tick_ms());
        }
    }
    WL_LOG_I(TAG, "status: guest %u of %u pages used (user %u, contiguous %u), %u frames (%u KB), "
             "%u faults resolved, %u PTE evictions",
             (unsigned)m.used_pages, (unsigned)m.ram_pages, (unsigned)m.user_pages, (unsigned)m.contig_pages,
             (unsigned)m.frames, (unsigned)(m.frames * 4u), (unsigned)m.faults_resolved, (unsigned)m.pte_evictions);
    // the host's heap (mem_census.h): every report its free memory (inside newlib's arena and above
    // its top), once a minute who holds it, the threads' host stacks and wl_mem's own heaps
    static uint32_t reports;
    (void)wl_xb_thread_reap();
    wl_xb_thread_stall_check(m.faults_resolved + m.swap_ins + m.file_ins); // paging I/O moves on too
    freeze_check();
    wl_mem_census_log(TAG, reports++ % 6u != 0u);
    if (reports % 6u == 1u) {
        wl_xb_thread_log_stacks();
        mem_tags_log();
        WlXbThreadStats ts;
        wl_xb_thread_stats(&ts);
        // a holder at DISPATCH_LEVEL keeps the lock (thread.h, wl_xb_safe_point): how often it did
        WL_LOG_I(TAG, "scheduling: %u handoffs, %u yields at safe points, %u kept at a raised IRQL, %u waits",
                 (unsigned)ts.handoffs, (unsigned)ts.safe_yields, (unsigned)ts.raised_holds, (unsigned)ts.waits);
    }
    // what the guest's frame clock lost: vertical blanks merged into a pending one, periods skipped
    static WlXbServiceStats last;
    WL_LOG_I(TAG, "vblanks: %u raised, %u run, %u raised while one was pending, %u late, %u dropped (10 s)",
             (unsigned)(g_stats.vblanks_raised - last.vblanks_raised), (unsigned)(g_stats.vblanks - last.vblanks),
             (unsigned)(g_stats.vblanks_merged - last.vblanks_merged),
             (unsigned)(g_stats.vblanks_skipped - last.vblanks_skipped),
             (unsigned)(g_stats.vblanks_dropped - last.vblanks_dropped));
    last = g_stats;
    if (g_vblank_waits != 0) {
        // a title's frame loop waiting for its vblank asleep (wl_xb_vblank_wait) instead of spinning
        WL_LOG_I(TAG, "vblank waits: %u, %u ms asleep (10 s)", (unsigned)g_vblank_waits,
                 (unsigned)(g_vblank_wait_us / 1000u));
        g_vblank_waits = 0;
        g_vblank_wait_us = 0;
    }
    wl_xb_thread_log_ready_waits();
    if (g_status_hook != NULL) {
        g_status_hook();
    }
    if (m.frames_total != 0) {
        // demand paging: what the frames' shortage costs
        // (two lines: the Wii's log cuts a long one)
        WL_LOG_I(TAG, "status: paging %u of %u frames free, %u pinned; %u evictions (%u clean, %u in the fault, "
                 "%u found zero); %u of %u swap pages used",
                 (unsigned)m.frames_free, (unsigned)m.frames_total, (unsigned)m.pinned_pages, (unsigned)m.evictions,
                 (unsigned)m.clean_evictions, (unsigned)m.fast_evictions, (unsigned)m.zero_outs,
                 (unsigned)m.swap_slots_used, (unsigned)m.swap_slots);
        WL_LOG_I(TAG, "status: swap %u ins (%u KB, %u spared by file reads), %u outs (%u KB in %u writes), I/O %u ms",
                 (unsigned)m.swap_ins, (unsigned)(m.swap_read_bytes >> 10), (unsigned)m.swap_ins_spared,
                 (unsigned)m.swap_outs, (unsigned)(m.swap_write_bytes >> 10), (unsigned)m.swap_writes,
                 (unsigned)(m.swap_us / 1000u));
        WL_LOG_I(TAG, "status: file pages %u kept, %u evicted, %u read back (%u more ahead); %u forgotten (%u read "
                 "back first)",
                 (unsigned)m.file_copies, (unsigned)m.file_evictions, (unsigned)m.file_ins, (unsigned)m.file_ahead,
                 (unsigned)m.file_forgets, (unsigned)m.file_forget_ins);
        WL_LOG_I(TAG, "status: paging %u clean evictions were zero; %u pages back after an eviction, %u refaults "
                 "(%u swap, %u file, %u zero)",
                 (unsigned)m.zero_evictions, (unsigned)m.returns,
                 (unsigned)(m.refaults_swap + m.refaults_file + m.refaults_zero), (unsigned)m.refaults_swap,
                 (unsigned)m.refaults_file, (unsigned)m.refaults_zero);
        WL_LOG_I(TAG, "status: paging I/O with the guest lock held %u ms (longest %u us), %u transfers without it (%u "
                 "waited for)",
                 (unsigned)(m.io_held_us / 1000u), (unsigned)m.io_held_max_us, (unsigned)m.io_unlocked,
                 (unsigned)m.transit_waits);
    }
}

// The deterministic mode's service thread: the idle thread. It is handed the lock when no guest
// thread is ready, moves the virtual clock on to what is due next and runs it; the vertical blank,
// the ticks and the timers otherwise come due at the holder's safe points (thread.c).
static void det_service_main(void) {
    uint64_t next_status = wl_platform_time_us() + 10000000u;
    bool warned = false;
    while (g_running) {
        if (wl_xb_det_idle_take(g_svc, 1000)) {
            g_stats.service_thread_runs++;
            int r = 0;
            while (r == 0 && g_running) {
                r = wl_xb_det_idle_step();
            }
            if (r < 0 && !warned) {
                warned = true;
                WL_LOG_W(TAG, "deterministic mode: every thread waits for good and nothing is due");
            }
            wl_xb_unlock(g_svc);
        }
        uint64_t now = wl_platform_time_us();
        if (now >= next_status) {
            next_status = now + 10000000u;
            status_report();
        }
    }
}

static void *service_main(void *arg) {
    (void)arg;
    if (wl_xb_deterministic()) {
        det_service_main();
        return NULL;
    }
    uint64_t vblank_period = g_cfg.vblank_hz_milli > 0 ? 1000000000ull / g_cfg.vblank_hz_milli : 16683u;
    uint64_t next_vblank = wl_platform_time_us() + vblank_period;
    uint64_t next_status = wl_platform_time_us() + 10000000u;
    uint64_t next_watch = wl_platform_time_us() + 1000000u;
    bool host_vblank = g_cfg.vblank;
#if defined(__wii__)
    host_vblank = false; // the retrace makes them (relay_main)
#endif
    while (g_running) {
        uint64_t now = wl_platform_time_us();
        uint64_t wake = now + (uint64_t)g_cfg.tick_ms * 1000u;
        if (host_vblank && next_vblank < wake) {
            wake = next_vblank;
        }
        uint64_t deadline = g_deadline;
        if (deadline != 0 && deadline < wake) {
            wake = deadline;
        }
        wake = ticks_due(now, wake);
        wl_platform_mutex_lock(g_sm);
        if (g_pending == 0 && g_running) {
            uint32_t ms = wake > now ? (uint32_t)((wake - now + 999u) / 1000u) : 0;
            if (ms > 0) {
                wl_platform_cond_wait_timeout(g_sc, g_sm, ms);
            }
        }
        wl_platform_mutex_unlock(g_sm);
        if (!g_running) {
            break;
        }
        update_tick();
        now = wl_platform_time_us();
        ticks_due(now, now);
        if (now >= next_status) {
            next_status = now + 10000000u;
            status_report();
        }
        if (now >= next_watch) {
            next_watch = now + 1000000u;
            wl_xb_mem_watch(0); // the paging rate: thrashing is logged
        }
        if (host_vblank && now >= next_vblank) {
            next_vblank += vblank_period;
            uint32_t skipped = 0;
            if (next_vblank < now) {
                skipped = (uint32_t)((now - next_vblank) / vblank_period + 1u);
                next_vblank = now + vblank_period; // fell behind: the missed ones are owed
            }
            note_vblank(skipped);
            __atomic_fetch_or(&g_pending, WL_XB_SVC_VBLANK, __ATOMIC_SEQ_CST);
        }
        deadline = g_deadline;
        bool due = g_pending != 0 || wl_xb_irq_lines() != 0 || (deadline != 0 && now >= deadline);
        if (due && g_handler != NULL) {
            g_stats.service_thread_runs++;
            __atomic_store_n(&g_svc_waiting, 1u, __ATOMIC_RELEASE);
            wl_xb_lock(g_svc);
            __atomic_store_n(&g_svc_waiting, 0u, __ATOMIC_RELEASE);
            run_service();
            wl_xb_unlock(g_svc);
        }
    }
    return NULL;
}

#if defined(__wii__)
static void on_retrace(void *user) {
    (void)user;
    wl_platform_sem_post(g_retrace_sem); // interrupt context: only a wake
}

static void *relay_main(void *arg) {
    (void)arg;
    while (g_running) {
        wl_platform_sem_wait(g_retrace_sem);
        if (g_running) {
            wl_xb_service_raise(WL_XB_SVC_VBLANK);
        }
    }
    return NULL;
}
#endif

WlResult wl_xb_service_start(const WlXbServiceConfig *cfg) {
    if (g_started) {
        return WL_ERR_STATE;
    }
    if (!wl_xb_threads_ready()) {
        return WL_ERR_ARG;
    }
    if (cfg != NULL) {
        g_cfg = *cfg;
    } else {
        wl_xb_service_config_default(&g_cfg);
    }
    if (g_cfg.tick_ms == 0) {
        g_cfg.tick_ms = 1;
    }
    if (g_tick_start_us == 0) {
        g_tick_start_us = wl_xb_time_us();
    }
    g_wall_start_us = wl_platform_time_us();
    g_vblank_period_us = g_cfg.vblank_hz_milli > 0 ? 1000000000ull / g_cfg.vblank_hz_milli : 16683u;
    g_det_vblank_us = wl_xb_time_us() + g_vblank_period_us;
    if (wl_platform_mutex_create(&g_sm) != WL_OK || wl_platform_cond_create(&g_sc) != WL_OK) {
        return WL_ERR_NOMEM;
    }
    // the service thread is a WlXbThread of its own (it takes the lock like a guest thread and
    // runs ISRs and DPCs on a guest stack of its own), above every guest priority
    WlXbThread *self = wl_xb_current();
    g_svc = calloc(1, sizeof *g_svc);
    if (g_svc == NULL || wl_platform_cond_create(&g_svc->cond) != WL_OK) {
        return WL_ERR_NOMEM;
    }
    wl_xb_cpu_reset(&g_svc->cpu);
    g_svc->id = 0;
    g_svc->priority = WL_XB_PRIORITY_INTERRUPT;
    g_svc->is_service = true;
    strcpy(g_svc->name, "service");
    if (self != NULL) {
        WlResult r = wl_xb_thread_setup_guest(g_svc, 64u << 10, 0, 0, 0);
        if (r != WL_OK) {
            return r;
        }
        g_work_esp = g_svc->cpu.g_esp;
    }
    g_running = true;
    g_started = true;
    if (wl_xb_deterministic()) {
        wl_xb_det_set_idle(g_svc);
    }
    const size_t host_stack = SVC_STACK;
    g_svc_stack = aligned_alloc(64, host_stack);
    if (g_svc_stack == NULL) {
        return WL_ERR_NOMEM;
    }
    wl_platform_stack_fill(g_svc_stack, host_stack);
    (void)wl_mem_census_add("service", census_service, NULL);
    WlResult r = wl_platform_thread_create(&g_svc_host, service_main, NULL, g_svc_stack, host_stack, WL_THREAD_PRIO_NET);
    if (r != WL_OK) {
        g_running = false;
        g_started = false;
        return r;
    }
#if defined(__wii__)
    if (g_cfg.vblank && !wl_xb_deterministic()) {
        if (wl_platform_sem_create(&g_retrace_sem, 0, 16) == WL_OK) {
            g_relay_stack = aligned_alloc(64, RELAY_STACK);
            if (g_relay_stack != NULL) {
                wl_platform_stack_fill(g_relay_stack, RELAY_STACK);
            }
            if (g_relay_stack != NULL &&
                wl_platform_thread_create(&g_relay_host, relay_main, NULL, g_relay_stack, RELAY_STACK, WL_THREAD_PRIO_AUDIO) ==
                    WL_OK) {
                wl_platform_video_set_retrace_hook(on_retrace, NULL);
            }
        }
    }
#else
    (void)g_relay_host;
    (void)g_relay_stack;
    (void)g_retrace_sem;
#endif
    WL_LOG_I(TAG, "service thread up (tick %u ms, vblank %s)", (unsigned)g_cfg.tick_ms,
             g_cfg.vblank ? "on" : "off");
    return WL_OK;
}

void wl_xb_service_stop(void) {
    if (!g_started) {
        return;
    }
    g_running = false;
    if (wl_xb_deterministic()) {
        wl_xb_det_set_idle(NULL);
    }
    wake_service();
    wl_platform_thread_join(g_svc_host, NULL);
#if defined(__wii__)
    wl_platform_video_set_retrace_hook(NULL, NULL);
#endif
    g_started = false;
}

void wl_xb_service_stats(WlXbServiceStats *out) {
    *out = g_stats;
}
