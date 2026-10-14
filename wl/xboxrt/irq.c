// SPDX-License-Identifier: MIT
// Interrupt lines, the service thread, KeTickCount and the vertical blank (irq.h).
#include <stdlib.h>
#include <string.h>

#include <malloc.h>

#include "wl/platform/platform.h"

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
static struct {
    WlXbVblankFn fn;
    void *user;
} g_vblank_hooks[MAX_VBLANK_HOOKS];
static uint32_t g_vblank_hook_count;
static volatile uint32_t g_vblank_count;

static WlPlatformMutex g_sm; // g_pending, g_deadline, the service thread's sleep
static WlPlatformCond g_sc;
static volatile uint32_t g_pending;
static volatile uint32_t g_lines;
static volatile uint64_t g_deadline;
static struct {
    WlXbIrqLevelFn fn;
    void *user;
} g_level[WL_XB_IRQ_LINES];
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
    const uint32_t n = __atomic_load_n(&g_tick_count, __ATOMIC_ACQUIRE);
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
    const uint32_t n = __atomic_load_n(&g_tick_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        if (__atomic_exchange_n(&g_tick[i].due, 0u, __ATOMIC_ACQ_REL) != 0) {
            g_tick[i].fn(g_tick[i].user);
        }
    }
}

bool wl_xb_in_service(void) {
    return g_in_service;
}

uint32_t wl_xb_tick_ms(void) {
    return (uint32_t)((wl_xb_time_us() - g_tick_start_us) / 1000u);
}

static void update_tick(void) {
    uint32_t va = g_tick_va;
    if (va != 0) {
        wl_xb_wr32_atomic(va, wl_xb_tick_ms());
    }
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

void wl_xb_service_raise(uint32_t reasons) {
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
    }
}

uint32_t wl_xb_irq_lines(void) {
    uint32_t lines = g_lines;
    for (uint32_t i = 0; i < WL_XB_IRQ_LINES; i++) {
        if (g_level[i].fn != NULL && g_level[i].fn(g_level[i].user)) {
            lines |= 1u << i;
        }
    }
    return lines;
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
            g_stats.vblanks++;
            g_vblank_count++;
            if (g_vblank_fn != NULL) {
                g_vblank_fn(g_vblank_user);
            }
            for (uint32_t i = 0; i < g_vblank_hook_count; i++) {
                g_vblank_hooks[i].fn(g_vblank_hooks[i].user);
            }
        }
        if (reasons & WL_XB_SVC_DEVICE) {
            run_ticks();
        }
        if (wl_xb_irq_lines() != 0) {
            reasons |= WL_XB_SVC_IRQ;
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
            WL_LOG_W(TAG, "interrupt lines 0x%08x stay asserted after %u rounds", (unsigned)wl_xb_irq_lines(), MAX_ROUNDS);
        }
    }
    g_in_service = false;
}

static void poll(bool levels) {
    if (g_in_service) {
        return;
    }
    update_tick();
    uint64_t deadline = g_deadline;
    bool due = g_pending != 0 || g_lines != 0 || (deadline != 0 && wl_xb_time_us() >= deadline);
    if (!due && levels) {
        for (uint32_t i = 0; i < WL_XB_IRQ_LINES && !due; i++) {
            due = g_level[i].fn != NULL && g_level[i].fn(g_level[i].user);
        }
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
            if (g_det_vblank_us <= now) {
                g_det_vblank_us = now + g_vblank_period_us; // fell behind: no burst of vblanks
            }
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
#if defined(__GLIBC__)
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo();
#endif
    WL_LOG_I(TAG, "status: guest %u of %u pages used (user %u, contiguous %u), %u frames (%u KB), "
             "%u faults resolved, %u PTE evictions; host malloc %u KB free of %u KB",
             (unsigned)m.used_pages, (unsigned)m.ram_pages, (unsigned)m.user_pages, (unsigned)m.contig_pages,
             (unsigned)m.frames, (unsigned)(m.frames * 4u), (unsigned)m.faults_resolved, (unsigned)m.pte_evictions,
             (unsigned)(mi.fordblks / 1024), (unsigned)(mi.arena / 1024));
    if (m.frames_total != 0) {
        // demand paging: what the frames' shortage costs
        WL_LOG_I(TAG, "status: paging %u of %u frames free, %u pinned; %u evictions (%u clean, %u in the fault), "
                 "%u swap-ins (%u KB), %u swap-outs (%u KB in %u writes), swap I/O %u ms; %u of %u swap pages used",
                 (unsigned)m.frames_free, (unsigned)m.frames_total, (unsigned)m.pinned_pages, (unsigned)m.evictions,
                 (unsigned)m.clean_evictions, (unsigned)m.fast_evictions, (unsigned)m.swap_ins,
                 (unsigned)(m.swap_read_bytes >> 10), (unsigned)m.swap_outs, (unsigned)(m.swap_write_bytes >> 10),
                 (unsigned)m.swap_writes, (unsigned)(m.swap_us / 1000u), (unsigned)m.swap_slots_used,
                 (unsigned)m.swap_slots);
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
            if (next_vblank < now) {
                next_vblank = now + vblank_period; // fell behind: no burst of vblanks
            }
            __atomic_fetch_or(&g_pending, WL_XB_SVC_VBLANK, __ATOMIC_SEQ_CST);
        }
        deadline = g_deadline;
        bool due = g_pending != 0 || wl_xb_irq_lines() != 0 || (deadline != 0 && now >= deadline);
        if (due && g_handler != NULL) {
            g_stats.service_thread_runs++;
            wl_xb_lock(g_svc);
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
    const size_t host_stack = 256u << 10;
    g_svc_stack = aligned_alloc(64, host_stack);
    if (g_svc_stack == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_platform_thread_create(&g_svc_host, service_main, NULL, g_svc_stack, host_stack, WL_THREAD_PRIO_NET);
    if (r != WL_OK) {
        g_running = false;
        g_started = false;
        return r;
    }
#if defined(__wii__)
    if (g_cfg.vblank && !wl_xb_deterministic()) {
        if (wl_platform_sem_create(&g_retrace_sem, 0, 16) == WL_OK) {
            g_relay_stack = aligned_alloc(64, 16u << 10);
            if (g_relay_stack != NULL &&
                wl_platform_thread_create(&g_relay_host, relay_main, NULL, g_relay_stack, 16u << 10, WL_THREAD_PRIO_AUDIO) ==
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
