// SPDX-License-Identifier: MIT
// What the kernel's files share (internal to wl/xboxkrnl): status codes, the object manager and
// handle table, the dispatcher (waits, signals, APCs), the pool, DPCs/timers/interrupts, the file
// layer, the data exports' page. Kernel functions are `void k_<Name>(void)`, reading their
// arguments with ARG(n) and returning through RET (thunk.h); exports.inc names them by ordinal.
#pragma once

#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxkrnl/xboxkrnl.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/regs.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/thunk.h"

// Every export's function (defined by the group files for the implemented ones).
#define WL_XK_EXPORT(ord, name, bytes, kind) void k_##name(void);
#include "wl/xboxkrnl/exports.inc"
#undef WL_XK_EXPORT

#define XK_TAG "xbkrnl"
#define ARG(n) WL_XB_ARG(n)
#define RET(v) WL_XB_RET(v)

// ---- NTSTATUS ------------------------------------------------------------------------------------
#define STATUS_SUCCESS 0x00000000u
#define STATUS_WAIT_0 0x00000000u
#define STATUS_ABANDONED_WAIT_0 0x00000080u
#define STATUS_USER_APC 0x000000C0u
#define STATUS_ALERTED 0x00000101u
#define STATUS_TIMEOUT 0x00000102u
#define STATUS_PENDING 0x00000103u
#define STATUS_OBJECT_NAME_EXISTS 0x40000000u
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define STATUS_NO_MORE_FILES 0x80000006u
#define STATUS_UNSUCCESSFUL 0xC0000001u
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_ACCESS_VIOLATION 0xC0000005u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_NO_SUCH_FILE 0xC000000Fu
#define STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_NO_MEMORY 0xC0000017u
#define STATUS_CONFLICTING_ADDRESSES 0xC0000018u
#define STATUS_ACCESS_DENIED 0xC0000022u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
#define STATUS_OBJECT_NAME_INVALID 0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_MUTANT_NOT_OWNED 0xC0000046u
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047u
#define STATUS_DISK_FULL 0xC000007Fu
#define STATUS_INSUFFICIENT_RESOURCES 0xC000009Au
#define STATUS_FILE_IS_A_DIRECTORY 0xC00000BAu
#define STATUS_NOT_SUPPORTED 0xC00000BBu
#define STATUS_DIRECTORY_NOT_EMPTY 0xC0000101u
#define STATUS_NOT_A_DIRECTORY 0xC0000103u
#define STATUS_CANNOT_DELETE 0xC0000121u
#define STATUS_THREAD_IS_TERMINATING 0xC000004Bu

#define NT_SUCCESS(s) ((int32_t)(s) >= 0)
#define NT_ERROR(s) (((uint32_t)(s) >> 30) == 3u) // severity 3: an error (not a warning)

// ---- Dispatcher objects (DISPATCHER_HEADER at the object's start) ------------------------------
#define DISP_TYPE 0x00u   // UCHAR
#define DISP_SIZE 0x02u   // UCHAR, in dwords
#define DISP_SIGNAL 0x04u // LONG SignalState
#define DISP_WAITLIST 0x08u

enum {
    OBJ_EVENT_NOTIFICATION = 0,
    OBJ_EVENT_SYNCHRONIZATION = 1,
    OBJ_MUTANT = 2,
    OBJ_PROCESS = 3,
    OBJ_QUEUE = 4,
    OBJ_SEMAPHORE = 5,
    OBJ_THREAD = 6,
    OBJ_TIMER_NOTIFICATION = 8,
    OBJ_TIMER_SYNCHRONIZATION = 9,
};

#define KSEMAPHORE_LIMIT 0x10u
#define KMUTANT_OWNER 0x18u
#define KMUTANT_ABANDONED 0x1Cu
#define KTIMER_DUE 0x10u
#define KTIMER_DPC 0x20u
#define KTIMER_PERIOD 0x24u
#define KTIMER_BYTES 0x28u
#define KDPC_TYPE 0x00u
#define KDPC_INSERTED 0x02u
#define KDPC_ROUTINE 0x0Cu
#define KDPC_CONTEXT 0x10u
#define KDPC_ARG1 0x14u
#define KDPC_ARG2 0x18u
#define KINT_ROUTINE 0x00u
#define KINT_CONTEXT 0x04u
#define KINT_LEVEL 0x08u
#define KINT_IRQL 0x0Cu
#define KINT_CONNECTED 0x10u
#define KINT_SHARE 0x11u
#define KINT_MODE 0x12u

#define DISPATCH_LEVEL 2u
#define PASSIVE_LEVEL 0u
#define KPCR_IRQL 0x24u

// Writes a fresh dispatcher header.
void xk_header_init(uint32_t obj, uint8_t type, uint8_t size_dwords, int32_t signal);

// ---- Pool (guest memory for kernel objects, ExAllocatePool) ------------------------------------
uint32_t xk_pool_alloc(uint32_t bytes);       // zeroed, 16-byte aligned; 0 when exhausted
void xk_pool_free(uint32_t va);
uint32_t xk_pool_size(uint32_t va);           // 0 for an address the pool did not hand out

// ---- Objects and handles -------------------------------------------------------------------------
typedef enum {
    XK_OBJ_NONE = 0,
    XK_OBJ_EVENT,
    XK_OBJ_SEMAPHORE,
    XK_OBJ_MUTANT,
    XK_OBJ_TIMER,
    XK_OBJ_THREAD,
    XK_OBJ_FILE,
    XK_OBJ_SYMLINK,
    XK_OBJ_DEVICE,
} XkObjType;

typedef struct XkFile XkFile;

typedef struct {
    XkObjType type;
    uint32_t body;      // guest VA of the object body (its dispatcher header), 0 for none
    uint32_t refs;      // handles, references by pointer and waits on it (ob.c)
    bool body_owned;    // the pool holds the body (freed with the last reference)
    WlXbThread *thread; // XK_OBJ_THREAD
    XkFile *file;       // XK_OBJ_FILE
    char *link_target;  // XK_OBJ_SYMLINK
} XkObject;

XkObject *xk_object_new(XkObjType type, uint32_t body_bytes); // body zeroed in the pool (0: none)
uint32_t xk_handle_open(XkObject *obj);                       // a new handle (refs + 1), 0 when full
XkObject *xk_handle_get(uint32_t handle);                     // NULL for a bad handle
XkObject *xk_handle_get_type(uint32_t handle, XkObjType type);
uint32_t xk_handle_close(uint32_t handle);                    // NTSTATUS
// References besides handles (a wait's, a pointer's): the last release frees the object.
void xk_object_reference(XkObject *obj);
void xk_object_release(XkObject *obj);
XkObject *xk_object_of_body(uint32_t body);                   // NULL for a body no object owns
#define XK_CURRENT_THREAD 0xFFFFFFFEu
#define XK_CURRENT_PROCESS 0xFFFFFFFFu
// The object body a handle waits on (the dispatcher header's VA), 0 when it is not waitable.
uint32_t xk_handle_waitable(uint32_t handle);
// The thread object of a runtime thread (made on first use).
XkObject *xk_thread_object(WlXbThread *t);

// ---- Dispatcher --------------------------------------------------------------------------------
// One kernel-side record per guest thread (WlXbThread.kernel).
#define XK_MAX_WAIT 64u
// NT queues user APCs without a bound; a thread that issues many reads with completion routines
// before its next alertable wait (XAPI's ReadFileEx completes through NtUserIoApcDispatcher) can
// have hundreds pending. A ring of 2048 (32 KB a thread) keeps every one.
#define XK_MAX_APCS 2048u
typedef struct {
    uint32_t routine, context, arg1, arg2; // a user APC: routine(context, arg1, arg2)
} XkApc;
// Mutants a thread owns at once (abandoned when it ends; more are owned but not tracked, logged).
#define XK_MAX_OWNED 64u
typedef struct {
    uint32_t wait_count;
    uint32_t wait_objects[XK_MAX_WAIT];
    bool waiting;
    bool wait_all;
    // A wait is satisfied when its objects are signalled, by the signaller (NT's satisfy at signal
    // time): the objects acquired for this thread and the status its wait returns, so a pulse, or
    // a set and reset before the thread runs again, still releases it.
    bool satisfied;
    uint32_t wait_status;
    uint32_t wait_seq; // the order the waits began: the first waiter is satisfied first
    XkApc apcs[XK_MAX_APCS]; // a ring: apc_count entries from apc_head
    uint32_t apc_head, apc_count;
    XkObject *object; // its thread object
    uint32_t owned[XK_MAX_OWNED]; // the mutants it owns (abandoned when it ends)
    uint32_t owned_count;
} XkThread;
XkThread *xk_thread_state(WlXbThread *t);

// Waits for `count` objects (guest VAs of dispatcher headers): any of them (`wait_all` false,
// returns STATUS_WAIT_0 + index) or all. `timeout` in 100 ns units: NULL forever, negative
// relative, positive absolute system time (KeQuerySystemTime's clock), 0 a poll. Alertable user
// waits deliver queued APCs and return STATUS_USER_APC. Returns the NTSTATUS.
uint32_t xk_wait(uint32_t count, const uint32_t *objects, bool wait_all, bool alertable, bool user_mode,
                 const int64_t *timeout);
// The object's state changed: the threads waiting on it whose waits it now satisfies are
// satisfied (in the order their waits began, while the object stays signalled) and woken.
void xk_object_signaled(uint32_t obj);
// A thread ends: the mutants it still owns are abandoned (their next waiter gets
// STATUS_ABANDONED_WAIT_0).
void xk_thread_abandon_mutants(WlXbThread *t);
// Mutant `mut` is owned by `t` now (made owned, or acquired): abandoned if `t` ends owning it.
void xk_mutant_owned_by(WlXbThread *t, uint32_t mut);
// Events, semaphores, mutants: the operations on the guest objects (NTSTATUS / previous state).
int32_t xk_event_set(uint32_t ev);
int32_t xk_event_reset(uint32_t ev);
int32_t xk_event_pulse(uint32_t ev);
uint32_t xk_semaphore_release(uint32_t sem, int32_t adjustment, int32_t *previous);
uint32_t xk_mutant_release(uint32_t mut, bool abandon, int32_t *previous);
// Queues a user APC for `t` (delivered at its next alertable user-mode wait).
void xk_apc_queue(WlXbThread *t, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2);
bool xk_apc_deliver(WlXbThread *t);
// The current system time (100 ns since 1601) and the conversions of a timeout.
uint64_t xk_system_time(void);
uint32_t xk_timeout_ms(const int64_t *timeout, uint64_t *deadline_us); // WL_WAIT_FOREVER for NULL

// ---- DPCs, timers, interrupts, IRQL (ke_irq.c) --------------------------------------------------
void xk_irq_init(void);
uint8_t xk_irql_get(void);
uint8_t xk_irql_set(uint8_t irql); // returns the previous one; lowering below DISPATCH runs DPCs
bool xk_dpc_queue(uint32_t dpc, uint32_t arg1, uint32_t arg2);
bool xk_dpc_dequeue(uint32_t dpc);
bool xk_timer_set(uint32_t timer, int64_t due, int32_t period_ms, uint32_t dpc);
bool xk_timer_cancel(uint32_t timer);

// ---- Files (nt_file.c, path.c) -------------------------------------------------------------------
void xk_files_init(const WlXkConfig *cfg);
bool xk_link_create(const char *link, const char *target); // "\??\D:" -> "\Device\CdRom0"
const char *xk_link_lookup(const char *link);
void xk_file_close(XkFile *f);
// The object name of an OBJECT_ATTRIBUTES (an ANSI OBJECT_STRING), joined to its RootDirectory's
// path. false (and a status) when it cannot be read.
bool xk_object_name(uint32_t object_attributes, char *out, uint32_t cap, uint32_t *status);
// Writes an IO_STATUS_BLOCK.
static inline void xk_iosb(uint32_t iosb, uint32_t status, uint32_t info) {
    if (iosb != 0 && wl_xb_range_ok(iosb, 8)) {
        wl_xb_wr32(iosb, status);
        wl_xb_wr32(iosb + 4u, info);
    }
}

// ---- Data exports' page (kdata.c) --------------------------------------------------------------
void xk_kdata_init(const WlXkConfig *cfg);
uint32_t xk_kdata_va(uint32_t ordinal); // the VA a data export's thunk gets, 0 for a function
uint32_t xk_kdata_tick_va(void);
uint32_t xk_object_type_va(XkObjType type); // the ExEventObjectType-style cookies

// ---- Odds and ends -------------------------------------------------------------------------------
extern WlXkConfig g_xk_cfg;
extern WlXkStats g_xk_stats;
// RtlNtStatusToDosError's table.
uint32_t xk_dos_error(uint32_t status);
// Logs a deliberate stub's first call (by name).
void xk_stub(const char *name);
// A guest OBJECT_STRING / ANSI_STRING (Length, MaximumLength, Buffer) into a C string.
bool xk_read_ansi(uint32_t str, char *out, uint32_t cap);
// Writes a C string into a guest ANSI_STRING's buffer (Length set; STATUS_BUFFER_TOO_SMALL when
// MaximumLength is short).
uint32_t xk_write_ansi(uint32_t str, const char *text);
// Each group file's link anchor (the init calls them, so the linker keeps the files and their
// k_ functions, which exports.inc references weakly).
void xk_link_ke(void);
void xk_link_ke_sync(void);
void xk_link_nt_sync(void);
void xk_link_ex(void);
void xk_link_mm(void);
void xk_link_nt(void);
void xk_link_ps(void);
void xk_link_rtl(void);
void xk_link_hal(void);
void xk_link_io(void);
void xk_link_xc(void);
