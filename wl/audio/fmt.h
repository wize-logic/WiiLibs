// SPDX-License-Identifier: MIT
// Readers for the PWST (stream) and PWSF (sample) containers: small big-endian wrappers around
// DSP-ADPCM data, laid out in wl/audio/README.md. Every field is bounds-checked against the
// payload size: the files are untrusted. Big-endian, byte-wise loads.
#pragma once

#include "wl/base/base.h"

#define WL_PWST_HEADER_BYTES 0x30u
#define WL_PWST_CHANNEL_BYTES 48u
#define WL_PWST_MAX_PROBE (WL_PWST_HEADER_BYTES + 2u * WL_PWST_CHANNEL_BYTES) // 0x90: bytes to read to parse
#define WL_PWSF_HEADER_BYTES 0x40u
#define WL_PWSF_PROBE (WL_PWSF_HEADER_BYTES + WL_PWST_CHANNEL_BYTES)

#define WL_PWST_FLAG_LOOP 1u
#define WL_PWST_FLAG_LOOP_MID_FRAME 2u

typedef struct {
    int16_t coef[16];
    uint16_t init_ps;
    int16_t init_h1, init_h2;
    uint16_t loop_ps;
    int16_t loop_h1, loop_h2;
} WlDspChannelInfo;

typedef struct {
    uint16_t flags;
    uint8_t channels; // 1 or 2
    uint32_t rate;
    uint32_t samples; // per channel, a multiple of 14
    uint32_t loop_start, loop_end; // sample indices; end inclusive; 0 without a loop
    uint32_t data_off, data_size; // data_size covers all channels, one after the other
    WlDspChannelInfo ch[2];
} WlStreamInfo;

// `bytes` holds at least the first min(payload_size, WL_PWST_MAX_PROBE) bytes of the payload.
WlResult wl_pwst_parse(const uint8_t *bytes, size_t n, uint64_t payload_size, WlStreamInfo *out);

typedef struct {
    uint16_t flags;
    uint8_t codec; // 0 DSP-ADPCM, 1 PCM16 big-endian
    uint8_t bus, priority;
    uint32_t rate, samples;
    uint32_t loop_start, loop_end; // inclusive end
    uint32_t data_off, data_size;
    int16_t pitch_cents;
    uint16_t volume; // 256 = 1.0
    int8_t pan;
    uint16_t play_ms; // 0 = whole sample
    uint32_t duration_ms;
    WlDspChannelInfo ch;
} WlSfxInfo;

WlResult wl_pwsf_parse(const uint8_t *bytes, size_t n, WlSfxInfo *out);
