// SPDX-License-Identifier: MIT
// The register file's definitions (regs.h, recomp_symbols.inc) and its copies in and out.
#include <string.h>

#include "wl/xboxrt/regs.h"

#define WL_XB_REG(type, name, dims, init) type name dims = init;
#define WL_XB_GLOBAL(type, name, dims, init) type name dims = init;
#include "wl/xboxrt/recomp_symbols.inc"
#undef WL_XB_REG
#undef WL_XB_GLOBAL

void wl_xb_cpu_save(WlXbCpu *cpu) {
#define WL_XB_REG(type, name, dims, init) memcpy(&cpu->name, &name, sizeof cpu->name);
#define WL_XB_GLOBAL(type, name, dims, init)
#include "wl/xboxrt/recomp_symbols.inc"
#undef WL_XB_REG
#undef WL_XB_GLOBAL
}

void wl_xb_cpu_load(const WlXbCpu *cpu) {
#define WL_XB_REG(type, name, dims, init) memcpy(&name, &cpu->name, sizeof cpu->name);
#define WL_XB_GLOBAL(type, name, dims, init)
#include "wl/xboxrt/recomp_symbols.inc"
#undef WL_XB_REG
#undef WL_XB_GLOBAL
}

void wl_xb_cpu_reset(WlXbCpu *cpu) {
    memset(cpu, 0, sizeof *cpu);
    cpu->g_fp_control_word = 0x037Fu;
    cpu->g_fp_cc = 0x4000u;
    cpu->g_mxcsr = 0x1F80u;   // every exception masked, round to nearest
}
