// SPDX-License-Identifier: MIT
// Nintendo LZ decoders: LZ10 / LZ11 (forward) and the backward "BLZ" used for the ARM9 binary and
// overlays. Formats: GBATEK "BIOS Decompression Functions" (LZ77) and the DS code-compression
// footer; LZ11 is the extended-length variant. Inputs are untrusted: every read is bounds-checked
// and failures return an error code.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

/* Decodes an LZ10 (0x10) or LZ11 (0x11) stream. *out is malloc'd (free it) on WL_OK.
 * max_out caps the declared output size (WL_ERR_RANGE when exceeded). */
WlResult wl_lz_decode(const uint8_t *src, size_t n, size_t max_out, uint8_t **out, size_t *out_len);

/* True when the first byte is 0x10 or 0x11 and the declared size is plausible for n input bytes. */
int wl_lz_looks_valid(const uint8_t *src, size_t n);

/* True when the last 8 bytes form a plausible BLZ footer (a non-zero size increment). */
int wl_blz_footer_ok(const uint8_t *src, size_t n);

/* Decodes a backward-LZ buffer (footer at the end). *out is malloc'd on WL_OK. */
WlResult wl_blz_decode(const uint8_t *src, size_t n, size_t max_out, uint8_t **out, size_t *out_len);
