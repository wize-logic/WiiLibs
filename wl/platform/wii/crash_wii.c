// SPDX-License-Identifier: MIT
// The crash report: when a CPU exception reaches libogc's panic handler (a data or instruction
// access fault, an alignment or program exception), the exception, the registers and the return
// addresses up the stack are written to the EXI UART (SYS_Report: Dolphin's OSREPORT log), and
// the last lines of the log ring after them, before libogc's own panic screen. Without it a crash
// under Dolphin's batch mode shows nothing: the panic screen reloads, and Dolphin exits with the
// emulation. Feed the addresses to addr2line (or the ELF's map) for the functions.
#include <stdint.h>

#include <ogc/system.h>
#include <tuxedo/ppc/exception.h>

#include "wl/platform/backtrace.h"
#include "wl/platform/platform.h"

#define CALLER_DEPTH 24u
#define LOG_TAIL 12u

static PPCExcptPanicFn g_previous;

static const char *exception_name(unsigned exid) {
    switch (exid) {
    case PPC_EXCPT_RESET: return "reset";
    case PPC_EXCPT_MCHK: return "machine check";
    case PPC_EXCPT_DSI: return "data access (DSI)";
    case PPC_EXCPT_ISI: return "instruction access (ISI)";
    case PPC_EXCPT_ALIGN: return "alignment";
    case PPC_EXCPT_UNDEF: return "program";
    case PPC_EXCPT_FPU: return "floating point unavailable";
    case PPC_EXCPT_TRACE: return "trace";
    case PPC_EXCPT_BKPT: return "breakpoint";
    default: return "exception";
    }
}

static void crash_report(unsigned exid, PPCContext *ctx) {
    uint32_t dar, dsisr;
    const char *lines[LOG_TAIL];
    uint32_t count, index;
    uint32_t callers[CALLER_DEPTH + 1u];

    __asm__ volatile("mfspr %0, 19" : "=r"(dar));
    __asm__ volatile("mfspr %0, 18" : "=r"(dsisr));
    SYS_Report("\n*** CRASH: %s exception (%u) at %08x\n", exception_name(exid), exid, (unsigned)ctx->pc);
    SYS_Report("  lr %08x  msr %08x  cr %08x  ctr %08x  dar %08x  dsisr %08x\n", (unsigned)ctx->lr,
               (unsigned)ctx->msr, (unsigned)ctx->cr, (unsigned)ctx->ctr, (unsigned)dar, (unsigned)dsisr);
    for (index = 0; index < 32; index += 4)
        SYS_Report("  r%-2u %08x %08x %08x %08x\n", (unsigned)index, (unsigned)ctx->gpr[index],
                   (unsigned)ctx->gpr[index + 1], (unsigned)ctx->gpr[index + 2], (unsigned)ctx->gpr[index + 3]);
    // the callers: the link register, then the return addresses up the frame chain
    count = wl_platform_backtrace_from(ctx->gpr[1], ctx->lr, callers, CALLER_DEPTH + 1u);
    SYS_Report("  callers:");
    for (index = 0; index < count; index++)
        SYS_Report(" %08x", (unsigned)callers[index]);
    SYS_Report("\n");
    count = wl_platform_log_tail(lines, LOG_TAIL);
    if (count) {
        SYS_Report("  the log's last %u lines:\n", (unsigned)count);
        for (index = 0; index < count; index++)
            SYS_Report("    %s\n", lines[index]);
    }
    if (g_previous)
        g_previous(exid, ctx);
}

void wl_platform_crash_report_install(void) {
    if (PPCExcptCurPanicFn == crash_report)
        return;
    g_previous = PPCExcptCurPanicFn;
    PPCExcptCurPanicFn = crash_report;
}
