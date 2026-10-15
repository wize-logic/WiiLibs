// SPDX-License-Identifier: MIT
// The console suite of the generated vertex programs' paired-single code (suite_vsh_ps.c) on the
// host: the code the Wii runs, emulated in C by vsh_ps_emulate.py.
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "greatest.h"

void wlt_suite_vsh_ps(void);
void wlt_check(const char *name, bool ok, const char *detail);
void wlt_report(const char *fmt, ...);

static unsigned g_checks, g_failures;

void wlt_check(const char *name, bool ok, const char *detail) {
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL %s: %s\n", name, detail != NULL ? detail : "");
    }
}

void wlt_report(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    printf("  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

TEST emulated_paired_singles_match_the_interpreter(void) {
    g_checks = g_failures = 0;
    wlt_suite_vsh_ps();
    ASSERT_EQ(3u, g_checks);
    ASSERT_EQ(0u, g_failures);
    PASS();
}

SUITE(vsh_ps_suite) {
    RUN_TEST(emulated_paired_singles_match_the_interpreter);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(vsh_ps_suite);
    GREATEST_MAIN_END();
}
