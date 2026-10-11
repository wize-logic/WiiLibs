// SPDX-License-Identifier: MIT
// WiiLibs basics shared by every module: result codes, asserts, logging, byte and string views.
//
// Only freestanding headers (<stdint.h>, <stddef.h>, <stdbool.h>) are included here, so this
// header compiles on host GCC/Clang and devkitPPC alike. Implementation: wl/base/base.c, wl/base/log.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------------------------
// Compiler helpers
// ---------------------------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define WL_PRINTF_FMT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#define WL_NORETURN __attribute__((noreturn))
#define WL_UNUSED __attribute__((unused))
#else
#define WL_PRINTF_FMT(fmt_index, first_arg)
#define WL_NORETURN
#define WL_UNUSED
#endif

#define WL_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define WL_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------------------------

// 0 = success, negative = error. Functions that can fail return WlResult and write their
// outputs through pointer parameters; outputs are unspecified when the result is an error.
typedef int WlResult;

#define WL_OK 0

// Common codes, usable by every module.
#define WL_ERR_ARG (-1)          // invalid argument (programmer error caught at the API)
#define WL_ERR_IO (-2)           // read or write failed (reported by an IO callback)
#define WL_ERR_NOMEM (-3)        // arena or heap exhausted
#define WL_ERR_FORMAT (-4)       // malformed external data (file format, wire message, save)
#define WL_ERR_RANGE (-5)        // value or offset out of range
#define WL_ERR_FAILED (-6)       // generic failure; prefer a specific code
#define WL_ERR_NOT_FOUND (-7)    // lookup missed (asset id, file name, entry)
#define WL_ERR_VERSION (-8)      // unsupported format or protocol version
#define WL_ERR_CHECKSUM (-9)     // CRC or hash mismatch
#define WL_ERR_STATE (-10)       // call not valid in the object's current state
#define WL_ERR_UNSUPPORTED (-11) // feature not implemented yet (or not on this platform)
#define WL_ERR_LIMIT (-12)       // fixed capacity reached (queue full, too many players)
#define WL_ERR_DENIED (-13)      // a request was refused (permission, illegal operation)
#define WL_ERR_AGAIN (-14)       // not ready yet; retry later (async IO, pending reply)

// Module-specific codes live in reserved blocks of 100 below these bases, e.g.
// WL_ERR_BASE_HOST - 1. Common codes above are always preferred when one fits. Applications
// using WiiLibs should allocate their own blocks from -10000 down.
#define WL_ERR_BASE_HOST (-800)
#define WL_ERR_BASE_FORMAT (-900)
#define WL_ERR_BASE_PLATFORM (-1000)
#define WL_ERR_BASE_AUDIO (-1100)
#define WL_ERR_BASE_GFX (-1200)

// Static, never-NULL name for a result code ("WL_ERR_FORMAT", "unknown"); for logs only.
const char *wl_result_name(WlResult r);

// ---------------------------------------------------------------------------------------------
// Asserts
// ---------------------------------------------------------------------------------------------

// WL_ASSERT is for programmer errors only; external input (files, wire, saves) is validated and
// returns an error instead. Asserts are on unless WL_ASSERTS_OFF is defined.
typedef void (*WlAssertHandler)(const char *expr, const char *file, int line);

// Calls the installed handler (default: log and abort). Debug handlers do not return; a
// release handler may log and return so the caller can try to recover.
void wl_assert_fail(const char *expr, const char *file, int line);

// Process-wide; set once at startup before any other WiiLibs call.
void wl_assert_set_handler(WlAssertHandler handler);

#if defined(WL_ASSERTS_OFF)
#define WL_ASSERT(cond) ((void)0)
#else
#define WL_ASSERT(cond) ((cond) ? (void)0 : wl_assert_fail(#cond, __FILE__, __LINE__))
#endif

// ---------------------------------------------------------------------------------------------
// Logging: paths, sizes, ids and counts are the intended payload; avoid logging secrets.
// ---------------------------------------------------------------------------------------------

typedef enum {
    WL_LOG_LEVEL_ERROR = 0,
    WL_LOG_LEVEL_WARN,
    WL_LOG_LEVEL_INFO,
    WL_LOG_LEVEL_DEBUG
} WlLogLevel;

// Receives one formatted line without a trailing newline. Called on the logging thread; sinks
// must be thread-safe (the client's ring buffer, the server's structured log).
typedef void (*WlLogSink)(void *user, WlLogLevel level, const char *tag, const char *line);

// Process-wide; set once at startup. Without a sink, the host build prints to stderr; the Wii build drops lines.
void wl_log_set_sink(WlLogSink sink, void *user);
void wl_log_set_level(WlLogLevel level);
void wl_log_write(WlLogLevel level, const char *tag, const char *fmt, ...) WL_PRINTF_FMT(3, 4);

#define WL_LOG(level, tag, ...) wl_log_write(level, tag, __VA_ARGS__)
#define WL_LOG_E(tag, ...) wl_log_write(WL_LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#define WL_LOG_W(tag, ...) wl_log_write(WL_LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define WL_LOG_I(tag, ...) wl_log_write(WL_LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define WL_LOG_D(tag, ...) wl_log_write(WL_LOG_LEVEL_DEBUG, tag, __VA_ARGS__)

// ---------------------------------------------------------------------------------------------
// Views and small helpers
// ---------------------------------------------------------------------------------------------

// Borrowed bytes. Lifetime is documented by whoever hands one out.
typedef struct {
    const uint8_t *data;
    uint32_t size;
} WlBytes;

// Borrowed UTF-8 text, not NUL-terminated. 
typedef struct {
    const char *ptr;
    uint16_t len;
} WlStr;

// Fixed-point number, 20.12 (1.0 = 4096), as the DS uses for positions and heights.
typedef int32_t WlFx32;
#define WL_FX32_ONE 4096

// Milliseconds. Core never reads a clock: time enters as parameters (ARCHITECTURE section 4).
typedef uint32_t WlDurationMs;
typedef uint64_t WlTimeMs;

// True when [off, off + len) lies inside a buffer of `size` bytes, without overflow.
static inline bool wl_range_ok(uint64_t off, uint64_t len, uint64_t size) {
    return off <= size && len <= size - off;
}
