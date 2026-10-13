// SPDX-License-Identifier: MIT
// The dispatcher: waits on objects that live in guest memory, their signals, user APCs.
//
// The objects' state is their DISPATCHER_HEADER's SignalState in guest memory, read afresh at every
// check (titles initialise and reset some of them inline), and the guest lock is the dispatcher
// lock: the waits re-check under it, so a wake is only a hint and a spurious one costs a check.
// A waiting thread records its objects; a signal wakes every thread that waits on the object and
// they race for it under the lock, as NT's satisfy-on-wake gives the same outcome to the first.
#include "wl/xboxkrnl/kernel_internal.h"

#define FILETIME_1970 116444736000000000ull // 100 ns intervals from 1601 to 1970

uint64_t xk_system_time(void) {
    return wl_platform_wall_time_us() * 10u + FILETIME_1970 + (uint64_t)((int64_t)g_xk_cfg.timezone_bias * 600000000);
}

uint32_t xk_timeout_ms(const int64_t *timeout, uint64_t *deadline_us) {
    uint64_t now = wl_platform_time_us();
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

static bool satisfied(uint32_t obj, const WlXbThread *self) {
    int32_t signal = (int32_t)wl_xb_rd32(obj + DISP_SIGNAL);
    if (is_mutant(obj)) {
        return signal > 0 || wl_xb_rd32(obj + KMUTANT_OWNER) == self->kthread;
    }
    return signal > 0;
}

// The side effects of a satisfied wait. True when the object was an abandoned mutant.
static bool acquire(uint32_t obj, const WlXbThread *self) {
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
        wl_xb_wr32(obj + KMUTANT_OWNER, self->kthread);
        wl_xb_wr8(obj + KMUTANT_ABANDONED, 0);
        return abandoned;
    }
    default:
        return false; // notification events and timers, threads: stay signalled
    }
}

bool xk_apc_deliver(WlXbThread *t) {
    XkThread *k = xk_thread_state(t);
    bool any = false;
    while (k != NULL && k->apc_count > 0) {
        XkApc apc = k->apcs[k->apc_head];
        k->apc_head = (k->apc_head + 1u) % XK_MAX_APCS;
        k->apc_count--;
        uint32_t args[3] = {apc.context, apc.arg1, apc.arg2};
        wl_xb_call_guest_preserving(apc.routine, 3, args);
        g_xk_stats.apcs_delivered++;
        any = true;
    }
    return any;
}

void xk_apc_queue(WlXbThread *t, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2) {
    XkThread *k = xk_thread_state(t);
    if (k == NULL || routine == 0) {
        return;
    }
    if (k->apc_count == XK_MAX_APCS) {
        WL_LOG_E(XK_TAG, "thread %u: APC queue full, APC to 0x%08x dropped", (unsigned)t->id, (unsigned)routine);
        return;
    }
    k->apcs[(k->apc_head + k->apc_count) % XK_MAX_APCS] = (XkApc){routine, context, arg1, arg2};
    k->apc_count++;
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
        if (alertable && user_mode && k->apc_count > 0) {
            xk_apc_deliver(self);
            return STATUS_USER_APC;
        }
        if (wait_all) {
            bool all = count > 0;
            for (uint32_t i = 0; i < count && all; i++) {
                all = satisfied(objects[i], self);
            }
            if (all) {
                bool abandoned = false;
                for (uint32_t i = 0; i < count; i++) {
                    abandoned |= acquire(objects[i], self);
                }
                return abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0;
            }
        } else {
            for (uint32_t i = 0; i < count; i++) {
                if (satisfied(objects[i], self)) {
                    return acquire(objects[i], self) ? STATUS_ABANDONED_WAIT_0 + i : STATUS_WAIT_0 + i;
                }
            }
        }
        uint32_t left = ms;
        if (ms != WL_WAIT_FOREVER) {
            uint64_t now = wl_platform_time_us();
            if (now >= deadline) {
                g_xk_stats.wait_timeouts++;
                return STATUS_TIMEOUT;
            }
            left = (uint32_t)((deadline - now + 999u) / 1000u);
        }
        k->wait_count = count;
        memcpy(k->wait_objects, objects, count * sizeof objects[0]);
        k->waiting = true;
        wl_xb_wait(self, left);
        k->waiting = false;
        k->wait_count = 0;
    }
}

typedef struct {
    uint32_t obj;
} SignalCtx;

static void wake_if_waiting(WlXbThread *t, void *user) {
    const SignalCtx *c = user;
    XkThread *k = t->kernel;
    if (k == NULL || !k->waiting) {
        return;
    }
    for (uint32_t i = 0; i < k->wait_count; i++) {
        if (k->wait_objects[i] == c->obj) {
            wl_xb_wake(t);
            return;
        }
    }
}

void xk_object_signaled(uint32_t obj) {
    SignalCtx c = {obj};
    wl_xb_thread_foreach(wake_if_waiting, &c);
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
// reset. Waiters run only once the caller gives up the lock, so the pulse yields to them before
// it resets the event.
int32_t xk_event_pulse(uint32_t ev) {
    int32_t prev = xk_event_set(ev);
    wl_xb_yield();
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
        xk_object_signaled(mut);
    }
    return STATUS_SUCCESS;
}
