// SPDX-License-Identifier: MIT
// The IRQL, DPCs, timers and interrupts, and their exports. The work runs from the runtime's
// service (irq.h) with the guest lock held and the interrupted thread's registers saved: expired
// timers are signalled (and queue their DPC), asserted interrupt lines call the connected
// KINTERRUPT's service routine at its IRQL, then the DPC queue drains at DISPATCH_LEVEL. A thread
// that raised its IRQL keeps the work out until it lowers it again.
//
// Layouts (KDPC, KTIMER, KINTERRUPT) are the Xbox kernel's as public kernel headers describe them
// (kernel_internal.h has the offsets).
#include "wl/xboxkrnl/kernel_internal.h"

#define MAX_TIMERS 64u
#define DPC_RING 256u
#define VECTOR_BASE 0x30u
#define SYNCH_LEVEL 28u
#define PROFILE_LEVEL 26u

typedef struct {
    uint32_t va;
    uint64_t due_us;
    uint32_t period_ms;
    bool active;
} Timer;

static Timer g_timers[MAX_TIMERS];
static uint32_t g_dpc_ring[DPC_RING];
static uint32_t g_dpc_head, g_dpc_count;
static uint32_t g_interrupts[WL_XB_IRQ_LINES]; // connected KINTERRUPT per line
static uint8_t g_irql_boot;                    // before any thread has a KPCR

uint8_t xk_irql_get(void) {
    WlXbThread *t = wl_xb_current();
    return t != NULL && t->kpcr != 0 ? wl_xb_rd8(t->kpcr + KPCR_IRQL) : g_irql_boot;
}

uint8_t xk_irql_set(uint8_t irql) {
    WlXbThread *t = wl_xb_current();
    uint8_t prev;
    if (t != NULL && t->kpcr != 0) {
        prev = wl_xb_rd8(t->kpcr + KPCR_IRQL);
        wl_xb_wr8(t->kpcr + KPCR_IRQL, irql);
    } else {
        prev = g_irql_boot;
        g_irql_boot = irql;
    }
    if (prev >= DISPATCH_LEVEL && irql < DISPATCH_LEVEL && g_dpc_count > 0) {
        wl_xb_service_raise(WL_XB_SVC_DPC);
        wl_xb_service_poll(); // the DPCs held back run now, as lowering the IRQL runs them on NT
    }
    return prev;
}

bool xk_dpc_queue(uint32_t dpc, uint32_t arg1, uint32_t arg2) {
    if (!wl_xb_range_ok(dpc, 0x1Cu) || wl_xb_rd8(dpc + KDPC_INSERTED) != 0) {
        return false;
    }
    if (g_dpc_count == DPC_RING) {
        WL_LOG_E(XK_TAG, "DPC queue full: DPC 0x%08x dropped", (unsigned)dpc);
        return false;
    }
    wl_xb_wr8(dpc + KDPC_INSERTED, 1);
    wl_xb_wr32(dpc + KDPC_ARG1, arg1);
    wl_xb_wr32(dpc + KDPC_ARG2, arg2);
    g_dpc_ring[(g_dpc_head + g_dpc_count) % DPC_RING] = dpc;
    g_dpc_count++;
    wl_xb_service_raise(WL_XB_SVC_DPC);
    return true;
}

bool xk_dpc_dequeue(uint32_t dpc) {
    for (uint32_t i = 0; i < g_dpc_count; i++) {
        uint32_t at = (g_dpc_head + i) % DPC_RING;
        if (g_dpc_ring[at] == dpc) {
            for (uint32_t k = i; k + 1u < g_dpc_count; k++) {
                g_dpc_ring[(g_dpc_head + k) % DPC_RING] = g_dpc_ring[(g_dpc_head + k + 1u) % DPC_RING];
            }
            g_dpc_count--;
            wl_xb_wr8(dpc + KDPC_INSERTED, 0);
            return true;
        }
    }
    return false;
}

static void run_dpcs(void) {
    uint32_t budget = g_dpc_count + 16u; // DPCs queued by these run too, within reason
    while (g_dpc_count > 0 && budget-- > 0) {
        uint32_t dpc = g_dpc_ring[g_dpc_head];
        g_dpc_head = (g_dpc_head + 1u) % DPC_RING;
        g_dpc_count--;
        wl_xb_wr8(dpc + KDPC_INSERTED, 0);
        uint32_t routine = wl_xb_rd32(dpc + KDPC_ROUTINE);
        uint32_t args[4] = {dpc, wl_xb_rd32(dpc + KDPC_CONTEXT), wl_xb_rd32(dpc + KDPC_ARG1), wl_xb_rd32(dpc + KDPC_ARG2)};
        if (routine != 0) {
            wl_xb_call_guest(routine, 4, args, NULL);
            g_xk_stats.dpcs_run++;
        }
    }
}

// ---- Timers --------------------------------------------------------------------------------------

static void update_deadline(void) {
    uint64_t next = 0;
    for (uint32_t i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].active && (next == 0 || g_timers[i].due_us < next)) {
            next = g_timers[i].due_us;
        }
    }
    wl_xb_service_set_deadline(next);
}

static Timer *timer_find(uint32_t va) {
    for (uint32_t i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].va == va) {
            return &g_timers[i];
        }
    }
    return NULL;
}

bool xk_timer_set(uint32_t timer, int64_t due, int32_t period_ms, uint32_t dpc) {
    Timer *t = timer_find(timer);
    bool was = t != NULL;
    if (t == NULL) {
        for (uint32_t i = 0; i < MAX_TIMERS && t == NULL; i++) {
            if (!g_timers[i].active) {
                t = &g_timers[i];
            }
        }
        if (t == NULL) {
            WL_LOG_E(XK_TAG, "too many timers: KTIMER 0x%08x not set", (unsigned)timer);
            return false;
        }
    }
    uint64_t deadline = 0;
    xk_timeout_ms(&due, &deadline);
    t->va = timer;
    t->due_us = deadline;
    t->period_ms = period_ms > 0 ? (uint32_t)period_ms : 0;
    t->active = true;
    wl_xb_wr32(timer + DISP_SIGNAL, 0);
    wl_xb_wr64(timer + KTIMER_DUE, (uint64_t)due);
    wl_xb_wr32(timer + KTIMER_DPC, dpc);
    wl_xb_wr32(timer + KTIMER_PERIOD, (uint32_t)period_ms);
    update_deadline();
    return was;
}

bool xk_timer_cancel(uint32_t timer) {
    Timer *t = timer_find(timer);
    if (t == NULL) {
        return false;
    }
    t->active = false;
    update_deadline();
    return true;
}

static void fire_timers(void) {
    uint64_t now = wl_platform_time_us();
    for (uint32_t i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timers[i];
        if (!t->active || t->due_us > now) {
            continue;
        }
        g_xk_stats.timers_fired++;
        if (t->period_ms > 0) {
            t->due_us += (uint64_t)t->period_ms * 1000u;
            if (t->due_us <= now) {
                t->due_us = now + (uint64_t)t->period_ms * 1000u;
            }
        } else {
            t->active = false;
        }
        wl_xb_wr32(t->va + DISP_SIGNAL, 1);
        xk_object_signaled(t->va);
        uint32_t dpc = wl_xb_rd32(t->va + KTIMER_DPC);
        if (dpc != 0) {
            uint64_t sys = xk_system_time();
            xk_dpc_queue(dpc, (uint32_t)sys, (uint32_t)(sys >> 32));
        }
    }
    update_deadline();
}

// ---- Interrupts and the service handler ----------------------------------------------------------

static void run_isrs(uint8_t irql) {
    uint32_t lines = wl_xb_irq_lines();
    for (uint32_t line = 0; line < WL_XB_IRQ_LINES && lines != 0; line++) {
        if ((lines & (1u << line)) == 0) {
            continue;
        }
        uint32_t ki = g_interrupts[line];
        if (ki == 0) {
            wl_xb_irq_deassert(line); // nobody listens: drop it
            continue;
        }
        uint8_t level = (uint8_t)wl_xb_rd32(ki + KINT_IRQL);
        if (irql >= level) {
            continue; // masked by the running thread's IRQL
        }
        if (wl_xb_rd8(ki + KINT_MODE) != 0) {
            wl_xb_irq_deassert(line); // latched: consumed by this delivery
        }
        uint8_t prev = xk_irql_set(level);
        uint32_t args[2] = {ki, wl_xb_rd32(ki + KINT_CONTEXT)};
        wl_xb_call_guest(wl_xb_rd32(ki + KINT_ROUTINE), 2, args, NULL);
        wl_xb_wr32(ki + 0x14u, wl_xb_rd32(ki + 0x14u) + 1u); // ServiceCount
        g_xk_stats.isrs_run++;
        WlXbThread *t = wl_xb_current();
        if (t != NULL && t->kpcr != 0) {
            wl_xb_wr8(t->kpcr + KPCR_IRQL, prev); // back without running DPCs from here
        } else {
            g_irql_boot = prev;
        }
    }
}

static void service(void *user, uint32_t reasons) {
    (void)user;
    (void)reasons;
    uint8_t irql = xk_irql_get();
    fire_timers();
    run_isrs(irql);
    if (irql < DISPATCH_LEVEL && g_dpc_count > 0) {
        uint8_t prev = xk_irql_get();
        WlXbThread *t = wl_xb_current();
        if (t != NULL && t->kpcr != 0) {
            wl_xb_wr8(t->kpcr + KPCR_IRQL, DISPATCH_LEVEL);
        }
        run_dpcs();
        if (t != NULL && t->kpcr != 0) {
            wl_xb_wr8(t->kpcr + KPCR_IRQL, prev);
        }
    }
}

void xk_irq_init(void) {
    wl_xb_service_set_handler(service, NULL);
}

// ---- Exports ---------------------------------------------------------------------------------------

// KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql)
void k_KfRaiseIrql(void) {
    RET(xk_irql_set((uint8_t)g_ecx));
}

// VOID FASTCALL KfLowerIrql(KIRQL NewIrql)
void k_KfLowerIrql(void) {
    xk_irql_set((uint8_t)g_ecx);
}

void k_KeRaiseIrqlToDpcLevel(void) {
    RET(xk_irql_set(DISPATCH_LEVEL));
}

void k_KeRaiseIrqlToSynchLevel(void) {
    RET(xk_irql_set(SYNCH_LEVEL));
}

void k_KeGetCurrentIrql(void) {
    RET(xk_irql_get());
}

// VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext)
void k_KeInitializeDpc(void) {
    uint32_t dpc = ARG(0);
    if (wl_xb_range_ok(dpc, 0x1Cu)) {
        wl_xb_fill(dpc, 0, 0x1Cu);
        wl_xb_wr16(dpc + KDPC_TYPE, 0x13); // DpcObject
        wl_xb_wr32(dpc + KDPC_ROUTINE, ARG(1));
        wl_xb_wr32(dpc + KDPC_CONTEXT, ARG(2));
    }
}

// BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2)
void k_KeInsertQueueDpc(void) {
    RET(xk_dpc_queue(ARG(0), ARG(1), ARG(2)) ? 1u : 0u);
}

// BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
void k_KeRemoveQueueDpc(void) {
    RET(xk_dpc_dequeue(ARG(0)) ? 1u : 0u);
}

void k_KeIsExecutingDpc(void) {
    RET(wl_xb_in_service() ? 1u : 0u);
}

// VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
void k_KeInitializeTimerEx(void) {
    uint32_t timer = ARG(0);
    if (wl_xb_range_ok(timer, KTIMER_BYTES)) {
        wl_xb_fill(timer, 0, KTIMER_BYTES);
        xk_header_init(timer, (uint8_t)(OBJ_TIMER_NOTIFICATION + (ARG(1) & 1u)), KTIMER_BYTES / 4u, 0);
        wl_xb_wr32(timer + 0x18u, timer + 0x18u); // TimerListEntry: empty
        wl_xb_wr32(timer + 0x1Cu, timer + 0x18u);
    }
}

// BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
void k_KeSetTimer(void) {
    int64_t due = (int64_t)((uint64_t)ARG(1) | ((uint64_t)ARG(2) << 32));
    RET(xk_timer_set(ARG(0), due, 0, ARG(3)) ? 1u : 0u);
}

// BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc)
void k_KeSetTimerEx(void) {
    int64_t due = (int64_t)((uint64_t)ARG(1) | ((uint64_t)ARG(2) << 32));
    RET(xk_timer_set(ARG(0), due, (int32_t)ARG(3), ARG(4)) ? 1u : 0u);
}

// BOOLEAN KeCancelTimer(PKTIMER Timer)
void k_KeCancelTimer(void) {
    RET(xk_timer_cancel(ARG(0)) ? 1u : 0u);
}

// ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql)
void k_HalGetInterruptVector(void) {
    uint32_t level = ARG(0), irql_out = ARG(1);
    uint32_t irql = level < PROFILE_LEVEL - DISPATCH_LEVEL ? PROFILE_LEVEL - level : DISPATCH_LEVEL + 1u;
    if (irql_out != 0) {
        wl_xb_wr8(irql_out, (uint8_t)irql);
    }
    RET(VECTOR_BASE + level);
}

// VOID KeInitializeInterrupt(PKINTERRUPT, PKSERVICE_ROUTINE, PVOID Context, ULONG Vector, KIRQL Irql,
//                            KINTERRUPT_MODE Mode, BOOLEAN ShareVector)
void k_KeInitializeInterrupt(void) {
    uint32_t ki = ARG(0);
    if (!wl_xb_range_ok(ki, 0x18u)) {
        return;
    }
    wl_xb_fill(ki, 0, 0x18u);
    wl_xb_wr32(ki + KINT_ROUTINE, ARG(1));
    wl_xb_wr32(ki + KINT_CONTEXT, ARG(2));
    wl_xb_wr32(ki + KINT_LEVEL, ARG(3) - VECTOR_BASE);
    wl_xb_wr32(ki + KINT_IRQL, ARG(4) & 0xFFu);
    wl_xb_wr8(ki + KINT_MODE, (uint8_t)ARG(5));
    wl_xb_wr8(ki + KINT_SHARE, (uint8_t)ARG(6));
}

// BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt)
void k_KeConnectInterrupt(void) {
    uint32_t ki = ARG(0);
    uint32_t line = wl_xb_range_ok(ki, 0x18u) ? wl_xb_rd32(ki + KINT_LEVEL) : WL_XB_IRQ_LINES;
    if (line >= WL_XB_IRQ_LINES || g_interrupts[line] != 0) {
        RET(0);
        return;
    }
    g_interrupts[line] = ki;
    wl_xb_wr8(ki + KINT_CONNECTED, 1);
    WL_LOG_I(XK_TAG, "interrupt line %u connected: ISR 0x%08x context 0x%08x IRQL %u", (unsigned)line,
             (unsigned)wl_xb_rd32(ki + KINT_ROUTINE), (unsigned)wl_xb_rd32(ki + KINT_CONTEXT),
             (unsigned)wl_xb_rd32(ki + KINT_IRQL));
    RET(1);
}

// BOOLEAN KeDisconnectInterrupt(PKINTERRUPT Interrupt)
void k_KeDisconnectInterrupt(void) {
    uint32_t ki = ARG(0);
    for (uint32_t line = 0; line < WL_XB_IRQ_LINES; line++) {
        if (g_interrupts[line] == ki) {
            g_interrupts[line] = 0;
            wl_xb_wr8(ki + KINT_CONNECTED, 0);
            RET(1);
            return;
        }
    }
    RET(0);
}

// BOOLEAN KeSynchronizeExecution(PKINTERRUPT, PKSYNCHRONIZE_ROUTINE, PVOID Context)
void k_KeSynchronizeExecution(void) {
    uint32_t ki = ARG(0), routine = ARG(1), ctx = ARG(2);
    uint8_t level = wl_xb_range_ok(ki, 0x18u) ? (uint8_t)wl_xb_rd32(ki + KINT_IRQL) : (uint8_t)SYNCH_LEVEL;
    uint8_t prev = xk_irql_set(level);
    uint32_t r = wl_xb_call_guest(routine, 1, &ctx, NULL);
    xk_irql_set(prev);
    RET(r & 0xFFu);
}

void xk_link_ke(void) {
}
