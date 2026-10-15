// SPDX-License-Identifier: MIT
// Nt exports for synchronisation objects (events, semaphores, mutants, timers: the bodies in guest
// memory, named ones found again by name), the waits on handles, NtClose and handle duplication,
// and the APC entry points (NtUserIoApcDispatcher, NtQueueApcThread).
#include <stdio.h>
#include <stdlib.h>

#include "wl/xboxkrnl/nt_file.h"

#define MAX_NAMED 64u

static struct {
    char name[96];
    XkObject *obj;
} g_named[MAX_NAMED];

static XkObject *named_find(const char *name, XkObjType type) {
    for (uint32_t i = 0; i < MAX_NAMED; i++) {
        if (g_named[i].obj != NULL && g_named[i].obj->type == type && strcasecmp(g_named[i].name, name) == 0) {
            return g_named[i].obj;
        }
    }
    return NULL;
}

static void named_add(const char *name, XkObject *o) {
    for (uint32_t i = 0; i < MAX_NAMED; i++) {
        if (g_named[i].obj == NULL) {
            snprintf(g_named[i].name, sizeof g_named[i].name, "%s", name);
            g_named[i].obj = o;
            o->refs++; // the name keeps it
            return;
        }
    }
}

// Makes (or, by name, finds) an object; writes its handle. Returns the status.
static uint32_t create_object(uint32_t handle_out, uint32_t oa, XkObjType type, uint32_t body_bytes, XkObject **out) {
    char name[96] = "";
    uint32_t status = 0;
    if (oa != 0 && wl_xb_range_ok(oa, 12) && wl_xb_rd32(oa + 4u) != 0) {
        xk_object_name(oa, name, sizeof name, &status);
    }
    XkObject *o = name[0] != '\0' ? named_find(name, type) : NULL;
    uint32_t result = STATUS_SUCCESS;
    if (o != NULL) {
        result = STATUS_OBJECT_NAME_EXISTS;
        *out = NULL;
    } else {
        o = xk_object_new(type, body_bytes);
        if (o == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (name[0] != '\0') {
            named_add(name, o);
        }
        *out = o;
    }
    uint32_t h = xk_handle_open(o);
    if (h == 0) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (handle_out != 0 && wl_xb_range_ok(handle_out, 4)) {
        wl_xb_wr32(handle_out, h);
    }
    return result;
}

// NTSTATUS NtCreateEvent(PHANDLE EventHandle, POBJECT_ATTRIBUTES, EVENT_TYPE EventType, BOOLEAN InitialState)
void k_NtCreateEvent(void) {
    XkObject *o = NULL;
    uint32_t s = create_object(ARG(0), ARG(1), XK_OBJ_EVENT, 16, &o);
    if (o != NULL) {
        xk_header_init(o->body, (uint8_t)(ARG(2) & 1u), 4, (int32_t)(ARG(3) & 0xFFu));
    }
    RET(s);
}

static uint32_t event_body(uint32_t handle) {
    XkObject *o = xk_handle_get_type(handle, XK_OBJ_EVENT);
    return o != NULL ? o->body : 0u;
}

// NTSTATUS NtSetEvent(HANDLE EventHandle, PLONG PreviousState)
void k_NtSetEvent(void) {
    uint32_t ev = event_body(ARG(0));
    if (ev == 0) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    int32_t prev = xk_event_set(ev);
    if (ARG(1) != 0 && wl_xb_range_ok(ARG(1), 4)) {
        wl_xb_wr32(ARG(1), (uint32_t)prev);
    }
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtClearEvent(HANDLE EventHandle)
void k_NtClearEvent(void) {
    uint32_t ev = event_body(ARG(0));
    if (ev == 0) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    xk_event_reset(ev);
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtPulseEvent(HANDLE EventHandle, PLONG PreviousState)
void k_NtPulseEvent(void) {
    uint32_t ev = event_body(ARG(0));
    if (ev == 0) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    int32_t prev = xk_event_pulse(ev);
    if (ARG(1) != 0 && wl_xb_range_ok(ARG(1), 4)) {
        wl_xb_wr32(ARG(1), (uint32_t)prev);
    }
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtQueryEvent(HANDLE, PEVENT_BASIC_INFORMATION): EventType, EventState
void k_NtQueryEvent(void) {
    uint32_t ev = event_body(ARG(0)), out = ARG(1);
    if (ev == 0 || !wl_xb_range_ok(out, 8)) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    wl_xb_wr32(out, wl_xb_rd8(ev + DISP_TYPE));
    wl_xb_wr32(out + 4u, wl_xb_rd32(ev + DISP_SIGNAL));
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtCreateSemaphore(PHANDLE, POBJECT_ATTRIBUTES, LONG InitialCount, LONG MaximumCount)
void k_NtCreateSemaphore(void) {
    XkObject *o = NULL;
    uint32_t s = create_object(ARG(0), ARG(1), XK_OBJ_SEMAPHORE, 20, &o);
    if (o != NULL) {
        xk_header_init(o->body, OBJ_SEMAPHORE, 5, (int32_t)ARG(2));
        wl_xb_wr32(o->body + KSEMAPHORE_LIMIT, ARG(3));
    }
    RET(s);
}

// NTSTATUS NtReleaseSemaphore(HANDLE, LONG ReleaseCount, PLONG PreviousCount)
void k_NtReleaseSemaphore(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_SEMAPHORE);
    if (o == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    int32_t prev = 0;
    uint32_t s = xk_semaphore_release(o->body, (int32_t)ARG(1), &prev);
    if (ARG(2) != 0 && wl_xb_range_ok(ARG(2), 4)) {
        wl_xb_wr32(ARG(2), (uint32_t)prev);
    }
    RET(s);
}

// NTSTATUS NtCreateMutant(PHANDLE, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner)
void k_NtCreateMutant(void) {
    XkObject *o = NULL;
    uint32_t s = create_object(ARG(0), ARG(1), XK_OBJ_MUTANT, 0x20, &o);
    if (o != NULL) {
        bool owned = (ARG(2) & 0xFFu) != 0;
        WlXbThread *self = wl_xb_current();
        xk_header_init(o->body, OBJ_MUTANT, 8, owned ? 0 : 1);
        wl_xb_wr32(o->body + 0x10u, o->body + 0x10u);
        wl_xb_wr32(o->body + 0x14u, o->body + 0x10u);
        wl_xb_wr32(o->body + KMUTANT_OWNER, owned && self != NULL ? self->kthread : 0u);
        if (owned) {
            xk_mutant_owned_by(self, o->body);
        }
    }
    RET(s);
}

// NTSTATUS NtReleaseMutant(HANDLE, PLONG PreviousCount)
void k_NtReleaseMutant(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_MUTANT);
    if (o == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    int32_t prev = 0;
    uint32_t s = xk_mutant_release(o->body, false, &prev);
    if (ARG(1) != 0 && wl_xb_range_ok(ARG(1), 4)) {
        wl_xb_wr32(ARG(1), (uint32_t)prev);
    }
    RET(s);
}

// NTSTATUS NtCreateTimer(PHANDLE, POBJECT_ATTRIBUTES, TIMER_TYPE TimerType)
void k_NtCreateTimer(void) {
    XkObject *o = NULL;
    uint32_t s = create_object(ARG(0), ARG(1), XK_OBJ_TIMER, KTIMER_BYTES, &o);
    if (o != NULL) {
        xk_header_init(o->body, (uint8_t)(OBJ_TIMER_NOTIFICATION + (ARG(2) & 1u)), KTIMER_BYTES / 4u, 0);
    }
    RET(s);
}

// NTSTATUS NtSetTimerEx(HANDLE, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE, KPROCESSOR_MODE,
//                       PVOID TimerContext, BOOLEAN ResumeTimer, LONG Period, PBOOLEAN PreviousState)
void k_NtSetTimerEx(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_TIMER);
    if (o == NULL || !wl_xb_range_ok(ARG(1), 8)) {
        RET(o == NULL ? STATUS_INVALID_HANDLE : STATUS_ACCESS_VIOLATION);
        return;
    }
    if (ARG(2) != 0) {
        xk_stub("NtSetTimerEx with an APC routine (the APC is not queued)");
    }
    int32_t prev = (int32_t)wl_xb_rd32(o->body + DISP_SIGNAL);
    xk_timer_set(o->body, (int64_t)wl_xb_rd64(ARG(1)), (int32_t)ARG(6), 0);
    if (ARG(7) != 0 && wl_xb_range_ok(ARG(7), 1)) {
        wl_xb_wr8(ARG(7), prev != 0 ? 1u : 0u);
    }
    RET(STATUS_SUCCESS);
}

// NTSTATUS NtCancelTimer(HANDLE, PBOOLEAN CurrentState)
void k_NtCancelTimer(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_TIMER);
    if (o == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    xk_timer_cancel(o->body);
    if (ARG(1) != 0 && wl_xb_range_ok(ARG(1), 1)) {
        wl_xb_wr8(ARG(1), wl_xb_rd32(o->body + DISP_SIGNAL) != 0 ? 1u : 0u);
    }
    RET(STATUS_SUCCESS);
}

// ---- Waits on handles --------------------------------------------------------------------------------

static bool timeout_of(uint32_t va, int64_t *t) {
    if (va == 0 || !wl_xb_range_ok(va, 8)) {
        return false;
    }
    *t = (int64_t)wl_xb_rd64(va);
    return true;
}

static uint32_t wait_handles(uint32_t count, const uint32_t *handles, bool all, bool alertable, bool user,
                             uint32_t timeout_va) {
    // (zeroed: the callers pass 1..XK_MAX_WAIT handles and the loop sets all of them, which GCC's
    // -O2 cannot see through xk_wait's const pointer)
    uint32_t objs[XK_MAX_WAIT] = {0};
    XkObject *held[XK_MAX_WAIT];
    for (uint32_t i = 0; i < count; i++) {
        held[i] = xk_handle_get(handles[i]);
        objs[i] = held[i] != NULL ? held[i]->body : 0u;
        if (objs[i] == 0) {
            return STATUS_INVALID_HANDLE;
        }
    }
    // the wait's references: a handle closed by another thread meanwhile leaves the bodies alive
    for (uint32_t i = 0; i < count; i++) {
        xk_object_reference(held[i]);
    }
    int64_t t;
    bool has = timeout_of(timeout_va, &t);
    uint32_t status = xk_wait(count, objs, all, alertable, user, has ? &t : NULL);
    for (uint32_t i = 0; i < count; i++) {
        xk_object_release(held[i]);
    }
    return status;
}

// NTSTATUS NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
void k_NtWaitForSingleObject(void) {
    uint32_t h = ARG(0);
    RET(wait_handles(1, &h, false, (ARG(1) & 0xFFu) != 0, true, ARG(2)));
}

// NTSTATUS NtWaitForSingleObjectEx(HANDLE, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER)
void k_NtWaitForSingleObjectEx(void) {
    uint32_t h = ARG(0);
    RET(wait_handles(1, &h, false, (ARG(2) & 0xFFu) != 0, (ARG(1) & 0xFFu) != 0, ARG(3)));
}

// NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, PHANDLE Handles, WAIT_TYPE WaitType,
//                                     KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER)
void k_NtWaitForMultipleObjectsEx(void) {
    uint32_t count = ARG(0), array = ARG(1);
    uint32_t handles[XK_MAX_WAIT];
    if (count == 0 || count > XK_MAX_WAIT || !wl_xb_range_ok(array, 4u * count)) {
        RET(STATUS_INVALID_PARAMETER);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        handles[i] = wl_xb_rd32(array + 4u * i);
    }
    RET(wait_handles(count, handles, ARG(2) == 0, (ARG(4) & 0xFFu) != 0, (ARG(3) & 0xFFu) != 0, ARG(5)));
}

// NTSTATUS NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle, HANDLE WaitHandle, KPROCESSOR_MODE,
//                                           BOOLEAN Alertable, PLARGE_INTEGER Timeout)
void k_NtSignalAndWaitForSingleObjectEx(void) {
    XkObject *s = xk_handle_get(ARG(0));
    if (s == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    if (s->type == XK_OBJ_EVENT) {
        xk_event_set(s->body);
    } else if (s->type == XK_OBJ_SEMAPHORE) {
        xk_semaphore_release(s->body, 1, NULL);
    } else if (s->type == XK_OBJ_MUTANT) {
        xk_mutant_release(s->body, false, NULL);
    }
    uint32_t h = ARG(1);
    RET(wait_handles(1, &h, false, (ARG(3) & 0xFFu) != 0, (ARG(2) & 0xFFu) != 0, ARG(4)));
}

// NTSTATUS NtClose(HANDLE Handle)
void k_NtClose(void) {
    RET(xk_handle_close(ARG(0)));
}

// NTSTATUS NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, DWORD Options)
void k_NtDuplicateObject(void) {
    XkObject *o = xk_handle_get(ARG(0));
    if (o == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t h = xk_handle_open(o);
    if (ARG(1) != 0 && wl_xb_range_ok(ARG(1), 4)) {
        wl_xb_wr32(ARG(1), h);
    }
    if (ARG(2) & 1u) { // DUPLICATE_CLOSE_SOURCE
        xk_handle_close(ARG(0));
    }
    RET(h != 0 ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES);
}

// ---- APCs ------------------------------------------------------------------------------------------

// VOID NtUserIoApcDispatcher(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved): the
// I/O APC XAPI's ReadFileEx/WriteFileEx pass; ApcContext is the completion routine, called as
// CompletionRoutine(dwErrorCode, dwNumberOfBytesTransferred, lpOverlapped) (the status block is the
// OVERLAPPED's first two fields)
void k_NtUserIoApcDispatcher(void) {
    uint32_t routine = ARG(0), iosb = ARG(1);
    uint32_t status = wl_xb_range_ok(iosb, 8) ? wl_xb_rd32(iosb) : 0u;
    uint32_t info = wl_xb_range_ok(iosb, 8) ? wl_xb_rd32(iosb + 4u) : 0u;
    uint32_t error = status != 0 ? xk_dos_error(status) : 0u;
    uint32_t args[3] = {error, info, iosb};
    wl_xb_call_guest(routine, 3, args, NULL);
}

// NTSTATUS NtQueueApcThread(HANDLE ThreadHandle, PPS_APC_ROUTINE ApcRoutine, PVOID ApcContext,
//                           PIO_STATUS_BLOCK, ULONG Reserved)
void k_NtQueueApcThread(void) {
    XkObject *o = xk_handle_get_type(ARG(0), XK_OBJ_THREAD);
    if (o == NULL || o->thread == NULL) {
        RET(STATUS_INVALID_HANDLE);
        return;
    }
    xk_apc_queue(o->thread, ARG(1), ARG(2), ARG(3), ARG(4));
    RET(STATUS_SUCCESS);
}

void xk_link_nt_sync(void) {
}
