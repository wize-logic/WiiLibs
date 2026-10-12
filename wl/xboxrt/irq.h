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

// Work reasons the handler is told about (bits).
#define WL_XB_SVC_IRQ 0x1u     // a line is asserted
#define WL_XB_SVC_TIMER 0x2u   // the deadline set by wl_xb_service_set_deadline passed
#define WL_XB_SVC_DPC 0x4u     // the kernel queued deferred work
#define WL_XB_SVC_VBLANK 0x8u  // a vertical blank happened (after the vblank hook ran)

// The kernel's handler: runs timers, ISRs and DPCs. Called with the lock held, never nested.
typedef void (*WlXbServiceFn)(void *user, uint32_t reasons);
void wl_xb_service_set_handler(WlXbServiceFn fn, void *user);
// Called at each vertical blank before the handler (the GPU model's vblank, which may assert the
// GPU line). Lock held.
typedef void (*WlXbVblankFn)(void *user);
void wl_xb_service_set_vblank(WlXbVblankFn fn, void *user);

typedef struct {
    uint32_t vblank_hz_milli; // the host's vblank rate: 59940 (the Wii follows the display)
    uint32_t tick_ms;         // how often KeTickCount moves on the service thread: 1 (host), 2 (Wii)
    bool vblank;              // make vertical blanks at all (tests turn it off)
} WlXbServiceConfig;
void wl_xb_service_config_default(WlXbServiceConfig *cfg);

// Starts the service thread (and on the Wii hooks the retrace). Needs wl_xb_threads_init.
WlResult wl_xb_service_start(const WlXbServiceConfig *cfg);
// Stops it (tests). Lock not held.
void wl_xb_service_stop(void);

// Asserts / deasserts an interrupt line (any thread, lock held or not). A level source is asked
// each time the handler looks at the line: `pending(user)` true means asserted.
void wl_xb_irq_assert(uint32_t line);
void wl_xb_irq_deassert(uint32_t line);
typedef bool (*WlXbIrqLevelFn)(void *user);
void wl_xb_irq_set_level(uint32_t line, WlXbIrqLevelFn pending, void *user);
// The lines asserted now (bit n = line n), level sources included.
uint32_t wl_xb_irq_lines(void);

// Asks for the handler to run soon (`reasons` are passed on). Any thread.
void wl_xb_service_raise(uint32_t reasons);
// The monotonic time (wl_platform_time_us) at which WL_XB_SVC_TIMER is due; 0 for none. Lock held.
void wl_xb_service_set_deadline(uint64_t when_us);
// A safe point: runs the handler if something is pending (the lock held, not inside the handler).
void wl_xb_service_poll(void);
// The same, asking the level sources too (a device access's safe point: the device may have just
// raised its line).
void wl_xb_service_poll_levels(void);
// True while the handler runs (an ISR's or DPC's kernel calls see it).
bool wl_xb_in_service(void);

// Where KeTickCount lives in guest memory (the kernel's data page); 0 stops the updates.
void wl_xb_service_set_tick_va(uint32_t va);
// The millisecond tick the guest sees (since wl_xb_service_start, from 0).
uint32_t wl_xb_tick_ms(void);

typedef struct {
    uint32_t vblanks, handler_runs, polls_with_work, service_thread_runs;
} WlXbServiceStats;
void wl_xb_service_stats(WlXbServiceStats *out);

#ifdef __cplusplus
}
#endif
