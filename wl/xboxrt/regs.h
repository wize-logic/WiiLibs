// SPDX-License-Identifier: MIT
// The guest CPU's register file as the generated code sees it: plain globals (libogc has no
// thread-local storage), one set for the process, swapped per guest thread when the guest lock
// changes hands (thread.h). The list is recomp_symbols.inc; the union types match the generated
// code's header (recomp_types.h defines the same ones behind the same guards, whichever comes
// first wins, and both are little-endian unions on every host).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(__GNUC__) || defined(__clang__)
// the generated code's memory model needs GCC's scalar_storage_order; so does this header
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "wl/xboxrt needs GCC (scalar_storage_order) on a big-endian host"
#endif
#define WL_XB_LE_UNION
#else
#define WL_XB_LE_UNION __attribute__((scalar_storage_order("little-endian")))
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union WL_XB_LE_UNION RecompXmm {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
} RecompXmm;
#endif

#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union WL_XB_LE_UNION RecompMmx {
    int8_t b[8];
    uint8_t ub[8];
    int16_t w[4];
    uint16_t uw[4];
    int32_t d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

#define WL_XB_REG(type, name, dims, init) extern type name dims;
#define WL_XB_GLOBAL(type, name, dims, init) extern type name dims;
#include "wl/xboxrt/recomp_symbols.inc"
#undef WL_XB_REG
#undef WL_XB_GLOBAL

// One guest thread's registers while it is not running.
typedef struct {
#define WL_XB_REG(type, name, dims, init) type name dims;
#define WL_XB_GLOBAL(type, name, dims, init)
#include "wl/xboxrt/recomp_symbols.inc"
#undef WL_XB_REG
#undef WL_XB_GLOBAL
} WlXbCpu;

// Copies the globals into `cpu` / `cpu` into the globals.
void wl_xb_cpu_save(WlXbCpu *cpu);
void wl_xb_cpu_load(const WlXbCpu *cpu);
// A register file at reset: zeros, the x87 control word 0x037F and condition codes 0x4000.
void wl_xb_cpu_reset(WlXbCpu *cpu);

#ifdef __cplusplus
}
#endif
