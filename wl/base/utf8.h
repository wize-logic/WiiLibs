// SPDX-License-Identifier: MIT
// Strict UTF-8 (RFC 3629): shortest form only, no surrogates (U+D800..DFFF), nothing above
// U+10FFFF. Text is length-delimited, never relied on to be NUL-terminated.
// Implementation: wl/base/utf8.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_UTF8_MAX_BYTES 4u

// True when the whole range is well-formed UTF-8 (an embedded NUL is valid).
bool wl_utf8_valid(const char *s, size_t len);

// Decodes the code point at s. Returns the bytes consumed (1..4), or 0 when len == 0 or the
// sequence is malformed, truncated, overlong or out of range (*cp is then unspecified).
size_t wl_utf8_decode(const char *s, size_t len, uint32_t *cp);

// Writes cp as 1..4 bytes to out (no terminator) and returns the count, or 0 for a surrogate
// or a value above 0x10FFFF.
size_t wl_utf8_encode(uint32_t cp, char out[4]);

// Counts code points. WL_ERR_FORMAT if the text is not valid UTF-8.
WlResult wl_utf8_length(const char *s, size_t len, size_t *out_count);
