// SPDX-License-Identifier: MIT
// Threads: PsCreateSystemThreadEx makes a guest thread (wl/xboxrt) with its own stack, KPCR,
// KTHREAD and TLS block, and a handle to its thread object; the start routine is the kernel's
// system routine (XAPI passes its own trampoline, with the user routine and its parameter as the
// two contexts). With inline_first_thread the first call runs its routine on the calling thread
// instead (xboxrecomp's model for titles whose entry point never returns).
#include "wl/xboxkrnl/kernel_internal.h"

#define BASE_PRIORITY 16 // a title thread's base: LOW_REALTIME_PRIORITY on the console
#define MIN_STACK 0x4000u

static uint32_t g_created;

// NTSTATUS PsCreateSystemThreadEx(PHANDLE ThreadHandle, SIZE_T ThreadExtensionSize,
//     SIZE_T KernelStackSize, SIZE_T TlsDataSize, PHANDLE ThreadId, PVOID StartContext1,
//     PVOID StartContext2, BOOLEAN CreateSuspended, BOOLEAN DebuggerThread,
//     PKSYSTEM_ROUTINE SystemRoutine)
void k_PsCreateSystemThreadEx(void) {
    uint32_t handle_out = ARG(0), stack = ARG(2), id_out = ARG(4);
    uint32_t ctx1 = ARG(5), ctx2 = ARG(6), routine = ARG(9);
    bool suspended = (ARG(7) & 0xFFu) != 0;
    g_created++;
    if (g_xk_cfg.inline_first_thread && g_created == 1) {
        WlXbThread *self = wl_xb_current();
        XkObject *o = xk_thread_object(self);
        if (handle_out != 0 && o != NULL) {
            wl_xb_wr32(handle_out, xk_handle_open(o));
        }
        if (id_out != 0) {
            wl_xb_wr32(id_out, self->id);
        }
        WL_LOG_I(XK_TAG, "first thread 0x%08x runs on the calling thread (ctx 0x%08x 0x%08x)", (unsigned)routine,
                 (unsigned)ctx1, (unsigned)ctx2);
        uint32_t args[2] = {ctx1, ctx2};
        wl_xb_call_guest(routine, 2, args, NULL);
        RET(STATUS_SUCCESS);
        return;
    }
    WlXbThread *t = NULL;
    WlResult r = wl_xb_thread_create(routine, ctx1, ctx2, stack < MIN_STACK ? MIN_STACK : stack, BASE_PRIORITY,
                                     suspended, &t);
    if (r != WL_OK) {
        WL_LOG_E(XK_TAG, "PsCreateSystemThreadEx(0x%08x): %s", (unsigned)routine, wl_result_name(r));
        RET(STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    XkObject *o = xk_thread_object(t);
    uint32_t h = o != NULL ? xk_handle_open(o) : 0;
    if (handle_out != 0) {
        wl_xb_wr32(handle_out, h);
    }
    if (id_out != 0) {
        wl_xb_wr32(id_out, t->id);
    }
    WL_LOG_I(XK_TAG, "thread %u: routine 0x%08x ctx 0x%08x 0x%08x stack 0x%x%s", (unsigned)t->id, (unsigned)routine,
             (unsigned)ctx1, (unsigned)ctx2, (unsigned)stack, suspended ? " (suspended)" : "");
    RET(STATUS_SUCCESS);
}

// NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, PHANDLE ThreadId, PKSTART_ROUTINE StartRoutine,
//                               PVOID StartContext, BOOLEAN DebuggerThread): a thread of the
// kernel's: its start routine takes one argument
void k_PsCreateSystemThread(void) {
    WlXbThread *t = NULL;
    if (wl_xb_thread_create(ARG(2), ARG(3), 0, 0x10000u, BASE_PRIORITY, false, &t) != WL_OK) {
        RET(STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    XkObject *o = xk_thread_object(t);
    if (ARG(0) != 0) {
        wl_xb_wr32(ARG(0), o != NULL ? xk_handle_open(o) : 0u);
    }
    if (ARG(1) != 0) {
        wl_xb_wr32(ARG(1), t->id);
    }
    RET(STATUS_SUCCESS);
}

// VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
void k_PsTerminateSystemThread(void) {
    uint32_t status = ARG(0);
    WlXbThread *t = wl_xb_current();
    WL_LOG_I(XK_TAG, "thread %u ends (status 0x%08x)", t != NULL ? (unsigned)t->id : 0u, (unsigned)status);
    wl_xb_thread_exit(status);
}

static WlXbThread *thread_of(uint32_t kthread) {
    WlXbThread *t = wl_xb_thread_by_kthread(kthread);
    return t != NULL ? t : wl_xb_current();
}

// LONG KeSetBasePriorityThread(PKTHREAD Thread, LONG Increment): the increment over the base
// (XAPI's SetThreadPriority: -2..2, or +-15 to saturate); returns the previous increment
void k_KeSetBasePriorityThread(void) {
    WlXbThread *t = thread_of(ARG(0));
    int32_t inc = (int32_t)ARG(1);
    int32_t prev = t->priority - BASE_PRIORITY;
    wl_xb_thread_set_priority(t, BASE_PRIORITY + inc);
    RET((uint32_t)prev);
}

void k_KeQueryBasePriorityThread(void) {
    WlXbThread *t = thread_of(ARG(0));
    RET((uint32_t)(t->priority - BASE_PRIORITY));
}

// KPRIORITY KeSetPriorityThread(PKTHREAD Thread, KPRIORITY Priority)
void k_KeSetPriorityThread(void) {
    WlXbThread *t = thread_of(ARG(0));
    int prev = t->priority;
    wl_xb_thread_set_priority(t, (int)ARG(1));
    RET((uint32_t)prev);
}

// NTSTATUS NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
void k_NtResumeThread(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_THREAD);
    if (o == NULL || o->thread == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t prev = wl_xb_thread_resume(o->thread);
    if (ARG(1) != 0) {
        wl_xb_wr32(ARG(1), prev);
    }
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
void k_NtSuspendThread(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_THREAD);
    if (o == NULL || o->thread == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t prev = wl_xb_thread_suspend(o->thread);
    if (ARG(1) != 0) {
        wl_xb_wr32(ARG(1), prev);
    }
    RET(STATUS_SUCCESS);
}

void k_KeResumeThread(void) {
    WlXbThread *t = wl_xb_thread_by_kthread(ARG(0));
    RET(t != NULL ? wl_xb_thread_resume(t) : 0u);
}

void k_KeSuspendThread(void) {
    WlXbThread *t = wl_xb_thread_by_kthread(ARG(0));
    RET(t != NULL ? wl_xb_thread_suspend(t) : 0u);
}

// NTSTATUS NtYieldExecution(VOID): the processor to the best ready thread, whatever its priority
// (SwitchToThread spins let a lower-priority loader run); STATUS_NO_YIELD_PERFORMED when none ran.
void k_NtYieldExecution(void) {
    if (g_xb_compat & WL_XB_COMPAT_OLDYIELD) {
        wl_xb_yield();
        RET(STATUS_SUCCESS);
        return;
    }
    RET(wl_xb_yield_any() ? STATUS_SUCCESS : 0x40000024u);
}

void xk_link_ps(void) {
}
