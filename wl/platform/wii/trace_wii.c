// SPDX-License-Identifier: MIT
// Logs every way out of the program with the code that took it (backtrace.h): exit, _exit, abort
// and SYS_ResetSystem. Linked only when the link wraps those symbols (wl_platform_trace_exits in
// cmake/WiiLibs.cmake: --wrap=exit --wrap=_exit --wrap=abort --wrap=SYS_ResetSystem), so a program
// that did not ask for it is unchanged. Each line is "exit(0) from 80001234 ..." under the tag
// "exit", for addr2line on the program's ELF. A game that ends by itself says how and from where.
#include <ogc/system.h>
#include <stdio.h>

#include "wl/platform/backtrace.h"

void __real_exit(int code) __attribute__((noreturn));
void __real__exit(int code) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));
s32 __real_SYS_ResetSystem(s32 reset, u32 reset_code, s32 force_menu);

void __wrap_exit(int code) __attribute__((noreturn));
void __wrap__exit(int code) __attribute__((noreturn));
void __wrap_abort(void) __attribute__((noreturn));
s32 __wrap_SYS_ResetSystem(s32 reset, u32 reset_code, s32 force_menu);

static void trace(const char *name, int code) {
    char what[40];

    snprintf(what, sizeof what, "%s(%d)", name, code);
    wl_platform_log_backtrace("exit", what, 1);
}

void __wrap_exit(int code) {
    trace("exit", code);
    __real_exit(code);
}

void __wrap__exit(int code) {
    trace("_exit", code);
    __real__exit(code);
}

void __wrap_abort(void) {
    trace("abort", 0);
    __real_abort();
}

s32 __wrap_SYS_ResetSystem(s32 reset, u32 reset_code, s32 force_menu) {
    trace("SYS_ResetSystem", (int)reset);
    return __real_SYS_ResetSystem(reset, reset_code, force_menu);
}
