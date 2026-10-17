// SPDX-License-Identifier: MIT
// Guest crash reports: a fault on guest memory that is neither committed nor a device, an access a
// decoder refuses, a call into nothing. The report names the guest address, the host PC and the
// generated function it is in (the nearest translated function below the PC), the guest
// registers, the guest call stack (the return addresses on the guest stack that follow a call
// instruction, as "GS <slot> <word>" lines xboxrecomp's tools/stackwalk.py reads) and the last
// indirect call targets; then the process stops (the stop screen on the Wii, abort elsewhere).
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *reason;  // what happened, one line
    const char *detail;  // more (an instruction's name), or NULL
    uint32_t guest_va;   // the guest address touched (0: none)
    uint32_t ea;         // its EA
    uintptr_t host_pc;   // where the host was (0: unknown)
    bool write;
} WlXbCrash;

// Logs the report (WL_LOG_E lines, tag "xbcrash").
void wl_xb_crash_report(const WlXbCrash *c);
// A guest return address: inside the code, and the bytes before it are a call (stack scans).
bool wl_xb_guest_return_address(uint32_t ra);
// Reports and stops.
WL_NORETURN void wl_xb_crash(const WlXbCrash *c);
// What stopping means (the default: wl_platform_stop_screen on the Wii, abort elsewhere). Tests
// set one that longjmps out.
typedef void (*WlXbCrashStop)(const WlXbCrash *c);
void wl_xb_crash_set_stop(WlXbCrashStop fn);

// The translated function a host PC is in: its guest VA and host start. A title's build may give
// a table (`wl_xb_symbols`, below); without one the dispatch table is searched over the guest's
// code range once (a second or two on the Wii) and kept. false when the PC is in none.
typedef struct {
    uintptr_t host;    // the function's host address
    uint32_t guest_va; // its guest VA
    const char *name;  // "sub_00012A27" or a better name, may be NULL
} WlXbSymbol;
bool wl_xb_symbolize(uintptr_t host_pc, WlXbSymbol *out);

// Optional, defined by a title's build (weak here): its translated functions, in any order.
extern const WlXbSymbol wl_xb_symbols[] __attribute__((weak));
extern const uint32_t wl_xb_symbol_count __attribute__((weak));

#ifdef __cplusplus
}
#endif
