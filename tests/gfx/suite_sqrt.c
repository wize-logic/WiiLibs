// SPDX-License-Identifier: MIT
// wl_sqrtf_exact against the C library's sqrtf, for the on-console runner: on the console (and in Dolphin)
// the estimate is the processor's own frsqrte, a table good to 5 bits, which a qemu-ppc host test cannot
// reproduce (qemu's is nearly exact). The host test (test_gfx_sqrt.c) covers the rest.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "tests/platform/check.h"
#include "wl/gfx/gfx_sqrt.h"

void wlt_suite_sqrt(void);

static uint32_t bits_of(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

void wlt_suite_sqrt(void) {
    unsigned long differ = 0, checked = 0;
    double worst = 0.0;
    // every 1999th float, every 17th of the smallest (the subnormals and the first binade)
    for (uint32_t bits = 1; bits < 0x7F800000u; bits += bits < 0x00100000u ? 17u : 1999u) {
        float x;
        memcpy(&x, &bits, sizeof(x));
        checked++;
        if (bits_of(wl_sqrtf_exact(x)) != bits_of(sqrtf(x))) {
            differ++;
        }
        // (the estimate's error, which the result must not depend on)
        double error = fabs(wl_rsqrt_estimate((double)x) * sqrt((double)x) - 1.0);
        if (error > worst) {
            worst = error;
        }
    }
    wlt_report("WLT sqrt: %lu floats, %lu differ, the estimate off by at most %.4f", checked, differ, worst);
    wlt_check("sqrt_exact", differ == 0, "wl_sqrtf_exact equals sqrtf");
    wlt_check("sqrt_estimate", worst < 0.07, "frsqrte good to about 5 bits");
    wlt_check("sqrt_zero", bits_of(wl_sqrtf_exact(-0.0f)) == 0x80000000u, "sqrt(-0) is -0");
    wlt_check("sqrt_negative", isnan(wl_sqrtf_exact(-1.0f)), "sqrt(-1) is NaN");
    wlt_check("sqrt_infinity", bits_of(wl_sqrtf_exact(INFINITY)) == 0x7F800000u, "sqrt(inf) is inf");
}
