// SPDX-License-Identifier: MIT
// Minimal JSON reader for tool configuration files: objects, arrays, strings, booleans, null and
// integers (no fractions or exponents). Bounded depth; input is treated as hostile.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/host/base.h"

typedef enum { WL_JSON_NULL = 0, WL_JSON_BOOL, WL_JSON_INT, WL_JSON_STRING, WL_JSON_ARRAY, WL_JSON_OBJECT } WlJsonType;

typedef uint32_t WlJsonRef;
#define WL_JSON_NONE ((WlJsonRef)0xFFFFFFFFu)

typedef struct {
    uint8_t type;
    int64_t num; /* integer value, or 0/1 for booleans */
    uint32_t str_off, str_len; /* string value */
    uint32_t key_off, key_len; /* member name when the parent is an object */
    uint32_t first, last, next; /* children and sibling */
    uint32_t count;             /* number of children */
} WlJsonNode;

typedef struct {
    WlJsonNode *nodes;
    size_t count, cap;
    char *pool;
    size_t pool_len, pool_cap;
} WlJson;

WlResult wl_json_parse(WlJson *doc, const char *text, size_t len);
void wl_json_free(WlJson *doc);

WlJsonRef wl_json_root(const WlJson *doc);
WlJsonType wl_json_type(const WlJson *doc, WlJsonRef ref);
/* Object member by name; WL_JSON_NONE when absent or ref is not an object. */
WlJsonRef wl_json_get(const WlJson *doc, WlJsonRef obj, const char *key);
size_t wl_json_len(const WlJson *doc, WlJsonRef ref);
/* Array element (or object member value) by index; WL_JSON_NONE when out of range. */
WlJsonRef wl_json_at(const WlJson *doc, WlJsonRef ref, size_t index);
/* NULL when ref is not a string. The pointer is valid until wl_json_free. */
const char *wl_json_str(const WlJson *doc, WlJsonRef ref);
size_t wl_json_str_len(const WlJson *doc, WlJsonRef ref);
WlResult wl_json_int(const WlJson *doc, WlJsonRef ref, int64_t *out);
WlResult wl_json_bool(const WlJson *doc, WlJsonRef ref, int *out);
