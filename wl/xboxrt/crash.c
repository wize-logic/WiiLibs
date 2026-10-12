// SPDX-License-Identifier: MIT
// Guest crash reports (crash.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/platform/stop.h"
#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/regs.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/thunk.h"

#define TAG "xbcrash"
#define STACK_WORDS 1024u
#define MAX_FRAMES 48u

static WlXbCrashStop g_stop;
static WlXbSymbol *g_syms;
static uint32_t g_sym_count;
static bool g_syms_built;

void wl_xb_crash_set_stop(WlXbCrashStop fn) {
    g_stop = fn;
}

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const WlXbSymbol *)a)->host, y = ((const WlXbSymbol *)b)->host;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void build_symbols(void) {
    g_syms_built = true;
    if (&wl_xb_symbol_count != NULL && wl_xb_symbol_count > 0) {
        g_syms = malloc(wl_xb_symbol_count * sizeof *g_syms);
        if (g_syms != NULL) {
            memcpy(g_syms, wl_xb_symbols, wl_xb_symbol_count * sizeof *g_syms);
            g_sym_count = wl_xb_symbol_count;
        }
    } else if (g_xbox_code_hi > g_xbox_code_lo) {
        // every address of the code range through the generated dispatch table
        uint32_t cap = 1024, n = 0;
        WlXbSymbol *s = malloc(cap * sizeof *s);
        for (uint32_t va = g_xbox_code_lo; s != NULL && va < g_xbox_code_hi; va++) {
            recomp_func_t fn = recomp_lookup(va);
            if (fn == NULL) {
                continue;
            }
            if (n == cap) {
                cap *= 2u;
                WlXbSymbol *t = realloc(s, cap * sizeof *s);
                if (t == NULL) {
                    break;
                }
                s = t;
            }
            s[n++] = (WlXbSymbol){(uintptr_t)fn, va, NULL};
        }
        g_syms = s;
        g_sym_count = s != NULL ? n : 0;
    }
    if (g_sym_count > 1) {
        qsort(g_syms, g_sym_count, sizeof *g_syms, by_host);
    }
}

bool wl_xb_symbolize(uintptr_t host_pc, WlXbSymbol *out) {
    if (!g_syms_built) {
        build_symbols();
    }
    uint32_t lo = 0, hi = g_sym_count;
    while (lo < hi) { // the last symbol at or below the PC
        uint32_t mid = (lo + hi) / 2u;
        if (g_syms[mid].host <= host_pc) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    *out = g_syms[lo - 1u];
    return true;
}

// A guest return address: inside the code, and the bytes before it are a call.
static bool after_call(uint32_t ra) {
    if (ra < g_xbox_code_lo + 7u || ra >= g_xbox_code_hi || !wl_xb_range_ok(ra - 7u, 7)) {
        return false;
    }
    uint8_t b[7];
    wl_xb_copy_out(b, ra - 7u, 7); // b[6] is the byte just before ra
    if (b[2] == 0xE8u) {
        return true; // call rel32
    }
    for (unsigned len = 2; len <= 7; len++) { // call r/m: FF /2 with 0..5 bytes of operand
        unsigned at = 7u - len;
        if (b[at] == 0xFFu && ((b[at + 1u] >> 3) & 7u) == 2u) {
            return true;
        }
    }
    return false;
}

void wl_xb_crash_report(const WlXbCrash *c) {
    WL_LOG_E(TAG, "GUEST CRASH: %s%s%s", c->reason != NULL ? c->reason : "?", c->detail != NULL ? ": " : "",
             c->detail != NULL ? c->detail : "");
    if (c->guest_va != 0 || c->ea != 0) {
        WL_LOG_E(TAG, "  %s guest VA 0x%08x (EA 0x%08x)", c->write ? "write to" : "access to", (unsigned)c->guest_va,
                 (unsigned)c->ea);
    }
    WlXbSymbol sym;
    if (c->host_pc != 0 && wl_xb_symbolize(c->host_pc, &sym)) {
        WL_LOG_E(TAG, "  host PC %p in sub_%08X+0x%x%s%s", (void *)c->host_pc, (unsigned)sym.guest_va,
                 (unsigned)(c->host_pc - sym.host), sym.name != NULL ? " " : "", sym.name != NULL ? sym.name : "");
    } else {
        WL_LOG_E(TAG, "  host PC %p (not in a translated function)", (void *)c->host_pc);
    }
    WlXbThread *t = wl_xb_current();
    WL_LOG_E(TAG, "  thread %u (%s)  eax %08x ecx %08x edx %08x ebx %08x", t != NULL ? (unsigned)t->id : 0u,
             t != NULL ? t->name : "-", (unsigned)g_eax, (unsigned)g_ecx, (unsigned)g_edx, (unsigned)g_ebx);
    WL_LOG_E(TAG, "  esp %08x ebp %08x esi %08x edi %08x fs %08x seh_ebp %08x", (unsigned)g_esp, (unsigned)g_ebp,
             (unsigned)g_esi, (unsigned)g_edi, (unsigned)g_fs_base, (unsigned)g_seh_ebp);
    uint32_t frames = 0;
    for (uint32_t i = 0; i < STACK_WORDS && frames < MAX_FRAMES; i++) {
        uint32_t slot = g_esp + 4u * i;
        if (!wl_xb_range_ok(slot, 4)) {
            break;
        }
        uint32_t w = wl_xb_rd32(slot);
        if (after_call(w)) {
            WL_LOG_E(TAG, "GS %08X %08X", (unsigned)slot, (unsigned)w);
            frames++;
        }
    }
    if (frames == 0) {
        WL_LOG_E(TAG, "  no guest return addresses on the stack");
    }
    char line[160];
    size_t n = 0;
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t idx = (g_icall_trace_idx - 1u - i) & 15u;
        n += (size_t)snprintf(line + n, sizeof line - n, " %08x", (unsigned)g_icall_trace[idx]);
        if (n >= sizeof line) {
            break;
        }
    }
    WL_LOG_E(TAG, "  last indirect calls (newest first):%s", line);
}

void wl_xb_crash(const WlXbCrash *c) {
    wl_xb_crash_report(c);
    if (g_stop != NULL) {
        g_stop(c);
    }
#if defined(__wii__)
    wl_platform_log_flush();
    wl_platform_stop_screen("Guest crash: %s\nguest VA 0x%08x\nSee the log for the guest stack.",
                            c->reason != NULL ? c->reason : "?", (unsigned)c->guest_va);
#else
    abort();
#endif
}
