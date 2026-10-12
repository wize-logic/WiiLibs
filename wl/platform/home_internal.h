// SPDX-License-Identifier: MIT
// Shared by home_menu.c and the files that draw it: not part of the API.
#pragma once

#include "wl/base/base.h"

// `templ` with `{port}` and `{percent}` put in, into `out` of `size` bytes (cut off if it does not
// fit, always terminated); the length written.
unsigned wl_home_fill_template(const char *templ, int port, int percent, char *out, unsigned size);
