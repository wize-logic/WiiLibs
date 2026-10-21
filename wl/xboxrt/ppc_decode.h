// SPDX-License-Identifier: MIT
// The PowerPC loads and stores a device access can fault on (the Wii's DSI trampoline, ppc-be's
// SIGSEGV), decoded and emulated against a saved register file. Portable: the host tests decode
// and emulate every form against a fake device.
//
// Covered: D-form lbz lbzu lhz lhzu lha lhau lwz lwzu stb stbu sth sthu stw stwu; X-form lbzx
// lbzux lhzx lhzux lhax lhaux lwzx lwzux stbx stbux sthx sthux stwx stwux and the byte-reversed
// lhbrx lwbrx sthbrx stwbrx (what the generated code's little-endian MEM macros compile to).
// Refused loudly (they mean something is wrong, not a register to emulate): lmw stmw, the string
// forms, lwarx stwcx., the cache-block instructions (dcbz on a device), floating-point loads
// and stores, eciwx ecowx.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name; // the mnemonic, for reports
    uint8_t size;     // bytes: 1, 2 or 4
    bool store;
    bool reverse;     // byte-reversed (the register holds the little-endian value)
    bool sign;        // lha forms: sign-extend the halfword
    bool update;      // ...u forms: ra gets the effective address
    bool indexed;     // X-form: ra + rb; else ra + disp (ra 0 reads as 0)
    uint8_t rt, ra, rb;
    int16_t disp;
} WlXbPpcAccess;

typedef enum {
    WL_XB_PPC_OK = 0,
    WL_XB_PPC_REFUSED, // a memory instruction that is not emulated (name says which)
    WL_XB_PPC_UNKNOWN  // not a load or store at all
} WlXbPpcDecode;

WlXbPpcDecode wl_xb_ppc_decode(uint32_t insn, WlXbPpcAccess *out);

// The host effective address the access computes from the registers.
uint32_t wl_xb_ppc_ea(const WlXbPpcAccess *a, const uint32_t gpr[32]);

// Emulates a decoded access whose device is at guest VA `va` (wl_xb_mmio_read/write), updating
// gpr (the loaded register, and ra for an update form, which gets `ea`). false when no device
// claims the address.
bool wl_xb_ppc_emulate(const WlXbPpcAccess *a, uint32_t gpr[32], uint32_t ea, uint32_t va);
// The same access answered by a mirrored page without its device (mmio.h's wl_xb_mmio_fast_read and
// _write), exception-safe: false (gpr untouched) when it is not one, and the device's call follows.
bool wl_xb_ppc_fast(const WlXbPpcAccess *a, uint32_t gpr[32], uint32_t ea, uint32_t va);

#ifdef __cplusplus
}
#endif
