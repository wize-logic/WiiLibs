// SPDX-License-Identifier: MIT
// Guest threads and the guest lock.
//
// The Xbox has one CPU; its titles rely on that. Each guest thread is a host thread (LWP on the
// Wii, a pthread on the host), and one guest lock decides which of them runs guest code: the
// holder runs the title and every kernel call; the register file (regs.h) is loaded from the
// holder's WlXbThread when it takes the lock and saved back when it gives it up. A thread gives
// the lock up when it waits (wl_xb_wait), when it ends, and at a yield point (a kernel call, an
// rdtsc, a device access: wl_xb_safe_point) when another thread is ready and its quantum is over; the next holder is the highest-priority ready
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
    size_t host_stack_size;    // its bytes (painted when made, so its depth can be measured)
    uint32_t host_done_ms;     // when its host thread ran its last statement (ms, odd; 0: not yet)
    bool ready;                // in the ready queue
    uint64_t ready_since_us;   // when it was queued
    // where its time off the lock went (wl_xb_time_us, g_m held): wl_xb_thread_times
    uint64_t ready_us;         // in the ready queue (its turn not come: another thread had the lock)
    uint64_t waited_us;        // in wl_xb_wait (a guest wait: an event, a sleep, a semaphore)
    uint64_t io_us;            // outside the lock in a kernel call's card I/O (wl_xb_block_begin)
    uint64_t off_since_us;     // when the wait or the I/O began (0: neither)
    bool off_io;               // ...which of the two
    bool woken;                // a wake arrived while it slept (wl_xb_wake)
    bool det_created;          // its host thread exists (wl_xb_thread_create): a resume queues it
    bool det_waiting;          // the deterministic mode: in wl_xb_wait, not queued
    uint64_t det_deadline_us;  // its timeout in the virtual clock (0: none)
    bool running_guest;        // between its routine's start and its end
    uint32_t kernel_depth;     // > 0 while in a kernel call (thunk.c): device accesses are no safe points
    uint32_t host_tid;         // wl_platform_thread_self_id of its host thread (set when it takes the lock)
    uintptr_t host_handle;     // the fault backend's name for that thread (the starvation probe)
    jmp_buf exit_jmp;          // where wl_xb_thread_exit lands
    WlXbThread *next_ready;
    WlXbThread *next_all;
    uint32_t jmp_count;        // guest setjmp buffers this thread armed (recomp_setjmp_slot)
    struct {
        uint32_t buf_va;
        uint32_t kernel_depth; // when it was armed
        jmp_buf native;
    } jmp[WL_XB_MAX_JMP_SLOTS];
};

// ---- Set-up ----------------------------------------------------------------------------------

typedef struct {
    uint32_t quantum_ms;          // how long a thread keeps the lock past a ready thread: 10
    uint32_t starve_ms;           // a ready thread that has waited this long gets the lock at the
                                  // holder's next safe point whatever the priorities (NT boosts a
                                  // starved thread so; a busy-waiting holder would otherwise keep
                                  // lower-priority threads out for good): 30
    uint32_t host_stack_bytes;    // the most a guest thread's host stack gets (recompiled frames
                                  // are C frames; each gets twice its guest stack, 64 KB at
                                  // least): 256 KB
    uint32_t default_stack_bytes; // a guest stack when the title asks for 0: 64 KB
    bool deterministic;           // the deterministic mode (below): off
    // what one safe point costs in the deterministic mode's clock (0: the defaults below)
    uint32_t det_backedge_ns;     // a backward jump: 40 ns
    uint32_t det_call_ns;         // a kernel call or an rdtsc: 400 ns
    uint32_t det_device_ns;       // a device register access: 400 ns
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
// A runtime thread that runs C code as a guest thread: `fn(self, user)` runs holding the guest lock,
// with a guest stack of `stack_bytes`, a KPCR and the title's TLS, at `priority`, so it may read
// and write guest memory, call guest code (wl_xb_call_guest) and wait (wl_xb_wait) like a title
// thread; it never runs at the same time as guest code. It does not count as one of the title's
// threads (the process does not wait for it). The play harness (play.h) runs on one.
typedef void (*WlXbHostThreadFn)(WlXbThread *self, void *user);
WlResult wl_xb_thread_create_host(const char *name, WlXbHostThreadFn fn, void *user, uint32_t stack_bytes, int priority,
                                  WlXbThread **out);
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
// Whether the calling host thread holds the guest lock (true before wl_xb_threads_init: one
// thread). The paging slow path asks it before it waits for the swap file. Lock not necessarily held.
bool wl_xb_lock_held(void);
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
// over (or one of higher priority is ready at all), the caller goes to the back of the queue
// (never at DISPATCH_LEVEL or above: see wl_xb_safe_point).
// Also runs pending interrupt work (irq.h).
void wl_xb_yield_point(void);
// Gives the lock up to every ready thread of equal or higher priority (a delay of 0); not at
// DISPATCH_LEVEL or above (see wl_xb_safe_point).
void wl_xb_yield(void);
// A kernel call's host work that blocks (the card's reads and writes) runs outside the lock, as an
// Xbox thread waits in its I/O while the others run (and the interrupts go on): wl_xb_block_begin
// saves the caller's registers and gives the lock up, returning the caller, or keeps it and returns
// NULL (the deterministic mode, whose clock does not see the card; DISPATCH_LEVEL or above; the
// service thread; a caller without the lock). Between the two the caller touches no guest memory and
// no state the lock guards; wl_xb_block_end(the result) takes the lock back (NULL: nothing).
WlXbThread *wl_xb_block_begin(void);
void wl_xb_block_end(WlXbThread *self);
// The card's lock: a host file's transfer at an offset (a seek, then the read or write, on one
// handle) that may run outside the guest lock is made under it (the kernel's file I/O after
// wl_xb_block_begin), and so is every other transfer on a handle such a one may use (the paging's
// reads of the files its pages remember). Taken with or without the guest lock; the guest lock is
// never taken while it is held. Nothing before wl_xb_threads_init.
void wl_xb_card_lock(void);
void wl_xb_card_unlock(void);
// Whether another thread has the card's lock now (a hint: it may change at once).
bool wl_xb_card_busy(void);
// NtYieldExecution: gives the lock to the best ready thread whatever its priority; one below the
// caller keeps it for a quantum (or until it waits). false when none was ready (or at DISPATCH_LEVEL).
bool wl_xb_yield_any(void);

// A safe point in the middle of guest code: what wl_xb_yield_point does (the interrupt work, with
// the level sources asked too, then a yield when one is due), for the device-access fault paths
// once the device has answered (fault_posix.c, mem_wii.c) and for the generated code's optional
// loop hook. It does nothing unless the calling host thread holds the guest lock outside a kernel
// call and outside the interrupt work, so it may be called from any fault. A title that polls a
// device register until an interrupt changes memory (D3D's fence wait) gets its ISR and DPCs this
// way, and a thread waiting for the lock (the service thread first) gets it at the next access,
// unless the holder's IRQL (its KPCR's) is DISPATCH_LEVEL or above: then it keeps the lock, as it
// keeps the console's processor, and its own safe points run the interrupts its IRQL lets in.
// Lock not necessarily held.
void wl_xb_safe_point(void);
// Non-zero when a safe point has something to do (interrupt work raised, a thread waiting for the
// lock). Generated code may test it at loop back edges, where a spin on plain guest memory has no
// other safe point: if (g_xb_attention) wl_xb_safe_point();
extern volatile uint32_t g_xb_attention;
// The safe point of a device access once the device has answered (fault_posix.c, mem_wii.c): what
// wl_xb_safe_point does, at a device access's cost in the deterministic mode's clock.
void wl_xb_device_safe_point(void);

// ---- The deterministic mode ------------------------------------------------------------------
//
// Off by default; WlXbThreadConfig.deterministic (WlXbTitleConfig.deterministic, the host's
// WL_XB_DETERMINISTIC=1) turns it on. Two runs of the same title with the same input then run the
// same guest instructions in the same order and see the same times: a failure seen once is seen
// again, and a run can be bisected.
//
// The guest's clock (wl_xb_time_us: KeTickCount, rdtsc, KeQueryPerformanceCounter, the interrupt
// and system times, timers, wait timeouts, the vertical blank, the devices' counters and the audio
// processor's frames) is virtual: it moves on by a fixed cost at each safe point of the thread
// that holds the guest lock (a backward jump, a kernel call or rdtsc, a device access), and when
// every guest thread waits it jumps to the next thing due (a vertical blank, a timer, a wait's
// timeout, a device's tick). The host's wall clock never enters. The service thread does no work
// of its own: the vertical blank, ticks and timers come due at the holder's safe points, and the
// service thread is the idle thread, handed the lock only when no guest thread is ready. A wake,
// a new thread and a resume put the thread in the ready queue at once (instead of when its host
// thread gets to run), so the queue, and every decision made from it, depends only on the guest.
// The title runs as fast as the host lets it, not at the console's speed.
bool wl_xb_deterministic(void);
// The guest's monotonic clock in microseconds: the host's (wl_platform_time_us) normally, the
// virtual one in the deterministic mode. Lock not necessarily held.
uint64_t wl_xb_time_us(void);
// The same in nanoseconds (rdtsc's resolution).
uint64_t wl_xb_time_ns(void);
// A busy wait of `us` (KeStallExecutionProcessor): the virtual clock moves on by it; nothing
// outside the deterministic mode. Lock held.
void wl_xb_time_spend_us(uint32_t us);

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
    uint32_t safe_points;   // wl_xb_safe_point calls that held the lock in guest code
    uint32_t starved_handoffs; // the lock given to a thread that waited past starve_ms
    uint32_t safe_yields;   // of those, the ones that gave the lock to a waiting thread
    uint32_t service_starved; // the service thread waited a second or more for the lock (logged)
    // safe points that kept the lock from a waiting thread: the holder was at DISPATCH_LEVEL or above
    uint32_t raised_holds;
    // yield points (wl_xb_yield_point: every kernel call's, and a stall's looks): a thread doing file
    // I/O alone moves on by them, as the deterministic mode counts a safe point only when something
    // is due
    uint32_t kernel_calls;
    uint32_t io_blocks; // kernel calls that gave the lock up for the card (wl_xb_block_begin)
    uint32_t yields, yields_lower; // NtYieldExecution calls; those that gave way to a lower priority
} WlXbThreadStats;
void wl_xb_thread_stats(WlXbThreadStats *out);
// A thread's time off the guest lock since it was made, in microseconds of wl_xb_time_us: queued
// for its turn, in guest waits, in the card's I/O (a span still going on counts up to now). A title's
// frame loop measures its own frame with it: what it waited for, and on what. Any thread.
typedef struct {
    uint64_t ready_us, waited_us, io_us;
} WlXbThreadTimes;
void wl_xb_thread_times(const WlXbThread *t, WlXbThreadTimes *out);
// What the stall watch counts as the guest moving on: hand-offs, safe points, waits, wakes,
// timeouts and kernel calls.
uint32_t wl_xb_thread_progress(void);
// The stall watch's state: one look every 10 s at a progress count (wl_xb_thread_progress plus the
// paging's faults and reads). Returns the seconds without progress when the look reports a stall
// (after two quiet looks, then once a minute while it lasts), else 0.
typedef struct {
    uint32_t last, quiet;
} WlXbStallWatch;
uint32_t wl_xb_stall_watch_look(WlXbStallWatch *w, uint32_t progress);
// The service thread's status, every 10 s: after 20 s with no progress at all
// (wl_xb_thread_progress plus `paging`, the paging's faults and reads), the lock's holder and every
// thread's state are logged (once a minute while the stall lasts).
void wl_xb_thread_stall_check(uint32_t paging);

// A histogram of waits in milliseconds (counters only: an increment per sample). Bounds, ms:
// <1 <2 <5 <10 <20 <30 <40 <60 <100 and above.
#define WL_XB_LAT_BUCKETS 10u
typedef struct {
    uint32_t n[WL_XB_LAT_BUCKETS];
    uint32_t count, max_ms;
} WlXbLatency;
void wl_xb_latency_add(WlXbLatency *h, uint64_t us);
// One log line ("<what>: <count>, ms <1:a <2:b ... max M"), and the histogram starts again; nothing
// when it is empty.
void wl_xb_latency_log(const char *tag, const char *what, WlXbLatency *h);
// The time guest threads spent in the ready queue (enqueued to handed the lock), since the last log.
void wl_xb_thread_log_ready_waits(void);
// Each guest thread's stack scanned for return addresses into the image, for a frozen title.
void wl_xb_thread_log_guest_stacks(void);

// The host memory the threads hold (registered with the census, mem_census.h, as "threads"): their
// records and host stacks; `deepest` is the most any host stack has been used, of `deepest_of`.
void wl_xb_thread_host_memory(size_t *bytes, uint32_t *stacks, size_t *deepest, size_t *deepest_of);
// Gives back the host stacks of the threads that ended (a title's entry thread often does, with
// its stack of 256 KB) a little while after their host threads are done; returns how many. The
// service thread calls it now and then.
uint32_t wl_xb_thread_reap(void);
// One INFO line: each thread's host stack, its size and the deepest it has been (KB), for sizing
// WlXbThreadConfig.host_stack_bytes.
void wl_xb_thread_log_stacks(void);

#ifdef __cplusplus
}
#endif
