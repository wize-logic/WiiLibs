// SPDX-License-Identifier: MIT
// Guest threads and the guest lock (thread.h).
//
// The lock is a baton passed by hand: `g_owner` is the thread allowed to run guest code, and a
// thread that wants it waits on its own condition variable until a releasing thread names it.
// One platform mutex (g_m) guards the baton, the ready queue and each thread's wake flag, and is
// only held for those few lines: guest code never runs under it. The ready queue is ordered by
// priority, first come first served within one, so a release is a strict-priority dispatch.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/platform/mem_census.h"
#include "wl/xboxrt/page_census.h"
#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/det_clock.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/thunk.h"

#define TAG "xbthread"

#define KPCR_IRQL 0x24u
#define DISPATCH_LEVEL 2u
#define KTHREAD_OFFSET 0x400u // in the thread's kernel page, after the KPCR
#define KTHREAD_BYTES 0x200u

static WlPlatformMutex g_m;
static WlPlatformMutex g_card; // wl_xb_card_lock
static bool g_card_made;
static WlPlatformCond g_proc_cond;
static bool g_ready_init;
static WlXbThreadConfig g_cfg;
static WlXbThread *volatile g_boosted; // handed the lock for starving: keeps it one quantum
static WlXbThread *volatile g_owner;
static WlXbThread *volatile g_ready; // the ready queue's head (read without g_m as a hint)
static WlXbThread *g_all;
static WlXbThread *g_cur;            // the lock holder, written only by it
static uint32_t g_next_id = 1;
static uint64_t g_quantum_start;
static WlXbThreadExitHook g_exit_hook;
static WlXbExitFn g_exit_fn;
static bool g_exiting;
static int g_exit_code;
static uint32_t g_live; // guest threads started and not ended
static uint32_t g_tls_data, g_tls_bytes, g_tls_zero;
static WlXbThreadStats g_stats;
static WlXbLatency g_ready_waits; // g_m held
static WlXbLatency g_gpu_backs;   // a GPU wait's end to the lock back (g_m held)
static uint64_t g_gpu_off_us;     // the GPU waits' time off the lock (g_m held)
volatile uint32_t g_xb_attention;
uint32_t g_xb_compat;
volatile uint32_t g_xb_det_work;

// The deterministic mode (thread.h). The virtual clock starts at 1 s, so that no deadline made
// from it is 0 (none); it is written only by the lock's holder.
#define DET_START_US 1000000u
static bool g_det;
static bool g_preempt; // WlXbThreadConfig.preempt, never in the deterministic mode
static volatile WlXbDetClock g_vt = {DET_START_US, 0}; // (det_clock.h: no 64-bit division at a safe point)
static uint64_t g_det_next_us;          // the holder looks at the clock's events from then on
static uint32_t g_det_backedge_ns, g_det_call_ns, g_det_device_ns;
static WlXbThread *volatile g_idle;     // the idle thread (the service thread)

// Quiet safe points (thread.h, wl_xb_thread_set_quiet_points): a window armed by a safe point that
// found only threads whose turn cannot come before g_quiet_until (microseconds of wl_xb_time_us,
// its low 32 bits); the service thread raises the attention when it ends (wl_xb_thread_quiet_due).
#define QUIET_MAX_US 100000u // a window ends within this whatever the deadlines (the service looks again)
#define QUIET_MIN_US 200u    // a deadline closer than this: no window (the safe points look as before)
#define QUIET_SERVICE_US 50000u // no window unless the service thread looked this lately (it ends them)
static bool g_quiet_on = true;
static volatile uint32_t g_quiet_armed;
static volatile uint32_t g_quiet_until;
static volatile uint32_t g_quiet_seen; // the service thread's last round (us, low 32 bits; 0: none yet)

static void census_threads(void *user, WlMemCensusReport *out);

void wl_xb_thread_config_default(WlXbThreadConfig *cfg) {
    cfg->quantum_ms = 10;
    cfg->starve_ms = 30;
    cfg->host_stack_bytes = 128u << 10;
    cfg->default_stack_bytes = 64u << 10;
    cfg->deterministic = false;
    cfg->det_backedge_ns = 40;
    cfg->det_call_ns = 400;
    cfg->det_device_ns = 400;
    cfg->preempt = true;
}

bool wl_xb_deterministic(void) {
    return g_det;
}

uint64_t wl_xb_time_us(void) {
    return g_det ? g_vt.us : wl_platform_time_us();
}

uint64_t wl_xb_time_ns(void) {
    return g_det ? wl_xb_det_clock_ns(&g_vt) : wl_platform_time_us() * 1000u;
}

void wl_xb_time_spend_us(uint32_t us) {
    if (g_det) {
        wl_xb_det_clock_add_us(&g_vt, us);
    }
}

bool wl_xb_threads_ready(void) {
    return g_ready_init;
}

WlXbThread *wl_xb_current(void) {
    return g_cur;
}

bool wl_xb_lock_held(void) {
    if (!g_ready_init) {
        return true;
    }
    WlXbThread *self = g_cur;
    return self != NULL && self->host_tid == wl_platform_thread_self_id();
}

// ---- Ready queue (g_m held) ------------------------------------------------------------------

// A wait or a card transfer begins / ends: its time to the thread's (g_m held).
static uint64_t *off_time(WlXbThread *t, uint8_t kind) {
    return kind == WL_XB_OFF_IO ? &t->io_us : kind == WL_XB_OFF_GPU ? &t->gpu_us : &t->waited_us;
}

static void off_begin(WlXbThread *t, uint8_t kind) {
    t->off_since_us = wl_xb_time_us();
    t->off_kind = kind;
}

static void off_end(WlXbThread *t, uint64_t now) {
    if (t->off_since_us != 0) {
        uint64_t us = now > t->off_since_us ? now - t->off_since_us : 0u;
        *off_time(t, t->off_kind) += us;
        if (t->off_kind == WL_XB_OFF_GPU) {
            g_gpu_off_us += us;
            g_stats.gpu_off_ms = (uint32_t)(g_gpu_off_us / 1000u);
        }
        t->off_since_us = 0;
    }
}

static void enqueue(WlXbThread *t) {
    WlXbThread *volatile *pp = &g_ready;
    if (t->io_back && g_preempt) {
        // back from the card: first after the service thread and the others back before it, as the
        // console completes an I/O at its interrupt (the holder hands over at its next safe point)
        while (*pp != NULL && ((*pp)->is_service || (*pp)->io_back)) {
            pp = &(*pp)->next_ready;
        }
    } else {
        t->io_back = false;
        while (*pp != NULL && (*pp)->priority >= t->priority) {
            pp = &(*pp)->next_ready;
        }
    }
    t->next_ready = *pp;
    *pp = t;
    t->ready = true;
    t->ready_since_us = wl_xb_time_us();
    off_end(t, t->ready_since_us);
    g_xb_attention = 1; // the holder gives the lock up at its next safe point when this one outranks it
}

// `t` leaves the ready queue for the lock: how long it waited (g_m held).
static void note_ready_wait(WlXbThread *t) {
    t->io_back = false;
    uint64_t us = wl_xb_time_us() - t->ready_since_us;
    t->ready_us += us;
    if (!t->is_service) {
        wl_xb_latency_add(&g_ready_waits, us);
    }
    if (t->gpu_back) {
        t->gpu_back = false;
        wl_xb_latency_add(&g_gpu_backs, us);
    }
}

static WlXbThread *dequeue(void) {
    WlXbThread *t = g_ready;
    if (t != NULL) {
        note_ready_wait(t);
        g_ready = t->next_ready;
        t->next_ready = NULL;
        t->ready = false;
    }
    return t;
}

// Takes `t` out of the ready queue (g_m held).
static void unqueue(WlXbThread *t) {
    WlXbThread *volatile *pp = &g_ready;
    while (*pp != NULL && *pp != t) {
        pp = &(*pp)->next_ready;
    }
    if (*pp == t) {
        *pp = t->next_ready;
        t->next_ready = NULL;
        t->ready = false;
        t->io_back = false;
    }
}

// A quiet window is its holder's (quiet_arm weighed the ready threads against it): the lock changing
// hands ends it, and the new holder's first safe point looks (g_m held).
static void quiet_end(void) {
    if (g_quiet_armed != 0) {
        g_quiet_armed = 0;
        g_xb_attention = 1;
    }
}

// Gives the baton away (g_m held; the caller is the holder): to the head of the queue, or to no
// one. In the deterministic mode no one is the idle thread, which moves the clock on.
static void hand_off(void) {
    if (g_boosted == g_owner) {
        g_boosted = NULL; // a boost lasts the turn it was given for
    }
    quiet_end();
    WlXbThread *next = dequeue();
    if (next == NULL && g_det && g_idle != NULL && g_owner != g_idle) {
        next = g_idle;
    }
    g_owner = next;
    if (next != NULL) {
        g_stats.handoffs++;
        wl_platform_cond_signal(next->cond);
    }
}

extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
static void log_guest_stack(const char *what, const WlXbThread *t, const WlXbThread *owner);

// Waits for the baton (g_m held). The service thread says so when it has waited a second or more,
// and names what the holder does: the safe points it reached meanwhile and how many of them kept
// the lock at a raised IRQL (none: it ran without a safe point; many: it stays at DISPATCH_LEVEL or
// above, where its own safe points run the interrupts and nothing else runs), its guest return
// addresses, and where its host thread is (wl_xb_backend_probe). Nothing of this runs before.
static uint64_t g_svc_wait_max_us; // the service thread's longest wait for the lock (g_m)
// The title's exit: the service thread's wait for the lock gives up (wl_xb_service_stop_within)
static volatile bool g_lock_stop;
static uint32_t g_svc_wait_holder;  // the thread that had the lock when that wait began

// Takes the lock for `self` (g_m held). A thread's wait is `timeout_ms` at most (WL_WAIT_FOREVER: none;
// the title's exit bounds the boot thread's), the service thread's until the exit stops it; false
// when it did not get the lock (out of the queue again).
static bool take(WlXbThread *self, uint32_t timeout_ms) {
    if (g_owner == NULL && g_ready == NULL) {
        off_end(self, wl_xb_time_us());
        self->io_back = false;
        if (self->gpu_back) {
            self->gpu_back = false;
            wl_xb_latency_add(&g_gpu_backs, 0); // (the lock free: at once)
        }
        g_owner = self;
    } else {
        enqueue(self);
        uint64_t since = wl_platform_time_ms(), warned = 0;
        const uint64_t since_us = self->is_service ? wl_platform_time_us() : 0;
        const uint32_t holder0 = g_owner != NULL ? g_owner->id : 0u;
        uint32_t points0 = g_stats.safe_points, raised0 = g_stats.raised_holds;
        while (g_owner != self) {
            if (!self->is_service) {
                if (timeout_ms == WL_WAIT_FOREVER) {
                    wl_platform_cond_wait(self->cond, g_m);
                    continue;
                }
                if (g_owner == NULL) {
                    // no one to hand it on (the deterministic mode with its idle thread stopped: a
                    // thread queued while the lock was free waits for a hand-off that never comes)
                    unqueue(self);
                    g_owner = self;
                    break;
                }
                uint64_t spent = wl_platform_time_ms() - since;
                if (spent >= timeout_ms) {
                    unqueue(self);
                    return false;
                }
                const uint32_t left = (uint32_t)(timeout_ms - spent);
                wl_platform_cond_wait_timeout(self->cond, g_m, left < 100u ? left : 100u);
                continue;
            }
            if (self->spared) {
                self->spared = false; // (out of the queue: yield_if_due)
                return false;
            }
            if (g_lock_stop) {
                unqueue(self);
                return false;
            }
            wl_platform_cond_wait_timeout(self->cond, g_m, 500);
            uint64_t waited = wl_platform_time_ms() - since;
            WlXbThread *holder = g_owner;
            if (holder != self && holder != NULL && waited >= 1000u &&
                (warned == 0 || waited - warned >= 5000u)) {
                warned = waited;
                g_stats.service_starved++;
                WL_LOG_W(TAG,
                         "interrupts held off for %u ms: thread %u (%s) keeps the guest lock: %u safe points, %u of "
                         "them at a raised IRQL",
                         (unsigned)waited, (unsigned)holder->id, holder->name, (unsigned)(g_stats.safe_points - points0),
                         (unsigned)(g_stats.raised_holds - raised0));
                log_guest_stack("held off", holder, holder);
                WlXbSymbol sym;
                (void)wl_xb_symbolize(0, &sym); // builds the table here, not in the probed thread
                wl_xb_backend_probe(holder->host_handle);
            } else if (warned != 0) {
                wl_xb_backend_probe(0); // the Wii: the probe's samples, once taken, into the log
            }
        }
        if (warned != 0) {
            wl_xb_backend_probe(0);
        }
        if (self->is_service) {
            uint64_t us = wl_platform_time_us() - since_us;
            if (us > g_svc_wait_max_us) {
                g_svc_wait_max_us = us;
                g_svc_wait_holder = holder0;
            }
        }
    }
    g_quantum_start = wl_xb_time_us();
    return true;
}

// The deterministic mode: `self` was handed the baton (it waited for it on its own): it becomes
// the holder (g_m not held).
static void det_enter(WlXbThread *self) {
    self->host_tid = wl_platform_thread_self_id();
    self->host_handle = wl_xb_backend_thread_handle();
    g_quantum_start = wl_xb_time_us();
    g_cur = self;
    wl_xb_cpu_load(&self->cpu);
}

// The deterministic mode: waits for the baton (g_m held).
static void det_await(WlXbThread *self) {
    while (g_owner != self) {
        wl_platform_cond_wait(self->cond, g_m);
    }
}

void wl_xb_lock(WlXbThread *self) {
    (void)wl_xb_lock_within(self, WL_WAIT_FOREVER);
}

bool wl_xb_lock_within(WlXbThread *self, uint32_t timeout_ms) {
    self->host_tid = wl_platform_thread_self_id();
    self->host_handle = wl_xb_backend_thread_handle();
    wl_platform_mutex_lock(g_m);
    bool took = take(self, timeout_ms);
    wl_platform_mutex_unlock(g_m);
    if (!took) {
        return false;
    }
    g_cur = self;
    wl_xb_cpu_load(&self->cpu);
    return true;
}

void wl_xb_thread_lock_stop(bool stop) {
    wl_platform_mutex_lock(g_m);
    g_lock_stop = stop;
    for (WlXbThread *t = g_ready; t != NULL; t = t->next_ready) {
        if (t->is_service) { // (queued: its wait for the lock looks at the stop now)
            wl_platform_cond_signal(t->cond);
        }
    }
    wl_platform_mutex_unlock(g_m);
}

WlXbThread *wl_xb_thread_lock_holder(void) {
    return g_owner;
}

WlXbThread *wl_xb_thread_ready_head(void) {
    return g_ready;
}

void wl_xb_unlock(WlXbThread *self) {
    wl_xb_cpu_save(&self->cpu);
    g_cur = NULL;
    wl_platform_mutex_lock(g_m);
    if (g_owner == self) {
        hand_off();
    }
    wl_platform_mutex_unlock(g_m);
}

// The deterministic mode's wait: the thread leaves the queue until a wake or its timeout in the
// virtual clock puts it back (wl_xb_wake, det_expire).
static bool det_wait(WlXbThread *self, uint32_t timeout_ms) {
    wl_xb_cpu_save(&self->cpu);
    g_cur = NULL;
    wl_platform_mutex_lock(g_m);
    g_stats.waits++;
    off_begin(self, WL_XB_OFF_WAIT);
    if (self->woken || timeout_ms == 0) {
        enqueue(self); // a wake came first, or no time to wait: only a yield
    } else {
        self->det_waiting = true;
        self->det_deadline_us = timeout_ms == WL_WAIT_FOREVER ? 0 : wl_xb_time_us() + (uint64_t)timeout_ms * 1000u;
        if (self->det_deadline_us != 0 && self->det_deadline_us < g_det_next_us) {
            g_det_next_us = self->det_deadline_us;
        }
    }
    hand_off();
    det_await(self);
    bool woke = self->woken;
    self->woken = false;
    if (!woke) {
        g_stats.timeouts++;
    }
    wl_platform_mutex_unlock(g_m);
    det_enter(self);
    return woke;
}

// The deterministic mode: the waits whose timeout has come go back in the queue; returns the
// earliest timeout still to come (UINT64_MAX: none). g_m not held.
static uint64_t det_expire(uint64_t now) {
    uint64_t next = UINT64_MAX;
    wl_platform_mutex_lock(g_m);
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        if (!t->det_waiting || t->det_deadline_us == 0) {
            continue;
        }
        if (t->det_deadline_us <= now) {
            t->det_waiting = false;
            enqueue(t);
        } else if (t->det_deadline_us < next) {
            next = t->det_deadline_us;
        }
    }
    wl_platform_mutex_unlock(g_m);
    return next;
}

bool wl_xb_wait(WlXbThread *self, uint32_t timeout_ms) {
    if (g_det) {
        return det_wait(self, timeout_ms);
    }
    wl_xb_cpu_save(&self->cpu);
    g_cur = NULL;
    wl_platform_mutex_lock(g_m);
    g_stats.waits++;
    off_begin(self, WL_XB_OFF_WAIT);
    hand_off();
    uint64_t start = wl_platform_time_ms();
    while (!self->woken) {
        if (timeout_ms == WL_WAIT_FOREVER) {
            wl_platform_cond_wait(self->cond, g_m);
            continue;
        }
        uint64_t spent = wl_platform_time_ms() - start;
        if (spent >= timeout_ms) {
            break;
        }
        wl_platform_cond_wait_timeout(self->cond, g_m, (uint32_t)(timeout_ms - spent));
    }
    bool woke = self->woken;
    self->woken = false;
    if (!woke) {
        g_stats.timeouts++;
    }
    take(self, WL_WAIT_FOREVER);
    wl_platform_mutex_unlock(g_m);
    g_cur = self;
    wl_xb_cpu_load(&self->cpu);
    return woke;
}

void wl_xb_wake(WlXbThread *t) {
    if (t == NULL) {
        return;
    }
    wl_platform_mutex_lock(g_m);
    t->woken = true;
    g_stats.wakes++;
    if (g_det) {
        if (t->det_waiting) {
            t->det_waiting = false; // queued now, not when its host thread gets to run
            enqueue(t);
        }
    } else if (!t->ready && g_owner != t) {
        wl_platform_cond_signal(t->cond);
    }
    wl_platform_mutex_unlock(g_m);
}

static void requeue(WlXbThread *self) {
    wl_xb_cpu_save(&self->cpu);
    g_cur = NULL;
    wl_platform_mutex_lock(g_m);
    enqueue(self);
    hand_off();
    while (g_owner != self) {
        wl_platform_cond_wait(self->cond, g_m);
    }
    g_quantum_start = wl_xb_time_us();
    wl_platform_mutex_unlock(g_m);
    g_cur = self;
    wl_xb_cpu_load(&self->cpu);
}

// The thread's IRQL (its KPCR's) is DISPATCH_LEVEL or above: it keeps the lock (yield_if_due).
static bool irql_raised(const WlXbThread *t) {
    return t->kpcr != 0 && wl_xb_rd8(t->kpcr + KPCR_IRQL) >= DISPATCH_LEVEL;
}

// The holder gives the lock to `to`, already out of the ready queue, for one quantum (g_boosted:
// queued by priority, the holder would otherwise take it back at the next safe point), and waits
// in the queue for its turn (g_m held; released on return).
static void give_to(WlXbThread *self, WlXbThread *to) {
    quiet_end();
    note_ready_wait(to);
    g_boosted = to;
    wl_xb_cpu_save(&self->cpu);
    g_cur = NULL;
    enqueue(self);
    g_owner = to;
    g_stats.handoffs++;
    wl_platform_cond_signal(to->cond);
    while (g_owner != self) {
        wl_platform_cond_wait(self->cond, g_m);
    }
    g_quantum_start = wl_xb_time_us();
    wl_platform_mutex_unlock(g_m);
    g_cur = self;
    wl_xb_cpu_load(&self->cpu);
}

// Gives the lock to the head of the ready queue when it outranks the caller, or ties with it and
// the caller's quantum is over. true when it did.
static bool yield_if_due(WlXbThread *self) {
    WlXbThread *head = g_ready;
    if (self == NULL || head == NULL || wl_xb_in_service()) {
        return false;
    }
    if (irql_raised(self)) {
        // At DISPATCH_LEVEL or above the holder keeps the processor, as on the console's one CPU:
        // no other thread runs, and the DPCs wait until it lowers its IRQL (KfLowerIrql runs them
        // then). Its own safe points still run the interrupts its IRQL lets in (their poll comes
        // first). Code at a raised IRQL counts on this as its lock: DirectSound edits its voice
        // lists so, and its DPC, run on the service thread in the middle of an edit, leaves a
        // stopped voice in the APU's list for good (the idle-voice trap at every frame, and a
        // wait for the voice that never ends).
        g_stats.raised_holds++;
        return false;
    }
    if (head->is_service && !(g_xb_compat & WL_XB_COMPAT_OLDSERVICE) && !wl_xb_service_wanted()) {
        // The service thread queued for the lock (an interrupt, a tick, a vertical blank), and the
        // holder's poll just before has run that work itself: the lock's round trip (two thread
        // switches, and the service finding nothing to do) is spared. It goes back to its sleep
        // unqueued; work raised from now on brings it back (or the holder's next poll runs it).
        wl_platform_mutex_lock(g_m);
        if (g_ready == head && !wl_xb_service_wanted()) {
            unqueue(head);
            head->spared = true;
            g_stats.service_spared++;
            wl_platform_cond_signal(head->cond);
        }
        wl_platform_mutex_unlock(g_m);
        head = g_ready;
        if (head == NULL) {
            return false;
        }
    }
    uint64_t now = wl_xb_time_us();
    bool boosted = self == g_boosted && now - g_quantum_start < (uint64_t)g_cfg.quantum_ms * 1000u;
    if (boosted && !head->is_service) {
        return false; // a starved thread keeps the lock for one quantum, as NT's boost gives it
    }
    g_boosted = NULL;
    const uint64_t quantum_us = (uint64_t)g_cfg.quantum_ms * 1000u;
    if (g_preempt && head->io_back) {
        // back from the card (enqueue put it first): the rest of its kernel call, and its guest code
        // up to its next wait, for a quantum whatever the priorities (a reader's completion routine
        // runs then, not when a busy holder of higher priority next waits)
        wl_platform_mutex_lock(g_m);
        WlXbThread *back = g_ready;
        if (back != NULL && back->io_back && back != self) {
            g_ready = back->next_ready;
            back->next_ready = NULL;
            back->ready = false;
            g_stats.io_handoffs++;
            give_to(self, back);
            return true;
        }
        wl_platform_mutex_unlock(g_m);
        head = g_ready;
        if (head == NULL) {
            return false;
        }
    }
    if (head->priority > self->priority ||
        (head->priority == self->priority && now - g_quantum_start >= quantum_us)) {
        requeue(self);
        return true;
    }
    if (g_preempt && head->priority == self->priority && now - head->ready_since_us >= quantum_us) {
        // its own quantum's wait: the holder's restarts at every interrupt the service took the lock for
        g_stats.tie_handoffs++;
        requeue(self);
        return true;
    }
    // A thread kept waiting past starve_ms goes first, for one quantum (the holder may be
    // busy-waiting for something only a lower-priority thread or the vertical blank brings, as a
    // title's frame throttle does, while a loader thread below it never runs). The waiter's time
    // alone decides: the holder's quantum restarts after every interrupt, which the service
    // thread takes the lock for, so a holder interrupted each vertical blank never finishes one.
    // With the console's preemption a quantum's wait is enough (a reader below a busy holder that
    // spins on SwitchToThread gets in after one, not after starve_ms).
    uint64_t starve_us = (uint64_t)g_cfg.starve_ms * 1000u;
    if (g_preempt && starve_us > quantum_us) {
        starve_us = quantum_us;
    }
    if (starve_us != 0) {
        wl_platform_mutex_lock(g_m);
        WlXbThread *volatile *pp = &g_ready;
        while (*pp != NULL && now - (*pp)->ready_since_us < starve_us) {
            pp = &(*pp)->next_ready;
        }
        WlXbThread *starved = *pp;
        if (starved == NULL || starved == self || starved->is_service) {
            wl_platform_mutex_unlock(g_m);
            return false;
        }
        // straight to it: queued by priority, the caller would come back ahead of it
        *pp = starved->next_ready;
        starved->next_ready = NULL;
        starved->ready = false;
        g_stats.starved_handoffs++;
        give_to(self, starved);
        return true;
    }
    return false;
}

// The holder's safe point found threads ready and gave none of them the lock. When none can have it
// before a time named now (none outranks the holder, none is the service thread or back from the
// card, and the quantum's end and the waiters' starvation are that far off), the attention stays
// down until then instead of every backward jump looking again (an emulated thread spends a third
// of its back edges' cost so in Dolphin): any new work raises it as before (a thread queued, an
// interrupt, a tick due), and the service thread raises it when the window ends, at its next round
// (a millisecond late at most). true when a window was armed.
static bool quiet_arm(WlXbThread *self) {
    if (!g_quiet_on || self == NULL) {
        return false;
    }
    const uint64_t now = wl_xb_time_us();
    uint32_t seen = g_quiet_seen;
    if (seen == 0 || (uint32_t)now - seen > QUIET_SERVICE_US) {
        return false; // no service thread going round to end the window (not started, or stopped)
    }
    const uint64_t quantum_us = (uint64_t)g_cfg.quantum_ms * 1000u;
    uint64_t starve_us = (uint64_t)g_cfg.starve_ms * 1000u;
    if (g_preempt && starve_us > quantum_us) {
        starve_us = quantum_us;
    }
    uint64_t until = now + QUIET_MAX_US;
    if (self == g_boosted && g_quantum_start + quantum_us < until) {
        until = g_quantum_start + quantum_us; // the boost's end (yield_if_due)
    }
    bool quiet = true;
    wl_platform_mutex_lock(g_m);
    for (const WlXbThread *t = g_ready; t != NULL; t = t->next_ready) {
        if (t->is_service || t->priority > self->priority || (g_preempt && t->io_back)) {
            quiet = false; // its turn comes at the next safe point the holder's IRQL lets it
            break;
        }
        uint64_t due = until;
        if (t->priority == self->priority) {
            due = g_quantum_start + quantum_us; // a tie: the holder's quantum's end
            if (g_preempt && t->ready_since_us + quantum_us < due) {
                due = t->ready_since_us + quantum_us; // ...or the waiter's own quantum's wait
            }
        }
        if (starve_us != 0 && t->ready_since_us + starve_us < due) {
            due = t->ready_since_us + starve_us;
        }
        until = due < until ? due : until;
    }
    quiet = quiet && until >= now + QUIET_MIN_US;
    if (quiet) {
        g_quiet_until = (uint32_t)until;
        __atomic_store_n(&g_quiet_armed, 1u, __ATOMIC_RELEASE);
        g_stats.quiet_windows++;
    }
    wl_platform_mutex_unlock(g_m);
    return quiet;
}

// The deterministic mode: the clock's events whose time has come (the service's, the waits'
// timeouts, KeTickCount's next millisecond) are marked due; the next one is remembered.
static void det_events(uint64_t now) {
    uint64_t next = wl_xb_det_service_due(now);
    uint64_t ms = (now / 1000u + 1u) * 1000u;
    if (ms < next) {
        next = ms;
    }
    uint64_t w = det_expire(now);
    if (w < next) {
        next = w;
    }
    g_det_next_us = next;
}

// The deterministic mode's safe point: the clock moves on by `ns`; the work (interrupts, a yield)
// is looked at when something is due, a thread is ready, or (`levels`) a device was accessed.
static void det_point(uint32_t ns, bool levels) {
    WlXbThread *self = g_cur;
    if (self == NULL || self->host_tid != wl_platform_thread_self_id()) {
        return; // not the holder
    }
    wl_xb_det_clock_add_ns(&g_vt, ns);
    if (self->kernel_depth != 0 || self->is_service || wl_xb_in_service()) {
        return;
    }
    uint64_t now = g_vt.us;
    if (!levels && now < g_det_next_us && g_xb_det_work == 0 && g_ready == NULL) {
        return;
    }
    g_xb_det_work = 0;
    g_stats.safe_points++;
    if (now >= g_det_next_us) {
        det_events(now);
    }
    wl_xb_service_poll_levels();
    if (yield_if_due(self)) {
        g_stats.safe_yields++;
    }
}

void wl_xb_yield_point(void) {
    g_stats.kernel_calls++; // (the holder's: a kernel call's own yield point)
    if (g_det) {
        det_point(g_det_call_ns, false);
        return;
    }
    wl_xb_service_poll();
    yield_if_due(g_cur);
}

void wl_xb_device_safe_point(void) {
    g_stats.device_points++;
    if (g_det) {
        det_point(g_det_device_ns, true);
        return;
    }
    if (g_xb_attention == 0 && g_xb_mmio_last_write == 0 && !(g_xb_compat & WL_XB_COMPAT_OLDDEVPOINT) &&
        wl_xb_irq_lines() == 0) {
        // A read raises no device's line, no line is up (one the holder's IRQL kept out goes in at
        // the first access once it is lowered), and nothing waits (no thread ready, or a quiet window;
        // new work raises the attention): nothing for the safe point to do, as at a backward jump.
        g_stats.read_device_points++;
        return;
    }
    if (g_quiet_armed != 0 && g_xb_attention == 0 && !(g_xb_compat & WL_XB_COMPAT_OLDDEVPOINT)) {
        // A quiet window, the holder's (a hand-off ends it): no ready thread's turn comes before its
        // end, and new work raises the attention, so the look for a turn (a lock round trip, and one
        // more to arm the window again) would find none. The access may have raised its device's
        // line: that alone is looked at, and the whole safe point runs when the poll's work raised
        // the attention (a thread it woke). Off: a file "olddevpoint" (WL_XB_OLDDEVPOINT).
        WlXbThread *self = g_cur;
        if (self == NULL || self->kernel_depth != 0 || self->is_service || wl_xb_in_service() ||
            self->host_tid != wl_platform_thread_self_id()) {
            return; // not the holder in guest code (wl_xb_safe_point)
        }
        g_stats.quiet_device_points++;
        wl_xb_service_poll_levels();
        if (g_xb_attention == 0) {
            return;
        }
    }
    wl_xb_safe_point();
}

void wl_xb_det_note_deadline(uint64_t when_us) {
    if (g_det && when_us != 0 && when_us < g_det_next_us) {
        g_det_next_us = when_us;
    }
}

void wl_xb_det_set_idle(WlXbThread *t) {
    wl_platform_mutex_lock(g_m);
    WlXbThread *old = g_idle;
    g_idle = t;
    if (old != NULL && t == NULL) {
        wl_platform_cond_signal(old->cond);
    }
    wl_platform_mutex_unlock(g_m);
}

bool wl_xb_det_idle_take(WlXbThread *self, uint32_t timeout_ms) {
    wl_platform_mutex_lock(g_m);
    if (g_owner != self && g_idle == self) {
        wl_platform_cond_wait_timeout(self->cond, g_m, timeout_ms);
    }
    if (g_owner == NULL && g_idle == self) {
        // nobody holds the lock: the idle thread gave it up when every thread waited for good and
        // nothing was due (wl_xb_det_idle_step -1). It takes it back to look again (a wake or an
        // interrupt line a host thread raised since), as no hand-off would ever reach it.
        g_owner = self;
    }
    bool took = g_owner == self;
    wl_platform_mutex_unlock(g_m);
    if (took) {
        det_enter(self);
    }
    return took;
}

int wl_xb_det_idle_step(void) {
    uint64_t now = g_vt.us;
    uint64_t next = wl_xb_det_service_due(now);
    uint64_t w = det_expire(now);
    if (w < next) {
        next = w;
    }
    wl_xb_det_service_run();
    g_det_next_us = 0; // the next holder looks at the events at its first safe point
    if (g_ready != NULL) {
        return 1;
    }
    if (next == UINT64_MAX) {
        return -1;
    }
    // every thread waits: the clock jumps to what is due next (on at least a microsecond, so a
    // deadline the work did not take away cannot hold it in place)
    if (next <= now) {
        next = now + 1u;
    }
    wl_xb_det_clock_set_us(&g_vt, next);
    return 0;
}

void wl_xb_safe_point(void) {
    if (g_det) {
        det_point(g_det_backedge_ns, false);
        return;
    }
    WlXbThread *self = g_cur;
    if (self == NULL || self->kernel_depth != 0 || self->is_service || wl_xb_in_service() ||
        self->host_tid != wl_platform_thread_self_id()) {
        return; // not the holder in guest code: a kernel call's own access, another host thread
    }
    g_xb_attention = 0; // cleared before looking, so work raised from here on sets it again
    g_stats.safe_points++;
    wl_xb_service_poll_levels();
    if (yield_if_due(self)) {
        g_stats.safe_yields++;
    }
    if (g_ready != NULL && !quiet_arm(g_cur)) {
        g_xb_attention = 1; // a thread of equal priority still waits for the quantum's end
    }
}

void wl_xb_thread_set_quiet_points(bool on) {
    g_quiet_on = on;
    if (!on && g_quiet_armed != 0) {
        g_quiet_armed = 0;
        g_xb_attention = 1;
    }
}

void wl_xb_thread_quiet_due(uint64_t now_us) {
    g_quiet_seen = (uint32_t)now_us | 1u;
    if (__atomic_load_n(&g_quiet_armed, __ATOMIC_ACQUIRE) != 0 && (int32_t)((uint32_t)now_us - g_quiet_until) >= 0) {
        g_quiet_armed = 0;
        g_xb_attention = 1;
    }
}

bool wl_xb_lock_wanted(void) {
    return g_xb_attention != 0 || (g_quiet_armed != 0 && g_ready != NULL);
}

void wl_xb_yield(void) {
    WlXbThread *self = g_cur;
    WlXbThread *head = g_ready;
    if (self == NULL || head == NULL || head->priority < self->priority || wl_xb_in_service()) {
        return;
    }
    if (irql_raised(self)) {
        g_stats.raised_holds++; // the processor stays its (yield_if_due)
        return;
    }
    requeue(self);
}

static WlXbThread *give_up(uint8_t kind) {
    WlXbThread *self = g_cur;
    if (g_det || !g_ready_init || self == NULL || self->is_service || wl_xb_in_service() ||
        self->host_tid != wl_platform_thread_self_id() || irql_raised(self)) {
        return NULL;
    }
    wl_platform_mutex_lock(g_m);
    off_begin(self, kind);
    if (kind == WL_XB_OFF_GPU) {
        g_stats.gpu_blocks++;
    } else {
        g_stats.io_blocks++;
    }
    wl_platform_mutex_unlock(g_m);
    wl_xb_unlock(self);
    return self;
}

WlXbThread *wl_xb_block_begin(void) {
    return give_up(WL_XB_OFF_IO);
}

void wl_xb_block_end(WlXbThread *self) {
    if (self != NULL) {
        self->io_back = g_preempt; // (enqueue reads it under g_m)
        wl_xb_lock(self);
    }
}

// The poll's sleep: the GPU's end is seen at most this late (a frame's wait is 1-20 ms on the Wii);
// each look costs two thread switches.
#define GPU_POLL_US 200u

WlXbThread *wl_xb_gpu_wait_begin(void) {
    // (raised first: the thread handed the lock may run at once, and this one must still get its looks)
    wl_platform_thread_set_own_priority(WL_THREAD_PRIO_NET);
    WlXbThread *self = give_up(WL_XB_OFF_GPU);
    if (self == NULL) {
        wl_platform_thread_set_own_priority(WL_THREAD_PRIO_MAIN);
        return NULL;
    }
    return self;
}

void wl_xb_gpu_wait_poll(void) {
    wl_platform_thread_sleep_us(GPU_POLL_US);
}

void wl_xb_gpu_wait_end(WlXbThread *self) {
    if (self == NULL) {
        return;
    }
    self->gpu_back = true; // (its wait for the lock is logged apart: note_ready_wait, take)
    wl_xb_block_end(self);
    wl_platform_thread_set_own_priority(WL_THREAD_PRIO_MAIN);
}

void wl_xb_card_lock(void) {
    if (g_card_made) {
        wl_platform_mutex_lock(g_card);
    }
}

void wl_xb_card_unlock(void) {
    if (g_card_made) {
        wl_platform_mutex_unlock(g_card);
    }
}

bool wl_xb_card_busy(void) {
    if (!g_card_made || !wl_platform_mutex_try_lock(g_card)) {
        return g_card_made;
    }
    wl_platform_mutex_unlock(g_card);
    return false;
}

bool wl_xb_yield_any(void) {
    WlXbThread *self = g_cur;
    g_stats.yields++;
    if (self == NULL || g_ready == NULL || wl_xb_in_service()) {
        return false;
    }
    if (irql_raised(self)) {
        g_stats.raised_holds++; // the processor stays its (yield_if_due)
        return false;
    }
    wl_platform_mutex_lock(g_m);
    WlXbThread *head = g_ready;
    if (head == NULL) {
        wl_platform_mutex_unlock(g_m);
        return false;
    }
    if (head->priority >= self->priority || head->is_service) {
        wl_platform_mutex_unlock(g_m);
        requeue(self);
        return true;
    }
    // below the caller: the console runs it for the rest of a quantum (the caller, ready above it,
    // comes back at the quantum's end or when it waits), as the starvation hand-off does
    g_ready = head->next_ready;
    head->next_ready = NULL;
    head->ready = false;
    g_stats.yields_lower++;
    give_to(self, head);
    return true;
}

// ---- Threads -----------------------------------------------------------------------------------

static WlXbThread *new_thread(int priority) {
    WlXbThread *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    if (wl_platform_cond_create(&t->cond) != WL_OK) {
        free(t);
        return NULL;
    }
    wl_xb_cpu_reset(&t->cpu);
    t->priority = priority;
    wl_platform_mutex_lock(g_m);
    t->id = g_next_id++;
    t->next_all = g_all;
    g_all = t;
    wl_platform_mutex_unlock(g_m);
    return t;
}

WlResult wl_xb_threads_init(const WlXbThreadConfig *cfg, WlXbThread **out) {
    if (g_ready_init) {
        return WL_ERR_STATE;
    }
    if (!wl_xb_mem_ready() || out == NULL) {
        return WL_ERR_ARG;
    }
    if (cfg != NULL) {
        g_cfg = *cfg;
    } else {
        wl_xb_thread_config_default(&g_cfg);
    }
    if (wl_platform_mutex_create(&g_m) != WL_OK || wl_platform_cond_create(&g_proc_cond) != WL_OK) {
        return WL_ERR_NOMEM;
    }
    if (!g_card_made) {
        if (wl_platform_mutex_create(&g_card) != WL_OK) {
            return WL_ERR_NOMEM;
        }
        g_card_made = true;
    }
    g_det = g_cfg.deterministic;
    g_preempt = g_cfg.preempt && !g_det;
    if (g_det) {
        g_det_backedge_ns = g_cfg.det_backedge_ns != 0 ? g_cfg.det_backedge_ns : 40u;
        g_det_call_ns = g_cfg.det_call_ns != 0 ? g_cfg.det_call_ns : 400u;
        g_det_device_ns = g_cfg.det_device_ns != 0 ? g_cfg.det_device_ns : 400u;
        g_xb_attention = 1; // every backward jump is a safe point: the clock counts them
        WL_LOG_I(TAG, "deterministic mode: a virtual clock (%u ns a backward jump, %u ns a kernel call, %u ns a "
                 "device access)", (unsigned)g_det_backedge_ns, (unsigned)g_det_call_ns, (unsigned)g_det_device_ns);
    }
    g_ready_init = true;
    (void)wl_mem_census_add("threads", census_threads, NULL);
    WlXbThread *t = new_thread(WL_XB_PRIORITY_NORMAL);
    if (t == NULL) {
        return WL_ERR_NOMEM;
    }
    strcpy(t->name, "boot");
    g_stats.threads_created = 1;
    wl_xb_lock(t);
    *out = t;
    return WL_OK;
}

// One page of kernel memory (the window), zeroed: VA 0x80000000|P.
static uint32_t kernel_page(void) {
    uint32_t phys = 0;
    if (wl_xb_phys_alloc(WL_XB_PAGE, 0, 0xFFFFFFFFu, WL_XB_PAGE, &phys) != WL_OK) {
        return 0;
    }
    return WL_XB_WINDOW | phys;
}

void wl_xb_thread_set_tls_template(uint32_t data_va, uint32_t data_bytes, uint32_t zero_bytes) {
    g_tls_data = data_va;
    g_tls_bytes = data_bytes;
    g_tls_zero = zero_bytes;
}

WlResult wl_xb_thread_setup_guest(WlXbThread *t, uint32_t stack_bytes, uint32_t tls_template, uint32_t tls_template_bytes,
                                  uint32_t tls_zero_bytes) {
    if (stack_bytes == 0) {
        stack_bytes = g_cfg.default_stack_bytes;
    }
    stack_bytes = (stack_bytes + WL_XB_PAGE - 1u) & ~(WL_XB_PAGE - 1u);
    uint32_t tls = tls_template_bytes + tls_zero_bytes;
    bool has_tls = tls_template != 0 || tls > 0;
    uint32_t area = has_tls ? ((tls < 64u ? 64u : tls) + 15u) & ~15u : 0;
    uint32_t block = has_tls ? ((tls + 15u) & ~15u) + 4u : 0;
    uint32_t extra = (area + block + WL_XB_PAGE - 1u) & ~(WL_XB_PAGE - 1u);
    // stacks go high in user space, away from the title's own allocations
    uint32_t va = 0;
    WlResult r = wl_xb_va_reserve(&va, stack_bytes + extra + WL_XB_PAGE, 0x0C000000u, WL_XB_PROT_READWRITE);
    if (r != WL_OK) {
        return r;
    }
    uint32_t bottom = va + WL_XB_PAGE; // the page below stays reserved, not committed: a guard
    // recompiled code addresses the stack as esp + disp without the address map (MEMxx_SP, PUSH32
    // and POP32 in recomp/recomp_types.h): it must lie where WL_XB_EA is the identity
    if ((uint64_t)va + stack_bytes + extra + WL_XB_PAGE > WL_XB_USER_LIMIT) {
        wl_xb_va_release(va, NULL);
        return WL_ERR_STATE;
    }
    r = wl_xb_va_commit(bottom, stack_bytes + extra, WL_XB_PROT_READWRITE);
    if (r != WL_OK) {
        wl_xb_va_release(va, NULL);
        return r;
    }
    wl_xb_census_alloc(WL_XB_CA_STACK, bottom, stack_bytes + extra, 0);
    uint32_t top = bottom + stack_bytes + extra;
    uint32_t block_start = top - block;
    uint32_t area_start = block_start - area;
    if (has_tls) {
        for (uint32_t i = 0; i < tls_template_bytes; i++) {
            uint8_t b = wl_xb_rd8(tls_template + i);
            wl_xb_wr8(area_start + i, b);
            if (i + 4u < block) {
                wl_xb_wr8(block_start + i, b);
            }
        }
        wl_xb_wr32(block_start, area_start); // the slot XAPI reads at [StackBase + index * 4]
    }
    uint32_t page = kernel_page();
    if (page == 0) {
        return WL_ERR_NOMEM;
    }
    uint32_t kpcr = page, kth = page + KTHREAD_OFFSET;
    wl_xb_wr32(kpcr + 0x00, 0xFFFFFFFFu); // NtTib.ExceptionList: the end of the chain
    wl_xb_wr32(kpcr + 0x04, top);         // NtTib.StackBase (the TLS block sits below it)
    wl_xb_wr32(kpcr + 0x08, bottom);      // NtTib.StackLimit
    wl_xb_wr32(kpcr + 0x18, kpcr);        // NtTib.Self
    wl_xb_wr32(kpcr + 0x1C, kpcr);        // SelfPcr
    wl_xb_wr32(kpcr + 0x20, kpcr + 0x28); // Prcb
    wl_xb_wr8(kpcr + KPCR_IRQL, 0);
    wl_xb_wr32(kpcr + 0x28, kth);         // PrcbData.CurrentThread
    // KTHREAD: a dispatcher header (ThreadObject, signalled when the thread ends)
    wl_xb_wr8(kth + 0x00, 6);
    wl_xb_wr8(kth + 0x02, (uint8_t)(KTHREAD_BYTES / 4u));
    wl_xb_wr32(kth + 0x04, 0);
    wl_xb_wr32(kth + 0x08, kth + 0x08);   // WaitListHead: empty
    wl_xb_wr32(kth + 0x0C, kth + 0x08);
    wl_xb_wr32(kth + 0x10, kth + 0x10);   // MutantListHead: empty
    wl_xb_wr32(kth + 0x14, kth + 0x10);
    wl_xb_wr32(kth + 0x1C, top);          // StackBase
    wl_xb_wr32(kth + 0x20, bottom);       // StackLimit
    wl_xb_wr32(kth + 0x28, area_start);   // TlsData
    wl_xb_wr8(kth + 0x32, (uint8_t)(t->priority & 31));
    wl_xb_wr32(kth + 0x134, t->id);       // ETHREAD.UniqueThread
    wl_xb_wr32(kth + 0x138, t->start_routine);
    t->kpcr = kpcr;
    t->kthread = kth;
    t->stack_base = bottom;
    t->stack_top = top;
    t->tls_data = has_tls ? area_start : 0;
    uint32_t esp = (has_tls ? area_start : top) - 16u;
    if (t == g_cur) {
        g_esp = esp;
        g_fs_base = kpcr;
    } else {
        t->cpu.g_esp = esp;
        t->cpu.g_fs_base = kpcr;
    }
    return WL_OK;
}

static void thread_finished(WlXbThread *t) {
    t->terminated = true;
    t->running_guest = false;
    if (t->kthread != 0) {
        wl_xb_wr32(t->kthread + 0x04, 1); // signalled: waiters on the thread see it end
    }
    if (g_exit_hook != NULL) {
        g_exit_hook(t);
    }
}

// The host side of a guest thread: wait to be resumed, take the lock, run the routine.
// The deterministic mode: a new or resumed thread goes in the ready queue now, at a point of the
// guest's (its host thread only waits for the baton).
static void det_start(WlXbThread *t) {
    wl_platform_mutex_lock(g_m);
    t->woken = false;
    if (!t->ready && g_owner != t && !t->running_guest && !t->terminated) {
        enqueue(t);
    }
    wl_platform_mutex_unlock(g_m);
}

static void *thread_main(void *arg) {
    WlXbThread *t = arg;
    if (g_det) {
        // queued by its creation or its resume (det_start): it waits for the baton
        wl_platform_mutex_lock(g_m);
        det_await(t);
        wl_platform_mutex_unlock(g_m);
        det_enter(t);
    } else {
        wl_platform_mutex_lock(g_m);
        while (t->suspend_count > 0) {
            wl_platform_cond_wait(t->cond, g_m);
        }
        t->woken = false;
        wl_platform_mutex_unlock(g_m);
        wl_xb_lock(t);
    }
    if (setjmp(t->exit_jmp) == 0) {
        t->running_guest = true;
        uint32_t args[2] = {t->start_ctx1, t->start_ctx2};
        bool found = false;
        wl_xb_call_guest(t->start_routine, 2, args, &found);
        if (!found) {
            WL_LOG_E(TAG, "thread %u: start routine 0x%08x is not code", (unsigned)t->id, (unsigned)t->start_routine);
        }
    }
    thread_finished(t);
    wl_platform_mutex_lock(g_m);
    g_live--;
    g_stats.threads_live = g_live;
    bool last = g_live == 0;
    if (last && !g_exiting) {
        g_exiting = true;
        g_exit_code = 0;
        wl_platform_cond_broadcast(g_proc_cond);
    }
    wl_platform_mutex_unlock(g_m);
    wl_xb_unlock(t);
    // (the last its host stack is used for: the stack may go back a while later, wl_xb_thread_reap)
    __atomic_store_n(&t->host_done_ms, (uint32_t)wl_platform_time_ms() | 1u, __ATOMIC_RELEASE);
    return NULL;
}

WlResult wl_xb_thread_create(uint32_t routine, uint32_t ctx1, uint32_t ctx2, uint32_t stack_bytes, int priority,
                             bool suspended, WlXbThread **out) {
    WlXbThread *t = new_thread(priority);
    if (t == NULL) {
        return WL_ERR_NOMEM;
    }
    snprintf(t->name, sizeof t->name, "guest%u", (unsigned)t->id);
    t->start_routine = routine;
    t->start_ctx1 = ctx1;
    t->start_ctx2 = ctx2;
    t->suspend_count = suspended ? 1u : 0u;
    t->counted = true;
    WlResult r = wl_xb_thread_setup_guest(t, stack_bytes, g_tls_data, g_tls_bytes, g_tls_zero);
    if (r != WL_OK) {
        return r;
    }
    // the host stack follows what the title asked for its guest stack (a recompiled guest frame is
    // a C frame, of about twice the guest's bytes), within [64 KB, host_stack_bytes]: Halo's
    // workers ask 16 KB and its main thread 512 KB, and seven 1 MB stacks were a quarter of what
    // the Wii's host side has. Real-Wii and Dolphin runs of Halo's levels (2026-10, ~5400 census
    // reports of each size) used at most 14 KB of the main thread's 1 MB and 4 KB of a worker's
    // 192 KB: the main thread's 256 KB and a worker's 64 KB are still 16 times that, and give
    // 1.25 MB back to the host side's heap (the census warns past three quarters of a stack).
    // Card runs 11-13 (2026-10-17): still 14 KB at most, so the main thread's is 128 KB since
    // 2026-10-18 (9 times that; "bigstacks" in the title's folder: 256 KB)
    size_t host = (size_t)(stack_bytes != 0 ? stack_bytes : g_cfg.default_stack_bytes) * 2u;
    if (host < (64u << 10)) {
        host = 64u << 10;
    }
    if (host > g_cfg.host_stack_bytes) {
        host = g_cfg.host_stack_bytes;
    }
    t->host_stack = aligned_alloc(64, host);
    if (t->host_stack == NULL) {
        return WL_ERR_NOMEM;
    }
    wl_platform_stack_fill(t->host_stack, host);
    t->host_stack_size = host;
    wl_platform_mutex_lock(g_m);
    g_live++;
    g_stats.threads_created++;
    g_stats.threads_live = g_live;
    wl_platform_mutex_unlock(g_m);
    r = wl_platform_thread_create(&t->host, thread_main, t, t->host_stack, host, WL_THREAD_PRIO_MAIN);
    if (r != WL_OK) {
        wl_platform_mutex_lock(g_m);
        g_live--;
        wl_platform_mutex_unlock(g_m);
        return r;
    }
    wl_platform_thread_detach(t->host);
    t->det_created = true;
    if (g_det && t->suspend_count == 0) {
        det_start(t);
    }
    if (out != NULL) {
        *out = t;
    }
    return WL_OK;
}

typedef struct {
    WlXbThread *t;
    WlXbHostThreadFn fn;
    void *user;
} HostStart;

static void *host_thread_main(void *arg) {
    HostStart start = *(HostStart *)arg;
    free(arg);
    WlXbThread *t = start.t;
    if (g_det) {
        wl_platform_mutex_lock(g_m);
        det_await(t);
        wl_platform_mutex_unlock(g_m);
        det_enter(t);
    } else {
        wl_xb_lock(t);
    }
    if (setjmp(t->exit_jmp) == 0) {
        t->running_guest = true;
        start.fn(t, start.user);
    }
    t->running_guest = false;
    t->terminated = true;
    wl_xb_unlock(t);
    __atomic_store_n(&t->host_done_ms, (uint32_t)wl_platform_time_ms() | 1u, __ATOMIC_RELEASE);
    return NULL;
}

WlResult wl_xb_thread_create_host(const char *name, WlXbHostThreadFn fn, void *user, uint32_t stack_bytes, int priority,
                                  WlXbThread **out) {
    if (fn == NULL) {
        return WL_ERR_ARG;
    }
    WlXbThread *t = new_thread(priority);
    HostStart *start = malloc(sizeof *start);
    if (t == NULL || start == NULL) {
        free(start);
        return WL_ERR_NOMEM;
    }
    snprintf(t->name, sizeof t->name, "%s", name != NULL ? name : "host");
    WlResult r = wl_xb_thread_setup_guest(t, stack_bytes, g_tls_data, g_tls_bytes, g_tls_zero);
    if (r != WL_OK) {
        free(start);
        return r;
    }
    size_t host = g_cfg.host_stack_bytes; // (the play harness's used at most 3 KB on the card)
    t->host_stack = aligned_alloc(64, host);
    if (t->host_stack == NULL) {
        free(start);
        return WL_ERR_NOMEM;
    }
    wl_platform_stack_fill(t->host_stack, host);
    t->host_stack_size = host;
    start->t = t;
    start->fn = fn;
    start->user = user;
    r = wl_platform_thread_create(&t->host, host_thread_main, start, t->host_stack, host, WL_THREAD_PRIO_MAIN);
    if (r != WL_OK) {
        free(start);
        return r;
    }
    wl_platform_thread_detach(t->host);
    if (g_det) {
        det_start(t);
    }
    if (out != NULL) {
        *out = t;
    }
    return WL_OK;
}

uint32_t wl_xb_thread_resume(WlXbThread *t) {
    wl_platform_mutex_lock(g_m);
    uint32_t before = t->suspend_count;
    if (t->suspend_count > 0) {
        t->suspend_count--;
        if (t->suspend_count == 0) {
            wl_platform_cond_signal(t->cond);
        }
    }
    wl_platform_mutex_unlock(g_m);
    if (g_det && before == 1u && t->det_created) {
        det_start(t);
    }
    return before;
}

uint32_t wl_xb_thread_suspend(WlXbThread *t) {
    // only a thread that has not started yet stops: a running guest thread cannot be frozen
    // between two instructions here (none of the titles this serves suspends one that runs)
    wl_platform_mutex_lock(g_m);
    uint32_t before = t->suspend_count;
    if (!t->running_guest && !t->terminated) {
        t->suspend_count++;
        if (g_det && t->ready) {
            unqueue(t); // not started yet: out of the queue until its resume
        }
    } else {
        WL_LOG_W(TAG, "suspend of running thread %u ignored", (unsigned)t->id);
    }
    wl_platform_mutex_unlock(g_m);
    return before;
}

void wl_xb_thread_exit(uint32_t status) {
    WlXbThread *t = g_cur;
    WL_ASSERT(t != NULL);
    t->exit_status = status;
    if (t->running_guest && !t->is_service && t->host.id != 0) {
        longjmp(t->exit_jmp, 1);
    }
    // a thread with no host thread of ours to unwind to (the boot thread): it is done; the
    // process goes on
    thread_finished(t);
    wl_platform_mutex_lock(g_m);
    if (t->counted && g_live > 0) {
        g_live--;
    }
    bool last = t->counted && g_live == 0;
    if (last && !g_exiting) {
        g_exiting = true;
        g_exit_code = (int)status;
        wl_platform_cond_broadcast(g_proc_cond);
    }
    wl_platform_mutex_unlock(g_m);
    wl_xb_cpu_save(&t->cpu);
    g_cur = NULL;
    wl_platform_mutex_lock(g_m);
    hand_off();
    for (;;) {
        wl_platform_cond_wait(t->cond, g_m); // never handed the lock again
    }
}

void wl_xb_thread_set_exit_hook(WlXbThreadExitHook hook) {
    g_exit_hook = hook;
}

void wl_xb_thread_set_priority(WlXbThread *t, int priority) {
    if (priority < 0) {
        priority = 0;
    }
    if (priority > 31 && !t->is_service) {
        priority = 31;
    }
    wl_platform_mutex_lock(g_m);
    if (t == g_cur && priority < t->priority) {
        g_xb_attention = 1; // the holder lowered itself: a ready thread may outrank it now, with no enqueue to say so
    }
    t->priority = priority;
    if (t->ready) {
        unqueue(t); // queued again at its new priority (a thread made and then given its priority)
        uint64_t since = t->ready_since_us;
        enqueue(t);
        t->ready_since_us = since;
    }
    wl_platform_mutex_unlock(g_m);
    if (t->kthread != 0) {
        wl_xb_wr8(t->kthread + 0x32, (uint8_t)(priority & 31));
    }
}

WlXbThread *wl_xb_thread_by_kthread(uint32_t kthread_va) {
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        if (t->kthread == kthread_va) {
            return t;
        }
    }
    return NULL;
}

WlXbThread *wl_xb_thread_by_id(uint32_t id) {
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        if (t->id == id) {
            return t;
        }
    }
    return NULL;
}

void wl_xb_thread_foreach(void (*fn)(WlXbThread *t, void *user), void *user) {
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        fn(t, user);
    }
}

// ---- Calling guest code ------------------------------------------------------------------------

uint32_t wl_xb_call_guest(uint32_t va, uint32_t argc, const uint32_t *args, bool *found) {
    recomp_func_t fn = wl_xb_lookup(va);
    if (found != NULL) {
        *found = fn != NULL;
    }
    if (fn == NULL) {
        WL_LOG_E(TAG, "call of guest 0x%08x: no code there", (unsigned)va);
        return 0;
    }
    uint32_t saved = g_esp;
    for (uint32_t i = argc; i-- > 0;) {
        g_esp -= 4u;
        wl_xb_wr32(g_esp, args[i]);
    }
    g_esp -= 4u;
    wl_xb_wr32(g_esp, 0); // the return address: nowhere (the callee returns to C)
    // guest code called from a kernel call (a thread's routine run inline, an APC) is guest code
    // again: its device accesses are safe points
    WlXbThread *self = g_cur;
    uint32_t depth = self != NULL ? self->kernel_depth : 0;
    if (self != NULL) {
        self->kernel_depth = 0;
    }
    fn();
    if (self != NULL) {
        self->kernel_depth = depth;
    }
    g_esp = saved;
    return g_eax;
}

uint32_t wl_xb_call_guest_preserving(uint32_t va, uint32_t argc, const uint32_t *args) {
    WlXbCpu saved;
    wl_xb_cpu_save(&saved);
    uint32_t r = wl_xb_call_guest(va, argc, args, NULL);
    wl_xb_cpu_load(&saved);
    return r;
}

// ---- The process -------------------------------------------------------------------------------

void wl_xb_set_process_exit(WlXbExitFn fn) {
    g_exit_fn = fn;
}

void wl_xb_process_exit(int code) {
    WL_LOG_I(TAG, "process exit %d", code);
    if (g_exit_fn != NULL) {
        g_exit_fn(code); // the boot code's: may not return
    }
    WlXbThread *self = g_cur;
    wl_platform_mutex_lock(g_m);
    if (!g_exiting) {
        g_exiting = true;
        g_exit_code = code;
    }
    wl_platform_cond_broadcast(g_proc_cond);
    if (self != NULL && g_owner == self) {
        wl_xb_cpu_save(&self->cpu);
        g_cur = NULL;
        hand_off();
    }
    for (;;) {
        if (self != NULL) {
            wl_platform_cond_wait(self->cond, g_m);
        } else {
            wl_platform_cond_wait(g_proc_cond, g_m);
        }
    }
}

int wl_xb_process_wait(void) {
    wl_platform_mutex_lock(g_m);
    while (!g_exiting) {
        wl_platform_cond_wait(g_proc_cond, g_m);
    }
    int code = g_exit_code;
    wl_platform_mutex_unlock(g_m);
    return code;
}

void wl_xb_thread_stats(WlXbThreadStats *out) {
    *out = g_stats;
}

void wl_xb_latency_add(WlXbLatency *h, uint64_t us) {
    static const uint32_t bound_ms[WL_XB_LAT_BUCKETS - 1u] = {1, 2, 5, 10, 20, 30, 40, 60, 100};
    uint32_t ms = us >= 0xFFFFFFFFull * 1000u ? 0xFFFFFFFFu : (uint32_t)(us / 1000u);
    uint32_t b = 0;
    while (b < WL_XB_LAT_BUCKETS - 1u && ms >= bound_ms[b]) {
        b++;
    }
    h->n[b]++;
    h->count++;
    if (ms > h->max_ms) {
        h->max_ms = ms;
    }
}

void wl_xb_latency_log(const char *tag, const char *what, WlXbLatency *h) {
    WlXbLatency c = *h;
    memset(h, 0, sizeof *h);
    if (c.count == 0) {
        return;
    }
    WL_LOG_I(tag, "%s: %u, ms <1:%u <2:%u <5:%u <10:%u <20:%u <30:%u <40:%u <60:%u <100:%u more:%u max %u", what,
             (unsigned)c.count, (unsigned)c.n[0], (unsigned)c.n[1], (unsigned)c.n[2], (unsigned)c.n[3],
             (unsigned)c.n[4], (unsigned)c.n[5], (unsigned)c.n[6], (unsigned)c.n[7], (unsigned)c.n[8],
             (unsigned)c.n[9], (unsigned)c.max_ms);
}

void wl_xb_thread_times(const WlXbThread *t, WlXbThreadTimes *out) {
    memset(out, 0, sizeof *out);
    if (t == NULL) {
        return;
    }
    wl_platform_mutex_lock(g_m);
    uint64_t now = wl_xb_time_us();
    out->ready_us = t->ready_us + (t->ready ? now - t->ready_since_us : 0u);
    out->waited_us = t->waited_us;
    out->io_us = t->io_us;
    out->gpu_us = t->gpu_us;
    if (t->off_since_us != 0 && now > t->off_since_us) {
        uint64_t us = now - t->off_since_us;
        *(t->off_kind == WL_XB_OFF_IO ? &out->io_us : t->off_kind == WL_XB_OFF_GPU ? &out->gpu_us : &out->waited_us) += us;
    }
    wl_platform_mutex_unlock(g_m);
}

uint32_t wl_xb_thread_service_wait_max(uint32_t *holder) {
    wl_platform_mutex_lock(g_m);
    uint32_t ms = (uint32_t)(g_svc_wait_max_us / 1000u);
    *holder = g_svc_wait_holder;
    g_svc_wait_max_us = 0;
    g_svc_wait_holder = 0;
    wl_platform_mutex_unlock(g_m);
    return ms;
}

void wl_xb_thread_log_ready_waits(void) {
    wl_platform_mutex_lock(g_m);
    WlXbLatency c = g_ready_waits, backs = g_gpu_backs;
    memset(&g_ready_waits, 0, sizeof g_ready_waits);
    memset(&g_gpu_backs, 0, sizeof g_gpu_backs);
    WlXbThreadStats st = g_stats;
    wl_platform_mutex_unlock(g_m);
    static uint32_t last_starved, last_lower, last_io, last_tie, last_gpu, last_gpu_ms;
    if (backs.count != 0) {
        WL_LOG_I(TAG, "GPU waits: %u gave the guest lock up, %u ms off it, since the last line",
                 (unsigned)(st.gpu_blocks - last_gpu), (unsigned)(st.gpu_off_ms - last_gpu_ms));
    }
    last_gpu = st.gpu_blocks;
    last_gpu_ms = st.gpu_off_ms;
    wl_xb_latency_log(TAG, "GPU waits' lock back", &backs);
    if (c.count != 0) {
        WL_LOG_I(TAG,
                 "ready waits: %u starved hand-offs, %u yields to a lower priority, %u hand-offs to a thread back "
                 "from the card, %u to one of equal priority after its quantum since the last line",
                 (unsigned)(st.starved_handoffs - last_starved), (unsigned)(st.yields_lower - last_lower),
                 (unsigned)(st.io_handoffs - last_io), (unsigned)(st.tie_handoffs - last_tie));
    }
    last_starved = st.starved_handoffs;
    last_lower = st.yields_lower;
    last_io = st.io_handoffs;
    last_tie = st.tie_handoffs;
    wl_xb_latency_log(TAG, "ready waits", &c);
}

void wl_xb_thread_host_memory(size_t *bytes, uint32_t *stacks, size_t *deepest, size_t *deepest_of) {
    size_t total = 0, deep = 0, deep_of = 0;
    uint32_t n = 0;
    wl_platform_mutex_lock(g_m);
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        total += sizeof *t + t->host_stack_size;
        if (t->host_stack != NULL) {
            size_t used = wl_platform_stack_used(t->host_stack, t->host_stack_size);
            n++;
            if (used * 1024u / t->host_stack_size > (deep_of != 0 ? deep * 1024u / deep_of : 0)) {
                deep = used;
                deep_of = t->host_stack_size;
            }
        }
    }
    wl_platform_mutex_unlock(g_m);
    *bytes = total;
    *stacks = n;
    *deepest = deep;
    *deepest_of = deep_of;
}

uint32_t wl_xb_thread_reap(void) {
    // (a grace of seconds after the thread's last statement: what is left of it, its function's
    // return and the platform thread's end, has long run by then)
    const uint32_t grace_ms = 2000u;
    uint32_t now = (uint32_t)wl_platform_time_ms();
    uint32_t freed = 0;
    size_t bytes = 0;
    if (!g_ready_init) {
        return 0;
    }
    wl_platform_mutex_lock(g_m);
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        uint32_t done = __atomic_load_n(&t->host_done_ms, __ATOMIC_ACQUIRE);
        if (t->host_stack != NULL && t->terminated && done != 0 && now - done >= grace_ms) {
            free(t->host_stack);
            bytes += t->host_stack_size;
            t->host_stack = NULL;
            t->host_stack_size = 0;
            freed++;
        }
    }
    wl_platform_mutex_unlock(g_m);
    if (freed != 0) {
        WL_LOG_I(TAG, "%u ended threads' host stacks given back (%u KB)", (unsigned)freed, (unsigned)(bytes >> 10));
    }
    return freed;
}

uint32_t wl_xb_thread_progress(void) {
    return g_stats.handoffs + g_stats.safe_points + g_stats.waits + g_stats.wakes + g_stats.timeouts +
           g_stats.kernel_calls;
}

uint32_t wl_xb_stall_watch_look(WlXbStallWatch *w, uint32_t progress) {
    if (progress != w->last) {
        w->last = progress;
        w->quiet = 0;
        return 0;
    }
    if (++w->quiet < 2u || (w->quiet - 2u) % 6u != 0u) {
        return 0;
    }
    return w->quiet * 10u;
}

// A stall: the guest made no progress (no hand-off, safe point, wait, wake, kernel call or page
// fault) since the last look. Called from the service thread's status (every 10 s); after two
// quiet looks it logs who holds the guest lock and every thread's state, once a minute while it
// lasts, so a hang names its threads. Kernel calls and faults count because the deterministic mode
// counts a safe point only when something is due on the virtual clock, which a kernel call moves on
// 400 ns and file I/O not at all: a lone thread rebuilding a map's cache slot on a loaded disk (10 ms
// a call) went 25 s between counted events and was reported as a stall.
void wl_xb_thread_stall_check(uint32_t paging) {
    static WlXbStallWatch watch;
    uint32_t quiet_s = wl_xb_stall_watch_look(&watch, wl_xb_thread_progress() + paging);
    if (quiet_s == 0) {
        return;
    }
    wl_platform_mutex_lock(g_m);
    WlXbThread *owner = g_owner;
    WL_LOG_W(TAG, "stall: no scheduling for %u s; the lock is %s's (%u), the ready queue's head %s", (unsigned)quiet_s,
             owner != NULL ? owner->name : "nobody's", owner != NULL ? (unsigned)owner->id : 0u,
             g_ready != NULL ? g_ready->name : "(empty)");
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        WL_LOG_W(TAG, "stall: %u %s prio %d%s%s%s%s%s susp %u kdepth %u esp %08x eax %08x", (unsigned)t->id, t->name,
                 t->priority, t->ready ? " ready" : "", t->woken ? " woken" : "", t->terminated ? " ended" : "",
                 t->running_guest ? " running" : "",
                 t->off_since_us != 0 ? (t->off_kind == WL_XB_OFF_GPU ? " in a GPU wait" : t->off_kind == WL_XB_OFF_IO ? " in I/O" : "") : "", (unsigned)t->suspend_count, (unsigned)t->kernel_depth,
                 (unsigned)t->cpu.g_esp, (unsigned)t->cpu.g_eax);
    }
    wl_platform_mutex_unlock(g_m);
}

// A guest thread's stack scanned for return addresses (words inside the image's code), the
// innermost first, and its IRQL, logged as "<what>: ...". The holder's stack pointer is the live
// one. Only pages with a frame are read (a fault here, on the service thread, must not happen).
// g_m held.
static void log_guest_stack(const char *what, const WlXbThread *t, const WlXbThread *owner) {
    uint32_t esp = t == owner ? g_esp : t->cpu.g_esp;
    char line[200];
    size_t used = 0;
    unsigned found = 0;
    line[0] = '\0';
    for (uint32_t i = 0; i < 256u && found < 14u && used + 12u < sizeof line; i++) {
        uint32_t va = esp + i * 4u;
        uint8_t b[4];
        if (!wl_xb_mem_peek(va, b, 4)) { // (no guest lock here: never a fault)
            break;
        }
        uint32_t w = wl_rd_le32(b);
        if (w >= g_xbox_code_lo && w < g_xbox_code_hi) {
            used += (size_t)snprintf(line + used, sizeof line - used, " %x", (unsigned)w);
            found++;
        }
    }
    // its IRQL (a holder at DISPATCH_LEVEL keeps the lock: thread.h, wl_xb_safe_point)
    uint8_t irql8 = 0;
    unsigned irql = t->kpcr != 0 && wl_xb_mem_peek(t->kpcr + KPCR_IRQL, &irql8, 1) ? irql8 : 0u;
    char at[16] = "";
    if (irql != 0) {
        snprintf(at, sizeof at, " irql %u", irql);
    }
    WL_LOG_W(TAG, "%s: %u %s%s%s esp %08x:%s", what, (unsigned)t->id, t->name, t == owner ? " (holds the lock)" : "",
             at, (unsigned)esp, found != 0 ? line : " no code addresses");
}

// Each live guest thread's stack: where the threads are when the title stops showing frames.
void wl_xb_thread_log_guest_stacks(void) {
    wl_platform_mutex_lock(g_m);
    WlXbThread *owner = g_owner;
    for (WlXbThread *t = g_all; t != NULL; t = t->next_all) {
        if (!t->terminated && !t->is_service) {
            log_guest_stack("freeze", t, owner);
        }
    }
    wl_platform_mutex_unlock(g_m);
}

void wl_xb_thread_log_stacks(void) {
    char line[200];
    size_t used = 0;
    line[0] = '\0';
    wl_platform_mutex_lock(g_m);
    for (WlXbThread *t = g_all; t != NULL && used + 40u < sizeof line; t = t->next_all) {
        if (t->host_stack != NULL) {
            used += (size_t)snprintf(line + used, sizeof line - used, "%s%s %u/%u%s", used != 0 ? ", " : "", t->name,
                                     (unsigned)(wl_platform_stack_used(t->host_stack, t->host_stack_size) >> 10),
                                     (unsigned)(t->host_stack_size >> 10), t->terminated ? " (ended)" : "");
        }
    }
    wl_platform_mutex_unlock(g_m);
    WL_LOG_I(TAG, "host stacks, deepest/size KB: %s", line);
}

static void census_threads(void *user, WlMemCensusReport *out) {
    (void)user;
    uint32_t stacks;
    size_t deepest, of;
    wl_xb_thread_host_memory(&out->bytes, &stacks, &deepest, &of);
    // (a host stack's overflow runs into the heap below it, silently: said while there is room)
    static bool warned;
    if (of != 0 && deepest > of / 4u * 3u && !warned) {
        warned = true;
        WL_LOG_W(TAG, "a host stack has been %u KB deep of its %u KB: raise WlXbThreadConfig.host_stack_bytes "
                 "or the 64 KB floor (thread.c)", (unsigned)(deepest >> 10), (unsigned)(of >> 10));
    }
    if (of != 0) {
        snprintf(out->note, sizeof out->note, "%u stacks, deepest %u of %u", (unsigned)stacks, (unsigned)(deepest >> 10),
                 (unsigned)(of >> 10));
    } else {
        // (the host's threads run on stacks of their own: these are not used there)
        snprintf(out->note, sizeof out->note, "%u stacks, not used here", (unsigned)stacks);
    }
}
