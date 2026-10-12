// SPDX-License-Identifier: MIT
// The host and ppc-be fault handler (mmio.h): SIGSEGV inside the guest EA span. A device address
// is emulated through the decoder of the host's instruction set (x86_decode.h on x86-64,
// ppc_decode.h on 32-bit PowerPC) and the thread resumes after the instruction; anything else is
// a guest crash report. A fault outside the span goes to the handler that was there before.
//
// After the device has answered, the access is a safe point (thread.h's wl_xb_safe_point): pending
// interrupt work runs and a waiting thread gets the lock, from inside the handler. That is sound
// here because the fault is synchronous, on the thread holding the guest lock, in guest code (the
// safe point refuses kernel calls and other threads), which holds no host locks; SA_NODEFER lets
// the ISR's own device accesses fault again, and the interrupted registers stay in the ucontext.
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <ucontext.h>

#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/ppc_decode.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/x86_decode.h"

static struct sigaction g_old_segv, g_old_bus;
static bool g_installed;

#if defined(__x86_64__)
static const int k_greg[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                               REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};

static bool emulate(ucontext_t *uc, uint32_t va, const char **what, uintptr_t *pc) {
    WlXbX86Regs r;
    mcontext_t *mc = &uc->uc_mcontext;
    for (int i = 0; i < 16; i++) {
        r.gpr[i] = (uint64_t)mc->gregs[k_greg[i]];
    }
    r.rip = (uint64_t)mc->gregs[REG_RIP];
    r.rflags = (uint64_t)mc->gregs[REG_EFL];
    if (mc->fpregs != NULL) {
        memcpy(r.xmm, mc->fpregs->_xmm, sizeof r.xmm);
    }
    *pc = (uintptr_t)r.rip;
    uint32_t len = wl_xb_x86_emulate((const uint8_t *)(uintptr_t)r.rip, &r, va, what);
    if (len == 0) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        mc->gregs[k_greg[i]] = (greg_t)r.gpr[i];
    }
    mc->gregs[REG_EFL] = (greg_t)r.rflags;
    mc->gregs[REG_RIP] = (greg_t)(r.rip + len);
    if (mc->fpregs != NULL) {
        memcpy(mc->fpregs->_xmm, r.xmm, sizeof r.xmm);
    }
    return true;
}
#elif defined(__powerpc__) && !defined(__powerpc64__)
#define PPC_NIP 32
static bool emulate(ucontext_t *uc, uint32_t va, const char **what, uintptr_t *pc) {
    unsigned long *g = uc->uc_mcontext.uc_regs->gregs;
    uint32_t nip = (uint32_t)g[PPC_NIP];
    *pc = nip;
    uint32_t insn = *(const uint32_t *)(uintptr_t)nip;
    WlXbPpcAccess a;
    WlXbPpcDecode d = wl_xb_ppc_decode(insn, &a);
    if (d != WL_XB_PPC_OK) {
        *what = d == WL_XB_PPC_REFUSED ? a.name : "not a load or store";
        return false;
    }
    uint32_t gpr[32];
    for (int i = 0; i < 32; i++) {
        gpr[i] = (uint32_t)g[i];
    }
    uint32_t ea = wl_xb_ppc_ea(&a, gpr);
    if (!wl_xb_ppc_emulate(&a, gpr, ea, va)) {
        *what = "no device at the address";
        return false;
    }
    for (int i = 0; i < 32; i++) {
        g[i] = gpr[i];
    }
    g[PPC_NIP] = nip + 4u;
    return true;
}
#else
static bool emulate(ucontext_t *uc, uint32_t va, const char **what, uintptr_t *pc) {
    (void)uc;
    (void)va;
    *pc = 0;
    *what = "no instruction decoder for this host";
    return false;
}
#endif

static void chain(int sig, siginfo_t *si, void *ctx) {
    struct sigaction *old = sig == SIGBUS ? &g_old_bus : &g_old_segv;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction != NULL) {
        old->sa_sigaction(sig, si, ctx);
        return;
    }
    if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN && old->sa_handler != NULL) {
        old->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL); // returning re-runs the access, which now kills the process normally
}

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    uintptr_t addr = (uintptr_t)si->si_addr;
    uintptr_t base = WL_XB_HOST_BASE;
    if (!wl_xb_mem_ready() || addr < base || addr - base >= WL_XB_EA_SPAN) {
        chain(sig, si, ctx);
        return;
    }
    uint32_t ea = (uint32_t)(addr - base);
    uint32_t va = wl_xb_ea_to_va(ea);
    const char *what = NULL;
    uintptr_t pc = 0;
    if (va >= WL_XB_MMIO_BASE && wl_xb_mmio_find(va) != NULL) {
        if (emulate((ucontext_t *)ctx, va, &what, &pc)) {
            wl_xb_safe_point();
            return;
        }
        WlXbCrash c = {"device access the decoder cannot emulate", what, va, ea, pc, false};
        wl_xb_crash(&c);
    }
#if defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
#elif defined(__powerpc__) && !defined(__powerpc64__)
    pc = (uintptr_t)((ucontext_t *)ctx)->uc_mcontext.uc_regs->gregs[32];
#endif
    WlXbCrash c = {va >= WL_XB_MMIO_BASE ? "access to an address no device claims" : "access to guest memory that is not there",
                   NULL, va, ea, pc, false};
    wl_xb_crash(&c);
}

WlResult wl_xb_fault_install(void) {
    if (g_installed) {
        return WL_OK;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_old_segv) != 0 || sigaction(SIGBUS, &sa, &g_old_bus) != 0) {
        return WL_ERR_FAILED;
    }
    g_installed = true;
    return WL_OK;
}
