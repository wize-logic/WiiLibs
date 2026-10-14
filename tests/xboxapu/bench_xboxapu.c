// SPDX-License-Identifier: MIT
// A benchmark of the APU model's voice processor with a scene shaped like what Halo plays in a
// level (see scene()): about two dozen voices at once, as DirectSound sets them up through the
// front end's methods in a synthetic guest memory. Prints a checksum of every frame's mixbins and
// stereo output, so a change to the voice processor can show that what it computes is the same
// bit for bit, and the time it took.
//
//     bench_xboxapu [frames] [expected checksum]
//
// With an expected checksum (hex), exits 1 when the checksum differs (ctest runs it so). Count
// instructions with tools/qemu_icount.py on a ppc-be build (--base with fewer frames for the cost
// a frame).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wl/base/endian.h"
#include "wl/xboxapu/apu.h"

#define MEM_BYTES (4u << 20) // STREAM + STREAM_BYTES .. the end: silence
#define VPV 0x010000u        // voice records
#define SGE 0x020000u        // buffer page table (256 pages)
#define SSL 0x021000u        // segment table
#define NOTE 0x022000u       // notifiers
#define DATA 0x100000u       // the buffer pages (the page table maps them in reverse order)
#define DATA_PAGES 256u
#define STREAM 0x200000u       // stream segments (guest-physical)
#define STREAM_BYTES 0x100000u // and silence after it

static uint8_t *g_mem;
static uint32_t g_rand = 12345u;

static uint32_t rnd(void) {
    g_rand = g_rand * 1103515245u + 12345u;
    return g_rand >> 8;
}

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

static uint8_t *lin_ptr(uint32_t lin) {
    const uint32_t page = DATA + (DATA_PAGES - 1u - (lin >> 12)) * 0x1000u;
    return g_mem + page + (lin & 0xFFFu);
}

#define FMT_S16 (1u << 28 | 1u << 30)
#define FMT_U8 0u
#define FMT_ADPCM (2u << 30)
#define FMT_STEREO (1u << 27)
#define FMT_LOOP (1u << 25)
#define FMT_STREAM (1u << 24)
#define FMT_PERSIST (1u << 23)
#define FMT_MULTIPASS (1u << 21)

// A smooth signal with some noise in it: a sine-like triangle of a period, at an amplitude.
static int16_t signal(uint32_t i, uint32_t period, int32_t amp) {
    const int32_t ph = (int32_t)(i % period), half = (int32_t)period / 2;
    const int32_t tri = ph < half ? ph * 2 * amp / half - amp : amp - (ph - half) * 2 * amp / half;
    return (int16_t)(tri + (int32_t)(rnd() % 64u) - 32);
}

// Xbox ADPCM blocks (36 bytes a channel) of a signal, IMA-encoded.
static void encode_adpcm(uint8_t *dst_lin_or_phys, bool lin, uint32_t at, uint32_t blocks,
                         uint32_t channels, uint32_t period, int32_t amp) {
    static const int steps[89] = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
        25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
        88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
        307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
        1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
        3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const int adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    int index[2] = {0, 0};
    uint32_t n = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t blk[72];
        memset(blk, 0, sizeof blk);
        for (uint32_t ch = 0; ch < channels; ch++) {
            int pred = signal(n, period, amp) / (int)(ch + 1u);
            blk[4 * ch] = (uint8_t)pred;
            blk[4 * ch + 1] = (uint8_t)((uint16_t)pred >> 8);
            blk[4 * ch + 2] = (uint8_t)index[ch];
            for (uint32_t k = 0; k < 64u; k++) {
                const int step = steps[index[ch]];
                int diff = signal(n + k + 1u, period, amp) / (int)(ch + 1u) - pred, code = 0;
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
                index[ch] += adj[code & 7];
                index[ch] = index[ch] < 0 ? 0 : index[ch] > 88 ? 88 : index[ch];
                uint8_t *q = blk + 4u * channels * (1u + k / 8u) + 4u * ch + (k % 8u) / 2u;
                *q = (k & 1u) ? (uint8_t)(*q | code << 4) : (uint8_t)code;
            }
        }
        n += 64u;
        const uint32_t bytes = 36u * channels;
        for (uint32_t i = 0; i < bytes; i++) {
            if (lin) {
                *lin_ptr(at + b * bytes + i) = blk[i];
            } else {
                dst_lin_or_phys[at + b * bytes + i] = blk[i];
            }
        }
    }
}

// pitch for a rate: 4096 log2(rate / 48000)
#define P_22K (-4598)
#define P_44K (-501)
#define P_11K (-8694)

typedef struct {
    uint32_t fmt;
    uint32_t list;   // 1 2D, 2 3D, 3 multipass
    uint32_t vbin;   // targets 0..5
    uint32_t bins67; // targets 6, 7
    uint16_t vol[8]; // the targets' attenuations (1/64 dB, 0xFFF silent)
    int16_t pitch;
    uint32_t base, lbo, ebo; // buffers
    uint32_t data;           // streams: 0 mono ADPCM, 1 stereo ADPCM, 2 silence
    bool moving;             // volumes move each game tick
    uint32_t env[3];         // the amplitude envelope: ENV0, ENVA, release (0: off)
} Voice;

#define VBIN_SPK (0u | 1u << 5 | 2u << 10 | 3u << 16 | 4u << 21 | 5u << 26)
#define BINS67_FX (10u | 11u << 5) // targets 6 and 7: the effect sends
#define X 0xFFFu                   // a silent target

static Voice g_voices[64];
static uint32_t g_nvoices;
static uint32_t g_release_voice; // released at game tick 40

// VOLA, VOLB, VOLC of the eight attenuations: 0..5 at bits 4 and 20, 6 and 7 in the low nibbles.
static void vol_words(const uint16_t *vol, uint32_t w[3]) {
    for (uint32_t i = 0; i < 3u; i++) {
        w[i] = (uint32_t)vol[2u * i] << 4 | (uint32_t)vol[2u * i + 1u] << 20 |
               ((vol[6] >> (4u * i)) & 0xFu) | ((uint32_t)(vol[7] >> (4u * i)) & 0xFu) << 16;
    }
}

static void voice_setup(WlXboxApu *apu, uint32_t v, const Voice *d) {
    uint32_t w[3];
    vol_words(d->vol, w);
    m(apu, 0x2F8, v);
    m(apu, 0x300, d->vbin);
    m(apu, 0x304, d->fmt | d->bins67);
    m(apu, 0x360, w[0]);
    m(apu, 0x364, w[1]);
    m(apu, 0x368, w[2]);
    m(apu, 0x37C, (uint32_t)(uint16_t)d->pitch << 16);
    if ((d->fmt & FMT_STREAM) == 0) {
        m(apu, 0x3A0, d->base);
        m(apu, 0x3A4, d->lbo);
        m(apu, 0x3DC, d->ebo);
    }
    if (d->env[0] != 0) {
        m(apu, 0x308, d->env[0]);
        m(apu, 0x30C, d->env[1]);
        m(apu, 0x36C, d->env[2]);
    }
}

static uint32_t add(const Voice *d) {
    g_voices[g_nvoices] = *d;
    return g_nvoices++;
}

// A stream's list A or B gets two segments of 40..63 ADPCM blocks from its data in STREAM (the
// first half stereo, the second mono; silence past STREAM_BYTES).
static uint32_t g_stream_at[2];
static void stream_feed(WlXboxApu *apu, uint32_t v, uint32_t which) {
    const uint32_t entry = 0x10u + v * 4u + which * 2u, data = g_voices[v].data;
    const uint32_t channels = data == 1 ? 2u : 1u;
    const uint32_t half = STREAM_BYTES / 2u;
    for (uint32_t s = 0; s < 2u; s++) {
        const uint32_t blocks = 40u + (rnd() % 24u);
        const uint32_t bytes = blocks * 36u * channels;
        uint32_t at = STREAM + STREAM_BYTES; // silence
        if (data < 2u) {
            uint32_t *next = &g_stream_at[data];
            if (*next + bytes > half) {
                *next = 0;
            }
            at = STREAM + (data == 1 ? 0 : half) + *next;
            *next += bytes;
        }
        wl_wr_le32(g_mem + SSL + (entry + s) * 8u, at);
        wl_wr_le32(g_mem + SSL + (entry + s) * 8u + 4u, (blocks * 64u) | 2u << 16);
    }
    m(apu, 0x2F8, v);
    m(apu, which == 0 ? 0x320u : 0x35Cu, entry << 8 | 2u);
}

static void on_frame(void *user, const int32_t *bins, const int16_t *out);

// The scene, in proportion to what Halo's DirectSound plays in a level: mostly mono ADPCM streams
// near 22 kHz sent to front left, front right and an effect bin, a few of them silent and some
// moving (3D), stereo ADPCM loops at 44.1 kHz, 16-bit PCM voices with more targets; then one of
// each of the rarer cases (8-bit, a one-shot that ends, a multipass voice).
static WlXboxApu *scene(bool sum) {
    g_mem = calloc(1, MEM_BYTES);
    if (g_mem == NULL) {
        return NULL;
    }
    // buffer data: ADPCM stereo (lin 0x00000..0x40000), PCM16 mono (0x40000..0x80000), 8-bit
    // (0x80000..0x84000), ADPCM mono (0x84000..0x94000)
    encode_adpcm(NULL, true, 0x00000u, 0x40000u / 72u, 2, 97, 7000);
    for (uint32_t i = 0; i < 0x40000u / 2u; i++) {
        wl_wr_le16(lin_ptr(0x40000u + i * 2u), (uint16_t)signal(i, 150, 12000));
    }
    for (uint32_t i = 0; i < 0x4000u; i++) {
        *lin_ptr(0x80000u + i) = (uint8_t)(128 + signal(i, 64, 100));
    }
    encode_adpcm(NULL, true, 0x84000u, 0x10000u / 36u, 1, 211, 9000);
    for (uint32_t i = 0; i < 0x4000u; i++) { // ADPCM of noise (0x94000..0x98000): every step size
        *lin_ptr(0x94000u + i) = (uint8_t)rnd();
    }
    // stream data: stereo, then mono, ADPCM
    encode_adpcm(g_mem, false, STREAM, STREAM_BYTES / 2u / 72u, 2, 333, 10000);
    encode_adpcm(g_mem, false, STREAM + STREAM_BYTES / 2u, STREAM_BYTES / 2u / 36u, 1, 177, 8000);

    WlXboxApuConfig cfg;
    wl_xbox_apu_config_default(&cfg);
    cfg.phys_ptr = phys_ptr;
    cfg.on_frame = sum ? on_frame : NULL;
    WlXboxApu *apu = wl_xbox_apu_create(&cfg);
    if (apu == NULL) {
        return NULL;
    }
    w32(apu, 0x202C, VPV);
    w32(apu, 0x2030, SGE);
    w32(apu, 0x2034, SSL);
    w32(apu, 0x115C, NOTE);
    w32(apu, 0x1004, 0xFFu);
    w32(apu, 0x1504, 0); // no idle-voice traps: an ended one-shot stays in its list
    for (uint32_t i = 0; i < DATA_PAGES; i++) {
        m(apu, 0x804, i);
        m(apu, 0x808, DATA + (DATA_PAGES - 1u - i) * 0x1000u);
    }
    m(apu, 0x2C0, 0x09070806u); // the 3D submixes
    w32(apu, 0x1100, 0x0);
    w32(apu, 0x2000, 0x0F);

    // mono ADPCM streams around 22 kHz: front left, front right, the effect send; four 3D ones
    // moving, three of silence
    for (uint32_t i = 0; i < 16; i++) {
        const uint16_t a = (uint16_t)(0x80 + i * 23u), b = (uint16_t)(0xC0 + i * 17u);
        Voice d = {FMT_ADPCM | FMT_STREAM | FMT_PERSIST,
                   i < 4 ? 2u : 1u,
                   VBIN_SPK,
                   BINS67_FX,
                   {a, b, X, X, X, X, (uint16_t)(0x200 + i * 8u), X},
                   (int16_t)(P_22K + (int32_t)(i * 97u % 600u) - 300),
                   0,
                   0,
                   0,
                   i >= 13 ? 2u : 0u,
                   i < 4,
                   {0, 0, 0}};
        add(&d);
    }
    // stereo ADPCM loops at 44.1 kHz
    for (uint32_t i = 0; i < 3; i++) {
        Voice d = {FMT_ADPCM | FMT_STEREO | FMT_LOOP, 1,     VBIN_SPK,       BINS67_FX,
                   {0x100, 0x100, X, X, X, X, X, X},  P_44K, i * 72u * 300u, 0,
                   (600u + 100u * i) * 64u - 1u,      0,     false,          {0, 0, 0}};
        add(&d);
    }
    // 16-bit PCM loops with more targets: at 48 kHz (unity) and at 22 kHz
    for (uint32_t i = 0; i < 2; i++) {
        Voice d = {FMT_S16 | FMT_LOOP,
                   1,
                   VBIN_SPK,
                   BINS67_FX,
                   {0x40, 0x60, 0x180, 0x200, 0x300, 0x300, i ? X : 0x400u, X},
                   i ? P_22K : 0,
                   0x40000u + i * 0x10000u,
                   100,
                   0x7000u + i * 0x111u,
                   0,
                   false,
                   {0, 0, 0}};
        add(&d);
    }
    // 8-bit at 11 kHz, a one-shot that ends, a voice at full attenuation
    {
        Voice d = {FMT_U8 | FMT_LOOP,
                   1,
                   VBIN_SPK,
                   BINS67_FX,
                   {0x100, 0x100, X, X, X, X, X, X},
                   P_11K,
                   0x80000u,
                   0,
                   0x3FFF,
                   0,
                   false,
                   {0, 0, 0}};
        add(&d);
        Voice e = {FMT_ADPCM, 1,        VBIN_SPK, BINS67_FX,       {0x20, 0x20, X, X, X, X, X, X},
                   P_22K,     0x84000u, 0,        300u * 64u - 1u, 0,
                   false,     {0, 0, 0}};
        add(&e);
        Voice f = {FMT_ADPCM | FMT_LOOP,     1,     VBIN_SPK, BINS67_FX,
                   {X, X, X, X, X, X, X, X}, P_22K, 0x84000u, 0,
                   100u * 64u - 1u,          0,     false,    {0, 0, 0}};
        add(&f);
    }
    // ADPCM noise (the largest steps, the prediction clamping) under an amplitude envelope:
    // attack, hold, decay to its sustain level; and a loop the "driver" releases (it ends)
    {
        Voice d = {FMT_ADPCM | FMT_LOOP,
                   1,
                   VBIN_SPK,
                   BINS67_FX,
                   {0x300, 0x300, X, X, X, X, X, X},
                   P_22K,
                   0x94000u,
                   0,
                   0x4000u / 36u * 64u - 1u,
                   0,
                   false,
                   {2u, 1u | 1u << 12 | 160u << 24, 3u}};
        add(&d);
        Voice e = {FMT_S16 | FMT_LOOP,
                   1,
                   VBIN_SPK,
                   BINS67_FX,
                   {0x200, 0x280, X, X, X, X, X, X},
                   P_44K,
                   0x50000u,
                   0,
                   0x2000u,
                   0,
                   false,
                   {1u, 1u | 1u << 12 | 200u << 24, 2u}};
        g_release_voice = add(&e);
    }
    // a multipass voice: the effect send bin 10 back into the front bins (clearing it)
    {
        Voice d = {FMT_MULTIPASS | 10u << 16 | 1u << 26,
                   3,
                   VBIN_SPK,
                   6u | 7u << 5,
                   {0x200, 0x200, X, X, X, X, X, X},
                   0,
                   0,
                   0,
                   0,
                   0,
                   false,
                   {0, 0, 0}};
        add(&d);
    }
    for (uint32_t v = 0; v < g_nvoices; v++) {
        voice_setup(apu, v, &g_voices[v]);
        if (g_voices[v].fmt & FMT_STREAM) {
            stream_feed(apu, v, 0);
            stream_feed(apu, v, 1);
        }
        m(apu, 0x120, 0xFFFFu | g_voices[v].list << 16);
        m(apu, 0x124, v | (g_voices[v].env[0] != 0 ? 2u << 28 : 0)); // EA from its attack
    }
    return apu;
}

// A game tick: the moving voices get new volumes and pitches; played stream lists get new segments.
static void game_tick(WlXboxApu *apu, uint32_t tick) {
    if (tick == 40u) {
        m(apu, 0x12C, g_release_voice);
    }
    for (uint32_t v = 0; v < g_nvoices; v++) {
        Voice *d = &g_voices[v];
        if (d->moving) {
            d->vol[0] = (uint16_t)(0x100u + (tick * 37u + v * 11u) % 0x300u);
            d->vol[1] = (uint16_t)(0x100u + (tick * 23u + v * 7u) % 0x300u);
            uint32_t w[3];
            vol_words(d->vol, w);
            m(apu, 0x2F8, v);
            m(apu, 0x360, w[0]);
            m(apu, 0x37C, (uint32_t)(uint16_t)(d->pitch + (int16_t)(tick % 8u) * 20) << 16);
        }
        if (d->fmt & FMT_STREAM) {
            for (uint32_t which = 0; which < 2u; which++) {
                uint8_t *n = g_mem + NOTE + (2u + v * 4u + which) * 16u;
                if (n[15] != 0) {
                    n[15] = 0;
                    stream_feed(apu, v, which);
                }
            }
        }
    }
}

static uint32_t g_sum_bins = 2166136261u, g_sum_out = 2166136261u;

static uint32_t fnv(uint32_t h, uint32_t w) {
    for (uint32_t b = 0; b < 32u; b += 8u) {
        h = (h ^ ((w >> b) & 0xFFu)) * 16777619u;
    }
    return h;
}

static void on_frame(void *user, const int32_t *bins, const int16_t *out) {
    (void)user;
    for (uint32_t i = 0; i < WL_XBOX_APU_BINS * WL_XBOX_APU_FRAME; i++) {
        g_sum_bins = fnv(g_sum_bins, (uint32_t)bins[i]);
    }
    for (uint32_t i = 0; out != NULL && i < WL_XBOX_APU_FRAME * 2u; i++) {
        g_sum_out = fnv(g_sum_out, (uint16_t)out[i]);
    }
}

int main(int argc, char **argv) {
    const uint32_t frames = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 15000u;
    const bool check = argc > 2;
    const uint32_t expect = check ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
    const bool sum = check || getenv("BENCH_NO_SUM") == NULL; // BENCH_NO_SUM=1: the model alone
    WlXboxApu *apu = scene(sum);
    if (apu == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    static int16_t out[50u * WL_XBOX_APU_FRAME * 2u];
    const clock_t t0 = clock();
    uint32_t done = 0, tick = 0;
    while (done < frames) {
        game_tick(apu, tick++);
        const uint32_t want = frames - done < 50u ? frames - done : 50u; // 30 game ticks a second
        uint32_t got = 0;
        while (got < want) {
            const uint32_t n = wl_xbox_apu_run(apu, want - got, out + got * WL_XBOX_APU_FRAME * 2u);
            if (n == 0) {
                break;
            }
            got += n;
        }
        done += want;
    }
    const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    WlXboxApuStats st;
    wl_xbox_apu_stats(apu, &st);
    const uint32_t checksum = fnv(g_sum_bins, g_sum_out);
    printf("frames %u voice frames %u (max %u a frame) notifies %u: checksum %08x bins %08x out "
           "%08x; %.3f s, %.2f us a frame\n",
           (unsigned)st.frames, (unsigned)st.voice_frames, (unsigned)st.voices_max,
           (unsigned)st.notifies, (unsigned)checksum, (unsigned)g_sum_bins, (unsigned)g_sum_out,
           secs, secs * 1e6 / (double)(frames ? frames : 1u));
    wl_xbox_apu_destroy(apu);
    free(g_mem);
    if (check && checksum != expect) {
        fprintf(stderr, "checksum %08x, expected %08x\n", (unsigned)checksum, (unsigned)expect);
        return 1;
    }
    return 0;
}
