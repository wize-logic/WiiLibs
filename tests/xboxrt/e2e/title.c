// SPDX-License-Identifier: MIT
// A tiny Xbox "title" for the end-to-end test: compiled for i386 (gcc -m32, freestanding), wrapped in
// an XBE by mkxbe_title.py with a kernel thunk table, lifted to C by xboxrecomp's pipeline, compiled
// for the host or ppc-be and run on wl/xboxrt + wl/xboxkrnl. It calls kernel imports through the
// thunk table (stdcall and fastcall), touches the NV2A's registers (the device faults), makes a
// thread and waits for it, writes a file, and records what it saw in `results`, which the test reads
// from guest memory after the title has left through HalReturnToFirmware.
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef long long s64;
#define STDCALL __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))

// The thunk table (mkxbe_title.py writes 0x80000000 | ordinal into each slot, in this order).
enum {
    T_NT_ALLOCATE_VM,      // 184
    T_MM_CONTIG_EX,        // 166
    T_MM_PHYS,             // 173
    T_KE_PERF_COUNTER,     // 126
    T_PS_CREATE_THREAD,    // 255
    T_PS_TERMINATE,        // 258
    T_NT_CREATE_EVENT,     // 189
    T_NT_SET_EVENT,        // 225
    T_NT_WAIT,             // 233
    T_NT_CLOSE,            // 187
    T_NT_CREATE_FILE,      // 190
    T_NT_WRITE_FILE,       // 236
    T_DBG_PRINT,           // 8
    T_INTERLOCKED_INC,     // 53
    T_HAL_RETURN,          // 49
    T_KE_TICK_COUNT,       // 156 (data)
    T_COUNT
};
__attribute__((section(".thunks"), used)) void *kthunks[T_COUNT + 1];

typedef struct {
    u16 length, max;
    const char *buffer;
} AnsiString;
typedef struct {
    void *root;
    AnsiString *name;
    u32 attributes;
} ObjectAttributes;
typedef struct {
    s32 status;
    u32 information;
} IoStatus;

#define CALL(slot, type) ((type)kthunks[slot])
typedef s32(STDCALL *NtAllocateVirtualMemory_t)(void **, u32, u32 *, u32, u32);
typedef void *(STDCALL *MmAllocateContiguousMemoryEx_t)(u32, u32, u32, u32, u32);
typedef u32(STDCALL *MmGetPhysicalAddress_t)(void *);
typedef s64(STDCALL *KeQueryPerformanceCounter_t)(void);
typedef s32(STDCALL *PsCreateSystemThreadEx_t)(void **, u32, u32, u32, u32 *, void *, void *, u8, u8, void *);
typedef void(STDCALL *PsTerminateSystemThread_t)(s32);
typedef s32(STDCALL *NtCreateEvent_t)(void **, ObjectAttributes *, u32, u8);
typedef s32(STDCALL *NtSetEvent_t)(void *, s32 *);
typedef s32(STDCALL *NtWaitForSingleObject_t)(void *, u8, s64 *);
typedef s32(STDCALL *NtClose_t)(void *);
typedef s32(STDCALL *NtCreateFile_t)(void **, u32, ObjectAttributes *, IoStatus *, s64 *, u32, u32, u32, u32);
typedef s32(STDCALL *NtWriteFile_t)(void *, void *, void *, void *, IoStatus *, const void *, u32, s64 *);
typedef u32 (*DbgPrint_t)(const char *, ...);
typedef s32(FASTCALL *InterlockedIncrement_t)(s32 *);
typedef void(STDCALL *HalReturnToFirmware_t)(u32);

#define R_MAGIC 0
#define R_ALLOC_OK 1
#define R_ALLOC_BACK 2
#define R_CONTIG 3
#define R_PHYS 4
#define R_CONTIG_BACK 5
#define R_WBC 6
#define R_GET 7
#define R_BOOT 8
#define R_PERF_OK 9
#define R_THREAD_RAN 10
#define R_THREAD_ARG 11
#define R_WAIT 12
#define R_FILE 13
#define R_WRITTEN 14
#define R_INTERLOCKED 15
#define R_TICK 16
#define R_LOOP 17
#define R_DONE 18

volatile u32 results[32];
static void *g_event;
static s32 g_shared;

static u32 STDCALL worker(void *ctx1, void *ctx2) {
    (void)ctx2;
    results[R_THREAD_RAN] = 1;
    results[R_THREAD_ARG] = (u32)ctx1;
    CALL(T_INTERLOCKED_INC, InterlockedIncrement_t)(&g_shared);
    CALL(T_NT_SET_EVENT, NtSetEvent_t)(g_event, 0);
    CALL(T_PS_TERMINATE, PsTerminateSystemThread_t)(0);
    return 0;
}

static u32 fib(u32 n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

void title_entry(void) {
    results[R_MAGIC] = 0x58424F58u; // "XBOX"

    // virtual memory: reserve and commit 64 KB, write and read back
    void *base = 0;
    u32 size = 0x10000;
    results[R_ALLOC_OK] = CALL(T_NT_ALLOCATE_VM, NtAllocateVirtualMemory_t)(&base, 0, &size, 0x3000, 4) == 0;
    volatile u32 *p = (volatile u32 *)base;
    p[100] = 0x12345678u;
    results[R_ALLOC_BACK] = p[100];

    // contiguous memory at a fixed physical address, and the physical address of it
    void *c = CALL(T_MM_CONTIG_EX, MmAllocateContiguousMemoryEx_t)(0x2000, 0x100000, 0x101FFF, 0, 4);
    results[R_CONTIG] = (u32)c;
    results[R_PHYS] = CALL(T_MM_PHYS, MmGetPhysicalAddress_t)(c);
    ((volatile u32 *)c)[4] = 0xCAFEBABEu;
    results[R_CONTIG_BACK] = *(volatile u32 *)(0x80000000u | (results[R_PHYS] + 16)); // the window alias

    // the NV2A: a write-back flush kick (or in a bit, wait for it to clear), PUT and GET, PMC_BOOT_0
    volatile u32 *wbc = (volatile u32 *)0xFD100410u;
    *wbc |= 0x10000u;
    u32 spins = 0;
    while ((*wbc & 0x10000u) != 0 && spins < 1000) {
        spins++;
    }
    results[R_WBC] = spins;
    *(volatile u32 *)0xFD800040u = 0x00012340u;
    results[R_GET] = *(volatile u32 *)0xFD800044u;
    results[R_BOOT] = *(volatile u32 *)0xFD000000u;

    s64 t0 = CALL(T_KE_PERF_COUNTER, KeQueryPerformanceCounter_t)();
    results[R_LOOP] = fib(20);
    s64 t1 = CALL(T_KE_PERF_COUNTER, KeQueryPerformanceCounter_t)();
    // (a 64-bit compare lifts to setge after sbb, which the lifter does not yet translate: compare
    // the low halves' difference instead)
    u32 dt = (u32)t1 - (u32)t0;
    results[R_PERF_OK] = dt < 0x10000000u;

    // a thread: it sets an event and ends; the wait returns 0
    CALL(T_NT_CREATE_EVENT, NtCreateEvent_t)(&g_event, 0, 0, 0);
    void *thread = 0;
    CALL(T_PS_CREATE_THREAD, PsCreateSystemThreadEx_t)(&thread, 0, 0x4000, 0, 0, (void *)0x5A5A, 0, 0, 0, (void *)worker);
    s64 timeout = -50000000; // 5 s
    results[R_WAIT] = (u32)CALL(T_NT_WAIT, NtWaitForSingleObject_t)(g_event, 0, &timeout);
    CALL(T_NT_CLOSE, NtClose_t)(thread);
    results[R_INTERLOCKED] = (u32)CALL(T_INTERLOCKED_INC, InterlockedIncrement_t)(&g_shared);

    // a file on the hard disk
    static const char path[] = "\\Device\\Harddisk0\\Partition1\\result.txt";
    AnsiString name = {sizeof path - 1, sizeof path, path};
    ObjectAttributes oa = {0, &name, 0};
    IoStatus io;
    void *file = 0;
    results[R_FILE] = (u32)CALL(T_NT_CREATE_FILE, NtCreateFile_t)(&file, 0xC0000000u, &oa, &io, 0, 0x80, 0, 5, 0x20);
    static const char text[] = "lifted and running\n";
    CALL(T_NT_WRITE_FILE, NtWriteFile_t)(file, 0, 0, 0, &io, text, sizeof text - 1, 0);
    results[R_WRITTEN] = io.information;
    CALL(T_NT_CLOSE, NtClose_t)(file);

    results[R_TICK] = **(volatile u32 **)&kthunks[T_KE_TICK_COUNT]; // KeTickCount, read as GetTickCount does
    CALL(T_DBG_PRINT, DbgPrint_t)("e2e title: fib(20)=%u, GET=%x\n", results[R_LOOP], results[R_GET]);
    results[R_DONE] = 1;
    CALL(T_HAL_RETURN, HalReturnToFirmware_t)(2); // quick reboot: the test sees exit code 2
}
