// SPDX-License-Identifier: MIT
// Ke exports: dispatcher objects initialised and signalled in place, waits, delays, the clocks,
// the Interlocked family (fastcall), bug checks.
#include <stdio.h>

#include "wl/xboxkrnl/kernel_internal.h"
#include "wl/xboxrt/crash.h"

#define PERF_HZ 3375000u // KeQueryPerformanceFrequency: the ACPI timer the console counts with

static bool timeout_arg(uint32_t va, int64_t *out) {
    if (va == 0 || !wl_xb_range_ok(va, 8)) {
        return false;
    }
    *out = (int64_t)wl_xb_rd64(va);
    return true;
}

// VOID KeInitializeEvent(PRKEVENT Event, EVENT_TYPE Type, BOOLEAN State)
void k_KeInitializeEvent(void) {
    uint32_t ev = ARG(0);
    if (wl_xb_range_ok(ev, 16)) {
        xk_header_init(ev, (uint8_t)(ARG(1) & 1u), 4, (int32_t)(ARG(2) & 0xFFu));
    }
}

// LONG KeSetEvent(PRKEVENT Event, KPRIORITY Increment, BOOLEAN Wait)
void k_KeSetEvent(void) {
    uint32_t ev = ARG(0);
    RET(wl_xb_range_ok(ev, 16) ? (uint32_t)xk_event_set(ev) : 0u);
}

void k_KeResetEvent(void) {
    uint32_t ev = ARG(0);
    RET(wl_xb_range_ok(ev, 16) ? (uint32_t)xk_event_reset(ev) : 0u);
}

void k_KePulseEvent(void) {
    uint32_t ev = ARG(0);
    RET(wl_xb_range_ok(ev, 16) ? (uint32_t)xk_event_pulse(ev) : 0u);
}

// VOID KeInitializeSemaphore(PRKSEMAPHORE Semaphore, LONG Count, LONG Limit)
void k_KeInitializeSemaphore(void) {
    uint32_t sem = ARG(0);
    if (wl_xb_range_ok(sem, 20)) {
        xk_header_init(sem, OBJ_SEMAPHORE, 5, (int32_t)ARG(1));
        wl_xb_wr32(sem + KSEMAPHORE_LIMIT, ARG(2));
    }
}

// LONG KeReleaseSemaphore(PRKSEMAPHORE Semaphore, KPRIORITY Increment, LONG Adjustment, BOOLEAN Wait)
void k_KeReleaseSemaphore(void) {
    int32_t prev = 0;
    uint32_t sem = ARG(0);
    if (wl_xb_range_ok(sem, 20) && xk_semaphore_release(sem, (int32_t)ARG(2), &prev) != STATUS_SUCCESS) {
        WL_LOG_W(XK_TAG, "KeReleaseSemaphore 0x%08x: over its limit", (unsigned)sem);
    }
    RET((uint32_t)prev);
}

// VOID KeInitializeMutant(PRKMUTANT Mutant, BOOLEAN InitialOwner)
void k_KeInitializeMutant(void) {
    uint32_t mut = ARG(0);
    if (!wl_xb_range_ok(mut, 0x20u)) {
        return;
    }
    bool owned = (ARG(1) & 0xFFu) != 0;
    WlXbThread *self = wl_xb_current();
    xk_header_init(mut, OBJ_MUTANT, 8, owned ? 0 : 1);
    wl_xb_wr32(mut + 0x10u, mut + 0x10u);
    wl_xb_wr32(mut + 0x14u, mut + 0x10u);
    wl_xb_wr32(mut + KMUTANT_OWNER, owned && self != NULL ? self->kthread : 0u);
    wl_xb_wr8(mut + KMUTANT_ABANDONED, 0);
}

// LONG KeReleaseMutant(PRKMUTANT Mutant, KPRIORITY Increment, BOOLEAN Abandoned, BOOLEAN Wait)
void k_KeReleaseMutant(void) {
    int32_t prev = 0;
    xk_mutant_release(ARG(0), (ARG(2) & 0xFFu) != 0, &prev);
    RET((uint32_t)prev);
}

// NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON, KPROCESSOR_MODE WaitMode,
//                                BOOLEAN Alertable, PLARGE_INTEGER Timeout)
void k_KeWaitForSingleObject(void) {
    uint32_t obj = ARG(0);
    int64_t t;
    bool has = timeout_arg(ARG(4), &t);
    RET(xk_wait(1, &obj, false, (ARG(3) & 0xFFu) != 0, (ARG(2) & 0xFFu) != 0, has ? &t : NULL));
}

// NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID Object[], WAIT_TYPE WaitType, KWAIT_REASON,
//                                   KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
//                                   PLARGE_INTEGER Timeout, PKWAIT_BLOCK WaitBlockArray)
void k_KeWaitForMultipleObjects(void) {
    uint32_t count = ARG(0), array = ARG(1);
    uint32_t objs[XK_MAX_WAIT];
    if (count == 0 || count > XK_MAX_WAIT || !wl_xb_range_ok(array, count * 4u)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        objs[i] = wl_xb_rd32(array + 4u * i);
    }
    int64_t t;
    bool has = timeout_arg(ARG(6), &t);
    RET(xk_wait(count, objs, ARG(2) == 0, (ARG(5) & 0xFFu) != 0, (ARG(4) & 0xFFu) != 0, has ? &t : NULL));
}

// NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval)
void k_KeDelayExecutionThread(void) {
    bool user = (ARG(0) & 0xFFu) != 0, alertable = (ARG(1) & 0xFFu) != 0;
    int64_t t = 0;
    timeout_arg(ARG(2), &t);
    WlXbThread *self = wl_xb_current();
    XkThread *k = self != NULL ? xk_thread_state(self) : NULL;
    if (t == 0 || k == NULL) {
        wl_xb_yield();
        RET(STATUS_SUCCESS);
        return;
    }
    uint64_t deadline = 0;
    xk_timeout_ms(&t, &deadline);
    for (;;) {
        if (alertable && user && k->apc_count > 0) {
            xk_apc_deliver(self);
            RET(STATUS_USER_APC);
            return;
        }
        uint64_t now = wl_xb_time_us();
        if (now >= deadline) {
            break;
        }
        k->wait_count = 0;
        k->waiting = alertable; // an APC may wake it
        wl_xb_wait(self, (uint32_t)((deadline - now + 999u) / 1000u));
        k->waiting = false;
    }
    RET(STATUS_SUCCESS);
}

// LARGE_INTEGER KeQueryPerformanceCounter(VOID) / KeQueryPerformanceFrequency(VOID)
void k_KeQueryPerformanceCounter(void) {
    uint64_t us = wl_xb_time_us();
    uint64_t ticks = (us / 1000000u) * PERF_HZ + (us % 1000000u) * PERF_HZ / 1000000u;
    g_eax = (uint32_t)ticks;
    g_edx = (uint32_t)(ticks >> 32);
}

void k_KeQueryPerformanceFrequency(void) {
    g_eax = PERF_HZ;
    g_edx = 0;
}

// ULONGLONG KeQueryInterruptTime(VOID): 100 ns since boot
void k_KeQueryInterruptTime(void) {
    uint64_t t = wl_xb_time_us() * 10u;
    g_eax = (uint32_t)t;
    g_edx = (uint32_t)(t >> 32);
}

// VOID KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
void k_KeQuerySystemTime(void) {
    uint32_t out = ARG(0);
    if (wl_xb_range_ok(out, 8)) {
        wl_xb_wr64(out, xk_system_time());
    }
}

// VOID KeStallExecutionProcessor(ULONG MicroSeconds): a busy wait, as on the console (short ones)
void k_KeStallExecutionProcessor(void) {
    uint32_t us = ARG(0);
    if (wl_xb_deterministic()) {
        wl_xb_time_spend_us(us); // the virtual clock moves on by it, at once
        return;
    }
    if (us >= 2000u) {
        wl_platform_thread_sleep_ms(us / 1000u); // long stalls give the host its CPU
        return;
    }
    uint64_t end = wl_xb_time_us() + us;
    while (wl_xb_time_us() < end) {
    }
}

void k_KeBugCheck(void) {
    char why[64];
    snprintf(why, sizeof why, "KeBugCheck(0x%08X) from 0x%08X", (unsigned)ARG(0), (unsigned)wl_xb_kernel_caller());
    WlXbCrash c = {why, NULL, 0, 0, 0, false};
    wl_xb_crash(&c);
}

void k_KeBugCheckEx(void) {
    char why[128];
    snprintf(why, sizeof why, "KeBugCheckEx(0x%08X, %08X %08X %08X %08X) from 0x%08X", (unsigned)ARG(0), (unsigned)ARG(1),
             (unsigned)ARG(2), (unsigned)ARG(3), (unsigned)ARG(4), (unsigned)wl_xb_kernel_caller());
    WlXbCrash c = {why, NULL, 0, 0, 0, false};
    wl_xb_crash(&c);
}

// The FPU state is per guest thread already (the register file): nothing to save
void k_KeSaveFloatingPointState(void) {
    RET(STATUS_SUCCESS);
}

void k_KeRestoreFloatingPointState(void) {
    RET(STATUS_SUCCESS);
}

void k_KeGetCurrentThread(void) {
    WlXbThread *t = wl_xb_current();
    RET(t != NULL ? t->kthread : 0u);
}

void k_KeEnterCriticalRegion(void) {
}

void k_KeLeaveCriticalRegion(void) {
}

// ---- Interlocked (fastcall: ecx, edx; InterlockedCompareExchange's comparand on the stack) ------
// One CPU and the guest lock: a plain read-modify-write is atomic for the guest.

void k_InterlockedCompareExchange(void) {
    uint32_t dst = g_ecx, exchange = g_edx, comparand = ARG(0);
    uint32_t old = wl_xb_rd32(dst);
    if (old == comparand) {
        wl_xb_wr32(dst, exchange);
    }
    RET(old);
}

void k_InterlockedDecrement(void) {
    uint32_t v = wl_xb_rd32(g_ecx) - 1u;
    wl_xb_wr32(g_ecx, v);
    RET(v);
}

void k_InterlockedIncrement(void) {
    uint32_t v = wl_xb_rd32(g_ecx) + 1u;
    wl_xb_wr32(g_ecx, v);
    RET(v);
}

void k_InterlockedExchange(void) {
    uint32_t old = wl_xb_rd32(g_ecx);
    wl_xb_wr32(g_ecx, g_edx);
    RET(old);
}

void k_InterlockedExchangeAdd(void) {
    uint32_t old = wl_xb_rd32(g_ecx);
    wl_xb_wr32(g_ecx, old + g_edx);
    RET(old);
}

// SLIST_HEADER: Next (4), Depth (2), Sequence (2)
void k_InterlockedFlushSList(void) {
    uint32_t head = g_ecx;
    uint32_t first = wl_xb_rd32(head);
    wl_xb_wr32(head, 0);
    wl_xb_wr16(head + 4u, 0);
    RET(first);
}

void k_InterlockedPopEntrySList(void) {
    uint32_t head = g_ecx;
    uint32_t first = wl_xb_rd32(head);
    if (first != 0) {
        wl_xb_wr32(head, wl_xb_rd32(first));
        wl_xb_wr16(head + 4u, (uint16_t)(wl_xb_rd16(head + 4u) - 1u));
        wl_xb_wr16(head + 6u, (uint16_t)(wl_xb_rd16(head + 6u) + 1u));
    }
    RET(first);
}

void k_InterlockedPushEntrySList(void) {
    uint32_t head = g_ecx, entry = g_edx;
    uint32_t first = wl_xb_rd32(head);
    wl_xb_wr32(entry, first);
    wl_xb_wr32(head, entry);
    wl_xb_wr16(head + 4u, (uint16_t)(wl_xb_rd16(head + 4u) + 1u));
    wl_xb_wr16(head + 6u, (uint16_t)(wl_xb_rd16(head + 6u) + 1u));
    RET(first);
}

void xk_link_ke_sync(void) {
}
