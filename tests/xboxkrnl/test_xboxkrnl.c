// SPDX-License-Identifier: MIT
// The kernel through its imports, without generated code: a synthetic XBE (built here) with a
// kernel thunk table is loaded and resolved, and "guest functions" written in C (found through the
// manual lookup, at VAs in its code section) call the imports the way translated code does (push
// the arguments, push a return address, call through the slot). Covers the thunk ABI (stdcall pops,
// fastcall, cdecl, 64-bit returns), dispatcher objects and waits under contention between guest
// threads, critical sections, timers with DPCs, an interrupt line with its ISR and DPC, an ISR and
// a thread switch delivered to a loop that only polls a device register, user APCs
// from overlapped reads, the file layer (D: read-only, Z: sparse, listings, renames), virtual and
// contiguous memory, Rtl's strings, time and printf, the crypto exports, the EEPROM settings.
#define _DEFAULT_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/base/endian.h"
#include "wl/platform/host/fs_host.h"
#include "wl/xboxkrnl/xboxkrnl.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/thunk.h"
#include "wl/xboxrt/xbe.h"

// ---- The synthetic title ------------------------------------------------------------------------

#define BASE 0x00010000u
#define TEXT_VA 0x00011000u
#define DATA_VA 0x00012000u
#define THUNK_VA DATA_VA
#define SCRATCH_VA 0x00013000u // a page of the image for strings and structures
#define IMAGE_SIZE 0x00008000u

// the imports, in thunk-slot order
static const uint16_t k_ordinals[] = {
    127, 126, 268, 53, 51, 8, 189, 225, 186, 233, 234, 235, 187, 192, 221, 255, 258, 291, 277, 294, 108, 145, 159, 158,
    113, 107, 149, 119, 109, 98, 44, 190, 202, 219, 236, 211, 226, 207, 210, 218, 184, 199, 217, 166, 171, 173, 168, 305,
    304, 289, 279, 362, 335, 336, 337, 340, 338, 339, 24, 99, 232, 238, 112, 132, 128, 156, 322, 224, 129, 161,
    195, 246, 250, 251, 364,
};
enum {
    S_PERF_FREQ, S_PERF_COUNTER, S_COMPARE_MEMORY, S_INTERLOCKED_INC, S_INTERLOCKED_CMPXCHG, S_DBGPRINT,
    S_NT_CREATE_EVENT, S_NT_SET_EVENT, S_NT_CLEAR_EVENT, S_NT_WAIT1, S_NT_WAIT1EX, S_NT_WAITN, S_NT_CLOSE,
    S_NT_CREATE_MUTANT, S_NT_RELEASE_MUTANT, S_PS_CREATE, S_PS_TERMINATE, S_RTL_INIT_CS, S_RTL_ENTER_CS,
    S_RTL_LEAVE_CS, S_KE_INIT_EVENT, S_KE_SET_EVENT, S_KE_WAIT1, S_KE_WAITN, S_KE_INIT_TIMER, S_KE_INIT_DPC,
    S_KE_SET_TIMER, S_KE_INSERT_DPC, S_KE_INIT_INTERRUPT, S_KE_CONNECT_INTERRUPT, S_HAL_VECTOR, S_NT_CREATE_FILE,
    S_NT_OPEN_FILE, S_NT_READ, S_NT_WRITE, S_NT_QUERY_INFO, S_NT_SET_INFO, S_NT_QUERY_DIR, S_NT_QUERY_ATTRS,
    S_NT_QUERY_VOLUME, S_NT_ALLOC_VM, S_NT_FREE_VM, S_NT_QUERY_VM, S_MM_CONTIG_EX, S_MM_FREE_CONTIG, S_MM_PHYS,
    S_MM_GPU_INSTANCE, S_RTL_TIME_TO_FIELDS, S_RTL_FIELDS_TO_TIME, S_RTL_INIT_ANSI, S_RTL_EQUAL_STRING,
    S_RTL_SPRINTF, S_SHA_INIT, S_SHA_UPDATE, S_SHA_FINAL, S_HMAC, S_RC4_KEY, S_RC4_CRYPT, S_EX_QUERY_NV,
    S_KE_DELAY, S_NT_USER_APC, S_NT_YIELD, S_KE_INIT_SEMAPHORE, S_KE_RELEASE_SEMAPHORE, S_KE_SYSTEM_TIME,
    S_KE_TICK_COUNT, S_HW_INFO, S_NT_RESUME, S_KE_RAISE_TO_DPC, S_KF_LOWER_IRQL, S_NT_DELETE_FILE, S_OB_REF_BY_HANDLE,
    S_OBF_DEREF, S_OBF_REF, S_RTL_VSPRINTF,
};

static uint8_t *g_image; // the XBE's bytes
static uint32_t g_image_bytes;

static void build_xbe(void) {
    g_image_bytes = 0x3000u; // headers page, .text page, .data page
    g_image = calloc(1, g_image_bytes);
    uint8_t *h = g_image;
    memcpy(h, "XBEH", 4);
    wl_wr_le32(h + 0x104, BASE);
    wl_wr_le32(h + 0x108, 0x1000);       // headers
    wl_wr_le32(h + 0x10C, IMAGE_SIZE);
    wl_wr_le32(h + 0x110, 0x178);
    wl_wr_le32(h + 0x118, BASE + 0x200); // certificate
    wl_wr_le32(h + 0x11C, 2);            // sections
    wl_wr_le32(h + 0x120, BASE + 0x400);
    wl_wr_le32(h + 0x128, TEXT_VA ^ WL_XBE_ENTRY_KEY_RETAIL);
    wl_wr_le32(h + 0x130, 0x10000);      // stack commit
    wl_wr_le32(h + 0x158, THUNK_VA ^ WL_XBE_THUNK_KEY_RETAIL);
    wl_wr_le32(h + 0x200 + 0x08, 0x4D530000u); // title id
    const char *title = "WLTEST";
    for (int i = 0; title[i]; i++) {
        h[0x200 + 0x0C + 2 * i] = (uint8_t)title[i];
    }
    // sections: .text (executable) and .data (the thunk table)
    uint8_t *s = h + 0x400;
    wl_wr_le32(s + 0, WL_XBE_SEC_EXECUTABLE | WL_XBE_SEC_PRELOAD);
    wl_wr_le32(s + 4, TEXT_VA);
    wl_wr_le32(s + 8, 0x1000);
    wl_wr_le32(s + 12, 0x1000);
    wl_wr_le32(s + 16, 0x1000);
    wl_wr_le32(s + 20, BASE + 0x600);
    s += 56;
    wl_wr_le32(s + 0, WL_XBE_SEC_WRITABLE | WL_XBE_SEC_PRELOAD);
    wl_wr_le32(s + 4, DATA_VA);
    wl_wr_le32(s + 8, 0x3000); // vsize: the scratch page and more as BSS
    wl_wr_le32(s + 12, 0x2000);
    wl_wr_le32(s + 16, 0x1000);
    wl_wr_le32(s + 20, BASE + 0x608);
    memcpy(h + 0x600, ".text", 6);
    memcpy(h + 0x608, ".data", 6);
    memset(g_image + 0x1000, 0xCC, 0x1000); // int3: the "code" (never run; C stands in)
    for (size_t i = 0; i < WL_ARRAY_COUNT(k_ordinals); i++) {
        wl_wr_le32(g_image + 0x2000 + 4 * i, 0x80000000u | k_ordinals[i]);
    }
}

static WlResult read_mem(void *user, uint32_t offset, void *dst, uint32_t n) {
    (void)user;
    if ((uint64_t)offset + n > g_image_bytes) {
        return WL_ERR_RANGE;
    }
    memcpy(dst, g_image + offset, n);
    return WL_OK;
}

// ---- Calling imports as translated code does -----------------------------------------------------

static uint32_t call_import(uint32_t slot, uint32_t argc, const uint32_t *args) {
    for (uint32_t i = argc; i-- > 0;) {
        g_esp -= 4;
        wl_xb_wr32(g_esp, args[i]);
    }
    g_esp -= 4;
    wl_xb_wr32(g_esp, TEXT_VA + 0x10u); // the return address
    recomp_func_t fn = wl_xb_lookup(wl_xb_rd32(THUNK_VA + 4u * slot));
    if (fn == NULL) {
        return 0xDEADDEADu;
    }
    fn();
    return g_eax;
}

#define K0(slot) call_import((slot), 0, NULL)
#define KN(slot, ...)                                                                                                 \
    call_import((slot), (uint32_t)(sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t)), (uint32_t[]){__VA_ARGS__})

// Bytes of guest scratch memory, handed out in order (reset per test).
static uint32_t g_scratch;
static uint32_t gmem(uint32_t bytes) {
    uint32_t va = (g_scratch + 15u) & ~15u;
    g_scratch = va + bytes;
    wl_xb_fill(va, 0, bytes);
    return va;
}

static uint32_t gstr(const char *s) {
    uint32_t va = gmem((uint32_t)strlen(s) + 1u);
    wl_xb_copy_in(va, s, (uint32_t)strlen(s) + 1u);
    return va;
}

// An OBJECT_ATTRIBUTES naming `path`.
static uint32_t gattr(const char *path) {
    uint32_t str = gmem(8), oa = gmem(12);
    uint32_t buf = gstr(path);
    wl_xb_wr16(str, (uint16_t)strlen(path));
    wl_xb_wr16(str + 2, (uint16_t)(strlen(path) + 1));
    wl_xb_wr32(str + 4, buf);
    wl_xb_wr32(oa + 4, str);
    return oa;
}

// ---- Guest functions written in C -----------------------------------------------------------------

#define MAX_GUEST_FNS 16u
static recomp_func_t g_guest_fns[MAX_GUEST_FNS];

static uint32_t guest_fn(uint32_t index, recomp_func_t fn) {
    g_guest_fns[index] = fn;
    wl_xb_lookup_flush(); // the manual table changed: the lookup's cache may hold the old function
    return TEXT_VA + 0x100u + index * 16u;
}

static recomp_func_t manual(uint32_t va) {
    if (va >= TEXT_VA + 0x100u && va < TEXT_VA + 0x100u + MAX_GUEST_FNS * 16u && ((va - TEXT_VA) & 15u) == 0) {
        return g_guest_fns[(va - TEXT_VA - 0x100u) / 16u];
    }
    return NULL;
}

// a stdcall return: pop the return address and `arg_bytes`
static void guest_ret(uint32_t arg_bytes) {
    g_esp += 4u + arg_bytes;
}

static uint32_t guest_arg(uint32_t n) {
    return wl_xb_rd32(g_esp + 4u + 4u * n);
}

// ---- Set-up ----------------------------------------------------------------------------------------

static char g_root[256];
static bool g_ready;

static void make_file(const char *rel, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

// The tests' clock: the guest's (the host's, or the deterministic mode's virtual one), and a pause
// on it (the virtual clock moves on by it at once, then a safe point looks at what came due).
static uint64_t now_ms(void) {
    return wl_xb_time_us() / 1000u;
}

static void pause_ms(uint32_t ms) {
    if (wl_xb_deterministic()) {
        wl_xb_time_spend_us(ms * 1000u);
        wl_xb_yield_point();
    } else {
        wl_platform_thread_sleep_ms(ms);
    }
}

static void boot(void) {
    if (g_ready) {
        return;
    }
    snprintf(g_root, sizeof g_root, "/tmp/wl_xk_test_%d", (int)getpid());
    char path[512];
    mkdir(g_root, 0777);
    snprintf(path, sizeof path, "%s/title", g_root);
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/title/maps", g_root);
    mkdir(path, 0777);
    make_file("title/maps/ui.map", "UIMAP-CONTENTS-0123456789");
    make_file("title/maps/b30.map", "B30");
    make_file("title/readme.txt", "hello");
    wl_host_fs_set_root(WL_DEVICE_SD, g_root);
    wl_platform_fs_mount(WL_DEVICE_SD);

    WlXbMemConfig mc;
    wl_xb_mem_config_default(&mc);
    if (wl_xb_mem_init(&mc) != WL_OK || wl_xb_fault_install() != WL_OK) {
        return;
    }
    WlXbThread *t = NULL;
    WlXbThreadConfig tc;
    wl_xb_thread_config_default(&tc);
#if defined(WL_XK_TEST_DETERMINISTIC)
    tc.deterministic = true; // the same tests on the virtual clock (thread.h)
#endif
    if (wl_xb_threads_init(&tc, &t) != WL_OK || wl_xb_thread_setup_guest(t, 0x20000u, 0, 0, 0) != WL_OK) {
        return;
    }
    build_xbe();
    WlXbe xbe;
    if (wl_xbe_parse(g_image, 0x1000, &xbe) != WL_OK || wl_xbe_load(&xbe, read_mem, NULL) != WL_OK) {
        return;
    }
    WlXkConfig kc;
    wl_xk_config_default(&kc);
    kc.title_dir = "sd:/title";
    kc.data_dir = "sd:/hdd";
    kc.title_id = xbe.title_id;
    if (wl_xk_init(&kc) != WL_OK) {
        return;
    }
    uint32_t n = 0;
    if (wl_xbe_resolve_thunks(&xbe, wl_xk_resolve, NULL, NULL, 0, false, &n) != WL_OK || n != WL_ARRAY_COUNT(k_ordinals)) {
        return;
    }
    wl_xb_set_manual_lookup(manual);
    WlXbServiceConfig sc;
    wl_xb_service_config_default(&sc);
    sc.vblank = false;
    if (wl_xb_service_start(&sc) != WL_OK) {
        return;
    }
    g_ready = true;
}

static void reset_scratch(void) {
    g_scratch = SCRATCH_VA;
}

// ---- Thunk ABI -----------------------------------------------------------------------------------

TEST thunk_abi(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    ASSERT(wl_xb_rd32(THUNK_VA) >= WL_XB_THUNK_BASE);           // a function slot: synthetic code address
    ASSERT(wl_xb_rd32(THUNK_VA + 4u * S_KE_TICK_COUNT) < WL_XB_THUNK_BASE); // a data slot: a variable
    uint32_t esp = g_esp;
    g_edx = 0x12345678u;
    ASSERT_EQ_FMT(3375000u, K0(S_PERF_FREQ), "%u"); // edx:eax
    ASSERT_EQ_FMT(0u, g_edx, "%u");
    ASSERT_EQ_FMT(esp, g_esp, "%08x"); // no arguments, the return address popped
    uint32_t a = gstr("abcdef"), b = gstr("abcxyz");
    ASSERT_EQ_FMT(3u, KN(S_COMPARE_MEMORY, a, b, 6), "%u");
    ASSERT_EQ_FMT(esp, g_esp, "%08x"); // stdcall: 12 bytes popped
    uint32_t v = gmem(4);
    wl_xb_wr32(v, 41);
    g_ecx = v;
    ASSERT_EQ_FMT(42u, K0(S_INTERLOCKED_INC), "%u"); // fastcall: the address in ecx, nothing on the stack
    ASSERT_EQ_FMT(esp, g_esp, "%08x");
    g_ecx = v;
    g_edx = 7;
    ASSERT_EQ_FMT(42u, KN(S_INTERLOCKED_CMPXCHG, 42), "%u"); // fastcall with its comparand on the stack
    ASSERT_EQ_FMT(7u, wl_xb_rd32(v), "%u");
    ASSERT_EQ_FMT(esp, g_esp, "%08x");
    uint32_t fmt = gstr("DbgPrint from the test: %d %s\n");
    KN(S_DBGPRINT, fmt, 42, gstr("ok"));
    ASSERT_EQ_FMT(esp - 12u, g_esp, "%08x"); // cdecl: the caller pops its three arguments
    g_esp = esp;
    // KeTickCount moves in guest memory
    uint32_t tick_va = wl_xb_rd32(THUNK_VA + 4u * S_KE_TICK_COUNT);
    uint32_t t0 = wl_xb_rd32(tick_va);
    pause_ms(20);
    ASSERT(wl_xb_rd32(tick_va) > t0);
    // XboxHardwareInfo: the NV2A revision
    ASSERT_EQ(0xA1, wl_xb_rd8(wl_xb_rd32(THUNK_VA + 4u * S_HW_INFO) + 4u));
    PASS();
}

// ---- Dispatcher objects and threads under contention ------------------------------------------

static uint32_t g_cs, g_counter_va, g_go_event, g_done_sem;

static void worker_routine(void) {
    // stdcall SystemRoutine(ctx1 = iterations, ctx2 = unused)
    uint32_t iters = guest_arg(0);
    uint32_t esp = g_esp;
    uint32_t go = g_go_event;
    KN(S_KE_WAIT1, go, 0, 0, 0, 0);
    for (uint32_t i = 0; i < iters; i++) {
        KN(S_RTL_ENTER_CS, g_cs);
        uint32_t x = wl_xb_rd32(g_counter_va);
        K0(S_NT_YIELD); // others run inside the critical section's window: they must wait
        wl_xb_wr32(g_counter_va, x + 1u);
        KN(S_RTL_LEAVE_CS, g_cs);
    }
    KN(S_KE_RELEASE_SEMAPHORE, g_done_sem, 0, 1, 0);
    g_esp = esp;
    guest_ret(8);
}

TEST threads_and_critical_sections(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    g_cs = gmem(0x20);
    g_counter_va = gmem(4);
    g_go_event = gmem(16);
    g_done_sem = gmem(20);
    KN(S_RTL_INIT_CS, g_cs);
    KN(S_KE_INIT_EVENT, g_go_event, 0, 0);       // notification, not signalled
    KN(S_KE_INIT_SEMAPHORE, g_done_sem, 0, 100); // count 0
    uint32_t routine = guest_fn(0, worker_routine);
    enum { WORKERS = 4, ITERS = 200 };
    uint32_t handles[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        uint32_t hp = gmem(4);
        ASSERT_EQ_FMT(0u, KN(S_PS_CREATE, hp, 0, 0x4000, 0, 0, ITERS, 0, 0, 0, routine), "%08x");
        handles[i] = wl_xb_rd32(hp);
        ASSERT(handles[i] != 0);
    }
    KN(S_KE_SET_EVENT, g_go_event, 0, 0);
    for (int i = 0; i < WORKERS; i++) {
        int64_t timeout = -50000000; // 5 s
        uint32_t tp = gmem(8);
        wl_xb_wr64(tp, (uint64_t)timeout);
        ASSERT_EQ_FMT(0u, KN(S_KE_WAIT1, g_done_sem, 0, 0, 0, tp), "%08x");
    }
    ASSERT_EQ_FMT((uint32_t)(WORKERS * ITERS), wl_xb_rd32(g_counter_va), "%u");
    // every worker's thread object is signalled once it has ended
    uint32_t hs = gmem(4 * WORKERS);
    for (int i = 0; i < WORKERS; i++) {
        wl_xb_wr32(hs + 4u * (uint32_t)i, handles[i]);
    }
    uint32_t tp = gmem(8);
    wl_xb_wr64(tp, (uint64_t)(int64_t)-50000000);
    ASSERT_EQ_FMT(0u, KN(S_NT_WAITN, WORKERS, hs, 0, 0, 0, tp), "%08x"); // WaitAll
    for (int i = 0; i < WORKERS; i++) {
        KN(S_NT_CLOSE, handles[i]);
    }
    PASS();
}

TEST events_timeouts_mutants(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t hp = gmem(4), tp = gmem(8);
    ASSERT_EQ_FMT(0u, KN(S_NT_CREATE_EVENT, hp, 0, 1, 0), "%08x"); // synchronization, not signalled
    uint32_t ev = wl_xb_rd32(hp);
    wl_xb_wr64(tp, (uint64_t)(int64_t)-200000); // 20 ms
    uint64_t t0 = now_ms();
    ASSERT_EQ_FMT(0x102u, KN(S_NT_WAIT1, ev, 0, tp), "%08x"); // STATUS_TIMEOUT
    ASSERT(now_ms() - t0 >= 15u);
    KN(S_NT_SET_EVENT, ev, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_WAIT1, ev, 0, tp), "%08x");
    ASSERT_EQ_FMT(0x102u, KN(S_NT_WAIT1, ev, 0, tp), "%08x"); // auto-reset by the satisfied wait
    // wait-any returns the index of the signalled one
    uint32_t hp2 = gmem(4);
    KN(S_NT_CREATE_EVENT, hp2, 0, 0, 1); // notification, signalled
    uint32_t arr = gmem(8);
    wl_xb_wr32(arr, ev);
    wl_xb_wr32(arr + 4, wl_xb_rd32(hp2));
    ASSERT_EQ_FMT(1u, KN(S_NT_WAITN, 2, arr, 1, 0, 0, tp), "%08x");
    // a mutant is recursive for its owner and refuses a release by anyone else
    uint32_t mp = gmem(4);
    ASSERT_EQ_FMT(0u, KN(S_NT_CREATE_MUTANT, mp, 0, 1), "%08x");
    uint32_t m = wl_xb_rd32(mp);
    ASSERT_EQ_FMT(0u, KN(S_NT_WAIT1, m, 0, tp), "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_RELEASE_MUTANT, m, 0), "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_RELEASE_MUTANT, m, 0), "%08x");
    ASSERT_EQ_FMT(0xC0000046u, KN(S_NT_RELEASE_MUTANT, m, 0), "%08x"); // not owned any more
    KN(S_NT_CLOSE, ev);
    KN(S_NT_CLOSE, wl_xb_rd32(hp2));
    KN(S_NT_CLOSE, m);
    ASSERT_EQ_FMT(0xC0000008u, KN(S_NT_CLOSE, m), "%08x"); // a closed handle
    // a delay sleeps about as long as asked
    wl_xb_wr64(tp, (uint64_t)(int64_t)-300000);
    t0 = now_ms();
    ASSERT_EQ_FMT(0u, KN(S_KE_DELAY, 0, 0, tp), "%08x");
    ASSERT(now_ms() - t0 >= 25u);
    PASS();
}

// ---- Timers, DPCs, interrupts ----------------------------------------------------------------------

static uint32_t g_dpc_runs, g_dpc_arg, g_isr_runs, g_isr_dpc;

static void dpc_routine(void) {
    // VOID DeferredRoutine(PKDPC, PVOID DeferredContext, PVOID Arg1, PVOID Arg2)
    g_dpc_runs++;
    g_dpc_arg = guest_arg(1);
    guest_ret(16);
}

static void isr_routine(void) {
    // BOOLEAN ServiceRoutine(PKINTERRUPT, PVOID ServiceContext): acknowledge and queue the DPC
    g_isr_runs++;
    wl_xb_irq_deassert(3);
    uint32_t esp = g_esp;
    KN(S_KE_INSERT_DPC, g_isr_dpc, 0, 0);
    g_esp = esp;
    g_eax = 1;
    guest_ret(8);
}

TEST timers_dpcs_interrupts(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t timer = gmem(0x28), dpc = gmem(0x20), tp = gmem(8);
    uint32_t dpc_fn = guest_fn(1, dpc_routine);
    g_dpc_runs = 0;
    KN(S_KE_INIT_TIMER, timer, 0);
    KN(S_KE_INIT_DPC, dpc, dpc_fn, 0xC0DEu);
    KN(S_KE_SET_TIMER, timer, (uint32_t)-100000, 0xFFFFFFFFu, dpc); // 10 ms
    wl_xb_wr64(tp, (uint64_t)(int64_t)-20000000);
    ASSERT_EQ_FMT(0u, KN(S_KE_WAIT1, timer, 0, 0, 0, tp), "%08x");
    // the DPC runs from the service once the timer fired: give it a kernel call or two
    for (int i = 0; i < 50 && g_dpc_runs == 0; i++) {
        wl_xb_wr64(tp, (uint64_t)(int64_t)-10000);
        KN(S_KE_DELAY, 0, 0, tp);
    }
    ASSERT_EQ_FMT(1u, g_dpc_runs, "%u");
    ASSERT_EQ_FMT(0xC0DEu, g_dpc_arg, "%08x");
    // an interrupt line with its ISR, which queues a DPC
    uint32_t irql = gmem(4), ki = gmem(0x80);
    g_isr_dpc = gmem(0x20);
    g_isr_runs = 0;
    g_dpc_runs = 0;
    KN(S_KE_INIT_DPC, g_isr_dpc, dpc_fn, 0x15Au);
    uint32_t vector = KN(S_HAL_VECTOR, 3, irql);
    ASSERT_EQ_FMT(0x33u, vector, "%x");
    KN(S_KE_INIT_INTERRUPT, ki, guest_fn(2, isr_routine), 0x1234, vector, wl_xb_rd8(irql), 0, 0);
    ASSERT_EQ_FMT(1u, KN(S_KE_CONNECT_INTERRUPT, ki), "%u");
    wl_xb_irq_assert(3);
    for (int i = 0; i < 50 && g_dpc_runs == 0; i++) {
        wl_xb_wr64(tp, (uint64_t)(int64_t)-10000);
        KN(S_KE_DELAY, 0, 0, tp);
    }
    ASSERT_EQ_FMT(1u, g_isr_runs, "%u");
    ASSERT_EQ_FMT(1u, g_dpc_runs, "%u");
    ASSERT_EQ_FMT(0x15Au, g_dpc_arg, "%08x");
    PASS();
}

// ---- A thread that spins on a device register ---------------------------------------------------
// D3D's fence wait: a loop that reads a GPU register until the GPU's ISR moves a word in memory,
// with no kernel call in it. The device access itself must be the safe point: the ISR runs there
// (or on the service thread, which is handed the lock there), and a thread that outranks the
// spinning one gets the lock there too.

#define SPIN_DEV_VA 0xFEF00000u
#define SPIN_LINE 9u

static volatile uint32_t g_spin_reads, g_spin_raise_at;
static volatile bool g_spin_irq;
static uint32_t g_spin_flag, g_spin_isr_runs, g_hi_flag, g_hi_tp;

static uint32_t spin_dev_read(void *user, uint32_t off, unsigned size) {
    (void)user;
    (void)size;
    g_spin_reads++;
    if (g_spin_raise_at != 0 && g_spin_reads == g_spin_raise_at) {
        g_spin_irq = true; // the device raises its line (a level source) in the middle of the loop
    }
    return off == 0x44u ? g_spin_reads : 0u;
}

static void spin_dev_write(void *user, uint32_t off, uint32_t v, unsigned size) {
    (void)user;
    (void)size;
    if (off == 0x100u && (v & 1u) != 0) {
        g_spin_irq = false; // acknowledged
    }
}

static bool spin_level(void *user) {
    (void)user;
    return g_spin_irq;
}

static void spin_isr(void) {
    // BOOLEAN ServiceRoutine(PKINTERRUPT, PVOID): acknowledge at the device, move the fence
    g_spin_isr_runs++;
    wl_xb_wr32(SPIN_DEV_VA + 0x100u, 1); // a device access inside the ISR: no safe point there
    wl_xb_wr32(g_spin_flag, wl_xb_rd32(g_spin_flag) + 1u);
    g_eax = 1;
    guest_ret(8);
}

static void hi_routine(void) {
    // stdcall SystemRoutine(ctx1, ctx2): go above the spinning thread, sleep, then mark the flag;
    // waking, it waits for the lock, which the spinning thread holds
    uint32_t esp = g_esp;
    wl_xb_thread_set_priority(wl_xb_current(), 20);
    KN(S_KE_DELAY, 0, 0, g_hi_tp);
    wl_xb_wr32(g_hi_flag, 1);
    g_esp = esp;
    guest_ret(8);
}

TEST device_polls_are_safe_points(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    WlXbDevice dev = {"spin", SPIN_DEV_VA, 0x1000u, spin_dev_read, spin_dev_write, NULL};
    ASSERT_EQ(WL_OK, wl_xb_mmio_register(&dev));
    wl_xb_irq_set_level(SPIN_LINE, spin_level, NULL);
    uint32_t irql = gmem(4), ki = gmem(0x80);
    g_spin_flag = gmem(4);
    uint32_t vector = KN(S_HAL_VECTOR, SPIN_LINE, irql);
    KN(S_KE_INIT_INTERRUPT, ki, guest_fn(3, spin_isr), 0, vector, wl_xb_rd8(irql), 0 /* level sensitive */, 0);
    ASSERT_EQ_FMT(1u, KN(S_KE_CONNECT_INTERRUPT, ki), "%u");

    // the fence wait: nothing but device reads until the ISR has moved the word
    g_spin_isr_runs = 0;
    g_spin_reads = 0;
    g_spin_raise_at = 100;
    uint64_t t0 = now_ms();
    while (wl_xb_rd32(g_spin_flag) == 0 && now_ms() - t0 < 3000u) {
        (void)wl_xb_rd32(SPIN_DEV_VA + 0x44u);
    }
    ASSERT_EQ_FMT(1u, wl_xb_rd32(g_spin_flag), "%u");
    ASSERT_EQ_FMT(1u, g_spin_isr_runs, "%u");
    ASSERT(!g_spin_irq);
    ASSERT(g_spin_reads >= 100u);

    // a thread that outranks the spinning one wakes from a delay: the spinning thread hands it the
    // lock at a device access (and first lets it start, its quantum over)
    g_hi_flag = gmem(4);
    g_hi_tp = gmem(8);
    wl_xb_wr64(g_hi_tp, (uint64_t)(int64_t)-100000); // 10 ms
    uint32_t hp = gmem(4);
    WlXbThreadStats ts0, ts1;
    wl_xb_thread_stats(&ts0);
    ASSERT_EQ_FMT(0u, KN(S_PS_CREATE, hp, 0, 0x4000, 0, 0, 0, 0, 0, 0, guest_fn(4, hi_routine)), "%08x");
    t0 = now_ms();
    while (wl_xb_rd32(g_hi_flag) == 0 && now_ms() - t0 < 3000u) {
        (void)wl_xb_rd32(SPIN_DEV_VA + 0x44u);
    }
    ASSERT_EQ_FMT(1u, wl_xb_rd32(g_hi_flag), "%u");
    wl_xb_thread_stats(&ts1);
    ASSERT(ts1.safe_yields > ts0.safe_yields);
    KN(S_NT_CLOSE, wl_xb_rd32(hp));
    PASS();
}

// A device's periodic work (wl_xb_service_add_tick): it runs at the holder's safe points, so a
// thread that waits on a word only the device's work changes gets past its wait (DirectSound
// waiting for its DSP command word to be taken).
static volatile uint32_t g_tick_runs;
static uint32_t g_tick_word;

static void tick_fn(void *user) {
    (void)user;
    g_tick_runs++;
    if (g_tick_word != 0) {
        wl_xb_wr32(g_tick_word, 0);
    }
}

TEST device_ticks_run_at_safe_points(void) {
    boot();
    ASSERT(g_ready);
    g_spin_raise_at = 0;
    WlXbDevice dev = {"spin", SPIN_DEV_VA, 0x1000u, spin_dev_read, spin_dev_write, NULL};
    ASSERT_EQ(WL_OK, wl_xb_mmio_register(&dev));
    g_tick_word = gmem(4);
    wl_xb_wr32(g_tick_word, 7);
    ASSERT_EQ(WL_OK, wl_xb_service_add_tick(tick_fn, NULL, 2000));
    uint64_t t0 = now_ms();
    while (wl_xb_rd32(g_tick_word) != 0 && now_ms() - t0 < 3000u) {
        (void)wl_xb_rd32(SPIN_DEV_VA + 0x44u);
    }
    ASSERT_EQ_FMT(0u, wl_xb_rd32(g_tick_word), "%u");
    g_tick_word = 0; // the tick stays: it must not write into the next tests' scratch memory
    const uint32_t before = g_tick_runs;
    t0 = now_ms();
    while (now_ms() - t0 < 60u) {
        (void)wl_xb_rd32(SPIN_DEV_VA + 0x44u);
    }
    ASSERT(g_tick_runs - before >= 5u); // about 30 at 2 ms
    ASSERT_EQ(WL_ERR_ARG, wl_xb_service_add_tick(NULL, NULL, 2000));
    PASS();
}

// ---- A raised IRQL keeps the processor -------------------------------------------------------------
// DirectSound edits its voice lists at DISPATCH_LEVEL (KeRaiseIrqlToDpcLevel) with no other lock,
// and polls the APU's registers meanwhile. On the console nothing but interrupts runs then: the
// device accesses run the ISR, but its DPC and a thread that would preempt the holder wait until
// the IRQL comes down. (Given the lock at a device access, the service thread ran the DPC at its
// own IRQL, and a ready thread ran too: Halo's DirectSound then lost a stopped voice for good.)

#define RAISED_DEV_VA 0xFEF01000u
#define RAISED_LINE 10u

static volatile uint32_t g_raised_reads;
static volatile bool g_raised_irq;
static uint32_t g_raised_dpc, g_raised_isr_runs;

static uint32_t raised_dev_read(void *user, uint32_t off, unsigned size) {
    (void)user;
    (void)off;
    (void)size;
    if (++g_raised_reads == 50u) {
        g_raised_irq = true; // the device raises its line in the middle of the raised section
    }
    return g_raised_reads;
}

static void raised_dev_write(void *user, uint32_t off, uint32_t v, unsigned size) {
    (void)user;
    (void)off;
    (void)v;
    (void)size;
    g_raised_irq = false; // acknowledged
}

static bool raised_level(void *user) {
    (void)user;
    return g_raised_irq;
}

static void raised_isr(void) {
    // BOOLEAN ServiceRoutine(PKINTERRUPT, PVOID): acknowledge at the device, queue the DPC
    g_raised_isr_runs++;
    wl_xb_wr32(RAISED_DEV_VA, 1);
    uint32_t esp = g_esp;
    KN(S_KE_INSERT_DPC, g_raised_dpc, 0, 0);
    g_esp = esp;
    g_eax = 1;
    guest_ret(8);
}

TEST a_raised_irql_keeps_the_processor(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    WlXbDevice dev = {"raised", RAISED_DEV_VA, 0x1000u, raised_dev_read, raised_dev_write, NULL};
    ASSERT_EQ(WL_OK, wl_xb_mmio_register(&dev));
    wl_xb_irq_set_level(RAISED_LINE, raised_level, NULL);
    uint32_t irql = gmem(4), ki = gmem(0x80);
    g_raised_dpc = gmem(0x20);
    KN(S_KE_INIT_DPC, g_raised_dpc, guest_fn(5, dpc_routine), 0xD15Cu);
    uint32_t vector = KN(S_HAL_VECTOR, RAISED_LINE, irql);
    ASSERT(wl_xb_rd8(irql) > 2u); // a device's IRQL: above DISPATCH_LEVEL
    KN(S_KE_INIT_INTERRUPT, ki, guest_fn(6, raised_isr), 0, vector, wl_xb_rd8(irql), 0 /* level */, 0);
    ASSERT_EQ_FMT(1u, KN(S_KE_CONNECT_INTERRUPT, ki), "%u");
    g_dpc_runs = 0;
    g_raised_isr_runs = 0;
    g_raised_reads = 0;
    g_hi_flag = gmem(4);
    g_hi_tp = gmem(8);
    wl_xb_wr64(g_hi_tp, (uint64_t)(int64_t)-100000); // 10 ms
    WlXbThreadStats ts0, ts1;
    wl_xb_thread_stats(&ts0);

    ASSERT_EQ_FMT(0u, K0(S_KE_RAISE_TO_DPC), "%u"); // the old IRQL
    // a thread made now is ready at once (with the holder's priority; it raises its own, sleeps
    // and marks the flag): not while the holder stays at DISPATCH_LEVEL
    uint32_t hp = gmem(4);
    ASSERT_EQ_FMT(0u, KN(S_PS_CREATE, hp, 0, 0x4000, 0, 0, 0, 0, 0, 0, guest_fn(4, hi_routine)), "%08x");
    uint64_t t0 = now_ms();
    while (now_ms() - t0 < 60u) { // the quantum (10 ms) over many times, the thread's sleep too
        (void)wl_xb_rd32(RAISED_DEV_VA + 4u);
    }
    ASSERT_EQ_FMT(1u, g_raised_isr_runs, "%u"); // the interrupt came in at a device access
    ASSERT(!g_raised_irq);
    ASSERT_EQ_FMT(0u, g_dpc_runs, "%u"); // its DPC waits for the IRQL
    ASSERT_EQ_FMT(0u, wl_xb_rd32(g_hi_flag), "%u"); // no other thread ran
    wl_xb_thread_stats(&ts1);
    ASSERT(ts1.raised_holds > ts0.raised_holds);

    // KfLowerIrql runs the DPC at once; the other thread gets the lock at a later safe point
    g_ecx = 0;
    K0(S_KF_LOWER_IRQL);
    ASSERT_EQ_FMT(1u, g_dpc_runs, "%u");
    ASSERT_EQ_FMT(0xD15Cu, g_dpc_arg, "%08x");
    t0 = now_ms();
    while (wl_xb_rd32(g_hi_flag) == 0 && now_ms() - t0 < 3000u) {
        (void)wl_xb_rd32(RAISED_DEV_VA + 4u);
    }
    ASSERT_EQ_FMT(1u, wl_xb_rd32(g_hi_flag), "%u");
    KN(S_NT_CLOSE, wl_xb_rd32(hp));
    PASS();
}

// ---- Files ----------------------------------------------------------------------------------------

static uint32_t g_apc_runs, g_apc_ctx, g_apc_info;

static void apc_routine(void) {
    // VOID ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG Reserved)
    g_apc_runs++;
    g_apc_ctx = guest_arg(0);
    g_apc_info = wl_xb_rd32(guest_arg(1) + 4u);
    guest_ret(12);
}

static uint32_t open_path(const char *path, uint32_t access, uint32_t disposition, uint32_t options, uint32_t *status) {
    uint32_t hp = gmem(4), iosb = gmem(8);
    *status = KN(S_NT_CREATE_FILE, hp, access, gattr(path), iosb, 0, 0x80, 0, disposition, options);
    return wl_xb_rd32(hp);
}

TEST files_dvd_and_overlapped(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0;
    // D: is the title's folder, read-only; a name in another case is found
    uint32_t h = open_path("\\??\\D:\\MAPS\\UI.map", 0x80000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t buf = gmem(64), iosb = gmem(8);
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, h, 0, 0, 0, iosb, buf, 5, 0), "%08x");
    char text[16] = {0};
    wl_xb_copy_out(text, buf, 5);
    ASSERT_STR_EQ("UIMAP", text);
    ASSERT_EQ_FMT(5u, wl_xb_rd32(iosb + 4), "%u");
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, h, 0, 0, 0, iosb, buf, 4, 0), "%08x"); // goes on from the position
    wl_xb_copy_out(text, buf, 4);
    text[4] = 0;
    ASSERT_STR_EQ("-CON", text);
    uint32_t info = gmem(24);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_INFO, h, iosb, info, 24, 5), "%08x"); // FileStandardInformation
    ASSERT_EQ_FMT(25u, wl_xb_rd32(info + 8), "%u");
    KN(S_NT_CLOSE, h);
    open_path("D:\\maps\\new.map", 0x40000000u, 2, 0x20, &st);
    ASSERT(st != 0); // nothing is created on the DVD
    open_path("d:\\nothere\\x.bin", 0x80000000u, 1, 0, &st);
    ASSERT_EQ_FMT(0xC000003Au, st, "%08x"); // STATUS_OBJECT_PATH_NOT_FOUND
    open_path("d:\\maps\\x.bin", 0x80000000u, 1, 0, &st);
    ASSERT_EQ_FMT(0xC0000034u, st, "%08x"); // STATUS_OBJECT_NAME_NOT_FOUND
    // an overlapped read with an event and an APC: the APC runs at the next alertable wait
    h = open_path("D:\\maps\\ui.map", 0x80000000u, 1, 0, &st); // no FILE_SYNCHRONOUS_IO: overlapped
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t ehp = gmem(4);
    KN(S_NT_CREATE_EVENT, ehp, 0, 0, 0);
    uint32_t ev = wl_xb_rd32(ehp);
    uint32_t off = gmem(8);
    wl_xb_wr64(off, 6);
    g_apc_runs = 0;
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, h, ev, guest_fn(3, apc_routine), 0xA9C0u, iosb, buf, 8, off), "%08x");
    wl_xb_copy_out(text, buf, 8);
    text[8] = 0;
    ASSERT_STR_EQ("CONTENTS", text);
    ASSERT_EQ_FMT(0u, g_apc_runs, "%u"); // not before an alertable wait
    uint32_t tp = gmem(8);
    wl_xb_wr64(tp, (uint64_t)(int64_t)-10000000);
    ASSERT_EQ_FMT(0xC0u, KN(S_NT_WAIT1EX, ev, 1, 1, tp), "%08x"); // STATUS_USER_APC
    ASSERT_EQ_FMT(1u, g_apc_runs, "%u");
    ASSERT_EQ_FMT(0xA9C0u, g_apc_ctx, "%08x");
    ASSERT_EQ_FMT(8u, g_apc_info, "%u");
    ASSERT_EQ_FMT(0u, KN(S_NT_WAIT1EX, ev, 1, 1, tp), "%08x"); // the event was set too
    KN(S_NT_CLOSE, ev);
    KN(S_NT_CLOSE, h);
    PASS();
}

TEST files_sparse_cache_drive(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0, iosb = gmem(8);
    // Z: (Partition5) keeps a logical size beyond what is written
    uint32_t h = open_path("z:\\cache002.map", 0xC0000000u, 3, 0x20, &st); // OPEN_IF
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t eof = gmem(8);
    wl_xb_wr64(eof, 0x02300000u); // 35 MB, as Halo's menu slot
    ASSERT_EQ_FMT(0u, KN(S_NT_SET_INFO, h, iosb, eof, 8, 20), "%08x");
    uint32_t hdr = gstr("head-block");
    uint32_t off = gmem(8);
    wl_xb_wr64(off, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_WRITE, h, 0, 0, 0, iosb, hdr, 10, off), "%08x");
    wl_xb_wr64(off, 0x01000000u);
    uint32_t buf = gmem(32);
    wl_xb_fill(buf, 0xEE, 32);
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, h, 0, 0, 0, iosb, buf, 16, off), "%08x");
    for (uint32_t i = 0; i < 16; i++) {
        ASSERT_EQ(0, wl_xb_rd8(buf + i)); // the sparse middle reads as zeros
    }
    KN(S_NT_CLOSE, h);
    char host[512];
    snprintf(host, sizeof host, "%s/hdd/Z/cache002.map", g_root);
    struct stat sb;
    ASSERT_EQ(0, stat(host, &sb));
    ASSERT(sb.st_size < 4096); // only what was written takes room
    h = open_path("\\Device\\Harddisk0\\Partition5\\CACHE002.MAP", 0x80000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t info = gmem(24);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_INFO, h, iosb, info, 24, 5), "%08x");
    ASSERT_EQ_FMT(0x02300000u, wl_xb_rd32(info + 8), "%08x"); // the logical size survives a reopen
    wl_xb_wr64(off, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, h, 0, 0, 0, iosb, buf, 10, off), "%08x");
    char text[16] = {0};
    wl_xb_copy_out(text, buf, 10);
    ASSERT_STR_EQ("head-block", text);
    wl_xb_wr64(off, 0x02300000u - 4u);
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, h, 0, 0, 0, iosb, buf, 16, off), "%08x");
    ASSERT_EQ_FMT(4u, wl_xb_rd32(iosb + 4), "%u");                           // short at the logical end
    wl_xb_wr64(off, 0x02300000u);
    ASSERT_EQ_FMT(0xC0000011u, KN(S_NT_READ, h, 0, 0, 0, iosb, buf, 16, off), "%08x"); // STATUS_END_OF_FILE
    KN(S_NT_CLOSE, h);
    // a directory listing hides the sidecar and shows the logical size
    h = open_path("Z:\\", 0x80000000u, 1, 0x21, &st); // a directory
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t mask = gmem(8), pat = gstr("cache*.map");
    wl_xb_wr16(mask, 10);
    wl_xb_wr16(mask + 2, 11);
    wl_xb_wr32(mask + 4, pat);
    uint32_t dir = gmem(0x200);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_DIR, h, 0, 0, 0, iosb, dir, 0x200, 1, mask, 1), "%08x");
    char name[32] = {0};
    wl_xb_copy_out(name, dir + 0x40, wl_xb_rd32(dir + 0x3C));
    ASSERT_STR_EQ("cache002.map", name);
    ASSERT_EQ_FMT(0x02300000u, wl_xb_rd32(dir + 0x28), "%08x");
    ASSERT_EQ_FMT(0x80000006u, KN(S_NT_QUERY_DIR, h, 0, 0, 0, iosb, dir, 0x200, 1, 0, 0), "%08x"); // no more
    KN(S_NT_CLOSE, h);
    // full attributes by name; volume size of the cache drive
    uint32_t na = gmem(56);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_ATTRS, gattr("z:\\cache002.map"), na), "%08x");
    ASSERT_EQ_FMT(0x02300000u, wl_xb_rd32(na + 40), "%08x");
    // T: (TDATA) through a link XAPI would make; a file created, renamed, deleted on close
    h = open_path("\\Device\\Harddisk0\\Partition1\\TDATA", 0x40000000u, 3, 0x1, &st); // a directory
    ASSERT_EQ_FMT(0u, st, "%08x");
    KN(S_NT_CLOSE, h);
    h = open_path("\\Device\\Harddisk0\\Partition1\\TDATA\\save.bin", 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_WRITE, h, 0, 0, 0, iosb, hdr, 4, 0), "%08x");
    uint32_t vol = gmem(32);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_VOLUME, h, iosb, vol, 24, 3), "%08x");
    ASSERT(wl_xb_rd32(vol + 8) > 0); // free units
    uint32_t disp = gmem(4);
    wl_xb_wr8(disp, 1);
    ASSERT_EQ_FMT(0u, KN(S_NT_SET_INFO, h, iosb, disp, 1, 13), "%08x"); // delete on close
    KN(S_NT_CLOSE, h);
    snprintf(host, sizeof host, "%s/hdd/E/TDATA/save.bin", g_root);
    ASSERT(stat(host, &sb) != 0);
    PASS();
}

// ---- Memory -------------------------------------------------------------------------------------

TEST virtual_and_contiguous_memory(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t base_p = gmem(4), size_p = gmem(4);
    wl_xb_wr32(base_p, 0);
    wl_xb_wr32(size_p, 0x100000);
    ASSERT_EQ_FMT(0u, KN(S_NT_ALLOC_VM, base_p, 0, size_p, 0x2000, 4), "%08x"); // reserve 1 MB
    uint32_t base = wl_xb_rd32(base_p);
    ASSERT(base != 0 && (base & 0xFFFFu) == 0);
    wl_xb_wr32(size_p, 0x3000);
    wl_xb_wr32(base_p, base + 0x10000);
    ASSERT_EQ_FMT(0u, KN(S_NT_ALLOC_VM, base_p, 0, size_p, 0x1000, 4), "%08x"); // commit 12 KB in it
    wl_xb_wr32(base + 0x10000, 0x55AA55AAu);
    uint32_t mbi = gmem(28);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_VM, base + 0x10000, mbi), "%08x");
    ASSERT_EQ_FMT(base, wl_xb_rd32(mbi + 4), "%08x");   // AllocationBase
    ASSERT_EQ_FMT(0x3000u, wl_xb_rd32(mbi + 12), "%x"); // RegionSize
    ASSERT_EQ_FMT(0x1000u, wl_xb_rd32(mbi + 16), "%x"); // MEM_COMMIT
    uint32_t phys = KN(S_MM_PHYS, base + 0x10004);
    ASSERT_EQ_FMT(0x55AA55AAu, wl_xb_rd32(0x80000000u | (phys - 4u)), "%08x"); // the user page, through the window
    wl_xb_wr32(base_p, base);
    wl_xb_wr32(size_p, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_FREE_VM, base_p, size_p, 0x8000), "%08x"); // MEM_RELEASE
    ASSERT_EQ_FMT(0x100000u, wl_xb_rd32(size_p), "%x");
    // a pinned contiguous allocation, as Halo's game state: lowest = the address, highest = its end
    uint32_t va = KN(S_MM_CONTIG_EX, 0x345000u, 0x61000u, 0x61000u + 0x345000u - 1u, 0, 4);
    ASSERT_EQ_FMT(0x80061000u, va, "%08x");
    ASSERT_EQ_FMT(0x61000u, KN(S_MM_PHYS, va), "%x");
    ASSERT_EQ_FMT(0x61000u, KN(S_MM_PHYS, 0xF0061000u), "%x"); // the tiled alias
    KN(S_MM_FREE_CONTIG, va);
    // the GPU's instance memory: the top of the RAM
    uint32_t pad = gmem(4);
    ASSERT_EQ_FMT(0x84000000u, KN(S_MM_GPU_INSTANCE, 0x5000, pad), "%08x");
    PASS();
}

// ---- Rtl, crypto, settings ---------------------------------------------------------------------

TEST rtl_crypto_settings(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    // time fields round trip: 2001-10-12 23:16:23.500, a Friday
    uint32_t tf = gmem(16), tm = gmem(8);
    uint16_t fields[8] = {2001, 10, 12, 23, 16, 23, 500, 0};
    for (int i = 0; i < 8; i++) {
        wl_xb_wr16(tf + 2u * (uint32_t)i, fields[i]);
    }
    ASSERT_EQ_FMT(1u, KN(S_RTL_FIELDS_TO_TIME, tf, tm), "%u");
    uint32_t tf2 = gmem(16);
    KN(S_RTL_TIME_TO_FIELDS, tm, tf2);
    for (int i = 0; i < 7; i++) {
        ASSERT_EQ(fields[i], wl_xb_rd16(tf2 + 2u * (uint32_t)i));
    }
    ASSERT_EQ(5, wl_xb_rd16(tf2 + 14)); // weekday
    // strings
    uint32_t s1 = gmem(8), s2 = gmem(8);
    KN(S_RTL_INIT_ANSI, s1, gstr("Halo"));
    KN(S_RTL_INIT_ANSI, s2, gstr("HALO"));
    ASSERT_EQ(4, wl_xb_rd16(s1));
    ASSERT_EQ_FMT(0u, KN(S_RTL_EQUAL_STRING, s1, s2, 0), "%u");
    ASSERT_EQ_FMT(1u, KN(S_RTL_EQUAL_STRING, s1, s2, 1), "%u");
    // printf over guest varargs (cdecl: the caller pops)
    uint32_t out = gmem(64), esp = g_esp;
    uint32_t n = KN(S_RTL_SPRINTF, out, gstr("%s %05d %x %c|%-4s|"), gstr("v"), 42, 0xBEEF, 'Z', gstr("ab"));
    g_esp = esp;
    char text[64];
    wl_xb_read_cstr(out, text, sizeof text);
    ASSERT_STR_EQ("v 00042 beef Z|ab  |", text);
    ASSERT_EQ_FMT(20u, n, "%u");
    // SHA-1("abc") = a9993e36...
    uint32_t ctx = gmem(116), digest = gmem(20), abc = gstr("abc");
    KN(S_SHA_INIT, ctx);
    KN(S_SHA_UPDATE, ctx, abc, 3);
    KN(S_SHA_FINAL, ctx, digest);
    uint8_t d[20];
    wl_xb_copy_out(d, digest, 20);
    static const uint8_t sha_abc[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    ASSERT_MEM_EQ(sha_abc, d, 20);
    // HMAC-SHA1, RFC 2202 case 2: key "Jefe", "what do ya want for nothing?" in two pieces
    uint32_t key = gstr("Jefe"), m1 = gstr("what do ya want "), m2 = gstr("for nothing?");
    KN(S_HMAC, key, 4, m1, 16, m2, 12, digest);
    wl_xb_copy_out(d, digest, 20);
    static const uint8_t hmac2[20] = {0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
                                      0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79};
    ASSERT_MEM_EQ(hmac2, d, 20);
    // RC4 key "Key" over "Plaintext" = BBF316E8D940AF0AD3
    uint32_t rc4 = gmem(260), rk = gstr("Key"), pt = gstr("Plaintext");
    KN(S_RC4_KEY, rc4, 3, rk);
    KN(S_RC4_CRYPT, rc4, 9, pt);
    uint8_t c[9];
    wl_xb_copy_out(c, pt, 9);
    static const uint8_t rc4_ct[9] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
    ASSERT_MEM_EQ(rc4_ct, c, 9);
    // the EEPROM: English, and no parental limit
    uint32_t type = gmem(4), value = gmem(4), rlen = gmem(4);
    ASSERT_EQ_FMT(0u, KN(S_EX_QUERY_NV, 7, type, value, 4, rlen), "%08x");
    ASSERT_EQ_FMT(1u, wl_xb_rd32(value), "%u");
    ASSERT_EQ_FMT(4u, wl_xb_rd32(type), "%u");
    ASSERT_EQ_FMT(0u, KN(S_EX_QUERY_NV, 0xA, type, value, 4, rlen), "%08x");
    ASSERT_EQ_FMT(0u, wl_xb_rd32(value), "%u");
    PASS();
}

// ---- The exports' corner cases ----------------------------------------------------------------------

static bool host_exists(const char *rel) {
    char path[512];
    struct stat sb;
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    return stat(path, &sb) == 0;
}

// A sparse file's sidecar on the host: its size, 0 when there is none.
static uint64_t host_sidecar(const char *rel) {
    char path[512], text[20] = {0};
    snprintf(path, sizeof path, "%s/%s.wlsz", g_root, rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = fread(text, 1, 16, f);
    fclose(f);
    text[n] = '\0';
    return strtoull(text, NULL, 16);
}

static uint64_t query_eof(uint32_t h) {
    uint32_t iosb = gmem(8), info = gmem(24);
    if (KN(S_NT_QUERY_INFO, h, iosb, info, 24, 5) != 0) {
        return ~0ull;
    }
    return wl_xb_rd64(info + 8);
}

static uint32_t set_eof(uint32_t h, uint64_t size) {
    uint32_t iosb = gmem(8), eof = gmem(8);
    wl_xb_wr64(eof, size);
    return KN(S_NT_SET_INFO, h, iosb, eof, 8, 20);
}

static uint32_t set_delete(uint32_t h, bool on) {
    uint32_t iosb = gmem(8), disp = gmem(4);
    wl_xb_wr8(disp, on ? 1u : 0u);
    return KN(S_NT_SET_INFO, h, iosb, disp, 1, 13);
}

TEST files_share_one_size(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0;
    uint32_t rw = open_path("z:\\shared.map", 0xC0000000u, 3, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ_FMT(0u, set_eof(rw, 0x01000000u), "%08x");
    uint32_t ro = open_path("z:\\SHARED.map", 0x80000000u, 1, 0x20, &st); // read-only, another case
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ_FMT(0u, set_eof(rw, 0x02000000u), "%08x"); // grown through the other handle
    ASSERT_EQ(0x02000000ull, query_eof(ro));
    // a read-only handle cannot write, on a writable drive too
    uint32_t iosb = gmem(8), data = gstr("abcd");
    ASSERT_EQ_FMT(0xC0000022u, KN(S_NT_WRITE, ro, 0, 0, 0, iosb, data, 4, 0), "%08x"); // STATUS_ACCESS_DENIED
    KN(S_NT_CLOSE, rw);
    ASSERT_EQ(0x02000000ull, host_sidecar("hdd/Z/shared.map"));
    KN(S_NT_CLOSE, ro); // a handle that changed nothing leaves the sidecar alone
    ASSERT_EQ(0x02000000ull, host_sidecar("hdd/Z/shared.map"));
    // every file slot taken: the open is refused and the size kept; one left: the sidecar is read
    // before the file takes it
    WlPlatformFile *held[WL_FS_MAX_FILES + 1];
    uint32_t n = 0;
    while (n < WL_FS_MAX_FILES + 1 && wl_platform_fs_open("sd:/title/readme.txt", WL_FILE_READ, &held[n]) == WL_OK) {
        n++;
    }
    ASSERT(n > 0 && n <= WL_FS_MAX_FILES);
    open_path("z:\\shared.map", 0x80000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0xC000009Au, st, "%08x"); // STATUS_INSUFFICIENT_RESOURCES
    wl_platform_fs_close(held[--n]);
    ro = open_path("z:\\shared.map", 0x80000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ(0x02000000ull, query_eof(ro));
    KN(S_NT_CLOSE, ro);
    while (n > 0) {
        wl_platform_fs_close(held[--n]);
    }
    ASSERT_EQ(0x02000000ull, host_sidecar("hdd/Z/shared.map"));
    // NtDeleteFile takes the sidecar with the file
    ASSERT_EQ_FMT(0u, KN(S_NT_DELETE_FILE, gattr("z:\\shared.map")), "%08x");
    ASSERT(!host_exists("hdd/Z/shared.map"));
    ASSERT(!host_exists("hdd/Z/shared.map.wlsz"));
    PASS();
}

TEST files_failures_leave_the_request_alone(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0;
    // an overlapped read past the end: no status block or APC, and the event and the file object
    // are left cleared (NT clears both before it dispatches the request), as NT for an error
    uint32_t h = open_path("D:\\maps\\ui.map", 0x80000000u, 1, 0, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t ehp = gmem(4), iosb = gmem(8), buf = gmem(16), off = gmem(8), tp = gmem(8);
    KN(S_NT_CREATE_EVENT, ehp, 0, 0, 0);
    uint32_t ev = wl_xb_rd32(ehp);
    wl_xb_fill(iosb, 0xAA, 8);
    wl_xb_wr64(off, 1000);
    wl_xb_wr64(tp, (uint64_t)(int64_t)-100000); // 10 ms
    KN(S_NT_SET_EVENT, ev, 0);                                     // signalled before the request
    ASSERT_EQ_FMT(0u, KN(S_NT_WAIT1EX, h, 0, 0, tp), "%08x"); // the idle file object is signalled
    g_apc_runs = 0;
    ASSERT_EQ_FMT(0xC0000011u, KN(S_NT_READ, h, ev, guest_fn(3, apc_routine), 0x1234u, iosb, buf, 8, off), "%08x");
    ASSERT_EQ_FMT(0xAAAAAAAAu, wl_xb_rd32(iosb), "%08x");
    ASSERT_EQ_FMT(0xAAAAAAAAu, wl_xb_rd32(iosb + 4), "%08x");
    ASSERT_EQ_FMT(0x102u, KN(S_NT_WAIT1EX, ev, 1, 1, tp), "%08x"); // STATUS_TIMEOUT: cleared, nothing queued
    ASSERT_EQ_FMT(0x102u, KN(S_NT_WAIT1EX, h, 0, 0, tp), "%08x");  // the file object too
    ASSERT_EQ_FMT(0u, g_apc_runs, "%u");
    // a read that succeeds signals both again
    wl_xb_wr64(off, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, h, ev, 0, 0, iosb, buf, 4, off), "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_WAIT1EX, ev, 0, 0, tp), "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_WAIT1EX, h, 0, 0, tp), "%08x");
    // a write on a read-only handle is refused with the handle: the event is not looked at
    wl_xb_fill(iosb, 0xAA, 8);
    ASSERT_EQ_FMT(0xC0000022u, KN(S_NT_WRITE, h, ev, guest_fn(3, apc_routine), 0x1234u, iosb, buf, 8, off), "%08x");
    ASSERT_EQ_FMT(0xAAAAAAAAu, wl_xb_rd32(iosb), "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_WAIT1EX, ev, 1, 1, tp), "%08x"); // still signalled
    ASSERT_EQ_FMT(0u, g_apc_runs, "%u");
    KN(S_NT_CLOSE, ev);
    KN(S_NT_CLOSE, h);
    // a write the card cannot take whole (a file size limit stands in for a full card): STATUS_DISK_FULL,
    // and the bytes written count in the sparse size
    uint32_t base_p = gmem(4), size_p = gmem(4);
    wl_xb_wr32(size_p, 0x11000u);
    ASSERT_EQ_FMT(0u, KN(S_NT_ALLOC_VM, base_p, 0, size_p, 0x1000, 4), "%08x");
    uint32_t big = wl_xb_rd32(base_p);
    h = open_path("z:\\full.map", 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    struct rlimit old_lim, lim;
    getrlimit(RLIMIT_FSIZE, &old_lim);
    lim = old_lim;
    lim.rlim_cur = 0x10000u;
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &lim);
    wl_xb_wr64(off, 0);
    uint32_t status = KN(S_NT_WRITE, h, 0, 0, 0, iosb, big, 0x10010u, off);
    setrlimit(RLIMIT_FSIZE, &old_lim);
    signal(SIGXFSZ, old_sig);
    ASSERT_EQ_FMT(0xC000007Fu, status, "%08x"); // STATUS_DISK_FULL
    ASSERT_EQ(0x10000ull, query_eof(h));
    ASSERT_EQ_FMT(0u, set_delete(h, true), "%08x");
    KN(S_NT_CLOSE, h);
    ASSERT(!host_exists("hdd/Z/full.map"));
    wl_xb_wr32(base_p, big);
    wl_xb_wr32(size_p, 0);
    KN(S_NT_FREE_VM, base_p, size_p, 0x8000);
    PASS();
}

TEST files_deleted_at_the_last_close(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0;
    const char *name = "\\Device\\Harddisk0\\Partition1\\two.bin";
    // FileDispositionInformation on one handle: the file stays while another is open
    uint32_t a = open_path(name, 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t b = open_path(name, 0xC0000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ_FMT(0u, set_delete(a, true), "%08x");
    KN(S_NT_CLOSE, a);
    ASSERT(host_exists("hdd/E/two.bin"));
    uint32_t iosb = gmem(8), info = gmem(24);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_INFO, b, iosb, info, 24, 5), "%08x");
    ASSERT_EQ(1, wl_xb_rd8(info + 20)); // DeletePending, seen through the other handle
    KN(S_NT_CLOSE, b);
    ASSERT(!host_exists("hdd/E/two.bin"));
    // FILE_DELETE_ON_CLOSE: the same
    a = open_path(name, 0xC0000000u, 2, 0x20 | 0x1000, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    b = open_path(name, 0x80000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    KN(S_NT_CLOSE, a);
    ASSERT(host_exists("hdd/E/two.bin"));
    KN(S_NT_CLOSE, b);
    ASSERT(!host_exists("hdd/E/two.bin"));
    // NtDeleteFile of an open file: gone at its last close
    a = open_path(name, 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_DELETE_FILE, gattr(name)), "%08x");
    ASSERT(host_exists("hdd/E/two.bin"));
    KN(S_NT_CLOSE, a);
    ASSERT(!host_exists("hdd/E/two.bin"));
    PASS();
}

TEST files_delete_pending_refuses_opens(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0;
    const char *name = "\\Device\\Harddisk0\\Partition1\\gone.bin";
    uint32_t a = open_path(name, 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ_FMT(0u, set_delete(a, true), "%08x");
    // while its delete is pending, no disposition gets a handle: not one that would join the
    // record and be deleted with it
    static const uint32_t dispositions[] = {0, 1, 2, 3, 4, 5}; // supersede .. overwrite-if
    for (uint32_t i = 0; i < 6; i++) {
        open_path(name, 0xC0000000u, dispositions[i], 0x20, &st);
        ASSERT_EQ_FMT(0xC0000056u, st, "%08x"); // STATUS_DELETE_PENDING
    }
    KN(S_NT_CLOSE, a);
    ASSERT(!host_exists("hdd/E/gone.bin"));
    // gone: a new file of the name is made, and stays after its close
    a = open_path(name, 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    KN(S_NT_CLOSE, a);
    ASSERT(host_exists("hdd/E/gone.bin"));
    // a pending delete taken back (the disposition set to false) lets opens in again
    a = open_path(name, 0xC0000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, set_delete(a, true), "%08x");
    ASSERT_EQ_FMT(0u, set_delete(a, false), "%08x");
    uint32_t b = open_path(name, 0x80000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    KN(S_NT_CLOSE, b);
    // NtDeleteFile of an open file makes it pending too
    ASSERT_EQ_FMT(0u, KN(S_NT_DELETE_FILE, gattr(name)), "%08x");
    open_path(name, 0x80000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0xC0000056u, st, "%08x");
    KN(S_NT_CLOSE, a);
    ASSERT(!host_exists("hdd/E/gone.bin"));
    PASS();
}

static uint32_t rename_to(uint32_t h, const char *to, bool replace) {
    // FILE_RENAME_INFORMATION: ReplaceIfExists, RootDirectory, then the OBJECT_STRING itself
    uint32_t iosb = gmem(8), info = gmem(16), text = gstr(to);
    wl_xb_wr8(info, replace ? 1u : 0u);
    wl_xb_wr32(info + 4, 0);
    wl_xb_wr16(info + 8, (uint16_t)strlen(to));
    wl_xb_wr16(info + 10, (uint16_t)(strlen(to) + 1u));
    wl_xb_wr32(info + 12, text);
    return KN(S_NT_SET_INFO, h, iosb, info, 16, 10);
}

TEST files_rename_moves_every_handle(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0, iosb = gmem(8), off = gmem(8), buf = gmem(16);
    const char *from = "\\Device\\Harddisk0\\Partition1\\from.bin";
    const char *to = "\\Device\\Harddisk0\\Partition1\\to.bin";
    uint32_t a = open_path(from, 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t a2 = open_path(from, 0xC0000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t t = open_path(to, 0xC0000000u, 2, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    wl_xb_wr64(off, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_WRITE, t, 0, 0, 0, iosb, gstr("old!"), 4, off), "%08x");
    // the file it would replace is open: refused, and nothing moves
    ASSERT_EQ_FMT(0xC0000035u, rename_to(a, to, false), "%08x"); // STATUS_OBJECT_NAME_COLLISION
    ASSERT_EQ_FMT(0xC0000022u, rename_to(a, to, true), "%08x");  // STATUS_ACCESS_DENIED
    ASSERT(host_exists("hdd/E/from.bin"));
    KN(S_NT_CLOSE, t);
    // closed: replaced, and the other handle on the file follows it to its new name
    ASSERT_EQ_FMT(0u, rename_to(a, to, true), "%08x");
    ASSERT(!host_exists("hdd/E/from.bin"));
    ASSERT_EQ_FMT(0u, KN(S_NT_WRITE, a2, 0, 0, 0, iosb, gstr("new!"), 4, off), "%08x");
    uint32_t name = gmem(64);
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_INFO, a2, iosb, name, 64, 9), "%08x"); // FileNameInformation
    char text[64] = {0};
    uint32_t nl = wl_xb_rd32(name);
    wl_xb_copy_out(text, name + 4, nl < 63u ? nl : 63u);
    ASSERT(strstr(text, "to.bin") != NULL);
    // one record under the new name: a delete asked for by a new handle waits for the old ones
    uint32_t c = open_path(to, 0xC0000000u, 1, 0x20, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    ASSERT_EQ_FMT(0u, KN(S_NT_READ, c, 0, 0, 0, iosb, buf, 4, off), "%08x");
    wl_xb_copy_out(text, buf, 4);
    text[4] = 0;
    ASSERT_STR_EQ("new!", text);
    ASSERT_EQ_FMT(0u, set_delete(c, true), "%08x");
    KN(S_NT_CLOSE, c);
    KN(S_NT_CLOSE, a);
    ASSERT(host_exists("hdd/E/to.bin"));
    KN(S_NT_CLOSE, a2);
    ASSERT(!host_exists("hdd/E/to.bin"));
    PASS();
}

TEST objects_live_while_referenced(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t st = 0;
    WlXkStats s0, s1;
    uint32_t h = open_path("z:\\byptr.map", 0xC0000000u, 2, 0x20 | 0x1000, &st); // delete on close
    ASSERT_EQ_FMT(0u, st, "%08x");
    wl_xk_stats(&s0);
    uint32_t out = gmem(4);
    ASSERT_EQ_FMT(0u, KN(S_OB_REF_BY_HANDLE, h, 0, out), "%08x");
    uint32_t body = wl_xb_rd32(out);
    ASSERT(body != 0);
    KN(S_NT_CLOSE, h); // the reference by pointer keeps the file
    ASSERT(host_exists("hdd/Z/byptr.map"));
    wl_xk_stats(&s1);
    ASSERT_EQ(s0.files_open, s1.files_open);
    g_ecx = body; // fastcall
    K0(S_OBF_REF);
    g_ecx = body;
    K0(S_OBF_DEREF);
    ASSERT(host_exists("hdd/Z/byptr.map"));
    g_ecx = body;
    K0(S_OBF_DEREF); // the last reference: the file closes (and is deleted)
    ASSERT(!host_exists("hdd/Z/byptr.map"));
    wl_xk_stats(&s1);
    ASSERT_EQ(s0.files_open - 1u, s1.files_open);
    ASSERT_EQ(s0.objects - 1u, s1.objects);
    PASS();
}

static uint32_t g_wait_handle, g_wait_status, g_wait_done;

static void waiter_routine(void) {
    uint32_t esp = g_esp;
    uint32_t tp = gmem(8);
    wl_xb_wr64(tp, (uint64_t)(int64_t)-50000000); // 5 s
    g_wait_status = KN(S_NT_WAIT1, g_wait_handle, 0, tp);
    KN(S_KE_RELEASE_SEMAPHORE, g_wait_done, 0, 1, 0);
    g_esp = esp;
    guest_ret(8);
}

TEST a_wait_keeps_its_object(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    uint32_t hp = gmem(4), out = gmem(4), tp = gmem(8);
    ASSERT_EQ_FMT(0u, KN(S_NT_CREATE_EVENT, hp, 0, 0, 0), "%08x"); // notification, not signalled
    g_wait_handle = wl_xb_rd32(hp);
    ASSERT_EQ_FMT(0u, KN(S_OB_REF_BY_HANDLE, g_wait_handle, 0, out), "%08x");
    uint32_t body = wl_xb_rd32(out);
    g_ecx = body;
    K0(S_OBF_DEREF); // the body's address, and only the handle holds it
    g_wait_done = gmem(20);
    KN(S_KE_INIT_SEMAPHORE, g_wait_done, 0, 1);
    g_wait_status = 0xFFFFFFFFu;
    uint32_t th = gmem(4);
    ASSERT_EQ_FMT(0u, KN(S_PS_CREATE, th, 0, 0x4000, 0, 0, 0, 0, 0, 0, guest_fn(4, waiter_routine)), "%08x");
    wl_xb_wr64(tp, (uint64_t)(int64_t)-100000); // 10 ms: the waiter starts its wait
    KN(S_KE_DELAY, 0, 0, tp);
    ASSERT_EQ_FMT(0xFFFFFFFFu, g_wait_status, "%08x");
    KN(S_NT_CLOSE, g_wait_handle); // the last handle, closed under the waiter
    ASSERT_EQ_FMT(0x4C504C57u, wl_xb_rd32(body - 16u), "%08x"); // still a pool block ("WLPL")
    KN(S_KE_SET_EVENT, body, 0, 0);
    wl_xb_wr64(tp, (uint64_t)(int64_t)-50000000);
    ASSERT_EQ_FMT(0u, KN(S_KE_WAIT1, g_wait_done, 0, 0, 0, tp), "%08x");
    ASSERT_EQ_FMT(0u, g_wait_status, "%08x"); // woken by the event
    ASSERT_EQ_FMT(0u, wl_xb_rd32(body - 16u), "%08x"); // freed once the wait ended
    KN(S_NT_CLOSE, wl_xb_rd32(th));
    PASS();
}

TEST virtual_memory_corners(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    // MEM_COMMIT alone with no address: reserved and committed
    uint32_t base_p = gmem(4), size_p = gmem(4), mbi = gmem(28);
    wl_xb_wr32(size_p, 0x5000);
    ASSERT_EQ_FMT(0u, KN(S_NT_ALLOC_VM, base_p, 0, size_p, 0x1000, 4), "%08x");
    uint32_t base = wl_xb_rd32(base_p);
    ASSERT(base != 0);
    ASSERT_EQ_FMT(0x5000u, wl_xb_rd32(size_p), "%x");
    wl_xb_wr32(base + 0x4FFCu, 0x12345678u);
    // an unaligned decommit takes every page it touches: 0x1800 + 0x1000 is pages 1 and 2
    wl_xb_wr32(base_p, base + 0x1800u);
    wl_xb_wr32(size_p, 0x1000);
    ASSERT_EQ_FMT(0u, KN(S_NT_FREE_VM, base_p, size_p, 0x4000), "%08x"); // MEM_DECOMMIT
    ASSERT_EQ_FMT(base + 0x1000u, wl_xb_rd32(base_p), "%08x");
    ASSERT_EQ_FMT(0x2000u, wl_xb_rd32(size_p), "%x");
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_VM, base + 0x1000u, mbi), "%08x");
    ASSERT_EQ_FMT(0x2000u, wl_xb_rd32(mbi + 16), "%x"); // MEM_RESERVE
    ASSERT_EQ_FMT(0x2000u, wl_xb_rd32(mbi + 12), "%x");
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_VM, base + 0x3000u, mbi), "%08x");
    ASSERT_EQ_FMT(0x1000u, wl_xb_rd32(mbi + 16), "%x"); // still committed
    // size 0: from the address to the end of the allocation
    wl_xb_wr32(base_p, base + 0x3000u);
    wl_xb_wr32(size_p, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_FREE_VM, base_p, size_p, 0x4000), "%08x");
    ASSERT_EQ_FMT(0x2000u, wl_xb_rd32(size_p), "%x");
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_VM, base + 0x4000u, mbi), "%08x");
    ASSERT_EQ_FMT(0x2000u, wl_xb_rd32(mbi + 16), "%x");
    ASSERT_EQ_FMT(0u, KN(S_NT_QUERY_VM, base, mbi), "%08x");
    ASSERT_EQ_FMT(0x1000u, wl_xb_rd32(mbi + 16), "%x"); // page 0 untouched
    ASSERT_EQ_FMT(0x1000u, wl_xb_rd32(mbi + 12), "%x");
    wl_xb_wr32(base_p, base);
    wl_xb_wr32(size_p, 0);
    ASSERT_EQ_FMT(0u, KN(S_NT_FREE_VM, base_p, size_p, 0x8000), "%08x");
    PASS();
}

TEST rtl_formats_past_the_stack_buffer(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    char piece[191];
    memset(piece, 'q', 190);
    piece[190] = '\0';
    piece[0] = '<';
    piece[189] = '>';
    uint32_t s = gstr(piece), fmt = gstr("%s%s%s%s%s%s%s%s%s%s%s%s!");
    uint32_t out = gmem(2400), esp = g_esp;
    uint32_t n = KN(S_RTL_SPRINTF, out, fmt, s, s, s, s, s, s, s, s, s, s, s, s);
    g_esp = esp;
    ASSERT_EQ_FMT(12u * 190u + 1u, n, "%u");
    for (uint32_t i = 0; i < 12; i++) { // every piece whole, past the 2 KB the text is first made in
        ASSERT_EQ('<', wl_xb_rd8(out + i * 190u));
        ASSERT_EQ('>', wl_xb_rd8(out + i * 190u + 189u));
    }
    ASSERT_EQ('!', wl_xb_rd8(out + 12u * 190u));
    ASSERT_EQ(0, wl_xb_rd8(out + 12u * 190u + 1u));
    // RtlVsprintf over a va_list
    uint32_t args = gmem(48);
    for (uint32_t i = 0; i < 12; i++) {
        wl_xb_wr32(args + 4u * i, s);
    }
    wl_xb_fill(out, 0xEE, 2400);
    esp = g_esp;
    n = KN(S_RTL_VSPRINTF, out, fmt, args);
    g_esp = esp;
    ASSERT_EQ_FMT(12u * 190u + 1u, n, "%u");
    ASSERT_EQ('>', wl_xb_rd8(out + 11u * 190u + 189u));
    ASSERT_EQ(0, wl_xb_rd8(out + 12u * 190u + 1u));
    PASS();
}

TEST directory_listing_keeps_entries(void) {
    boot();
    ASSERT(g_ready);
    reset_scratch();
    make_file("title/maps/noext", "x");
    uint32_t st = 0;
    uint32_t h = open_path("D:\\maps", 0x80000000u, 1, 0x21, &st);
    ASSERT_EQ_FMT(0u, st, "%08x");
    uint32_t mask = gmem(8), pat = gstr("*.*"), iosb = gmem(8), dir = gmem(0x200);
    wl_xb_wr16(mask, 3);
    wl_xb_wr16(mask + 2, 4);
    wl_xb_wr32(mask + 4, pat);
    // a buffer that cannot take the name: the entry comes with the next call
    ASSERT_EQ_FMT(0x80000005u, KN(S_NT_QUERY_DIR, h, 0, 0, 0, iosb, dir, 0x41, 1, mask, 1), "%08x");
    bool seen_ui = false, seen_b30 = false, seen_noext = false;
    uint32_t count = 0, status;
    while ((status = KN(S_NT_QUERY_DIR, h, 0, 0, 0, iosb, dir, 0x200, 1, 0, 0)) == 0) {
        char name[64] = {0};
        uint32_t nl = wl_xb_rd32(dir + 0x3C);
        wl_xb_copy_out(name, dir + 0x40, nl < 63u ? nl : 63u);
        seen_ui |= strcmp(name, "ui.map") == 0;
        seen_b30 |= strcmp(name, "b30.map") == 0;
        seen_noext |= strcmp(name, "noext") == 0; // "*.*" takes a name without a dot, as on DOS and NT
        count++;
    }
    ASSERT_EQ_FMT(0x80000006u, status, "%08x");
    ASSERT_EQ_FMT(3u, count, "%u");
    ASSERT(seen_ui && seen_b30 && seen_noext);
    KN(S_NT_CLOSE, h);
    char path[512];
    snprintf(path, sizeof path, "%s/title/maps/noext", g_root);
    unlink(path);
    PASS();
}

SUITE(kernel_suite) {
    RUN_TEST(thunk_abi);
    RUN_TEST(threads_and_critical_sections);
    RUN_TEST(events_timeouts_mutants);
    RUN_TEST(timers_dpcs_interrupts);
    RUN_TEST(device_polls_are_safe_points);
    RUN_TEST(device_ticks_run_at_safe_points);
    RUN_TEST(a_raised_irql_keeps_the_processor);
    RUN_TEST(files_dvd_and_overlapped);
    RUN_TEST(files_sparse_cache_drive);
    RUN_TEST(virtual_and_contiguous_memory);
    RUN_TEST(rtl_crypto_settings);
    RUN_TEST(files_share_one_size);
    RUN_TEST(files_failures_leave_the_request_alone);
    RUN_TEST(files_deleted_at_the_last_close);
    RUN_TEST(files_delete_pending_refuses_opens);
    RUN_TEST(files_rename_moves_every_handle);
    RUN_TEST(objects_live_while_referenced);
    RUN_TEST(a_wait_keeps_its_object);
    RUN_TEST(virtual_memory_corners);
    RUN_TEST(rtl_formats_past_the_stack_buffer);
    RUN_TEST(directory_listing_keeps_entries);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(kernel_suite);
    GREATEST_MAIN_END();
}
