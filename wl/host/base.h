// SPDX-License-Identifier: MIT
// Host-tool basics: the common result codes and logging come from wl/base/base.h; the codes
// below are specific to host tools (block WL_ERR_BASE_HOST and below).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_ERR_ENCRYPTED (WL_ERR_BASE_HOST - 1)   /* image content is encrypted */
#define WL_ERR_NO_TOOL (WL_ERR_BASE_HOST - 2)     /* external tool (e.g. dolphin-tool) not found */
#define WL_ERR_TOOL_FAILED (WL_ERR_BASE_HOST - 3) /* external tool exited with an error */
#define WL_ERR_EMPTY (WL_ERR_BASE_HOST - 4)       /* tool reported success, wrote nothing */

/* malloc'd copy of a string; NULL when out of memory. */
char *wl_strdup(const char *s);
