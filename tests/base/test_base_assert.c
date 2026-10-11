// SPDX-License-Identifier: MIT
// WL_ASSERT, the assert handler hook, result names and the log sink.
#include <string.h>

#include "wl/base/base.h"
#include "greatest.h"

static int g_calls;
static char g_expr[64];
static int g_line;
static const char *g_file;

static void recording_handler(const char *expr, const char *file, int line) {
    g_calls++;
    strncpy(g_expr, expr, sizeof g_expr - 1);
    g_file = file;
    g_line = line;
}

TEST handler_called_on_failure(void) {
    g_calls = 0;
    wl_assert_set_handler(recording_handler);
    WL_ASSERT(1 + 1 == 2);
    ASSERT_EQ(0, g_calls);
    int line = __LINE__ + 1;
    WL_ASSERT(1 + 1 == 3);
    ASSERT_EQ(1, g_calls);
    ASSERT_STR_EQ("1 + 1 == 3", g_expr);
    ASSERT_EQ(line, g_line);
    ASSERT(strstr(g_file, "test_base_assert.c") != NULL);
    wl_assert_fail("direct", "f.c", 7);
    ASSERT_EQ(2, g_calls);
    ASSERT_STR_EQ("direct", g_expr);
    wl_assert_set_handler(NULL);
    PASS();
}

TEST result_names(void) {
    ASSERT_STR_EQ("WL_OK", wl_result_name(WL_OK));
    ASSERT_STR_EQ("WL_ERR_FORMAT", wl_result_name(WL_ERR_FORMAT));
    ASSERT_STR_EQ("WL_ERR_AGAIN", wl_result_name(WL_ERR_AGAIN));
    ASSERT_STR_EQ("unknown", wl_result_name(-12345));
    PASS();
}

static char g_line_buf[128];
static int g_level_seen;

static void sink(void *user, WlLogLevel level, const char *tag, const char *line) {
    (void)user;
    g_level_seen = (int)level;
    strncpy(g_line_buf, tag, 20);
    strcat(g_line_buf, "|");
    strncat(g_line_buf, line, 80);
}

TEST log_sink_and_level(void) {
    wl_log_set_sink(sink, NULL);
    wl_log_set_level(WL_LOG_LEVEL_WARN);
    g_line_buf[0] = '\0';
    WL_LOG_I("t", "hidden %d", 1);
    ASSERT_STR_EQ("", g_line_buf);
    WL_LOG_E("t", "shown %d", 2);
    ASSERT_STR_EQ("t|shown 2", g_line_buf);
    ASSERT_EQ((int)WL_LOG_LEVEL_ERROR, g_level_seen);
    wl_log_set_sink(NULL, NULL);
    wl_log_set_level(WL_LOG_LEVEL_INFO);
    PASS();
}

TEST range_helper(void) {
    ASSERT(wl_range_ok(0, 10, 10));
    ASSERT(!wl_range_ok(1, 10, 10));
    ASSERT(!wl_range_ok(UINT64_MAX, 2, 10));
    PASS();
}

SUITE(assert_suite) {
    RUN_TEST(handler_called_on_failure);
    RUN_TEST(result_names);
    RUN_TEST(log_sink_and_level);
    RUN_TEST(range_helper);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(assert_suite);
    GREATEST_MAIN_END();
}
