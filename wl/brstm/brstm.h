// SPDX-License-Identifier: MIT
// BRSTM reader: the NW4R stream container "RSTM" (HEAD, ADPC, DATA chunks, big-endian) holding
// DSP-ADPCM audio of one or two channels. The audio is block-interleaved (one block per channel in
// turn, the last block shorter); the readers here de-interleave it into one plain frame stream per
// channel and decode it, carrying the predictor history across blocks. Layout notes: README.md.
// Inputs are untrusted: every offset, count and size is checked before use. No code from any decomp
// or tool.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"
#include "wl/brstm/dsp.h"

#define WL_BRSTM_MAGIC 0x5253544Du /* "RSTM" */
#define WL_BRSTM_MAX_SAMPLES 0x10000000u

/* Parsed RSTM header. Sample indices are per channel. */
typedef struct {
    uint32_t rate;
    uint32_t samples;       /* real length */
    uint32_t channels;      /* 1 or 2 */
    int loop;
    uint32_t loop_start;    /* sample index, valid when loop; the loop runs to the last sample */
    uint32_t data_off;      /* absolute offset of the first audio block */
    uint32_t blocks;        /* number of blocks per channel */
    uint32_t block_size;    /* bytes per channel block */
    uint32_t block_samples; /* samples per full block */
    uint32_t last_size;     /* ADPCM bytes of the last block (the partial frame is cut short) */
    uint32_t last_samples;
    uint32_t last_padded;   /* bytes each channel occupies in the last block */
    uint32_t adpc_entries;  /* seek-table entries (checked for size only; the table is not used) */
    int16_t coef[2][16];
    uint16_t init_ps[2], loop_ps[2];
    int16_t init_hist[2][2], loop_hist[2][2];
} WlBrstm;

/* Parses and validates (WL_ERR_FORMAT for anything that is not a well-formed DSP-ADPCM RSTM of one
 * or two channels, WL_ERR_RANGE for offsets or sizes outside the file). */
WlResult wl_brstm_parse(const uint8_t *file, size_t len, WlBrstm *out);

/* Frames (8 bytes, 14 samples) per channel of the de-interleaved stream. */
#define WL_BRSTM_FRAMES(samples) WL_DSP_FRAMES(samples)

/* One channel's frames, WL_BRSTM_FRAMES(samples) * 8 bytes into out. The partial last frame keeps
 * its header and the bytes holding real samples; the rest of it is zero. The frames are copied
 * byte for byte (no re-encode). */
WlResult wl_brstm_channel_frames(const WlBrstm *b, const uint8_t *file, size_t len, uint32_t ch,
                                 uint8_t *out);

/* Decodes channel ch straight from the blocks of the file, carrying the predictor history across
 * blocks, to b->samples samples (out holds that many). WL_ERR_FORMAT for a frame whose predictor
 * index is above 7. */
WlResult wl_brstm_decode(const WlBrstm *b, const uint8_t *file, size_t len, uint32_t ch,
                         int16_t *out);

/* The DSP state a player needs, taken from the audio itself rather than from the header. */
typedef struct {
    uint16_t init_ps[2];      /* header byte of each channel's first frame */
    uint16_t loop_ps[2];      /* header byte of the frame holding the loop start (when loop) */
    int16_t loop_hist[2][2];  /* the two decoded samples before the loop start: [0] newest */
} WlBrstmState;

typedef struct {
    int ps_fixed;       /* a channel's initial predictor/scale in the header differed from its first frame */
    int loop_ps_bad;    /* the loop frame header in the stream header differed from the data */
    int loop_hist_bad;  /* the loop history in the stream header differed from the decoded audio */
} WlBrstmNotes;

/* Derives the initial and loop state of every channel from the audio (decoding it when the stream
 * loops) and reports in notes (optional) where the stream header disagrees. WL_ERR_FORMAT for a
 * frame with a predictor index above 7, WL_ERR_NOMEM. */
WlResult wl_brstm_state(const WlBrstm *b, const uint8_t *file, size_t len, WlBrstmState *out,
                        WlBrstmNotes *notes);
