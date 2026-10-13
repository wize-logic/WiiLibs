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

// Counters for tests and reports.
typedef struct {
    uint32_t reads, writes, unclaimed;
} WlXbMmioStats;
void wl_xb_mmio_stats(WlXbMmioStats *out);

// ---- Fault handling ----------------------------------------------------------------------------

// Installs the platform's fault handler for the guest EA span (SIGSEGV on the host and ppc-be; the
// Wii's DSI handler is part of wl_xb_mem_init). Idempotent. A fault that is not a device access
// becomes a guest crash report (crash.h) and the process stops.
WlResult wl_xb_fault_install(void);

#ifdef __cplusplus
}
#endif
