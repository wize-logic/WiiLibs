// SPDX-License-Identifier: MIT
// Result names and the assert handler (see base.h).
#include <stdio.h>
#include <stdlib.h>

#include "wl/base/base.h"

const char *wl_result_name(WlResult r) {
    switch (r) {
    case WL_OK: return "WL_OK";
    case WL_ERR_ARG: return "WL_ERR_ARG";
    case WL_ERR_IO: return "WL_ERR_IO";
    case WL_ERR_NOMEM: return "WL_ERR_NOMEM";
    case WL_ERR_FORMAT: return "WL_ERR_FORMAT";
    case WL_ERR_RANGE: return "WL_ERR_RANGE";
    case WL_ERR_FAILED: return "WL_ERR_FAILED";
    case WL_ERR_NOT_FOUND: return "WL_ERR_NOT_FOUND";
    case WL_ERR_VERSION: return "WL_ERR_VERSION";
    case WL_ERR_CHECKSUM: return "WL_ERR_CHECKSUM";
    case WL_ERR_STATE: return "WL_ERR_STATE";
    case WL_ERR_UNSUPPORTED: return "WL_ERR_UNSUPPORTED";
    case WL_ERR_LIMIT: return "WL_ERR_LIMIT";
    case WL_ERR_DENIED: return "WL_ERR_DENIED";
    case WL_ERR_AGAIN: return "WL_ERR_AGAIN";
    default: return "unknown";
    }
}

static WlAssertHandler g_assert_handler;

static void default_assert_handler(const char *expr, const char *file, int line) {
    wl_log_write(WL_LOG_LEVEL_ERROR, "assert", "%s:%d: assertion failed: %s", file, line, expr);
    abort();
}

void wl_assert_set_handler(WlAssertHandler handler) {
    g_assert_handler = handler;
}

void wl_assert_fail(const char *expr, const char *file, int line) {
    if (g_assert_handler != NULL) {
        g_assert_handler(expr, file, line);
        return;
    }
    default_assert_handler(expr, file, line);
}
