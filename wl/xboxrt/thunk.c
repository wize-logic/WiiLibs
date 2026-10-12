// SPDX-License-Identifier: MIT
// The runtime functions generated code calls, and the kernel thunk dispatcher (thunk.h).
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/thunk.h"

#define TAG "xbthunk"
#define XBOX_TSC_KHZ 733333u // the Pentium III's clock: rdtsc counts at 733.33 MHz

typedef struct {
    WlXbKernelFn fn;
    uint32_t ordinal;
    uint32_t arg_bytes;
    const char *name;
    bool warned;
} Slot;

static Slot g_slots[WL_XB_MAX_THUNKS];
static uint32_t g_slot_count;
static volatile uint32_t g_pending_slot; // set by recomp_lookup_kernel, read at once by the dispatcher
static uint32_t g_caller;
static WlXbManualLookup g_manual;
static bool g_trace_all, g_trace_first;
static uint64_t g_calls_by_ordinal[512];
static uint64_t g_calls;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va);
void recomp_unimpl(const char *text, uint32_t va);
void recomp_trace_enter(const char *name, uint32_t va);
void recomp_trace_exit(const char *name, uint32_t va);
void recomp_trace_esp(const char *name, const char *tag);
void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0, uint32_t edi0, uint32_t esp0);
void recomp_call_profile_hit(uint32_t va);

void wl_xb_set_manual_lookup(WlXbManualLookup fn) {
    g_manual = fn;
}

__attribute__((weak)) recomp_func_t recomp_lookup_manual(uint32_t xbox_va) {
    return g_manual != NULL ? g_manual(xbox_va) : NULL;
}

// The generated dispatch table's lookup, referenced weakly: a program without generated code (the
// runtime's own tests) links, and finds nothing translated. Generated code must be linked as
// objects (sources of the executable, or an OBJECT library): from a static archive nothing would
// pull it in.
extern recomp_func_t recomp_lookup(uint32_t xbox_va) __attribute__((weak));

recomp_func_t wl_xb_lookup(uint32_t va) {
    recomp_func_t fn = recomp_lookup_manual(va);
    if (fn == NULL && recomp_lookup != NULL) {
        fn = recomp_lookup(va);
    }
    if (fn == NULL) {
        fn = recomp_lookup_kernel(va);
    }
    return fn;
}

void wl_xb_set_code_bounds(uint32_t lo, uint32_t hi) {
    g_xbox_code_lo = lo;
    g_xbox_code_hi = hi;
}

// ---- Thunks ------------------------------------------------------------------------------------

void wl_xb_thunk_bind(uint32_t slot, uint32_t ordinal, WlXbKernelFn fn, uint32_t arg_bytes, const char *name) {
    if (slot >= WL_XB_MAX_THUNKS) {
        return;
    }
    g_slots[slot] = (Slot){fn, ordinal, arg_bytes, name, false};
    if (slot + 1u > g_slot_count) {
        g_slot_count = slot + 1u;
    }
}

uint32_t wl_xb_thunk_va(uint32_t slot) {
    return WL_XB_THUNK_BASE + slot * 4u;
}

uint32_t wl_xb_thunk_ordinal(uint32_t va, const char **name) {
    uint32_t slot = (va - WL_XB_THUNK_BASE) / 4u;
    if (va < WL_XB_THUNK_BASE || slot >= g_slot_count) {
        if (name != NULL) {
            *name = NULL;
        }
        return 0;
    }
    if (name != NULL) {
        *name = g_slots[slot].name;
    }
    return g_slots[slot].ordinal;
}

uint32_t wl_xb_kernel_caller(void) {
    return g_caller;
}

void wl_xb_thunk_trace(bool all, bool first_calls) {
    g_trace_all = all;
    g_trace_first = first_calls;
}

void wl_xb_thunk_stats(WlXbThunkStats *out) {
    out->calls = g_calls;
    out->ordinals_used = 0;
    for (uint32_t i = 0; i < 512u; i++) {
        out->ordinals_used += g_calls_by_ordinal[i] != 0;
    }
}

uint64_t wl_xb_thunk_calls(uint32_t ordinal) {
    return ordinal < 512u ? g_calls_by_ordinal[ordinal] : 0;
}

static void thunk_dispatch(void) {
    uint32_t slot = g_pending_slot;
    uint32_t ret = wl_xb_rd32(g_esp);
    g_esp += 4u; // the return address the call pushed: the callee's ret would pop it
    // a kernel call is where the single-CPU Xbox would have taken an interrupt or switched threads
    wl_xb_yield_point();
    uint32_t outer_caller = g_caller;
    g_caller = ret;
    Slot *s = &g_slots[slot];
    WlXbThread *self = wl_xb_current();
    if (self != NULL) {
        self->kernel_depth++; // the kernel's own device accesses are no safe points
    }
    g_calls++;
    uint64_t n = s->ordinal < 512u ? ++g_calls_by_ordinal[s->ordinal] : 0;
    if (g_trace_all || (g_trace_first && n == 1)) {
        WL_LOG_I(TAG, "%s (%u) from 0x%08x esp 0x%08x", s->name != NULL ? s->name : "?", (unsigned)s->ordinal,
                 (unsigned)ret, (unsigned)g_esp);
    }
    if (s->fn != NULL) {
        s->fn();
    } else {
        if (!s->warned) {
            s->warned = true;
            WL_LOG_E(TAG, "kernel ordinal %u (%s) has no implementation: returns 0", (unsigned)s->ordinal,
                     s->name != NULL ? s->name : "?");
        }
        g_eax = 0;
    }
    if (self != NULL) {
        self->kernel_depth--;
    }
    g_esp += s->arg_bytes;
    g_caller = outer_caller;
}

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va) {
    if (xbox_va >= WL_XB_THUNK_BASE && ((xbox_va - WL_XB_THUNK_BASE) & 3u) == 0) {
        uint32_t slot = (xbox_va - WL_XB_THUNK_BASE) / 4u;
        if (slot < g_slot_count) {
            g_pending_slot = slot;
            return thunk_dispatch;
        }
    }
    return NULL;
}

// ---- Indirect call failures, debug output, time ---------------------------------------------

// Each failing target is reported once (they arrive in loops); 64 remembered.
static bool first_time(uint32_t va) {
    static uint32_t seen[64];
    static uint32_t count;
    for (uint32_t i = 0; i < count && i < 64u; i++) {
        if (seen[i] == va) {
            return false;
        }
    }
    seen[count++ % 64u] = va;
    return true;
}

void recomp_icall_fail_log(uint32_t va) {
    if (first_time(va)) {
        WL_LOG_E(TAG, "indirect call to 0x%08x: no translated function there (esp 0x%08x)", (unsigned)va, (unsigned)g_esp);
    }
}

void recomp_icall_not_code_log(uint32_t va) {
    if (first_time(va ^ 0x80000000u)) {
        WL_LOG_E(TAG, "indirect call to 0x%08x: not code (a null or wild function pointer), skipped", (unsigned)va);
    }
}

void recomp_debug_service(uint32_t service, uint32_t arg_va) {
    if (service == 1 && wl_xb_range_ok(arg_va, 8)) { // print an ANSI_STRING (OutputDebugString)
        uint32_t len = wl_xb_rd16(arg_va), buf = wl_xb_rd32(arg_va + 4u);
        char text[512];
        if (len >= sizeof text) {
            len = sizeof text - 1u;
        }
        if (wl_xb_range_ok(buf, len)) {
            wl_xb_copy_out(text, buf, len);
            text[len] = '\0';
            while (len > 0 && (text[len - 1u] == '\n' || text[len - 1u] == '\r')) {
                text[--len] = '\0';
            }
            WL_LOG_I("guest", "%s", text);
            return;
        }
    }
    WL_LOG_D(TAG, "debug service %u (0x%08x)", (unsigned)service, (unsigned)arg_va);
}

uint64_t xbox_ReadTimeStampCounter(void) {
    // a spin on rdtsc is a wait: let interrupts in and other threads run
    if (wl_xb_current() != NULL) {
        wl_xb_yield_point();
    }
    uint64_t us = wl_platform_time_us();
    return us * (XBOX_TSC_KHZ / 1000u) + (us * (XBOX_TSC_KHZ % 1000u)) / 1000u;
}

// ---- setjmp / longjmp ---------------------------------------------------------------------------
// The native half of the guest's setjmp: a host jmp_buf per guest buffer, per thread, so a guest
// longjmp unwinds the host frames of the translated functions it abandons.

jmp_buf *recomp_setjmp_slot(uint32_t buf_va) {
    WlXbThread *t = wl_xb_current();
    WL_ASSERT(t != NULL);
    for (uint32_t i = 0; i < t->jmp_count; i++) {
        if (t->jmp[i].buf_va == buf_va) {
            t->jmp_count = i + 1u; // re-armed: the inner ones are gone
            t->jmp[i].kernel_depth = t->kernel_depth;
            return &t->jmp[i].native;
        }
    }
    if (t->jmp_count >= WL_XB_MAX_JMP_SLOTS) {
        t->jmp_count = WL_XB_MAX_JMP_SLOTS - 1u; // keep the deepest
    }
    t->jmp[t->jmp_count].buf_va = buf_va;
    t->jmp[t->jmp_count].kernel_depth = t->kernel_depth;
    return &t->jmp[t->jmp_count++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value) {
    WlXbThread *t = wl_xb_current();
    if (t == NULL) {
        return 0;
    }
    for (uint32_t i = t->jmp_count; i-- > 0;) {
        if (t->jmp[i].buf_va != buf_va) {
            continue;
        }
        // the registers the CRT's longjmp restores: ebp, ebx, edi, esi, esp (+4 for setjmp's ret)
        g_ebx = wl_xb_rd32(buf_va + 0x04u);
        g_edi = wl_xb_rd32(buf_va + 0x08u);
        g_esi = wl_xb_rd32(buf_va + 0x0Cu);
        g_esp = wl_xb_rd32(buf_va + 0x10u) + 4u;
        g_seh_ebp = wl_xb_rd32(buf_va + 0x00u);
        g_ebp = g_seh_ebp;
        t->jmp_count = i + 1u;
        t->kernel_depth = t->jmp[i].kernel_depth; // a jump out of guest code a kernel call made
        longjmp(t->jmp[i].native, value != 0 ? (int)value : 1);
    }
    return 0;
}

// ---- Port I/O and the generated code's optional hooks --------------------------------------------

uint32_t wl_xb_port_in(uint32_t port, unsigned size) {
    (void)size;
    if (port == 0x80C0u) {
        return 0; // the video encoder's field flag: progressive
    }
    return 0;
}

void wl_xb_port_out(uint32_t port, uint32_t value, unsigned size) {
    (void)port;
    (void)value;
    (void)size;
}

__attribute__((weak)) void recomp_unimpl(const char *text, uint32_t va) {
    if (first_time(va ^ 0x40000000u)) {
        WL_LOG_W(TAG, "unimplemented instruction at 0x%08x: %s", (unsigned)va, text != NULL ? text : "?");
    }
}

__attribute__((weak)) void recomp_trace_enter(const char *name, uint32_t va) {
    WL_LOG_D(TAG, "enter %s 0x%08x esp 0x%08x", name, (unsigned)va, (unsigned)g_esp);
}

__attribute__((weak)) void recomp_trace_exit(const char *name, uint32_t va) {
    WL_LOG_D(TAG, "exit %s 0x%08x eax 0x%08x", name, (unsigned)va, (unsigned)g_eax);
}

__attribute__((weak)) void recomp_trace_esp(const char *name, const char *tag) {
    WL_LOG_D(TAG, "%s %s esp 0x%08x", name, tag, (unsigned)g_esp);
}

__attribute__((weak)) void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0, uint32_t edi0, uint32_t esp0) {
    if (first_time(va ^ 0x20000000u)) {
        WL_LOG_E(TAG, "call 0x%08x broke the ABI: ebx %08x->%08x esi %08x->%08x edi %08x->%08x esp %08x->%08x",
                 (unsigned)va, (unsigned)ebx0, (unsigned)g_ebx, (unsigned)esi0, (unsigned)g_esi, (unsigned)edi0,
                 (unsigned)g_edi, (unsigned)esp0, (unsigned)g_esp);
    }
}

__attribute__((weak)) void recomp_call_profile_hit(uint32_t va) {
    (void)va;
}
