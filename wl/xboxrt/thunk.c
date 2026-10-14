// SPDX-License-Identifier: MIT
// The runtime functions generated code calls, and the kernel thunk dispatcher (thunk.h).
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
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

// ---- Indirect call targets ---------------------------------------------------------------------

// Every indirect call and jump of generated code resolves its target through wl_xb_lookup
// (RECOMP_RESOLVE in recomp_types.h). The generated table's recomp_lookup is a binary search over
// every translated function (13 probes for Halo's 7,600, with a mispredicted branch at most of
// them: 3.8% of a10's time on the Wii, most of it the CRT's qsort calling its comparator), so the
// answers are kept in a direct-mapped cache in front of it: a hit is an index, a load, a compare.
// 4096 entries (32 KB of bss): the targets one frame of a game reaches are a few hundred.
//
// Only what the manual and generated tables give is kept. Both are fixed for a run (a title that
// changes what its own recomp_lookup_manual answers calls wl_xb_lookup_flush), so an entry never
// goes stale. A kernel thunk is not kept: its lookup tells the dispatcher which slot is coming
// (g_pending_slot), so it has to run every time. Guest code runs under the guest lock, one thread
// at a time, which is what makes the two-word entries safe to fill without anything more.
#define ICACHE_BITS 12u
#define ICACHE_SIZE (1u << ICACHE_BITS)

typedef struct {
    uint32_t va;
    recomp_func_t fn; // NULL: empty (an entry is never filled with a miss)
} ICacheEntry;

static ICacheEntry g_icache[ICACHE_SIZE];

// Function starts are mostly 16-aligned and the code spans a few MB: the bits above the alignment
// with the bits a cache size higher folded in, so code 64 KB apart does not collide every time.
static inline uint32_t icache_index(uint32_t va) {
    return ((va >> 2) ^ (va >> (2u + ICACHE_BITS))) & (ICACHE_SIZE - 1u);
}

void wl_xb_lookup_flush(void) {
    memset(g_icache, 0, sizeof g_icache);
}

void wl_xb_set_manual_lookup(WlXbManualLookup fn) {
    g_manual = fn;
    wl_xb_lookup_flush(); // the manual table comes first: what it now answers wins
}

__attribute__((weak)) recomp_func_t recomp_lookup_manual(uint32_t xbox_va) {
    return g_manual != NULL ? g_manual(xbox_va) : NULL;
}

// The generated dispatch table's lookup, referenced weakly: a program without generated code (the
// runtime's own tests) links, and finds nothing translated. Generated code must be linked as
// objects (sources of the executable, or an OBJECT library): from a static archive nothing would
// pull it in.
extern recomp_func_t recomp_lookup(uint32_t xbox_va) __attribute__((weak));

// The miss: manual, generated, kernel, as the generated code used to ask them itself. A title's code
// lies in its XBE's sections, never in the thunks' synthetic range, so a thunk skips the search.
__attribute__((noinline)) static recomp_func_t lookup_miss(uint32_t va, ICacheEntry *e) {
    recomp_func_t fn = recomp_lookup_manual(va);
    if (fn == NULL && va < WL_XB_THUNK_BASE && recomp_lookup != NULL) {
        fn = recomp_lookup(va);
    }
    if (fn != NULL) {
        e->va = va;
        e->fn = fn;
        return fn;
    }
    return recomp_lookup_kernel(va);
}

recomp_func_t wl_xb_lookup(uint32_t va) {
    ICacheEntry *e = &g_icache[icache_index(va)];
    recomp_func_t fn = e->fn;
    if (__builtin_expect(e->va == va && fn != NULL, 1)) {
        return fn;
    }
    return lookup_miss(va, e);
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

// The targets that had no translated function, kept as a seed file for the next run of the
// pipeline (tools/xbox_recomp.py's "seed_functions"; entries marked observed, which the
// disassembler trusts): recompiling with it translates them. A new one is appended to the file as
// it is, whatever else it holds (a title project's seed file also keeps entries of its own, with
// their notes, and may be far longer than what a run adds).
static char g_seed_path[WL_FS_PATH_MAX];
static uint32_t *g_seeds;
static uint32_t g_seed_count, g_seed_cap;

static bool seed_known(uint32_t va) {
    for (uint32_t i = 0; i < g_seed_count; i++) {
        if (g_seeds[i] == va) {
            return true;
        }
    }
    return false;
}

static void seed_note(uint32_t va) {
    if (g_seed_count == g_seed_cap) {
        uint32_t cap = g_seed_cap != 0 ? g_seed_cap * 2u : 512u;
        uint32_t *more = realloc(g_seeds, cap * sizeof *more);
        if (more == NULL) {
            return;
        }
        g_seeds = more;
        g_seed_cap = cap;
    }
    g_seeds[g_seed_count++] = va;
}

// the whole file, NUL-terminated (malloc'd; NULL when there is none)
static char *seeds_read(uint32_t *len) {
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(g_seed_path, WL_FILE_READ, &f) != WL_OK) {
        return NULL;
    }
    uint64_t size = wl_platform_fs_size(f);
    char *text = size < (16u << 20) ? malloc((size_t)size + 1u) : NULL;
    if (text != NULL && wl_platform_fs_read_at(f, 0, text, (uint32_t)size) != WL_OK) {
        free(text);
        text = NULL;
    }
    wl_platform_fs_close(f);
    if (text != NULL) {
        text[size] = '\0';
        *len = (uint32_t)size;
    }
    return text;
}

static void seeds_append(uint32_t va) {
    uint32_t len = 0;
    char *text = seeds_read(&len);
    char *close = text != NULL ? strrchr(text, ']') : NULL;
    char tmp[WL_FS_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_seed_path);
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(tmp, WL_FILE_WRITE_NEW, &f) != WL_OK) {
        free(text);
        return;
    }
    char line[128];
    int n = snprintf(line, sizeof line, " {\"start\": \"0x%08X\", \"observed\": true, \"note\": \"observed at runtime: indirect call\"}\n]\n",
                     (unsigned)va);
    if (close != NULL) {
        char *end = close;
        while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        bool entries = memchr(text, '{', (size_t)(end - text)) != NULL;
        wl_platform_fs_write(f, text, (uint32_t)(end - text));
        wl_platform_fs_write(f, entries ? ",\n" : "\n", entries ? 2u : 1u);
    } else {
        wl_platform_fs_write(f, "[\n", 2);
    }
    wl_platform_fs_write(f, line, (uint32_t)n);
    wl_platform_fs_close(f);
    free(text);
    wl_platform_fs_remove(g_seed_path);
    wl_platform_fs_rename(tmp, g_seed_path);
}

void wl_xb_set_seed_file(const char *path) {
    g_seed_count = 0;
    snprintf(g_seed_path, sizeof g_seed_path, "%s", path != NULL ? path : "");
    if (g_seed_path[0] == '\0') {
        return;
    }
    // what earlier runs and the title recorded: every "0x" number in the file
    uint32_t len = 0;
    char *text = seeds_read(&len);
    for (const char *c = text != NULL ? strstr(text, "0x") : NULL; c != NULL; c = strstr(c + 2, "0x")) {
        uint32_t va = (uint32_t)strtoul(c, NULL, 16);
        if (va != 0 && !seed_known(va)) {
            seed_note(va);
        }
    }
    free(text);
}

void recomp_icall_fail_log(uint32_t va) {
    if (first_time(va)) {
        WL_LOG_E(TAG, "indirect call to 0x%08x: no translated function there (esp 0x%08x)", (unsigned)va, (unsigned)g_esp);
    }
    if (g_seed_path[0] != '\0' && va >= g_xbox_code_lo && va < g_xbox_code_hi && !seed_known(va)) {
        seed_note(va);
        seeds_append(va);
        WL_LOG_I(TAG, "0x%08x added to %s: recompile with it as a seed to translate it", (unsigned)va, g_seed_path);
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
    if (wl_xb_deterministic()) {
        uint64_t ns = wl_xb_time_ns();
        return (ns / 1000000u) * XBOX_TSC_KHZ + (ns % 1000000u) * XBOX_TSC_KHZ / 1000000u;
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
