// SPDX-License-Identifier: MIT
// Nitro file-name table (FNT) walker shared by the ROM file system and NARC's BTNF.
// Format: GBATEK "DS Cartridge Nitro File System"; summary in README.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_FNT_MAX_DEPTH 32
#define WL_FNT_MAX_PATH 1024

/* Called once per named file, in directory order; a non-WL_OK result stops the walk. */
typedef WlResult (*WlFntFileFn)(void *ud, const char *path, uint32_t file_id);

/* Walks the whole tree. Every directory may be entered once (cycles are errors) and the depth
 * is capped. */
WlResult wl_fnt_walk(const uint8_t *fnt, size_t fnt_size, WlFntFileFn fn, void *ud);
