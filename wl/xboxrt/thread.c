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

#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/thunk.h"

#define TAG "xbthread"

#define KPCR_IRQL 0x24u
#define KTHREAD_OFFSET 0x400u // in the thread's kernel page, after the KPCR
#define KTHREAD_BYTES 0x200u

static WlPlatformMutex g_m;
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
volatile uint32_t g_xb_attention;

void wl_xb_thread_config_default(WlXbThreadConfig *cfg) {
    cfg->quantum_ms = 10;
    cfg->starve_ms = 30;
    cfg->host_stack_bytes = 1u << 20;
    cfg->default_stack_bytes = 64u << 10;
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

static void enqueue(WlXbThread *t) {
    WlXbThread *volatile *pp = &g_ready;
    while (*pp != NULL && (*pp)->priority >= t->priority) {
        pp = &(*pp)->next_ready;
    }
    t->next_ready = *pp;
    *pp = t;
    t->ready = true;
    t->ready_since_us = wl_platform_time_us();
    g_xb_attention = 1; // the holder gives the lock up at its next safe point when this one outranks it
}

static WlXbThread *dequeue(void) {
    WlXbThread *t = g_ready;
    if (t != NULL) {
        g_ready = t->next_ready;
        t->next_ready = NULL;
        t->ready = false;
    }
    return t;
}

// Gives the baton away (g_m held): to the head of the queue, or to no one.
static void hand_off(void) {
    WlXbThread *next = dequeue();
    g_owner = next;
    if (next != NULL) {
        g_stats.handoffs++;
        wl_platform_cond_signal(next->cond);
    }
}

// Waits for the baton (g_m held). The service thread says so when it has waited a second or more:
// the holder spins without reaching a safe point, and interrupts are held off meanwhile.
static void take(WlXbThread *self) {
    if (g_owner == NULL && g_ready == NULL) {
        g_owner = self;
    } else {
        enqueue(self);
        uint64_t since = wl_platform_time_ms(), warned = 0;
        while (g_owner != self) {
            if (!self->is_service) {
                wl_platform_cond_wait(self->cond, g_m);
                continue;
            }
            wl_platform_cond_wait_timeout(self->cond, g_m, 500);
            uint64_t waited = wl_platform_time_ms() - since;
            WlXbThread *holder = g_owner;
            if (holder != self && holder != NULL && waited >= 1000u && (warned == 0 || waited - warned >= 5000u)) {
                warned = waited;
                g_stats.service_starved++;
                WL_LOG_W(TAG,
                         "interrupts held off for %u ms: thread %u (%s) keeps the guest lock without a safe point "
                         "(a kernel call, rdtsc or a device access)",
                         (unsigned)waited, (unsigned)holder->id, holder->name);
                WlXbSymbol sym;
                (void)wl_xb_symbolize(0, &sym); // builds the table here, not in the probed thread
                wl_xb_backend_probe(holder->host_handle);
            }
        }
    }
    g_quantum_start = wl_platform_time_us();
}

void wl_xb_lock(WlXbThread *self) {
    self->host_tid = wl_platform_thread_self_id();
    self->host_handle = wl_xb_backend_thread_handle();
    wl_platform_mutex_lock(g_m);
    take(self);
    wl_platform_mutex_unlock(g_m);
    g_cur = self;
    wl_xb_cpu_load(&self->cpu);
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

bool wl_xb_wait(WlXbThread *self, uint32_t timeout_ms) {
    wl_xb_cpu_save(&self->cpu);
    g_cur = NULL;
    wl_platform_mutex_lock(g_m);
    g_stats.waits++;
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
    take(self);
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
    if (!t->ready && g_owner != t) {
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
    g_quantum_start = wl_platform_time_us();
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
    uint64_t now = wl_platform_time_us();
    bool boosted = self == g_boosted && now - g_quantum_start < (uint64_t)g_cfg.quantum_ms * 1000u;
    if (boosted && !head->is_service) {
        return false; // a starved thread keeps the lock for one quantum, as NT's boost gives it
    }
    g_boosted = NULL;
    if (head->priority > self->priority ||
        (head->priority == self->priority && now - g_quantum_start >= (uint64_t)g_cfg.quantum_ms * 1000u)) {
        requeue(self);
        return true;
    }
    // A thread kept waiting past starve_ms goes first, for one quantum (the holder may be
    // busy-waiting for something only a lower-priority thread or the vertical blank brings, as a
    // title's frame throttle does, while a loader thread below it never runs). The waiter's time
    // alone decides: the holder's quantum restarts after every interrupt, which the service
    // thread takes the lock for, so a holder interrupted each vertical blank never finishes one.
    if (g_cfg.starve_ms != 0) {
        wl_platform_mutex_lock(g_m);
        WlXbThread *volatile *pp = &g_ready;
        while (*pp != NULL && now - (*pp)->ready_since_us < (uint64_t)g_cfg.starve_ms * 1000u) {
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
        g_boosted = starved;
        wl_xb_cpu_save(&self->cpu);
        g_cur = NULL;
        enqueue(self);
        g_owner = starved;
        g_stats.handoffs++;
        wl_platform_cond_signal(starved->cond);
        while (g_owner != self) {
            wl_platform_cond_wait(self->cond, g_m);
        }
        g_quantum_start = wl_platform_time_us();
        wl_platform_mutex_unlock(g_m);
        g_cur = self;
        wl_xb_cpu_load(&self->cpu);
        return true;
    }
    return false;
}

void wl_xb_yield_point(void) {
    wl_xb_service_poll();
    yield_if_due(g_cur);
}

void wl_xb_safe_point(void) {
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
    if (g_ready != NULL) {
        g_xb_attention = 1; // a thread of equal priority still waits for the quantum's end
    }
}

void wl_xb_yield(void) {
    WlXbThread *self = g_cur;
    WlXbThread *head = g_ready;
    if (self != NULL && head != NULL && head->priority >= self->priority && !wl_xb_in_service()) {
        requeue(self);
    }
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
    g_ready_init = true;
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
    r = wl_xb_va_commit(bottom, stack_bytes + extra, WL_XB_PROT_READWRITE);
    if (r != WL_OK) {
        wl_xb_va_release(va, NULL);
        return r;
    }
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
static void *thread_main(void *arg) {
    WlXbThread *t = arg;
    wl_platform_mutex_lock(g_m);
    while (t->suspend_count > 0) {
        wl_platform_cond_wait(t->cond, g_m);
    }
    t->woken = false;
    wl_platform_mutex_unlock(g_m);
    wl_xb_lock(t);
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
    // a C frame, of about twice the guest's bytes), within [192 KB, host_stack_bytes]: Halo's
    // workers ask 16 KB and its main thread 512 KB, and seven 1 MB stacks were a quarter of what
    // the Wii's host side has
    size_t host = (size_t)(stack_bytes != 0 ? stack_bytes : g_cfg.default_stack_bytes) * 2u;
    if (host < (192u << 10)) {
        host = 192u << 10;
    }
    if (host > g_cfg.host_stack_bytes) {
        host = g_cfg.host_stack_bytes;
    }
    t->host_stack = aligned_alloc(64, host);
    if (t->host_stack == NULL) {
        return WL_ERR_NOMEM;
    }
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
    wl_xb_lock(t);
    if (setjmp(t->exit_jmp) == 0) {
        t->running_guest = true;
        start.fn(t, start.user);
    }
    t->running_guest = false;
    t->terminated = true;
    wl_xb_unlock(t);
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
    size_t host = 256u << 10;
    t->host_stack = aligned_alloc(64, host);
    if (t->host_stack == NULL) {
        free(start);
        return WL_ERR_NOMEM;
    }
    start->t = t;
    start->fn = fn;
    start->user = user;
    r = wl_platform_thread_create(&t->host, host_thread_main, start, t->host_stack, host, WL_THREAD_PRIO_MAIN);
    if (r != WL_OK) {
        free(start);
        return r;
    }
    wl_platform_thread_detach(t->host);
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
    return before;
}

uint32_t wl_xb_thread_suspend(WlXbThread *t) {
    // only a thread that has not started yet stops: a running guest thread cannot be frozen
    // between two instructions here (none of the titles this serves suspends one that runs)
    wl_platform_mutex_lock(g_m);
    uint32_t before = t->suspend_count;
    if (!t->running_guest && !t->terminated) {
        t->suspend_count++;
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
    t->priority = priority;
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
