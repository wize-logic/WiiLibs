// SPDX-License-Identifier: MIT
// The deterministic checks of the `sseq` suite (suite_sseq.c) as a test that
// also builds for the big-endian ppc-be target: parsers, every sequence command, envelopes, pitch,
// ADPCM and loop exactness, stealing, and the digest of a rendered piece.
#include <stdarg.h>
#include <stdio.h>

#include "tests/audio/check.h"
#include "greatest.h"

static int g_pass, g_fail;

void wlt_report(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vprintf(fmt, ap);
    va_end(ap);
    (void)printf("\n");
}

void wlt_check(const char *name, bool ok, const char *detail) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        (void)printf("FAIL %s: %s\n", name, detail ? detail : "-");
    }
}

void wlt_sseq_core(void);

TEST sseq_core_checks_on_this_byte_order(void) {
    wlt_sseq_core();
    const uint16_t probe = 1;
    (void)printf("sseq core on a %s-endian target: %d checks passed, %d failed\n",
                 ((const uint8_t *)&probe)[0] == 0 ? "big" : "little", g_pass, g_fail);
    ASSERT(g_pass >= 140);
    ASSERT_EQ(0, g_fail);
    PASS();
}

SUITE(synth_be) {
    RUN_TEST(sseq_core_checks_on_this_byte_order);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(synth_be);
    GREATEST_MAIN_END();
}
