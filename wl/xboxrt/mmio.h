// SPDX-License-Identifier: MIT
// The Xbox's devices behind faulting addresses. Device ranges (the NV2A at 0xFD000000, the MCPX's
// APU, AC'97, USB and NIC, the flash) are never mapped: an access faults, the fault handler of the
// platform decodes the instruction (ppc_decode.h on the Wii and ppc-be, x86_decode.h on an x86-64
// host), calls the device here and resumes after the instruction. Device callbacks run in normal
// context on the thread that touched the register, with the guest lock held: they may take time
// and call anything a kernel call may.
//
// Values are numbers as the x86 sees them: a 32-bit read returns the register's value, which the
// decoder puts into the host register in the byte order the instruction asked for.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (*WlXbMmioRead)(void *user, uint32_t offset, unsigned size);
typedef void (*WlXbMmioWrite)(void *user, uint32_t offset, uint32_t value, unsigned size);

typedef struct {
    const char *name;
    uint32_t base;  // guest VA, at or above WL_XB_MMIO_BASE
    uint32_t size;
    WlXbMmioRead read;   // offset from base; size 1, 2 or 4
    WlXbMmioWrite write;
    void *user;
} WlXbDevice;

#define WL_XB_MAX_DEVICES 16u

// Adds a device (copied). A device registered over an earlier one's range replaces it there
// (the later one is found first). WL_ERR_LIMIT past WL_XB_MAX_DEVICES, WL_ERR_ARG for a range
// below the device space.
WlResult wl_xb_mmio_register(const WlXbDevice *dev);
// Removes every device (tests).
void wl_xb_mmio_reset(void);
// The device at a guest VA (NULL when none).
const WlXbDevice *wl_xb_mmio_find(uint32_t va);

// An access to a device register. false when no device covers `va` (the caller reports a crash).
// 8-byte accesses (an x86 movq or a pair of words) are split in two 4-byte ones, low first.
bool wl_xb_mmio_read(uint32_t va, unsigned size, uint64_t *value);
bool wl_xb_mmio_write(uint32_t va, uint64_t value, unsigned size);
// > 0 while a device's register access is emulated (the guest lock's holder): a fault on guest RAM
// in there is the device's own access, which must not give the guest lock up halfway (paging.c).
extern uint32_t g_xb_mmio_depth;

// The host PC of the guest access being emulated, as the fault handler saw it (0 when it does not
// say: the Wii's handler, or a call from C): a device's trace names the translated function with
// it (crash.h's wl_xb_symbolize).
void wl_xb_mmio_set_fault_pc(uintptr_t pc);
uintptr_t wl_xb_mmio_fault_pc(void);

// The last access dispatched was a write (1) or a read (0): the device access's safe point after a
// read looks only when something waits (wl_xb_device_safe_point; a read raises no device's line).
extern uint32_t g_xb_mmio_last_write;

// Counters for tests and reports. reads and writes are the device calls (a fault each); fast_reads
// and fast_writes the accesses to a mirrored page answered without the device's call (a fault the
// handler answered at once: wl_xb_mmio_fast_*); check_* the mirror's check (below).
typedef struct {
    uint32_t reads, writes, unclaimed;
    uint32_t fast_reads, fast_writes;
    uint32_t check_reads, check_misses;
} WlXbMmioStats;
void wl_xb_mmio_stats(WlXbMmioStats *out);
// The `top` registers accessed most over the run (device, offset, read or written), a line each.
void wl_xb_mmio_log_hot(const char *tag, uint32_t top);
// The status line's part (every 10 s): the device faults since the last call over the frames shown
// since (`flips`: wl_xb_gpu_flips), and the `top` registers and guest sites (the translated function
// and the offset in it) of the window, a frame (those two the host's only: on the Wii the totals).
void wl_xb_mmio_log_window(const char *tag, uint32_t flips, uint32_t top);

// ---- Mirrored pages ------------------------------------------------------------------------------
// A page of device registers whose reads have no effect and whose values change only by the guest's
// writes to that page (the NV2A's PFB page with the write-back cache's flush; the APU's front-end
// FIFO with its free count) is mirrored: a page of memory holds what each of its words reads, the
// device's own reads at the start and the written word read again after each write. Where the
// platform maps it for reading (the host's PROT_READ; the Wii's read-only PTE when the MMU keeps a
// store out of one: a probe at the first mirror), a guest read is a plain load and never faults; a
// store faults as before and goes to the device. Where it is not mapped (an MMU without page
// protection: Dolphin's, if it has none), a read still faults and the handler answers it from the
// page without the device's call or the safe point (wl_xb_mmio_fast_read), in exception context
// on the Wii. A write the device only keeps (a value, some bits dropped: the flush that is done at
// once) may be answered so too: a WlXbMmioFastStore.
//
// A mirrored read is no device access: it is no safe point, and the deterministic mode, which
// counts device accesses in its clock, mirrors nothing (its runs stay comparable with the Wii's and
// with earlier builds'). Off: a file "nommiomirror" in the title folder, WL_XB_NOMMIOMIRROR on the
// host (wl_xb_mmio_set_mirrors). WL_XB_MMIO_CHECK on the host keeps the pages unmapped and compares
// every read's device value with the page's (check_misses), the deterministic mode included.
typedef struct {
    uint32_t offset; // in the page, word aligned; 4-byte stores only
    uint32_t keep;   // the bits of the stored value the register keeps
    uint32_t *model; // the device model's copy of the register (it reads it from there)
} WlXbMmioFastStore;
// Set before the devices attach (boot): mirrors on (the default) or off, and the check.
void wl_xb_mmio_set_mirrors(bool on, bool check);
// Mirrors the page at guest VA `va` (page aligned, inside a registered device; at most
// WL_XB_MIRROR_PAGES, mem_internal.h). `stores` (kept, not copied) are the writes answered at once.
// WL_OK also when mirrors are off or the run is deterministic (nothing done); WL_ERR_ARG for a page
// no device covers; WL_ERR_UNSUPPORTED when the backend has no page for it.
WlResult wl_xb_mmio_mirror(uint32_t va, const WlXbMmioFastStore *stores, uint32_t n_stores);
// The word of a mirrored page at `va` read again from its device (a value the device changed by
// itself). Nothing for a page that is not mirrored.
void wl_xb_mmio_mirror_refresh(uint32_t va);
// The fault handler's answer without the device (exception-safe: integers, no locks, no calls out):
// a read of `size` bytes (1, 2, 4) from a mirrored page, the value as wl_xb_mmio_read gives it; a
// store of a WlXbMmioFastStore's word. false when it is not one (the device's call follows) or when
// the guest lock is wanted (the attention up: the safe point's work is due).
bool wl_xb_mmio_fast_read(uint32_t va, unsigned size, uint32_t *value);
bool wl_xb_mmio_fast_write(uint32_t va, uint32_t value, unsigned size);

// ---- Fault handling ----------------------------------------------------------------------------

// Installs the platform's fault handler for the guest EA span (SIGSEGV on the host and ppc-be; the
// Wii's DSI handler is part of wl_xb_mem_init). Idempotent. A fault that is not a device access
// becomes a guest crash report (crash.h) and the process stops.
WlResult wl_xb_fault_install(void);

#ifdef __cplusplus
}
#endif
