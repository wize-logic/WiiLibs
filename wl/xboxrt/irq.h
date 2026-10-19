// SPDX-License-Identifier: MIT
// Interrupt lines, the service thread and the time the guest reads.
//
// The kernel (wl/xboxkrnl) decides what an interrupt does (its KINTERRUPTs, DPC queue, timers);
// the runtime decides when that work runs: at the next yield point of the thread holding the
// guest lock (every kernel call is one, so is the generated code's rdtsc, and so is every device
// access once the device has answered: thread.h's wl_xb_safe_point), or on the service thread,
// which takes the lock itself when no guest thread runs and is handed it at the holder's next
// safe point when one does. Either way the work runs with the lock held and the interrupted
// thread's registers saved, on the interrupted thread's KPCR, never inside itself, and on a guest
// stack of its own (the service thread's): a device access can come in the middle of a translated
// function whose esp the compiler has not stored yet, so the interrupted stack is left alone.
//
// The service thread also keeps KeTickCount moving in guest memory (GetTickCount reads it
// directly) and makes the vertical blank: from the retrace interrupt on the Wii, from a 59.94 Hz
// timer on the host.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XB_IRQ_LINES 32u
#define WL_XB_IRQ_GPU 3u     // the NV2A (HalGetInterruptVector(3) in D3D)
#define WL_XB_IRQ_USB0 1u
#define WL_XB_IRQ_NIC 4u
#define WL_XB_IRQ_APU 5u
#define WL_XB_IRQ_AC97 6u
#define WL_XB_IRQ_USB1 9u

// Work reasons the handler is told about (bits).
#define WL_XB_SVC_IRQ 0x1u     // a line is asserted
#define WL_XB_SVC_TIMER 0x2u   // the deadline set by wl_xb_service_set_deadline passed
#define WL_XB_SVC_DPC 0x4u     // the kernel queued deferred work
#define WL_XB_SVC_VBLANK 0x8u  // a vertical blank happened (after the vblank hook ran)
#define WL_XB_SVC_DEVICE 0x10u // a device's periodic work ran (wl_xb_service_add_tick)

// The kernel's handler: runs timers, ISRs and DPCs. Called with the lock held, never nested.
typedef void (*WlXbServiceFn)(void *user, uint32_t reasons);
void wl_xb_service_set_handler(WlXbServiceFn fn, void *user);
// Called at each vertical blank before the handler (the GPU model's vblank, which may assert the
// GPU line). Lock held.
typedef void (*WlXbVblankFn)(void *user);
void wl_xb_service_set_vblank(WlXbVblankFn fn, void *user);
// More work at each vertical blank, after that hook and in the order added (the controller ports,
// the USB frame tick, the play harness). WL_ERR_LIMIT past 8.
WlResult wl_xb_service_add_vblank_hook(WlXbVblankFn fn, void *user);
// Vertical blanks so far (the guest's frame clock: the play harness counts them).
uint32_t wl_xb_vblank_count(void);
// The calling guest thread sleeps until the next vertical blank's round has run (its hooks, and so
// the title's own vblank callback, included) or `timeout_ms` passes; the others run meanwhile. For a
// title's loop that spins on a counter its vblank callback moves (the caller checks it again). true
// when a vertical blank came; false at once from the service thread or inside the handler, or when 4
// threads wait already. Lock held.
bool wl_xb_vblank_wait(uint32_t timeout_ms);
// A device's periodic work (the audio processor's frames): `fn` runs about every `period_us`
// microseconds, lock held, before the handler in the same round (an interrupt it raises is
// delivered at once). Up to WL_XB_MAX_TICKS; before or after wl_xb_service_start.
#define WL_XB_MAX_TICKS 4u
typedef void (*WlXbTickFn)(void *user);
WlResult wl_xb_service_add_tick(WlXbTickFn fn, void *user, uint32_t period_us);

typedef struct {
    uint32_t vblank_hz_milli; // the host's vblank rate: 59940 (the Wii follows the display)
    uint32_t tick_ms;         // how often KeTickCount moves on the service thread: 1 (host), 2 (Wii)
    bool vblank;              // make vertical blanks at all (tests turn it off)
    uint32_t host_stack_bytes; // the service thread's host stack: 128 KB (card runs: 14 KB at most)
} WlXbServiceConfig;
void wl_xb_service_config_default(WlXbServiceConfig *cfg);

// Starts the service thread (and on the Wii hooks the retrace). Needs wl_xb_threads_init.
WlResult wl_xb_service_start(const WlXbServiceConfig *cfg);
// Stops it (tests). Lock not held.
void wl_xb_service_stop(void);
// The same within `timeout_ms` (WL_WAIT_FOREVER: none): false, logged with what the thread was
// doing, when it did not end in time (it is left running; the title's exit goes on without it).
bool wl_xb_service_stop_within(uint32_t timeout_ms);

// Asserts / deasserts an interrupt line (any thread, lock held or not). A level source is asked
// each time the handler looks at the line: `pending(user)` true means asserted.
void wl_xb_irq_assert(uint32_t line);
void wl_xb_irq_deassert(uint32_t line);
typedef bool (*WlXbIrqLevelFn)(void *user);
void wl_xb_irq_set_level(uint32_t line, WlXbIrqLevelFn pending, void *user);
// The lines asserted now (bit n = line n), level sources included.
uint32_t wl_xb_irq_lines(void);
// The handler's view of which lines it can deliver now (the kernel: the lines whose ISR's IRQL is
// above the current thread's, and the lines nobody listens to). Lines it masks stay asserted
// without making the safe points run the handler. Called with the lock held.
typedef uint32_t (*WlXbIrqMaskFn)(void *user);
void wl_xb_irq_set_mask(WlXbIrqMaskFn fn, void *user);
// The asserted lines the handler can deliver now (wl_xb_irq_lines through the mask). Lock held.
uint32_t wl_xb_irq_deliverable(void);

// Asks for the handler to run soon (`reasons` are passed on). Any thread.
void wl_xb_service_raise(uint32_t reasons);
// The monotonic time (wl_platform_time_us) at which WL_XB_SVC_TIMER is due; 0 for none. Lock held.
void wl_xb_service_set_deadline(uint64_t when_us);
// A safe point: runs the handler if something is pending (the lock held, not inside the handler).
void wl_xb_service_poll(void);
// The same, asking the level sources too (a device access's safe point: the device may have just
// raised its line).
void wl_xb_service_poll_levels(void);
// Work the service thread would take the lock for: something pending, the timer's deadline come, an
// interrupt line asserted (whatever the holder's IRQL lets in). Any thread.
bool wl_xb_service_wanted(void);
// True while the handler runs (an ISR's or DPC's kernel calls see it).
bool wl_xb_in_service(void);

// Where KeTickCount lives in guest memory (the kernel's data page); 0 stops the updates.
void wl_xb_service_set_tick_va(uint32_t va);
// The millisecond tick the guest sees (since wl_xb_service_start, from 0).
uint32_t wl_xb_tick_ms(void);

typedef struct {
    uint32_t vblanks, handler_runs, polls_with_work, service_thread_runs;
    // vertical blanks raised (each runs, up to a second's worth owed); raised while one was still
    // pending; periods a late service thread raised at once; owed past the second's worth (lost)
    uint32_t vblanks_raised, vblanks_merged, vblanks_skipped, vblanks_dropped;
    // service calls that ran the handler; of those, the ones that used every round (MAX_ROUNDS: the
    // rest waits for the next call) with vertical blanks still owed, and with interrupt lines still
    // deliverable (a line no ISR takes down); the most rounds in one call since the last status line
    uint32_t services, services_full_owed, services_full_lines, rounds_max;
    // what the service costs the holder and the service thread (the runtime's counts at the exit):
    // polls (wl_xb_service_poll/_poll_levels outside the handler), of them the ones that asked the
    // level sources; the service thread's rounds (each wake-up of its loop) and the ticks run
    uint32_t polls, level_scans, service_rounds, ticks_run;
} WlXbServiceStats;
void wl_xb_service_stats(WlXbServiceStats *out);
// More lines for the status report every 10 s (the kernel's I/O numbers); called on the service
// thread, lock not held. NULL removes it.
typedef void (*WlXbStatusFn)(void);
void wl_xb_service_set_status_hook(WlXbStatusFn fn);

// ---- Between irq.c and thread.c: the deterministic mode (thread.h) ----------------------------

struct WlXbThread;

// Set (beside g_xb_attention) when interrupt work is raised: the deterministic safe point's fast
// path looks at it, as g_xb_attention stays set in that mode.
extern volatile uint32_t g_xb_det_work;
// The vertical blank and the devices' ticks whose virtual time `now_us` has come are marked due;
// returns the earliest virtual time something of the service's is due next (the kernel's timer
// deadline included; UINT64_MAX: nothing). Lock held.
uint64_t wl_xb_det_service_due(uint64_t now_us);
// The service's work due now (wl_xb_service_poll_levels with the lock held by the idle thread).
void wl_xb_det_service_run(void);
// thread.c: a new deadline of the service's (lowers the next virtual time the holder looks at).
void wl_xb_det_note_deadline(uint64_t when_us);
// thread.c: the idle thread (the service thread) gets the lock whenever no guest thread is ready;
// NULL takes it back (wl_xb_service_stop). Lock not held.
void wl_xb_det_set_idle(struct WlXbThread *t);
// thread.c: the idle thread waits up to `timeout_ms` for the lock; true when it has it (registers
// loaded, as wl_xb_lock leaves them). Lock not held.
bool wl_xb_det_idle_take(struct WlXbThread *self, uint32_t timeout_ms);
// thread.c, by the idle thread holding the lock: timed-out waits are queued and the service work
// due now runs; then 1 when a guest thread is ready, else the clock jumps to the next thing due (a
// service deadline, a wait's timeout) and 0, or -1 when nothing at all is due.
int wl_xb_det_idle_step(void);

#ifdef __cplusplus
}
#endif
