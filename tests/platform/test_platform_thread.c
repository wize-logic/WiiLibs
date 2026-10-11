// SPDX-License-Identifier: MIT
// The threads suite (suite_threads.c) run on the pthreads host stub:
// thread/semaphore/mutex wrappers, the SPSC ring between real threads, the IO thread, the heap
// lock under contention. Priority-order checks are skipped here (the stub ignores priorities).
#include <stdarg.h>
#include <stdio.h>

#include "tests/platform/check.h"
#include "greatest.h"

static int g_fail;
static int g_pass;

void wlt_report(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void wlt_check(const char *name, bool ok, const char *detail) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL %s: %s\n", name, detail ? detail : "-");
    }
}

void wlt_suite_threads(void);

TEST threads_suite(void) {
    wlt_suite_threads();
    ASSERT(g_pass > 20);
    ASSERT_EQ(0, g_fail);
    PASS();
}

SUITE(thread_suite) {
    RUN_TEST(threads_suite);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(thread_suite);
    GREATEST_MAIN_END();
}
