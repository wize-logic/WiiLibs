// SPDX-License-Identifier: MIT
// The console and stop screen (wl/platform/stop.h) on the host backend: the message and its log
// line, the missing-symbol wording, a long message cut, and that the first stop wins. The Wii's
// waiting for HOME is checked on the console.
#include <setjmp.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/input_host.h"
#include "wl/platform/host/stop_host.h"
#include "wl/platform/platform.h"
#include "wl/platform/stop.h"

static jmp_buf g_escape;
static char g_message[512];
static bool g_again;
static unsigned g_calls;

static void hook(const char *message, bool again) {
    g_calls++;
    strncpy(g_message, message, sizeof g_message - 1u);
    g_again = again;
    longjmp(g_escape, 1);
}

static bool log_has(const char *needle) {
    const char *lines[32];
    uint32_t count = wl_platform_log_tail(lines, 32);
    for (uint32_t i = 0; i < count; i++) {
        if (strstr(lines[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

TEST console_starts_and_has_no_framebuffer_on_the_host(void) {
    ASSERT_EQ(WL_OK, wl_platform_console_start());
    ASSERT_EQ(WL_OK, wl_platform_console_start());
    ASSERT_EQ(NULL, wl_platform_console_framebuffer());
    PASS();
}

TEST report_goes_to_the_log(void) {
    wl_platform_log_init(NULL);
    wl_platform_console_report("boot", "IOS %u\n", 58u);
    ASSERT(log_has("[I][boot] IOS 58"));
    PASS();
}

// (the first stop of the run: later tests see "again")
TEST the_first_stop_says_its_message_and_logs_an_error(void) {
    wl_platform_log_init(NULL);
    wl_host_stop_set_hook(hook);
    g_calls = 0;
    wl_platform_rumble(0, true);
    if (setjmp(g_escape) == 0) {
        wl_platform_stop_screen("no %s for %d", "memory", 7);
        FAIL();
    }
    ASSERT_EQ(1u, g_calls);
    ASSERT(!wl_host_remote_rumble(0)); // (a motor does not run while the screen waits)
    ASSERT_STR_EQ("no memory for 7", g_message);
    ASSERT(!g_again);
    ASSERT(log_has("[E][stop] stopped: no memory for 7"));
    PASS();
}

TEST a_second_stop_only_waits(void) {
    g_calls = 0;
    if (setjmp(g_escape) == 0) {
        wl_platform_stop_screen("second");
        FAIL();
    }
    ASSERT_EQ(1u, g_calls);
    ASSERT_STR_EQ("second", g_message);
    ASSERT(g_again);
    ASSERT(!log_has("stopped: second"));
    PASS();
}

TEST a_missing_symbol_is_named(void) {
    if (setjmp(g_escape) == 0) {
        wl_platform_missing_symbol("D3DDevice_Clear");
        FAIL();
    }
    ASSERT(strstr(g_message, "D3DDevice_Clear is not provided yet") == g_message);
    PASS();
}

TEST a_long_message_is_cut(void) {
    char long_text[600];
    memset(long_text, 'x', sizeof long_text - 1u);
    long_text[sizeof long_text - 1u] = '\0';
    if (setjmp(g_escape) == 0) {
        wl_platform_stop_screen("%s", long_text);
        FAIL();
    }
    ASSERT_EQ(255u, strlen(g_message));
    PASS();
}

SUITE(stop_suite) {
    RUN_TEST(console_starts_and_has_no_framebuffer_on_the_host);
    RUN_TEST(report_goes_to_the_log);
    RUN_TEST(the_first_stop_says_its_message_and_logs_an_error);
    RUN_TEST(a_second_stop_only_waits);
    RUN_TEST(a_missing_symbol_is_named);
    RUN_TEST(a_long_message_is_cut);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(stop_suite);
    GREATEST_MAIN_END();
}
