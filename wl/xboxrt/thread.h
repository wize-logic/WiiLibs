// SPDX-License-Identifier: MIT
// Guest threads and the guest lock.
//
// The Xbox has one CPU; its titles rely on that. Each guest thread is a host thread (LWP on the
// Wii, a pthread on the host), and one guest lock decides which of them runs guest code: the
// holder runs the title and every kernel call; the register file (regs.h) is loaded from the
// holder's WlXbThread when it takes the lock and saved back when it gives it up. A thread gives
// the lock up when it waits (wl_xb_wait), when it ends, and at a yield point (a kernel call) when
// another thread is ready and its quantum is over; the next holder is the highest-priority ready
// thread, first come first served within a priority. Interrupt work (the vblank ISR, DPCs, timer
// expiry: irq.h) runs at yield points of whichever thread holds the lock, or on the service
// thread when no one does.
//
// Every function here is called with the guest lock held unless it says otherwise.
#pragma once

#include <setjmp.h>

#include "wl/base/base.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/regs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XB_PRIORITY_IDLE 0
#define WL_XB_PRIORITY_NORMAL 16       // the Xbox's base priority for a title thread (LOW_REALTIME 16)
#define WL_XB_PRIORITY_INTERRUPT 64    // the service thread: above every guest priority
#define WL_XB_MAX_JMP_SLOTS 32

typedef struct WlXbThread WlXbThread;

// What the kernel keeps per thread is its own (waits, APCs); the runtime keeps a pointer for it.
struct WlXbThread {
    uint32_t id;               // the Xbox thread id (1, 2, ...)
    char name[24];
    int priority;              // 0..31 (WL_XB_PRIORITY_INTERRUPT for the service thread)
    WlXbCpu cpu;               // registers while not running
    uint32_t kpcr;             // guest VA of the thread's KPCR (its fs base): NtTib, Prcb, Irql
    uint32_t kthread;          // guest VA of its KTHREAD (a dispatcher object, Type 6)
    uint32_t stack_base, stack_top; // its guest stack: [base, top)
    uint32_t tls_data;         // guest VA of its TLS data (0: none)
    uint32_t start_routine, start_ctx1, start_ctx2;
    uint32_t suspend_count;
    bool terminated;
    bool is_service;           // the runtime's service thread (not a guest thread)
    bool counted;              // made by wl_xb_thread_create: the process ends when the last ends
    uint32_t exit_status;
    void *kernel;              // the kernel's per-thread state
    // runtime internals
    WlPlatformThread host;
    WlPlatformCond cond;       // the thread sleeps here
    void *host_stack;
    bool ready;                // in the ready queue
    bool woken;                // a wake arrived while it slept (wl_xb_wake)
    bool running_guest;        // between its routine's start and its end
    jmp_buf exit_jmp;          // where wl_xb_thread_exit lands
    WlXbThread *next_ready;
    WlXbThread *next_all;
    uint32_t jmp_count;        // guest setjmp buffers this thread armed (recomp_setjmp_slot)
    struct {
        uint32_t buf_va;
        jmp_buf native;
    } jmp[WL_XB_MAX_JMP_SLOTS];
};

// ---- Set-up ----------------------------------------------------------------------------------

typedef struct {
    uint32_t quantum_ms;          // how long a thread keeps the lock past a ready thread: 10
    uint32_t host_stack_bytes;    // each guest thread's host stack (the Wii; recompiled frames
                                  // are C frames): 1 MB
    uint32_t default_stack_bytes; // a guest stack when the title asks for 0: 64 KB
} WlXbThreadConfig;
void wl_xb_thread_config_default(WlXbThreadConfig *cfg);

// Sets up the guest lock and makes the calling host thread a guest thread (the boot thread,
// which then holds the lock). Needs wl_xb_mem_init. `*out` is its WlXbThread (no guest stack or
// KPCR yet: wl_xb_thread_setup_guest gives them).
WlResult wl_xb_threads_init(const WlXbThreadConfig *cfg, WlXbThread **out);
bool wl_xb_threads_ready(void);

// Gives `t` its guest-side pieces: a stack of `stack_bytes` (64 KB if 0) with a guard page below
// it, a KPCR (NtTib with the SEH chain end, StackBase/StackLimit, Self, Prcb, Irql 0, CurrentThread)
// and a KTHREAD (Type 6, not signalled, TlsData), and the TLS block the XBE's TLS directory asks for
// (`tls_template`, `tls_template_bytes` raw data from the image, then `tls_zero_bytes` zeros; the
// slot that XAPI reads at the block's start points at the thread's copy). Loads esp and fs.
WlResult wl_xb_thread_setup_guest(WlXbThread *t, uint32_t stack_bytes, uint32_t tls_template, uint32_t tls_template_bytes,
                                  uint32_t tls_zero_bytes);

// ---- Guest threads -----------------------------------------------------------------------------

// A new guest thread running `routine(ctx1, ctx2)` (stdcall, the Xbox kernel's system routine) on
// a stack of `stack_bytes`. Not started while `suspended` (wl_xb_thread_resume). The TLS block is
// the process's (wl_xb_thread_set_tls_template). `*out` stays valid for the process's life.
WlResult wl_xb_thread_create(uint32_t routine, uint32_t ctx1, uint32_t ctx2, uint32_t stack_bytes, int priority,
                             bool suspended, WlXbThread **out);
// The TLS layout every new thread gets (from the XBE's TLS directory).
void wl_xb_thread_set_tls_template(uint32_t data_va, uint32_t data_bytes, uint32_t zero_bytes);
// Lowers the suspend count; the thread starts (or goes on) at 0. Returns the count before.
uint32_t wl_xb_thread_resume(WlXbThread *t);
uint32_t wl_xb_thread_suspend(WlXbThread *t);
// Ends the calling guest thread (PsTerminateSystemThread): unwinds its host frames back to the
// thread's start and gives the lock up. The boot thread ends the process instead (exit hook).
WL_NORETURN void wl_xb_thread_exit(uint32_t status);
// Called when a guest thread has ended (after its KTHREAD is marked signalled), with the lock
// held: the kernel wakes its waiters.
typedef void (*WlXbThreadExitHook)(WlXbThread *t);
void wl_xb_thread_set_exit_hook(WlXbThreadExitHook hook);
// Called when the boot thread's work is done (its routine returned or it called
// wl_xb_thread_exit): the default waits until the process ends (no guest thread left, or
// wl_xb_process_exit).
void wl_xb_thread_set_priority(WlXbThread *t, int priority);

// The thread holding the guest lock (the caller, for code that runs under it).
WlXbThread *wl_xb_current(void);
WlXbThread *wl_xb_thread_by_kthread(uint32_t kthread_va);
WlXbThread *wl_xb_thread_by_id(uint32_t id);
// Visits every thread (the lock held): crash reports, the kernel's bookkeeping.
void wl_xb_thread_foreach(void (*fn)(WlXbThread *t, void *user), void *user);

// ---- The lock, waits, yields -------------------------------------------------------------------

// Takes the guest lock for `self` (not held) and loads its registers.
void wl_xb_lock(WlXbThread *self);
// Saves the registers into `self` and gives the lock to the next ready thread.
void wl_xb_unlock(WlXbThread *self);
// Gives the lock up and sleeps until wl_xb_wake(self) or `timeout_ms` (WL_WAIT_FOREVER: no
// deadline), then takes it again. true when woken, false on timeout. The caller re-checks what
// it waits for (a wake may be for something else).
bool wl_xb_wait(WlXbThread *self, uint32_t timeout_ms);
// Wakes `t` out of wl_xb_wait (nothing when it is not waiting: the wake is remembered until it
// next waits). May be called from the service thread.
void wl_xb_wake(WlXbThread *t);
// A yield point: if a thread of equal or higher priority is ready and the caller's quantum is
// over (or one of higher priority is ready at all), the caller goes to the back of the queue.
// Also runs pending interrupt work (irq.h).
void wl_xb_yield_point(void);
// Gives the lock up to every ready thread of equal or higher priority (NtYieldExecution).
void wl_xb_yield(void);

// ---- Calling guest code ------------------------------------------------------------------------

// Calls the guest function at `va` with `argc` 32-bit arguments (pushed right to left, a
// return address after them) and returns eax. Works for stdcall and cdecl alike: esp is put back
// to its value before the arguments. The target is found the way an indirect call finds it
// (manual overrides, the generated table, the kernel thunks). false in `*found` (may be NULL)
// when nothing is at `va` (logged; returns 0).
uint32_t wl_xb_call_guest(uint32_t va, uint32_t argc, const uint32_t *args, bool *found);
// The same, keeping every register but eax and edx as they were (for an interrupt or an APC
// that arrives in the middle of a kernel call).
uint32_t wl_xb_call_guest_preserving(uint32_t va, uint32_t argc, const uint32_t *args);

// ---- The process -------------------------------------------------------------------------------

// Ends the process with `code` (HalReturnToFirmware, the last guest thread ending): the boot
// code's wl_xb_boot_run returns it. May be called from any guest thread; does not return.
WL_NORETURN void wl_xb_process_exit(int code);
// Set by the boot code: what wl_xb_process_exit does (the default calls exit()).
typedef void (*WlXbExitFn)(int code);
void wl_xb_set_process_exit(WlXbExitFn fn);
// Waits (lock not held) until a guest thread calls wl_xb_process_exit or every guest thread has
// ended; returns the code (0 when the threads simply ended).
int wl_xb_process_wait(void);

typedef struct {
    uint32_t threads_created, threads_live;
    uint32_t handoffs;      // the lock changed hands
    uint32_t waits, wakes, timeouts;
} WlXbThreadStats;
void wl_xb_thread_stats(WlXbThreadStats *out);

#ifdef __cplusplus
}
#endif
