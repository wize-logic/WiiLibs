// SPDX-License-Identifier: MIT
// The dispatcher: waits on objects that live in guest memory, their signals, user APCs.
//
// The objects' state is their DISPATCHER_HEADER's SignalState in guest memory, read afresh at every
// check (titles initialise and reset some of them inline), and the guest lock is the dispatcher
// lock. A waiting thread records its objects. A signal satisfies the waits it can at once, as NT
// does: in the order the waits began, each satisfied waiter's objects are acquired for it (a
// synchronization event reset, a semaphore's count taken, a mutant owned) and its status kept for
// it to return when it runs. So a pulse, or a set and a reset before the waiter runs, still
// releases it, and a waiter of lower priority than the signaller is not overtaken.
#include "wl/xboxkrnl/kernel_internal.h"

#define FILETIME_1970 116444736000000000ull // 100 ns intervals from 1601 to 1970

// The deterministic mode's calendar starts on 2004-11-15 00:00 UTC (a fixed date, not the host's).
#define DET_EPOCH_UNIX_US 1100476800000000ull

static uint32_t g_wait_seq; // counts the waits begun (XkThread.wait_seq)

uint64_t xk_system_time(void) {
    if (wl_xb_deterministic()) {
        return (DET_EPOCH_UNIX_US + wl_xb_time_us()) * 10u + FILETIME_1970;
    }
    return wl_platform_wall_time_us() * 10u + FILETIME_1970 + (uint64_t)((int64_t)g_xk_cfg.timezone_bias * 600000000);
}

uint32_t xk_timeout_ms(const int64_t *timeout, uint64_t *deadline_us) {
    uint64_t now = wl_xb_time_us();
    if (timeout == NULL) {
        *deadline_us = 0;
        return WL_WAIT_FOREVER;
    }
    int64_t t = *timeout;
    uint64_t delta_us;
    if (t < 0) {
        delta_us = (uint64_t)(-(t + 1)) / 10u + 1u; // relative
    } else if (t == 0) {
        delta_us = 0;
    } else {
        uint64_t sys = xk_system_time();
        delta_us = (uint64_t)t > sys ? ((uint64_t)t - sys) / 10u : 0;
    }
    *deadline_us = now + delta_us;
    uint64_t ms = (delta_us + 999u) / 1000u;
    return ms >= WL_WAIT_FOREVER ? WL_WAIT_FOREVER - 1u : (uint32_t)ms;
}

static bool is_mutant(uint32_t obj) {
    return wl_xb_rd8(obj + DISP_TYPE) == OBJ_MUTANT;
}

static bool satisfied(uint32_t obj, const WlXbThread *t) {
    int32_t signal = (int32_t)wl_xb_rd32(obj + DISP_SIGNAL);
    if (is_mutant(obj)) {
        return signal > 0 || wl_xb_rd32(obj + KMUTANT_OWNER) == t->kthread;
    }
    return signal > 0;
}

// ---- Owned mutants -------------------------------------------------------------------------------

static void owned_remove(WlXbThread *t, uint32_t mut) {
    XkThread *k = t != NULL ? t->kernel : NULL;
    if (k == NULL) {
        return;
    }
    for (uint32_t i = 0; i < k->owned_count; i++) {
        if (k->owned[i] == mut) {
            k->owned[i] = k->owned[--k->owned_count];
            return;
        }
    }
}

void xk_mutant_owned_by(WlXbThread *t, uint32_t mut) {
    XkThread *k = t != NULL ? xk_thread_state(t) : NULL;
    if (k == NULL) {
        return;
    }
    for (uint32_t i = 0; i < k->owned_count; i++) {
        if (k->owned[i] == mut) {
            return;
        }
    }
    if (k->owned_count == XK_MAX_OWNED) {
        WL_LOG_W(XK_TAG, "thread %u owns more than %u mutants: 0x%08x is not abandoned if it ends", (unsigned)t->id,
                 (unsigned)XK_MAX_OWNED, (unsigned)mut);
        return;
    }
    k->owned[k->owned_count++] = mut;
}

// The side effects of a satisfied wait for thread `t`. True when the object was an abandoned
// mutant.
static bool acquire(uint32_t obj, WlXbThread *t) {
    uint8_t type = wl_xb_rd8(obj + DISP_TYPE);
    int32_t signal = (int32_t)wl_xb_rd32(obj + DISP_SIGNAL);
    switch (type) {
    case OBJ_EVENT_SYNCHRONIZATION:
    case OBJ_TIMER_SYNCHRONIZATION:
        wl_xb_wr32(obj + DISP_SIGNAL, 0);
        return false;
    case OBJ_SEMAPHORE:
        wl_xb_wr32(obj + DISP_SIGNAL, (uint32_t)(signal - 1));
        return false;
    case OBJ_MUTANT: {
        bool abandoned = wl_xb_rd8(obj + KMUTANT_ABANDONED) != 0;
        wl_xb_wr32(obj + DISP_SIGNAL, (uint32_t)(signal - 1));
        wl_xb_wr32(obj + KMUTANT_OWNER, t->kthread);
        wl_xb_wr8(obj + KMUTANT_ABANDONED, 0);
        xk_mutant_owned_by(t, obj);
        return abandoned;
    }
    default:
        return false; // notification events and timers, threads: stay signalled
    }
}

// Whether thread `t`'s wait on `objects` is satisfied now: if so its objects are acquired for it
// and `*status` is what the wait returns.
static bool try_satisfy(WlXbThread *t, uint32_t count, const uint32_t *objects, bool wait_all, uint32_t *status) {
    if (wait_all) {
        if (count == 0) {
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!satisfied(objects[i], t)) {
                return false;
            }
        }
        bool abandoned = false;
        for (uint32_t i = 0; i < count; i++) {
            abandoned |= acquire(objects[i], t);
        }
        *status = abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0;
        return true;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (satisfied(objects[i], t)) {
            *status = acquire(objects[i], t) ? STATUS_ABANDONED_WAIT_0 + i : STATUS_WAIT_0 + i;
            return true;
        }
    }
    return false;
}

// A read's issue to its completion routine (the APCs of reads, delivered since the last report).
static WlXbLatency g_read_apc_lat;

uint32_t xk_read_issued(void) {
    return (uint32_t)(wl_xb_time_us() / 1000u) + 1u;
}

void xk_status_report(void) {
    static WlXkStats last;
    WlXkStats s = g_xk_stats;
    if (s.apcs_queued != last.apcs_queued || s.zero_delays != last.zero_delays) {
        WL_LOG_I(XK_TAG, "APCs: %u queued, %u delivered; %u zero delays, %u of them with an APC queued (10 s)",
                 (unsigned)(s.apcs_queued - last.apcs_queued), (unsigned)(s.apcs_delivered - last.apcs_delivered),
                 (unsigned)(s.zero_delays - last.zero_delays),
                 (unsigned)(s.zero_delays_with_apc - last.zero_delays_with_apc));
    }
    last = s;
    wl_xb_latency_log(XK_TAG, "reads issued to completion routine", &g_read_apc_lat);
}

bool xk_apc_deliver(WlXbThread *t) {
    XkThread *k = xk_thread_state(t);
    bool any = false;
    while (k != NULL && k->apc_count > 0) {
        XkApc apc = k->apcs[k->apc_head];
        k->apc_head = (k->apc_head + 1u) % XK_MAX_APCS;
        k->apc_count--;
        if (apc.read_issued != 0) {
            uint32_t now = xk_read_issued();
            wl_xb_latency_add(&g_read_apc_lat, (uint64_t)(now - apc.read_issued) * 1000u);
        }
        uint32_t args[3] = {apc.context, apc.arg1, apc.arg2};
        wl_xb_call_guest_preserving(apc.routine, 3, args);
        g_xk_stats.apcs_delivered++;
        any = true;
    }
    return any;
}

void xk_apc_queue(WlXbThread *t, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2,
                  uint32_t read_issued) {
    XkThread *k = xk_thread_state(t);
    if (k == NULL || routine == 0) {
        return;
    }
    if (k->apc_count == XK_MAX_APCS) {
        WL_LOG_E(XK_TAG, "thread %u: APC queue full, APC to 0x%08x dropped", (unsigned)t->id, (unsigned)routine);
        return;
    }
    k->apcs[(k->apc_head + k->apc_count) % XK_MAX_APCS] = (XkApc){routine, context, arg1, arg2, read_issued};
    k->apc_count++;
    g_xk_stats.apcs_queued++;
    if (k->waiting) {
        wl_xb_wake(t);
    }
}

uint32_t xk_wait(uint32_t count, const uint32_t *objects, bool wait_all, bool alertable, bool user_mode,
                 const int64_t *timeout) {
    WlXbThread *self = wl_xb_current();
    XkThread *k = self != NULL ? xk_thread_state(self) : NULL;
    if (k == NULL || count > XK_MAX_WAIT) {
        return STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!wl_xb_range_ok(objects[i], 16)) {
            return STATUS_INVALID_HANDLE;
        }
    }
    g_xk_stats.waits++;
    uint64_t deadline = 0;
    uint32_t ms = xk_timeout_ms(timeout, &deadline);
    for (;;) {
        // the objects first: a wait they satisfy returns their status even with an APC queued
        uint32_t status = 0;
        if (try_satisfy(self, count, objects, wait_all, &status)) {
            return status;
        }
        if (alertable && user_mode && k->apc_count > 0) {
            xk_apc_deliver(self);
            return STATUS_USER_APC;
        }
        uint32_t left = ms;
        if (ms != WL_WAIT_FOREVER) {
            uint64_t now = wl_xb_time_us();
            if (now >= deadline) {
                g_xk_stats.wait_timeouts++;
                return STATUS_TIMEOUT;
            }
            left = (uint32_t)((deadline - now + 999u) / 1000u);
        }
        k->wait_count = count;
        memcpy(k->wait_objects, objects, count * sizeof objects[0]);
        k->wait_all = wait_all;
        k->satisfied = false;
        k->wait_seq = ++g_wait_seq;
        k->waiting = true;
        wl_xb_wait(self, left);
        k->waiting = false;
        k->wait_count = 0;
        if (k->satisfied) { // by a signal while it waited (even one undone since)
            k->satisfied = false;
            return k->wait_status;
        }
    }
}

typedef struct {
    uint32_t obj;
    uint32_t after; // the wait_seq of the waiter tried last
    WlXbThread *best; // the waiter on obj whose wait began first after it
    uint32_t best_seq;
} SignalCtx;

static void next_waiter(WlXbThread *t, void *user) {
    SignalCtx *c = user;
    XkThread *k = t->kernel;
    if (k == NULL || !k->waiting || k->satisfied || k->wait_seq <= c->after ||
        (c->best != NULL && k->wait_seq >= c->best_seq)) {
        return;
    }
    for (uint32_t i = 0; i < k->wait_count; i++) {
        if (k->wait_objects[i] == c->obj) {
            c->best = t;
            c->best_seq = k->wait_seq;
            return;
        }
    }
}

void xk_object_signaled(uint32_t obj) {
    // the waiters on the object in the order their waits began, each satisfied if the object (for a
    // wait for all, every object) satisfies it now
    SignalCtx c = {obj, 0, NULL, 0};
    for (;;) {
        c.best = NULL;
        wl_xb_thread_foreach(next_waiter, &c);
        if (c.best == NULL) {
            return;
        }
        c.after = c.best_seq;
        XkThread *k = c.best->kernel;
        if (try_satisfy(c.best, k->wait_count, k->wait_objects, k->wait_all, &k->wait_status)) {
            k->satisfied = true;
            wl_xb_wake(c.best);
        }
    }
}

int32_t xk_event_set(uint32_t ev) {
    int32_t prev = (int32_t)wl_xb_rd32(ev + DISP_SIGNAL);
    wl_xb_wr32(ev + DISP_SIGNAL, 1);
    if (prev == 0) {
        xk_object_signaled(ev);
    }
    return prev;
}

int32_t xk_event_reset(uint32_t ev) {
    int32_t prev = (int32_t)wl_xb_rd32(ev + DISP_SIGNAL);
    wl_xb_wr32(ev + DISP_SIGNAL, 0);
    return prev;
}

// A pulse releases the threads waiting now (one for a synchronization event) and leaves the event
// reset: the set satisfies their waits at once, and they run when they get the lock.
int32_t xk_event_pulse(uint32_t ev) {
    int32_t prev = xk_event_set(ev);
    wl_xb_wr32(ev + DISP_SIGNAL, 0);
    return prev;
}

uint32_t xk_semaphore_release(uint32_t sem, int32_t adjustment, int32_t *previous) {
    int32_t signal = (int32_t)wl_xb_rd32(sem + DISP_SIGNAL);
    int32_t limit = (int32_t)wl_xb_rd32(sem + KSEMAPHORE_LIMIT);
    if (previous != NULL) {
        *previous = signal;
    }
    if (adjustment <= 0 || signal + adjustment > limit || signal + adjustment < signal) {
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    wl_xb_wr32(sem + DISP_SIGNAL, (uint32_t)(signal + adjustment));
    xk_object_signaled(sem);
    return STATUS_SUCCESS;
}

uint32_t xk_mutant_release(uint32_t mut, bool abandon, int32_t *previous) {
    WlXbThread *self = wl_xb_current();
    int32_t signal = (int32_t)wl_xb_rd32(mut + DISP_SIGNAL);
    if (previous != NULL) {
        *previous = signal;
    }
    if (abandon) {
        uint32_t owner = wl_xb_rd32(mut + KMUTANT_OWNER);
        if (owner != 0) {
            owned_remove(wl_xb_thread_by_kthread(owner), mut);
        }
        wl_xb_wr32(mut + DISP_SIGNAL, 1);
        wl_xb_wr32(mut + KMUTANT_OWNER, 0);
        wl_xb_wr8(mut + KMUTANT_ABANDONED, 1);
        xk_object_signaled(mut);
        return STATUS_SUCCESS;
    }
    if (self == NULL || wl_xb_rd32(mut + KMUTANT_OWNER) != self->kthread) {
        return STATUS_MUTANT_NOT_OWNED;
    }
    signal++;
    wl_xb_wr32(mut + DISP_SIGNAL, (uint32_t)signal);
    if (signal > 0) {
        wl_xb_wr32(mut + KMUTANT_OWNER, 0);
        owned_remove(self, mut);
        xk_object_signaled(mut);
    }
    return STATUS_SUCCESS;
}

void xk_thread_abandon_mutants(WlXbThread *t) {
    XkThread *k = t != NULL ? t->kernel : NULL;
    while (k != NULL && k->owned_count > 0) {
        uint32_t mut = k->owned[k->owned_count - 1u];
        if (wl_xb_rd32(mut + KMUTANT_OWNER) == t->kthread) {
            xk_mutant_release(mut, true, NULL); // takes it off the list
        } else {
            owned_remove(t, mut);
        }
    }
}
