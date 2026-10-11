// SPDX-License-Identifier: MIT
// RWSD reader: the NW4R wave sound data file of a BRSAR archive (big-endian). A wave sound (BRSAR
// sound type 3) names an RWSD file and one of its wave sound entries (the sound's info0); the
// entry's note points at a wave of the RWSD, whose DSP-ADPCM frames live in the wave data area of
// the group holding the file (wl_brsar_file_where / wl_brsar_read_file). This resolves one wave
// sound to its audio and playback parameters; the frames are left untouched (decode them with
// wl_brstm_dsp_decode). Only mono DSP-ADPCM waves are supported. Layout notes: README.md. Inputs
// are untrusted: every offset, count and size is checked. No code from any decomp or tool.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_RWSD_ADPCM_INFO 0x30u /* size of the DSP channel block (coefficients, gain, ps, history) */

typedef struct {
    uint32_t wsd, wave;           /* wave sound entry and wave index inside the RWSD */
    uint32_t rate, samples;
    int loop;
    int loop_mid;                 /* the loop starts inside a frame (not on a 14-sample boundary) */
    uint32_t loop_start, loop_end; /* sample indices, 0 without a loop; the end is the last sample */
    double pitch;                 /* the entry's pitch times the note's pitch (1.0 = as recorded) */
    int32_t pitch_cents;          /* round(1200 * log2(pitch)), clamped to +-32767 */
    uint8_t info_pan, note_pan;   /* 0..127, 64 = centre */
    int32_t pan;                  /* both pans combined: round(((info - 64) + (note - 64)) * 127 / 64), -127..127 */
    uint8_t note_volume;          /* 0..127 */
    uint8_t note_key;             /* original key of the note (60 = middle C) */
    uint32_t duration_ms;
    /* DSP state from the ADPCM info block */
    int16_t coef[16];
    uint16_t init_ps, loop_ps;
    int16_t init_hist[2], loop_hist[2];
    const uint8_t *adpcm_info;    /* the WL_RWSD_ADPCM_INFO-byte DSP block inside the RWSD bytes */
    const uint8_t *frames;        /* the audio inside the wave data: frame_count 8-byte frames */
    uint32_t frame_count;
    size_t frames_size;
} WlRwsdWave;

/* Resolves wave sound entry `wsd` of the RWSD (rwsd, rwsd_len) against its wave data (wave,
 * wave_len). The pointers in *out point into those buffers. WL_ERR_FORMAT for a malformed file,
 * WL_ERR_RANGE for an entry or wave index or a data range outside the buffers, WL_ERR_UNSUPPORTED
 * for a wave that is not mono DSP-ADPCM stored at an offset. */
WlResult wl_rwsd_wave_sound(const uint8_t *rwsd, size_t rwsd_len, uint32_t wsd, const uint8_t *wave,
                            size_t wave_len, WlRwsdWave *out);

/* Combined linear volume, 256 = 1.0: round(256 * sound_volume / 127 * note_volume / 127), both
 * clamped to 127 (sound_volume is the BRSAR sound's volume byte). */
uint32_t wl_rwsd_volume(uint8_t sound_volume, uint8_t note_volume);
