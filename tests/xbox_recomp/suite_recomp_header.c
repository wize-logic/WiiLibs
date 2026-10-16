// SPDX-License-Identifier: MIT
// wl/xboxrt/recomp/recomp_types.h compiled by the console's compiler, for the on-console runner: the
// SSE case devkitPPC's GCC 16 miscompiled (recomp_simd_case.h). The host and ppc-be build the header
// with another GCC, so only this suite sees what devkitPPC makes of it.
#define RECOMP_GENERATED_CODE 1
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "recomp_types.h"
#include "tests/platform/check.h"

void wlt_suite_recomp_header(void);

// what the header declares and the case touches: guest memory is this buffer
ptrdiff_t g_xbox_mem_offset;
static uint8_t g_guest[0x400] __attribute__((aligned(32)));

#include "recomp_simd_case.h"

void wlt_suite_recomp_header(void) {
    memset(g_guest, 0, sizeof g_guest);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest;
    int wrong = recomp_case_shufps_movhps();
    wlt_report("WLT recomp_header: shufps then movhps, %d of 3 words wrong", wrong);
    wlt_check("recomp_shufps_movhps", wrong == 0, "lanes stored after XMM_SHUFFLE are little-endian");
}
