// SPDX-License-Identifier: MIT
// The MCPX APU and AC'97 models (wl/xboxapu) driven as DirectSound drives them: voices set up
// through the front end's methods in a synthetic guest memory, played, and their mixbins, stereo
// output, positions, notifiers and interrupts checked against values worked out here.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/audio/adpcm.h"
#include "wl/base/endian.h"
#include "wl/xboxapu/ac97.h"
#include "wl/xboxapu/apu.h"

// ---- A guest memory and the setup DirectSound makes -------------------------------------------

#define MEM_BYTES (2u << 20)
#define VPV 0x010000u  // voice records
#define SGE 0x020000u  // buffer page table
#define SSL 0x021000u  // segment table
#define NOTE 0x022000u // notifiers
#define GPS 0x027000u  // the GP's scratch page table
#define DATA 0x040000u // sample pages (the page table maps them in reverse order)
#define DATA_PAGES 64u
#define STREAM 0x100000u // stream segments (guest-physical, contiguous)

static uint8_t *g_mem;

static uint8_t *phys_ptr(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    return phys < MEM_BYTES && bytes <= MEM_BYTES - phys ? g_mem + phys : NULL;
}

static void w32(WlXboxApu *apu, uint32_t off, uint32_t v) {
    wl_xbox_apu_write(apu, off, v, 4);
}

static void m(WlXboxApu *apu, uint32_t method, uint32_t arg) {
    w32(apu, 0x20000u + method, arg);
}

// Linear offset `lin` of the buffer page space: page i of it is DATA page (DATA_PAGES - 1 - i).
static uint8_t *lin_ptr(uint32_t lin) {
    const uint32_t page = DATA + (DATA_PAGES - 1u - (lin >> 12)) * 0x1000u;
    return g_mem + page + (lin & 0xFFFu);
}

static WlXboxApu *setup(void) {
    if (g_mem == NULL) {
        g_mem = malloc(MEM_BYTES);
    }
    memset(g_mem, 0, MEM_BYTES);
    WlXboxApuConfig cfg;
    wl_xbox_apu_config_default(&cfg);
    cfg.phys_ptr = phys_ptr;
    WlXboxApu *apu = wl_xbox_apu_create(&cfg);
    w32(apu, 0x202C, VPV);
    w32(apu, 0x2030, SGE);
    w32(apu, 0x2034, SSL);
    w32(apu, 0x115C, NOTE);
    w32(apu, 0x1004, 0xFFu);  // every interrupt
    w32(apu, 0x1504, 0x8000); // idle voices trap
    for (uint32_t i = 0; i < DATA_PAGES; i++) {
        m(apu, 0x804, i);
        m(apu, 0x808, DATA + (DATA_PAGES - 1u - i) * 0x1000u);
    }
    w32(apu, 0x1100, 0x0);  // the front end runs
    w32(apu, 0x2000, 0x0F); // the counter (and the processor) runs
    return apu;
}

// A buffer voice as DirectSound sets one up. fmt: the format word's high bits (stereo, sizes);
// vbin/vola: targets and volumes.
static void voice(WlXboxApu *apu, uint32_t v, uint32_t fmt, uint32_t base, uint32_t lbo,
                  uint32_t ebo, int16_t pitch, uint32_t vola, uint32_t volb) {
    m(apu, 0x2F8, v);
    m(apu, 0x300, 0x14830820u);        // targets 0..5 -> bins 0..5
    m(apu, 0x304, fmt | 6u | 7u << 5); // targets 6, 7 -> bins 6, 7
    m(apu, 0x360, vola);
    m(apu, 0x364, volb);
    m(apu, 0x368, 0xFFFFFFFFu);
    m(apu, 0x37C, (uint32_t)(uint16_t)pitch << 16);
    m(apu, 0x3A0, base);
    m(apu, 0x3A4, lbo);
    m(apu, 0x3DC, ebo);
}

static void play(WlXboxApu *apu, uint32_t v, uint32_t env) {
    m(apu, 0x120, 0x1FFFFu); // at the head of the 2D list
    m(apu, 0x124, v | env);
}

#define VOL(a, b)                                                                                  \
    ((uint32_t)(a) << 4 | (uint32_t)(b) << 20 | 0x000F000Fu) // targets 0, 1 (6 and 7 silent)
#define FMT_S16 (1u << 28 | 1u << 30)
#define FMT_U8 0u
#define FMT_S32 (3u << 28 | 3u << 30)
#define FMT_S24 (2u << 28 | 3u << 30)
#define FMT_ADPCM (2u << 30)
#define FMT_STEREO (1u << 27)
#define FMT_LOOP (1u << 25)
#define FMT_STREAM (1u << 24)
#define FMT_PERSIST (1u << 23)

static int16_t ramp(uint32_t i) {
    return (int16_t)((int32_t)(i * 37u % 2000u) - 1000);
}

static uint8_t notifier(uint32_t v, uint32_t which) {
    return g_mem[NOTE + (2u + v * 4u + which) * 16u + 15u];
}

static uint32_t rec32(uint32_t v, uint32_t off) {
    return wl_rd_le32(g_mem + VPV + v * 0x80u + off);
}

// ---- Buffers -----------------------------------------------------------------------------------

TEST pcm16_at_unity_pitch_reaches_its_bins_exactly(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 6000; i++) { // three pages: the page table is followed
        wl_wr_le16(lin_ptr(i * 2u), (uint16_t)ramp(i));
    }
    voice(apu, 100, FMT_S16, 0, 0, 5999, 0, VOL(0, 0xFFF), 0xFFFFFFFFu);
    play(apu, 100, 0);
    int16_t out[64 * 2 * 32];
    ASSERT_EQ(64u, wl_xbox_apu_run(apu, 64, out));
    // the volume ramps up over the first frame; then sample i is output i
    for (uint32_t i = 32; i < 64u * 32u; i++) {
        ASSERT_EQ_FMT((int)ramp(i), (int)out[2u * i], "%d");
        ASSERT_EQ(0, out[2u * i + 1u]);
    }
    const int32_t *bins = wl_xbox_apu_mixbins(apu);
    ASSERT_EQ((int32_t)ramp(63u * 32u + 5u), bins[0 * 32 + 5]);
    ASSERT_EQ(0, bins[1 * 32 + 5]);
    ASSERT_EQ(63u * 32u + 32u, rec32(100, 0x58) & 0xFFFFFFu); // CBO: where s0 is
    wl_xbox_apu_destroy(apu);
    PASS();
}

TEST volume_is_sixtyfourths_of_a_decibel(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 4096; i++) {
        wl_wr_le16(lin_ptr(i * 2u), 16000);
    }
    voice(apu, 70, FMT_S16, 0, 0, 4095, 0, VOL(0x180, 0x300), 0xFFFFFFFFu); // -6 dB, -12 dB
    play(apu, 70, 0);
    int16_t out[4 * 64];
    wl_xbox_apu_run(apu, 4, out);
    // 16000 x 10^(-6/20) = 8019, x 10^(-12/20) = 4019 (fixed-point: within 1%)
    ASSERT_IN_RANGE(8019, out[2 * 100], 80);
    ASSERT_IN_RANGE(4019 + 8019 * 0, out[2 * 100 + 1], 40);
    wl_xbox_apu_destroy(apu);
    PASS();
}

TEST formats_8_24_32_bit_and_stereo(void) {
    WlXboxApu *apu = setup();
    // voice 1: unsigned 8-bit mono; voice 2: 24-bit in 32-bit words, stereo; voice 3: 32-bit
    for (uint32_t i = 0; i < 64; i++) {
        lin_ptr(i)[0] = (uint8_t)(128 + i); // 8-bit at offset 0
        wl_wr_le32(lin_ptr(0x1000u + i * 8u),
                   (uint32_t)(i * 256) << 8 & 0xFFFFFFu); // L: i*256 in 24 bits
        wl_wr_le32(lin_ptr(0x1000u + i * 8u + 4u), (uint32_t)(-(int32_t)i * 256 * 256) & 0xFFFFFFu);
        wl_wr_le32(lin_ptr(0x2000u + i * 4u), (uint32_t)((int32_t)i * 100) << 16);
    }
    voice(apu, 1, FMT_U8, 0, 0, 63, 0, VOL(0, 0xFFF), 0xFFFFFFFFu);
    play(apu, 1, 0);
    voice(apu, 2, FMT_S24 | FMT_STEREO, 0x1000, 0, 63, 0, VOL(0xFFF, 0), 0xFFFFFFFFu); // R only
    play(apu, 2, 0);
    voice(apu, 3, FMT_S32, 0x2000, 0, 63, 0, 0xFFFFFFFFu, VOL(0, 0xFFF)); // bin 2 (centre)
    play(apu, 3, 0);
    int16_t out[2 * 64];
    wl_xbox_apu_run(apu, 2, out);
    const int32_t *bins = wl_xbox_apu_mixbins(apu);
    for (uint32_t i = 0; i < 32; i++) {
        const uint32_t s = 32 + i;                        // the second frame: no ramp
        ASSERT_EQ((int32_t)(s * 256), bins[0 * 32 + i]);  // U8: (b - 128) << 8
        ASSERT_EQ(-(int32_t)(s * 256), bins[1 * 32 + i]); // the right channel of voice 2
        ASSERT_EQ((int32_t)(s * 100), bins[2 * 32 + i]);  // S32: the high half
    }
    wl_xbox_apu_destroy(apu);
    PASS();
}

TEST pitch_halves_and_doubles_the_rate(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 4096; i++) {
        wl_wr_le16(lin_ptr(i * 2u), (uint16_t)(int16_t)(i * 8));
    }
    voice(apu, 10, FMT_S16, 0, 0, 4095, -4096, VOL(0, 0xFFF), 0xFFFFFFFFu); // 24 kHz into bin 0
    play(apu, 10, 0);
    voice(apu, 11, FMT_S16, 0, 0, 4095, 4096, VOL(0xFFF, 0), 0xFFFFFFFFu); // 96 kHz into bin 1
    play(apu, 11, 0);
    int16_t out[4 * 64];
    wl_xbox_apu_run(apu, 4, out);
    for (uint32_t i = 32; i < 128; i++) {
        ASSERT_EQ((int)(i * 4), out[2 * i]);      // half: source i/2, interpolated
        ASSERT_EQ((int)(i * 16), out[2 * i + 1]); // double: every other sample
    }
    // 44.1 kHz: pitch 4096 log2(44100/48000) = -501.4, so -501: 0.9188 source samples a sample
    wl_xbox_apu_destroy(apu);
    PASS();
}

TEST a_loop_wraps_to_its_start_and_a_one_shot_ends(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 40; i++) {
        wl_wr_le16(lin_ptr(i * 2u), (uint16_t)(int16_t)(i + 1));
    }
    voice(apu, 20, FMT_S16 | FMT_LOOP, 0, 8, 39, 0, VOL(0, 0xFFF), 0xFFFFFFFFu);
    play(apu, 20, 0);
    voice(apu, 21, FMT_S16, 0, 0, 39, 0, VOL(0xFFF, 0), 0xFFFFFFFFu);
    play(apu, 21, 0);
    w32(apu, 0x1504, 0); // no idle-voice traps here
    int16_t out[4 * 64];
    ASSERT_EQ(4u, wl_xbox_apu_run(apu, 4, out));
    // left: 1..40, then 9..40 again
    for (uint32_t i = 32; i < 128; i++) {
        const uint32_t s = i < 40 ? i : 8 + (i - 40) % 32;
        ASSERT_EQ((int)(s + 1), out[2 * i]);
    }
    // right: 1..40 then silence; the voice ended, its notifier says so, an interrupt is up
    ASSERT_EQ(33, out[2 * 32 + 1]);
    ASSERT_EQ(40, out[2 * 39 + 1]);
    ASSERT_EQ(0, out[2 * 40 + 1]);
    ASSERT_EQ(0u, rec32(21, 0x54) & 0x00200000u);
    ASSERT_EQ(39u, rec32(21, 0x58) & 0xFFFFFFu);
    ASSERT_EQ(1, notifier(21, 0));
    ASSERT(wl_xbox_apu_irq(apu));
    ASSERT_EQ(0x61u, wl_xbox_apu_read(apu, 0x1000, 4) & 0x61u); // global, notify, voice
    WlXboxApuStats st;
    wl_xbox_apu_stats(apu, &st);
    ASSERT_EQ(1u, st.voices_ended);
    wl_xbox_apu_destroy(apu);
    PASS();
}

TEST an_idle_voice_traps_the_front_end_until_the_guest_answers(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 16; i++) {
        wl_wr_le16(lin_ptr(i * 2u), 1000);
    }
    voice(apu, 30, FMT_S16, 0, 0, 15, 0, VOL(0, 0xFFF), 0xFFFFFFFFu);
    play(apu, 30, 0);
    int16_t out[8 * 64];
    // it ends in frame 0 (16 samples); frame 1 finds it idle in its list: a requested trap
    // naming it, and the run stops there for the guest's handler
    ASSERT_EQ(2u, wl_xbox_apu_run(apu, 8, out));
    ASSERT_EQ(0xFE0u, wl_xbox_apu_read(apu, 0x1100, 4) & 0xFE0u);
    ASSERT_EQ(0x8000u, wl_xbox_apu_read(apu, 0x1300, 4));
    ASSERT_EQ(30u, wl_xbox_apu_read(apu, 0x1304, 4));
    ASSERT_EQ(0x10u, wl_xbox_apu_read(apu, 0x1000, 4) & 0x10u);
    ASSERT_FALSE(wl_xbox_apu_running(apu));
    // trapped: frames are silence and no new trap comes
    ASSERT_EQ(8u, wl_xbox_apu_run(apu, 8, out));
    WlXboxApuStats st;
    wl_xbox_apu_stats(apu, &st);
    ASSERT_EQ(30u, st.trap_voice);
    ASSERT_EQ(1u, st.trap_repeats);
    // a handler that lets the front end run with the voice still in its list: the next frame
    // traps on it again (counted: the runtime logs a voice the guest keeps leaving there)
    w32(apu, 0x1100, 0x00);
    w32(apu, 0x1000, 0xFFFFFFFFu);
    ASSERT_EQ(1u, wl_xbox_apu_run(apu, 8, out));
    wl_xbox_apu_stats(apu, &st);
    ASSERT_EQ(30u, st.trap_voice);
    ASSERT_EQ(2u, st.trap_repeats);
    ASSERT_EQ(2u, st.traps);
    // DirectSound takes it out of the list (here: empties the 2D list) and lets the front end run
    w32(apu, 0x2054, 0xFFFF);
    w32(apu, 0x1100, 0x80);
    w32(apu, 0x1100, 0x00);
    w32(apu, 0x1000, 0xFFFFFFFFu);
    ASSERT(wl_xbox_apu_running(apu));
    ASSERT_EQ(8u, wl_xbox_apu_run(apu, 8, out));
    wl_xbox_apu_stats(apu, &st);
    ASSERT_EQ(2u, st.traps);
    wl_xbox_apu_destroy(apu);
    PASS();
}

// ---- Xbox ADPCM --------------------------------------------------------------------------------

static const int k_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// The textbook IMA encoder, one channel of one block: header = pcm[0], nibbles for pcm[1..64].
static void encode_block(const int16_t *pcm, uint32_t stride, uint8_t *blk, uint32_t channels,
                         uint32_t ch, int *index) {
    int pred = pcm[0];
    blk[4 * ch] = (uint8_t)pcm[0];
    blk[4 * ch + 1] = (uint8_t)((uint16_t)pcm[0] >> 8);
    blk[4 * ch + 2] = (uint8_t)*index;
    blk[4 * ch + 3] = 0;
    for (int k = 0; k < 64; k++) {
        const int step = k_steps[*index];
        int diff = pcm[(uint32_t)(k + 1) * stride] - pred, code = 0;
        if (diff < 0) {
            code = 8;
            diff = -diff;
        }
        int vp = step >> 3;
        if (diff >= step) {
            code |= 4;
            diff -= step;
            vp += step;
        }
        if (diff >= step >> 1) {
            code |= 2;
            diff -= step >> 1;
            vp += step >> 1;
        }
        if (diff >= step >> 2) {
            code |= 1;
            vp += step >> 2;
        }
        pred += (code & 8) ? -vp : vp;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        static const int adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
        *index += adj[code & 7];
        *index = *index < 0 ? 0 : *index > 88 ? 88 : *index;
        uint8_t *b =
            blk + 4u * channels * (1u + (uint32_t)k / 8u) + 4u * ch + ((uint32_t)k % 8u) / 2u;
        *b = (k & 1) ? (uint8_t)(*b | code << 4) : (uint8_t)code;
    }
}

TEST adpcm_voices_play_64_samples_a_block(void) {
    WlXboxApu *apu = setup();
    enum { BLOCKS = 40 };
    static int16_t pcm[(BLOCKS * 64 + 1) * 2];
    for (uint32_t i = 0; i < BLOCKS * 64 + 1; i++) {
        const int32_t tri = (int32_t)(i % 200u);
        pcm[2 * i] =
            (int16_t)(tri < 100 ? 240 * tri - 12000 : 12000 - 240 * (tri - 100)); // a triangle
        pcm[2 * i + 1] = (int16_t)(-pcm[2 * i] / 2);
    }
    int index[2] = {0, 0};
    for (uint32_t b = 0; b < BLOCKS; b++) {
        uint8_t blk[72];
        memset(blk, 0, sizeof blk);
        encode_block(pcm + b * 128u, 2, blk, 2, 0, &index[0]);
        encode_block(pcm + b * 128u + 1u, 2, blk, 2, 1, &index[1]);
        for (uint32_t i = 0; i < 72; i++) {
            *lin_ptr(b * 72u + i) = blk[i]; // blocks cross pages
        }
    }
    voice(apu, 40, FMT_ADPCM | FMT_STEREO | 1u << 16, 0, 0, BLOCKS * 64 - 1, 0, VOL(0, 0),
          0xFFFFFFFFu);
    play(apu, 40, 0);
    static int16_t out[BLOCKS * 2 * 64];
    ASSERT_EQ((uint32_t)(BLOCKS * 2), wl_xbox_apu_run(apu, BLOCKS * 2, out));
    // the reference: each block decoded by wl/audio, its first 64 samples
    for (uint32_t b = 1; b < BLOCKS; b++) {
        uint8_t blk[72];
        for (uint32_t i = 0; i < 72; i++) {
            blk[i] = *lin_ptr(b * 72u + i);
        }
        int16_t dec[130];
        wl_xbox_adpcm_decode_block(blk, 2, dec);
        for (uint32_t j = 0; j < 64; j++) {
            const uint32_t s = b * 64 + j;
            ASSERT_EQ_FMT((int)dec[2 * j], (int)out[2 * s], "%d");
            ASSERT_EQ_FMT((int)dec[2 * j + 1], (int)out[2 * s + 1], "%d");
            ASSERT_IN_RANGE(pcm[2 * s], dec[2 * j], 1500); // and it is the signal
        }
    }
    wl_xbox_apu_destroy(apu);
    PASS();
}

// ---- Streams -----------------------------------------------------------------------------------

static void segment(uint32_t index, uint32_t addr, uint32_t samples, uint32_t container) {
    wl_wr_le32(g_mem + SSL + index * 8u, addr);
    wl_wr_le32(g_mem + SSL + index * 8u + 4u, samples | container << 16);
}

TEST a_stream_plays_its_lists_in_turn_and_waits_for_more(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 300; i++) {
        wl_wr_le16(g_mem + STREAM + i * 2u, (uint16_t)(int16_t)(i + 1));
    }
    // list A: two segments of 40 and 24 samples; list B: one of 64 (segment table entries via
    // methods)
    m(apu, 0x190, 0x10); // segment table entry 0x10 + (method - 0x600) / 8
    m(apu, 0x600, STREAM);
    m(apu, 0x604, 40 | 1u << 16);
    m(apu, 0x608, STREAM + 80);
    m(apu, 0x60C, 24 | 1u << 16);
    segment(0x20, STREAM + 128, 64, 1);
    m(apu, 0x2F8, 50);
    m(apu, 0x300, 0x14830820u);
    m(apu, 0x304, FMT_S16 | FMT_STREAM | FMT_PERSIST | 6u | 7u << 5);
    m(apu, 0x360, VOL(0, 0xFFF));
    m(apu, 0x364, 0xFFFFFFFFu);
    m(apu, 0x368, 0xFFFFFFFFu);
    m(apu, 0x37C, 0);
    m(apu, 0x320, 0x10u << 8 | 2u); // A: entries 0x10, 0x11
    play(apu, 50, 0);
    m(apu, 0x2F8, 50);
    m(apu, 0x35C, 0x20u << 8 | 1u); // B: entry 0x20
    int16_t out[8 * 64];
    ASSERT_EQ(8u, wl_xbox_apu_run(apu, 8, out));
    for (uint32_t i = 32; i < 128; i++) {
        ASSERT_EQ((int)(i + 1), out[2 * i]); // A then B back to back: 1..128
    }
    for (uint32_t i = 128; i < 256; i++) {
        ASSERT_EQ(0, out[2 * i]); // out of data: silence, the voice still on
    }
    ASSERT_EQ(1, notifier(50, 0));
    ASSERT_EQ(1, notifier(50, 1));
    ASSERT(rec32(50, 0x54) & 0x00200000u);
    // DirectSound gives list A new segments: play goes on
    segment(0x30, STREAM + 256, 32, 1);
    m(apu, 0x2F8, 50);
    m(apu, 0x320, 0x30u << 8 | 1u);
    ASSERT_EQ(2u, wl_xbox_apu_run(apu, 2, out));
    ASSERT_EQ(0, out[0]);   // from the silence ...
    ASSERT_EQ(129, out[2]); // ... into the new data (byte 256: sample 128)
    ASSERT_EQ(129 + 31, out[2 * 32]);
    wl_xbox_apu_destroy(apu);
    PASS();
}

// ---- Envelopes ---------------------------------------------------------------------------------

TEST the_amplitude_envelope_attacks_holds_decays_and_releases(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 64; i++) {
        wl_wr_le16(lin_ptr(i * 2u), 16384);
    }
    voice(apu, 60, FMT_S16 | FMT_LOOP, 0, 0, 63, 0, VOL(0, 0xFFF), 0xFFFFFFFFu);
    m(apu, 0x308, 1u);                         // EA attack 1: 16 frames
    m(apu, 0x30C, 2u | 1u << 12 | 128u << 24); // decay 2, hold 1 (16 frames), sustain 128
    m(apu, 0x36C, 1u);                         // EA release 1: 16 frames
    play(apu, 60, 2u << 28);                   // EA starts in its attack
    int16_t out[64];
    int32_t level[80];
    for (int f = 0; f < 80; f++) {
        wl_xbox_apu_run(apu, 1, out);
        level[f] = out[62]; // the frame's last sample
    }
    // attack: linear, count k of 16 at frame k
    ASSERT_IN_RANGE(16384 * 8 / 16, level[8], 200);
    ASSERT(level[4] < level[8] && level[8] < level[12]);
    // hold at full, then decay to the sustain level (128/255)
    ASSERT_IN_RANGE(16384, level[20], 100);
    ASSERT_IN_RANGE(16384 * 128 / 255, level[79], 100);
    ASSERT_EQ(5u, rec32(60, 0x54) >> 28); // sustain
    // release: down 60 dB over 16 frames, then the voice ends
    m(apu, 0x12C, 60);
    for (int f = 0; f < 20; f++) {
        wl_xbox_apu_run(apu, 1, out);
        level[f] = out[62];
    }
    ASSERT(level[8] < 16384 * 128 / 255 / 4);
    ASSERT_EQ(0, level[19]);
    ASSERT_EQ(0u, rec32(60, 0x54) & 0x00200000u);
    ASSERT_EQ(1, notifier(60, 0));
    wl_xbox_apu_destroy(apu);
    PASS();
}

// ---- Multipass, the GP's command word, the fence ------------------------------------------------

TEST a_multipass_voice_replays_a_bin(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 64; i++) {
        wl_wr_le16(lin_ptr(i * 2u), 1000);
    }
    // voice 80 plays into bin 20 only (target 0 -> 20)
    voice(apu, 80, FMT_S16 | FMT_LOOP, 0, 0, 63, 0, VOL(0, 0xFFF), 0xFFFFFFFFu);
    m(apu, 0x2F8, 80);
    m(apu, 0x300, 20u | 1u << 5 | 2u << 10 | 3u << 16 | 4u << 21 | 5u << 26);
    play(apu, 80, 0);
    // voice 81: multipass from bin 20 (cleared), to bin 1 at -6 dB, in the multipass list
    voice(apu, 81, (1u << 21) | (20u << 16) | (1u << 26), 0, 0, 0, 0, VOL(0xFFF, 0x180),
          0xFFFFFFFFu);
    m(apu, 0x120, 0x3FFFFu);
    m(apu, 0x124, 81);
    int16_t out[2 * 64];
    wl_xbox_apu_run(apu, 2, out);
    const int32_t *bins = wl_xbox_apu_mixbins(apu);
    ASSERT_EQ(0, bins[20 * 32 + 10]);
    ASSERT_IN_RANGE(501, bins[1 * 32 + 10], 6);
    ASSERT_IN_RANGE(501, out[2 * 42 + 1], 6);
    wl_xbox_apu_destroy(apu);
    PASS();
}

TEST the_gp_command_word_is_taken_and_the_fence_lands(void) {
    WlXboxApu *apu = setup();
    w32(apu, 0x2040, GPS);
    wl_wr_le32(g_mem + GPS, 0x30000); // scratch page 0
    wl_wr_le32(g_mem + 0x30810, 3);   // a command
    int16_t out[64];
    wl_xbox_apu_run(apu, 1, out);
    ASSERT_EQ(3u, wl_rd_le32(g_mem + 0x30810)); // the GP is in reset
    w32(apu, 0x3FFFC, 1);
    w32(apu, 0x3FFFC, 3);
    ASSERT_EQ(3u, wl_xbox_apu_read(apu, 0x3FFFC, 4));
    wl_xbox_apu_run(apu, 1, out);
    ASSERT_EQ(0u, wl_rd_le32(g_mem + 0x30810));
    // FEMEMADDR / FEMEMDATA: the value is stored at the address
    w32(apu, 0x1324, 0x31000);
    w32(apu, 0x1334, 0xCAFE1234u);
    ASSERT_EQ(0xCAFE1234u, wl_rd_le32(g_mem + 0x31000));
    // the method FIFO is always free; the notify method writes notifier 0
    ASSERT_EQ(0x80u, wl_xbox_apu_read(apu, 0x20010, 4));
    m(apu, 0x104, 2);
    ASSERT_EQ(1, g_mem[NOTE + 15]);
    // the sample counter moves 32 a frame while the processor runs
    w32(apu, 0x200C, 0);
    wl_xbox_apu_run(apu, 3, NULL);
    ASSERT_EQ(96u, wl_xbox_apu_read(apu, 0x200C, 4));
    wl_xbox_apu_destroy(apu);
    PASS();
}

TEST a_3d_voice_reaches_the_hrtf_submixes(void) {
    WlXboxApu *apu = setup();
    for (uint32_t i = 0; i < 64; i++) {
        wl_wr_le16(lin_ptr(i * 2u), 2000);
    }
    m(apu, 0x2C0, 0x09070806u); // the 3D targets 0..3 -> bins 6, 8, 7, 9
    voice(apu, 5, FMT_S16 | FMT_LOOP, 0, 0, 63, 0, VOL(0, 0xFFF), 0xFFFFFFFFu);
    m(apu, 0x120, 0x2FFFFu); // the 3D list
    m(apu, 0x124, 5);
    int16_t out[2 * 64];
    wl_xbox_apu_run(apu, 2, out);
    const int32_t *bins = wl_xbox_apu_mixbins(apu);
    ASSERT_EQ(2000, bins[6 * 32 + 3]);
    ASSERT_EQ(0, bins[0 * 32 + 3]);
    ASSERT_EQ(2000, out[2 * 40]); // the 3D front left is mixed to the left
    wl_xbox_apu_destroy(apu);
    PASS();
}

// ---- AC'97 -------------------------------------------------------------------------------------

TEST the_ac97_bus_master_runs_its_descriptors(void) {
    if (g_mem == NULL) {
        g_mem = malloc(MEM_BYTES);
    }
    memset(g_mem, 0, MEM_BYTES);
    WlXboxAc97Config cfg = {phys_ptr, NULL, true};
    WlXboxAc97 *ac = wl_xbox_ac97_create(&cfg);
    ASSERT_EQ(0x100u, wl_xbox_ac97_read(ac, 0x130, 4) & 0x100u); // codec ready
    ASSERT_EQ(0u, wl_xbox_ac97_read(ac, 0x134, 1));              // semaphore free
    wl_xbox_ac97_write(ac, 0x26, 0, 2);
    ASSERT_EQ(0xFu, wl_xbox_ac97_read(ac, 0x26, 2) & 0xFu);
    for (uint32_t i = 0; i < 4; i++) { // four buffers of 128 words (64 frames), interrupt on each
        wl_wr_le32(g_mem + 0x5000 + i * 8u, 0x6000 + i * 0x100u);
        wl_wr_le32(g_mem + 0x5000 + i * 8u + 4u, 128u | 0x80000000u);
    }
    wl_xbox_ac97_write(ac, 0x11B, 0x02, 1); // reset registers
    ASSERT_EQ(0u, wl_xbox_ac97_read(ac, 0x11B, 1));
    wl_xbox_ac97_write(ac, 0x110, 0x5000, 4);
    wl_xbox_ac97_write(ac, 0x115, 1, 1);    // last valid: 1
    wl_xbox_ac97_write(ac, 0x11B, 0x1D, 1); // run, interrupts on completion and last buffer
    wl_xbox_ac97_advance(ac, 32);
    ASSERT_EQ(64u, wl_xbox_ac97_read(ac, 0x118, 2)); // half the first buffer left
    ASSERT_FALSE(wl_xbox_ac97_irq(ac));
    wl_xbox_ac97_advance(ac, 32);
    ASSERT(wl_xbox_ac97_irq(ac));
    ASSERT_EQ(1u, wl_xbox_ac97_read(ac, 0x114, 1));            // CIV moved on
    ASSERT_EQ(0x40u, wl_xbox_ac97_read(ac, 0x130, 4) & 0x40u); // PCM out interrupt
    wl_xbox_ac97_write(ac, 0x116, 0x08, 2);                    // acknowledge
    ASSERT_FALSE(wl_xbox_ac97_irq(ac));
    wl_xbox_ac97_advance(ac, 64);                              // the last valid buffer: halt
    ASSERT_EQ(0x0Fu, wl_xbox_ac97_read(ac, 0x116, 2) & 0x0Fu); // halted, last, last done, done
    wl_xbox_ac97_write(ac, 0x116, 0x1C, 2);
    wl_xbox_ac97_write(ac, 0x115, 1, 1); // the same last buffer again: it stays halted on it
    ASSERT_EQ(1u, wl_xbox_ac97_read(ac, 0x114, 1));
    ASSERT_EQ(0x01u, wl_xbox_ac97_read(ac, 0x116, 2) & 0x01u);
    wl_xbox_ac97_advance(ac, 64);
    ASSERT_EQ(1u, wl_xbox_ac97_read(ac, 0x114, 1)); // nothing played
    wl_xbox_ac97_write(ac, 0x115, 3, 1); // more buffers: it goes on with 2
    ASSERT_EQ(2u, wl_xbox_ac97_read(ac, 0x114, 1));
    ASSERT_EQ(0u, wl_xbox_ac97_read(ac, 0x116, 2) & 0x01u);
    wl_xbox_ac97_destroy(ac);
    PASS();
}

SUITE(xboxapu_suite) {
    RUN_TEST(pcm16_at_unity_pitch_reaches_its_bins_exactly);
    RUN_TEST(volume_is_sixtyfourths_of_a_decibel);
    RUN_TEST(formats_8_24_32_bit_and_stereo);
    RUN_TEST(pitch_halves_and_doubles_the_rate);
    RUN_TEST(a_loop_wraps_to_its_start_and_a_one_shot_ends);
    RUN_TEST(an_idle_voice_traps_the_front_end_until_the_guest_answers);
    RUN_TEST(adpcm_voices_play_64_samples_a_block);
    RUN_TEST(a_stream_plays_its_lists_in_turn_and_waits_for_more);
    RUN_TEST(the_amplitude_envelope_attacks_holds_decays_and_releases);
    RUN_TEST(a_multipass_voice_replays_a_bin);
    RUN_TEST(the_gp_command_word_is_taken_and_the_fence_lands);
    RUN_TEST(a_3d_voice_reaches_the_hrtf_submixes);
    RUN_TEST(the_ac97_bus_master_runs_its_descriptors);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(xboxapu_suite);
    GREATEST_MAIN_END();
}
