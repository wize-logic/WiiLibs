// SPDX-License-Identifier: MIT
// The APU's voice processor (apu.h): once a frame (32 samples at 48 kHz) it walks the 2D, 3D and
// multipass voice lists in guest memory, steps each active voice's two envelopes, fetches its
// samples (8-, 16-, 24- and 32-bit PCM through the buffer page table or a stream's segments, Xbox
// ADPCM through wl/audio's IMA step), resamples them by the voice's pitch (linear interpolation)
// and adds them to its eight target mixbins at their volumes. Integer only, so the output is the
// same on every host. A clean implementation from the facts listed in apu_regs.h; the envelope
// curves follow xboxdevwiki's description (linear attack, the decay law given there, an
// exponential release to -60 dB).
#include <string.h>

#include "wl/xboxapu/apu_internal.h"

// ---- Fixed-point helpers -----------------------------------------------------------------------

// 65536 * 2^(-i/64), i = 0..64
static const uint16_t k_exp2_neg[65] = {
    65535, 64830, 64132, 63441, 62757, 62081, 61413, 60751, 60097, 59449, 58809, 58176, 57549,
    56929, 56316, 55709, 55109, 54515, 53928, 53347, 52773, 52204, 51642, 51085, 50535, 49991,
    49452, 48920, 48393, 47871, 47356, 46846, 46341, 45842, 45348, 44859, 44376, 43898, 43425,
    42958, 42495, 42037, 41584, 41136, 40693, 40255, 39821, 39392, 38968, 38548, 38133, 37722,
    37316, 36914, 36516, 36123, 35734, 35349, 34968, 34591, 34219, 33850, 33486, 33125, 32768};

// 2^(-x / 65536) in Q16 (x >= 0); 65536 for x = 0.
static uint32_t exp2_neg_q16(uint32_t x) {
    if (x == 0) {
        return 65536u;
    }
    const uint32_t ip = x >> 16;
    if (ip >= 17u) {
        return 0;
    }
    const uint32_t f = x & 0xFFFFu, i = f >> 10, rem = f & 0x3FFu;
    const uint32_t a = i == 0 ? 65536u : k_exp2_neg[i], b = k_exp2_neg[i + 1u];
    return (a - (((a - b) * rem) >> 10)) >> ip;
}

// The resampling step for a pitch p (4096 per octave, 0 = 48 kHz): 2^(p / 4096) in Q16, kept
// between 1/256 and 16.
static uint32_t pitch_step(int32_t p) {
    if (p > 4 * 4096) {
        p = 4 * 4096;
    }
    if (p < -8 * 4096) {
        p = -8 * 4096;
    }
    // 2^(p/4096) = 2^(ceil) * 2^(-(ceil*4096 - p)/4096)
    const int32_t up = (p + 4095) >> 12;                 // ceil(p / 4096)
    const uint32_t neg = (uint32_t)(up * 4096 - p) << 4; // Q16 octaves below 2^up
    const uint32_t m = exp2_neg_q16(neg);                // Q16, 32768..65536
    return up >= 0 ? m << up : m >> -up;
}

// A volume (12 bits, 1/64 dB of attenuation; 0xFFF silent) as a Q16 gain.
static uint32_t volume_gain(uint32_t vol) {
    vol &= 0xFFFu;
    return vol == 0xFFFu ? 0u : exp2_neg_q16((vol * 174165u) >> 10); // log2(10) / 1280 per step
}

// ---- Envelopes ---------------------------------------------------------------------------------

// Steps one of a voice's envelopes by a frame and returns its value, Q16 (0..65536). amp: the
// amplitude envelope (EA), else the pitch/filter one (EF). Stage, count and level live in the
// voice's record, as the hardware keeps them.
static uint32_t envelope(WlXboxApu *apu, uint8_t *rec, uint32_t v, bool amp, bool *off) {
    uint32_t state = wl_rd_le32(rec + WL_APU_V_STATE);
    uint32_t ecnt = wl_rd_le32(rec + WL_APU_V_ECNT);
    const uint32_t lvl_off = amp ? WL_APU_V_CBO : WL_APU_V_EBO;
    uint32_t lvl_word = wl_rd_le32(rec + lvl_off);
    uint32_t stage = amp ? state >> 28 : (state >> 24) & 0xFu;
    uint32_t count = amp ? ecnt & 0xFFFFu : ecnt >> 16;
    uint32_t level = lvl_word >> 24;
    const uint32_t cfg0 = wl_rd_le32(rec + (amp ? WL_APU_V_ENV0 : WL_APU_V_ENV1));
    const uint32_t cfga = wl_rd_le32(rec + (amp ? WL_APU_V_ENVA : WL_APU_V_ENVF));
    const uint32_t attack = (cfg0 & 0xFFFu) * 16u, hold = ((cfga >> 12) & 0xFFFu) * 16u;
    const uint32_t decay = (cfga & 0xFFFu) * 16u, sustain = cfga >> 24;
    const uint32_t release =
        ((amp ? wl_rd_le32(rec + WL_APU_V_LFO_ENV) : wl_rd_le32(rec + WL_APU_V_MISC)) & 0xFFFu) *
        16u;
    uint32_t value = 0; // Q16
    switch (stage) {
    case WL_APU_ENV_OFF:
        level = 255;
        value = 65536u;
        count = 0;
        break;
    case WL_APU_ENV_DELAY:
        level = 0;
        if (count == 0) {
            stage = WL_APU_ENV_ATTACK;
        } else {
            count--;
        }
        break;
    case WL_APU_ENV_ATTACK: // linear from 0 to full over 16 x the rate in frames
        if (attack == 0 || count >= attack) {
            value = 65536u;
            level = 255;
            stage = WL_APU_ENV_HOLD;
            count = hold;
        } else {
            value = ((count << 15) / attack) << 1; // 32-bit: count < 2^16
            level = (count * 255u) / attack;
            count++;
        }
        break;
    case WL_APU_ENV_HOLD:
        level = 255;
        value = 65536u;
        if (count == 0) {
            stage = WL_APU_ENV_DECAY;
            count = decay;
        } else {
            count--;
        }
        break;
    case WL_APU_ENV_DECAY: {
        // 255 * 0.99988799 ^ ((16 rate - count) * 4096 / rate): log2(0.99988799) * 4096 = -0.66193
        const uint32_t sus = (sustain << 16) / 255u;
        if (decay == 0 || count == 0) {
            value = sus;
            level = sustain;
            stage = WL_APU_ENV_SUSTAIN;
            count = 0;
            break;
        }
        const uint32_t n = decay - count;
        value = exp2_neg_q16((n * 43380u) / (decay / 16u)); // n < 2^16: no overflow
        if (value <= sus) {
            value = sus;
            level = sustain;
            stage = WL_APU_ENV_SUSTAIN;
            count = 0;
        } else {
            level = (value * 255u) >> 16;
            count--;
        }
        break;
    }
    case WL_APU_ENV_SUSTAIN:
        level = sustain;
        value = (sustain << 16) / 255u;
        count = 0;
        break;
    case WL_APU_ENV_RELEASE:
        // from the level the release started at, e^(-6.91 x) over 16 x the rate (to -60 dB):
        // 6.91 / ln 2 = 9.969 octaves
        if (release == 0 || count == 0) {
            stage = WL_APU_ENV_FORCE_RELEASE;
            count = 0;
        } else {
            const uint32_t x = ((release - count) * 40833u) / (release / 16u); // 653330 / 16
            value = (exp2_neg_q16(x) * level) / 255u;
            count--;
        }
        break;
    default: // forced release: the amplitude envelope ends the voice
        if (amp) {
            *off = true;
        }
        break;
    }
    if (amp) {
        state = (state & 0x0FFFFFFFu) | stage << 28;
        ecnt = (ecnt & 0xFFFF0000u) | (count & 0xFFFFu);
    } else {
        state = (state & 0xF0FFFFFFu) | (stage & 0xFu) << 24;
        ecnt = (ecnt & 0x0000FFFFu) | count << 16;
    }
    wl_wr_le32(rec + WL_APU_V_STATE, state);
    wl_wr_le32(rec + WL_APU_V_ECNT, ecnt);
    wl_wr_le32(rec + lvl_off, (lvl_word & 0x00FFFFFFu) | (level & 0xFFu) << 24);
    (void)apu;
    (void)v;
    return value;
}

// ---- Sample fetch ------------------------------------------------------------------------------

enum { FETCH_OK = 0, FETCH_END, FETCH_STARVE };

// Copies n bytes at `linear` in the buffer page table's space (VPSGEADDR: 4 KB pages, an 8-byte
// entry each, word 0 the page's guest-physical address).
static bool sge_read(WlXboxApu *apu, uint32_t linear, uint8_t *dst, uint32_t n) {
    const uint32_t table = wl_apu_reg(apu, WL_APU_VPSGEADDR);
    while (n > 0) {
        const uint32_t in_page = 4096u - (linear & 0xFFFu);
        const uint32_t take = n < in_page ? n : in_page;
        const uint32_t page = wl_apu_rd32(apu, table + (linear >> 12) * 8u) & 0xFFFFF000u;
        const uint8_t *src = page != 0 ? wl_apu_mem(apu, page + (linear & 0xFFFu), take) : NULL;
        if (src == NULL) {
            return false;
        }
        memcpy(dst, src, take);
        dst += take;
        linear += take;
        n -= take;
    }
    return true;
}

// One PCM sample of a container (1, 2 or 4 bytes) as a 16-bit value.
static int32_t pcm16(const uint8_t *p, uint32_t container, uint32_t sample) {
    switch (container) {
    case 0:
        return sample == 0 ? ((int32_t)p[0] - 128) << 8 : (int32_t)(int8_t)p[0] * 256;
    case 1:
        return (int16_t)wl_rd_le16(p);
    default: {
        const uint32_t w = wl_rd_le32(p);
        if (sample == 2) {
            return (int32_t)(w << 8) >> 16; // 24 bits in the low three bytes
        }
        if (sample == 3) {
            return (int32_t)w >> 16;
        }
        return (int16_t)w;
    }
    }
}

// IMA's step as one table lookup: for each step index and nibble, the signed difference it adds
// (as wl_ima_step makes it: step / 8 + step for bit 2 + step / 2 for bit 1 + step / 4 for bit 0,
// each term truncated, negated for bit 3; but in 16 bits, as this model has always kept it) times
// 2^13, plus the next step index (clamped to 0..88) as the byte offset of its row (index x 64).
#define IMA_ROW 64u // bytes a row: 16 int32 entries
static int32_t k_ima[89][16];

void wl_apu_vp_init(void) {
    for (uint32_t i = 0; i < 89u; i++) {
        const int32_t step = wl_ima_step_table[i];
        for (uint32_t n = 0; n < 16u; n++) {
            // in 16 bits: at the top step indices a difference above 32767 wraps (where
            // wl_ima_step's does not)
            const int32_t diff = (int16_t)((step >> 3) + ((n & 4u) ? step : 0) +
                                           ((n & 2u) ? step >> 1 : 0) + ((n & 1u) ? step >> 2 : 0));
            int32_t next = (int32_t)i + wl_ima_index_table[n & 7u];
            next = next < 0 ? 0 : next > 88 ? 88 : next;
            k_ima[i][n] = ((n & 8u) ? -diff : diff) * 8192 + next * (int32_t)IMA_ROW;
        }
    }
}

// One nibble (bits sh..sh+3 of word): the row (row_off) gives its entry, the entry the
// difference and the next row; the prediction clamps to 16 bits.
#define IMA_NIBBLE(word, sh, out)                                                                  \
    do {                                                                                           \
        const int32_t e_ =                                                                         \
            *(const int32_t *)(const void *)(tab + row_off + ((((word) >> (sh)) & 15u) << 2));     \
        pred += e_ >> 13;                                                                          \
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;                               \
        row_off = (uint32_t)e_ & 0x1FFFu;                                                          \
        (out) = (int16_t)pred;                                                                     \
    } while (0)

// Decodes the 64 samples the Xbox plays of a block (the header's and the first 63 nibbles'; the
// 64th nibble's sample is the next block's header again) into pcm, interleaved by channel. The IMA
// step is wl/audio's (wl_ima_step), over the table above.
static void adpcm_block(const uint8_t *blk, uint32_t channels, int16_t *pcm) {
    const uint8_t *tab = (const uint8_t *)k_ima;
    for (uint32_t ch = 0; ch < channels; ch++) {
        WlImaState st;
        int32_t pred = wl_xbox_adpcm_header(blk, ch, &st);
        uint32_t row_off = st.index * IMA_ROW;
        pcm[ch] = (int16_t)pred;
        int16_t *o = pcm + channels + ch; // sample 1 on
        const uint32_t c = channels;      // 1 or 2: o[k * c]
        const uint8_t *q = blk + 4u * channels + 4u * ch;
        for (uint32_t g = 0; g < 7u; g++) {      // groups of 8 nibbles: 4 bytes of this channel
            const uint32_t word = wl_rd_le32(q); // nibble k at bits 4k
            IMA_NIBBLE(word, 0, o[0]);
            IMA_NIBBLE(word, 4, o[c]);
            IMA_NIBBLE(word, 8, o[2u * c]);
            IMA_NIBBLE(word, 12, o[3u * c]);
            IMA_NIBBLE(word, 16, o[4u * c]);
            IMA_NIBBLE(word, 20, o[5u * c]);
            IMA_NIBBLE(word, 24, o[6u * c]);
            IMA_NIBBLE(word, 28, o[7u * c]);
            o += 8u * c;
            q += 4u * channels;
        }
        const uint32_t word = wl_rd_le32(q); // the last group: 7 nibbles (the 64th is not played)
        IMA_NIBBLE(word, 0, o[0]);
        IMA_NIBBLE(word, 4, o[c]);
        IMA_NIBBLE(word, 8, o[2u * c]);
        IMA_NIBBLE(word, 12, o[3u * c]);
        IMA_NIBBLE(word, 16, o[4u * c]);
        IMA_NIBBLE(word, 20, o[5u * c]);
        IMA_NIBBLE(word, 24, o[6u * c]);
    }
}

// Sample `idx` of an Xbox ADPCM block sequence starting at `start` (in the buffer's page space, or
// guest-physical for a stream segment), 64 samples a block. The voice keeps the block it decoded
// last.
static bool adpcm_sample(WlXboxApu *apu, WlApuVoice *vs, bool phys, uint32_t start, uint32_t idx,
                         uint32_t channels, int32_t out[2]) {
    const uint32_t bytes = WL_XBOX_ADPCM_BLOCK * channels;
    const uint32_t blk = start + (idx / WL_XBOX_ADPCM_BLOCK_SAMPLES) * bytes;
    const uint32_t j = idx % WL_XBOX_ADPCM_BLOCK_SAMPLES;
    if (vs->blk_next == 0xFFu || vs->blk_addr != blk) {
        uint8_t tmp[2u * WL_XBOX_ADPCM_BLOCK];
        const uint8_t *src = NULL;
        if (phys) {
            src = wl_apu_mem(apu, blk, bytes);
        } else if (sge_read(apu, blk, tmp, bytes)) {
            src = tmp;
        }
        if (src == NULL) {
            vs->blk_next = 0xFFu;
            out[0] = out[1] = 0;
            return false;
        }
        adpcm_block(src, channels, vs->blk_pcm);
        vs->blk_addr = blk;
        vs->blk_next = 0;
    }
    out[0] = vs->blk_pcm[j * channels];
    out[1] = vs->blk_pcm[j * channels + channels - 1u];
    return true;
}

// A frame's worth of format facts about a voice.
typedef struct {
    uint32_t fmt;
    uint32_t channels;
    bool loop, stream, persist;
    uint32_t base, lbo, ebo; // buffer: byte offset, loop start, last sample
} VoiceFormat;

static int fetch_buffer(WlXboxApu *apu, WlApuVoice *vs, const VoiceFormat *f, int32_t out[2],
                        uint32_t *at) {
    if (vs->rd > f->ebo) {
        if (!f->loop) {
            return FETCH_END;
        }
        vs->rd = f->lbo <= f->ebo ? f->lbo : 0;
    }
    const uint32_t idx = vs->rd++;
    *at = idx;
    const uint32_t container = WL_APU_FMT_CONTAINER(f->fmt);
    if (container == WL_APU_CONTAINER_ADPCM) {
        if (adpcm_sample(apu, vs, false, f->base, idx, f->channels, out)) {
            // the rest of the block, up to the buffer's end
            const uint32_t j = idx % WL_XBOX_ADPCM_BLOCK_SAMPLES,
                           block_left = WL_XBOX_ADPCM_BLOCK_SAMPLES - 1u - j;
            vs->run = (const uint8_t *)&vs->blk_pcm[(j + 1u) * f->channels];
            vs->run_left = block_left < f->ebo - idx ? block_left : f->ebo - idx;
            vs->run_host = true;
        }
        return FETCH_OK;
    }
    const uint32_t size = container == 0 ? 1u : container == 1 ? 2u : 4u;
    const uint32_t bytes = size * f->channels;
    const uint32_t lin = f->base + idx * bytes;
    const uint8_t *p;
    uint8_t b[8];
    if (vs->page != NULL && vs->page_key == lin >> 12 && vs->page_gen == apu->sge_gen &&
        (lin & 0xFFFu) + bytes <= 4096u) {
        p = vs->page + (lin & 0xFFFu); // the page the last samples came from
    } else if ((lin & 0xFFFu) + bytes <= 4096u) {
        const uint32_t table = wl_apu_reg(apu, WL_APU_VPSGEADDR);
        const uint32_t page = wl_apu_rd32(apu, table + (lin >> 12) * 8u) & 0xFFFFF000u;
        vs->page = page != 0 ? wl_apu_mem(apu, page, 4096u) : NULL;
        vs->page_key = lin >> 12;
        vs->page_gen = apu->sge_gen;
        if (vs->page == NULL) {
            out[0] = out[1] = 0;
            return FETCH_OK;
        }
        p = vs->page + (lin & 0xFFFu);
    } else { // a sample frame across two pages
        if (!sge_read(apu, lin, b, bytes)) {
            out[0] = out[1] = 0;
            return FETCH_OK;
        }
        p = b;
    }
    const uint32_t sample = WL_APU_FMT_SAMPLE(f->fmt);
    out[0] = pcm16(p, container, sample);
    out[1] = f->channels == 2 ? pcm16(p + size, container, sample) : out[0];
    if (container == 1 && p != b) { // 16-bit: the rest of the page, up to the buffer's end
        const uint32_t page_left = (4096u - (lin & 0xFFFu)) / bytes - 1u;
        vs->run = p + bytes;
        vs->run_left = page_left < f->ebo - idx ? page_left : f->ebo - idx;
        vs->run_host = false;
    }
    return FETCH_OK;
}

// A stream voice plays its segment lists A and B in turn: each list is `count` entries of the
// segment table (VPSSLADDR) from its base; each entry a guest-physical address and a length in
// samples. A list played to its end is notified (notifier 0 for A, 1 for B) and the other list
// follows; the guest gives the finished one new segments meanwhile.
static int fetch_stream(WlXboxApu *apu, uint32_t v, WlApuVoice *vs, const VoiceFormat *f,
                        int32_t out[2], uint32_t *at) {
    const uint32_t table = wl_apu_reg(apu, WL_APU_VPSSLADDR);
    for (uint32_t guard = 0; guard < 64u; guard++) {
        const uint32_t count = vs->ssl_count[vs->ssl];
        if (count == 0 || table == 0) {
            return FETCH_STARVE;
        }
        const uint32_t index = vs->ssl_base[vs->ssl] + vs->seg;
        if (vs->seg_entry != index || vs->seg_gen != apu->sge_gen) {
            const uint32_t entry = table + index * 8u, word = wl_apu_rd32(apu, entry + 4u);
            vs->seg_entry = index;
            vs->seg_gen = apu->sge_gen;
            vs->seg_addr = wl_apu_rd32(apu, entry);
            vs->seg_len = WL_APU_SEG_SAMPLES(word);
            vs->seg_container = WL_APU_SEG_CONTAINER(word);
            vs->seg_data = NULL;
        }
        if (vs->seg_addr != 0 && vs->rd < vs->seg_len) {
            const uint32_t idx = vs->rd++;
            *at = idx;
            const uint32_t container = vs->seg_container;
            const uint32_t seg_left = vs->seg_len - 1u - idx;
            if (container == WL_APU_CONTAINER_ADPCM) {
                if (adpcm_sample(apu, vs, true, vs->seg_addr, idx, f->channels, out)) {
                    const uint32_t j = idx % WL_XBOX_ADPCM_BLOCK_SAMPLES,
                                   block_left = WL_XBOX_ADPCM_BLOCK_SAMPLES - 1u - j;
                    vs->run = (const uint8_t *)&vs->blk_pcm[(j + 1u) * f->channels];
                    vs->run_left = block_left < seg_left ? block_left : seg_left;
                    vs->run_host = true;
                }
                return FETCH_OK;
            }
            const uint32_t size = container == 0 ? 1u : container == 1 ? 2u : 4u;
            if (vs->seg_data == NULL) {
                vs->seg_data = wl_apu_mem(apu, vs->seg_addr, vs->seg_len * size * f->channels);
                if (vs->seg_data == NULL) {
                    out[0] = out[1] = 0;
                    return FETCH_OK;
                }
            }
            const uint8_t *p = vs->seg_data + idx * size * f->channels;
            const uint32_t sample = WL_APU_FMT_SAMPLE(f->fmt);
            out[0] = pcm16(p, container, sample);
            out[1] = f->channels == 2 ? pcm16(p + size, container, sample) : out[0];
            if (container == 1) { // 16-bit: the rest of the segment
                vs->run = p + size * f->channels;
                vs->run_left = seg_left;
                vs->run_host = false;
            }
            return FETCH_OK;
        }
        // the segment is played: the next one, or the other list
        vs->rd = 0;
        vs->seg++;
        if (vs->seg >= count) {
            const uint32_t done = vs->ssl;
            vs->ssl_count[done] = 0; // played: it needs new segments before it plays again
            vs->ssl ^= 1u;
            vs->seg = 0;
            wl_apu_notify_voice(apu, v, done);
        }
    }
    return FETCH_STARVE;
}

static int fetch(WlXboxApu *apu, uint32_t v, WlApuVoice *vs, const VoiceFormat *f, int32_t out[2],
                 uint32_t *at) {
    return f->stream ? fetch_stream(apu, v, vs, f, out, at) : fetch_buffer(apu, vs, f, out, at);
}

// Fills up to n samples after s1 into l (and r for a stereo voice): runs are copied in tight loops,
// anything else goes through fetch. Stops early at a buffer's end or where a stream has no data
// (vs->ending / vs->starving). Keeps pos1 (the last sample's position) and pos_prev (the one
// before it).
static uint32_t fill(WlXboxApu *apu, uint32_t v, WlApuVoice *vs, const VoiceFormat *f, int32_t *l,
                     int32_t *r, uint32_t n) {
    const uint32_t ch = f->channels;
    uint32_t k = 0;
    while (k < n) {
        if (vs->run_left != 0) { // within a page, a segment or a decoded block: no lookups
            const uint32_t take = n - k < vs->run_left ? n - k : vs->run_left;
            const uint8_t *p = vs->run;
            if (vs->run_host) {
                const int16_t *h = (const int16_t *)(const void *)p;
                if (ch == 2) {
                    for (uint32_t j = 0; j < take; j++) {
                        l[k + j] = h[2u * j];
                        r[k + j] = h[2u * j + 1u];
                    }
                } else {
                    for (uint32_t j = 0; j < take; j++) {
                        l[k + j] = h[j];
                    }
                }
            } else if (ch == 2) {
                for (uint32_t j = 0; j < take; j++) {
                    l[k + j] = (int16_t)wl_rd_le16(p + 4u * j);
                    r[k + j] = (int16_t)wl_rd_le16(p + 4u * j + 2u);
                }
            } else {
                for (uint32_t j = 0; j < take; j++) {
                    l[k + j] = (int16_t)wl_rd_le16(p + 2u * j);
                }
            }
            vs->pos_prev = take >= 2u ? vs->rd + take - 2u : vs->pos1;
            vs->run = p + 2u * ch * take;
            vs->run_left -= take;
            vs->rd += take;
            vs->pos1 = vs->rd - 1u;
            k += take;
            continue;
        }
        int32_t s[2];
        uint32_t at = vs->pos1;
        const int st = fetch(apu, v, vs, f, s, &at);
        if (st != FETCH_OK) {
            vs->ending = st == FETCH_END;
            vs->starving = st == FETCH_STARVE;
            break;
        }
        l[k] = s[0];
        r[k] = s[1];
        vs->pos_prev = vs->pos1;
        vs->pos1 = at;
        k++;
    }
    return k;
}

#define SRC_MAX (WL_XBOX_APU_FRAME * 16u + 2u) // source samples a frame can take (step up to 16)

// 32 samples of a voice at its pitch into src (16-bit scale). Returns false when the voice ended
// in this frame (the rest of src is silence): a buffer without a loop played to its end, or a
// stream that is not persistent out of data. A persistent stream out of data plays silence and
// goes on once the guest gives it segments. The frame's source samples are fetched first (b[0] is
// s0, b[1] s1), then interpolated.
static bool voice_samples(WlXboxApu *apu, uint32_t v, WlApuVoice *vs, const VoiceFormat *f,
                          int32_t src[2][WL_XBOX_APU_FRAME]) {
    memset(src, 0, sizeof(int32_t) * 2u * WL_XBOX_APU_FRAME);
    const bool keep_waiting = !f->stream || f->persist;
    int32_t bl[SRC_MAX], br_[SRC_MAX];
    int32_t *br = f->channels == 2 ? br_ : bl; // a mono voice's right is its left
    if (!vs->primed) {
        vs->ending = vs->starving = vs->stalled = false;
        vs->run_left = 0;
        uint32_t at = vs->rd;
        const int r = fetch(apu, v, vs, f, vs->s0, &at);
        if (r != FETCH_OK) {
            apu->stats.stream_starved += r == FETCH_STARVE;
            return r == FETCH_STARVE && keep_waiting;
        }
        vs->pos = vs->pos1 = at;
        vs->frac = 0;
        vs->primed = true;
        if (fill(apu, v, vs, f, bl, br, 1) == 1) {
            vs->s1[0] = bl[0];
            vs->s1[1] = br[0];
        } else {
            vs->s1[0] = vs->s1[1] = 0;
        }
    } else if (vs->stalled || vs->starving) {
        // waiting where a stream's data ran out: a segment list may have come
        vs->starving = false;
        if (fill(apu, v, vs, f, bl, br, 1) != 1) {
            if (vs->stalled) {
                apu->stats.stream_starved++;
                return keep_waiting;
            }
        } else {
            if (vs->stalled) { // from the silence it played to the new data
                vs->s0[0] = vs->s0[1] = 0;
                vs->frac = 0;
                vs->stalled = false;
            }
            vs->s1[0] = bl[0];
            vs->s1[1] = br[0];
        }
    }
    const uint32_t step = vs->step;
    const uint32_t end = vs->frac + step * WL_XBOX_APU_FRAME; // Q16 from s0
    const uint32_t adv = end >> 16;                           // samples s0 moves on by
    bl[0] = vs->s0[0];
    br[0] = vs->s0[1];
    bl[1] = vs->s1[0];
    br[1] = vs->s1[1];
    uint32_t valid = 2; // b[valid] is the first sample that is not there (a buffer's end, no data)
    if (vs->ending || vs->starving) {
        valid = 1;
    } else if (adv > 0) {
        valid = 2u + fill(apu, v, vs, f, bl + 2, br + 2, adv);
    }
    for (uint32_t i = valid; i < adv + 2u && i < SRC_MAX; i++) {
        bl[i] = br[i] = 0; // silence after the end, as the last sample's partner
    }
    uint32_t p = vs->frac;
    bool stopped = false;
    if (f->channels == 2) {
        for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++, p += step) {
            const uint32_t k = p >> 16;
            if (k >= valid) {
                stopped = true;
                break;
            }
            const int32_t t = (int32_t)((p & 0xFFFFu) >> 1); // Q15
            src[0][i] = bl[k] + (((bl[k + 1u] - bl[k]) * t) >> 15);
            src[1][i] = br[k] + (((br[k + 1u] - br[k]) * t) >> 15);
        }
    } else {
        for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++, p += step) {
            const uint32_t k = p >> 16;
            if (k >= valid) {
                stopped = true;
                break;
            }
            const int32_t t = (int32_t)((p & 0xFFFFu) >> 1);
            src[0][i] = bl[k] + (((bl[k + 1u] - bl[k]) * t) >> 15);
        }
    }
    if (stopped || adv >= valid) {
        // s0 has gone past the last sample there is
        if (vs->ending || !vs->starving) {
            return false;
        }
        vs->stalled = true;
        vs->s0[0] = vs->s0[1] = 0;
        vs->frac = 0;
        return keep_waiting;
    }
    vs->s0[0] = bl[adv];
    vs->s0[1] = br[adv];
    vs->s1[0] = bl[adv + 1u];
    vs->s1[1] = br[adv + 1u];
    vs->frac = end & 0xFFFFu;
    if (adv > 0) {
        vs->pos = adv + 1u < valid ? vs->pos_prev : vs->pos1;
    }
    return true;
}

// ---- A voice's frame ---------------------------------------------------------------------------

static void voice_frame(WlXboxApu *apu, uint32_t v, uint8_t *rec, uint32_t list) {
    uint32_t state = wl_rd_le32(rec + WL_APU_V_STATE);
    if (state & WL_APU_STATE_PAUSED) {
        return;
    }
    WlApuVoice *vs = &apu->voice[v];
    VoiceFormat f;
    f.fmt = wl_rd_le32(rec + WL_APU_V_FMT);
    f.channels = (f.fmt & WL_APU_FMT_STEREO) ? 2u : 1u;
    f.loop = (f.fmt & WL_APU_FMT_LOOP) != 0;
    f.stream = (f.fmt & WL_APU_FMT_STREAM) != 0;
    f.persist = (f.fmt & WL_APU_FMT_PERSIST) != 0;
    f.base = wl_rd_le32(rec + WL_APU_V_BASE) & 0x00FFFFFFu;
    f.lbo = wl_rd_le32(rec + WL_APU_V_LBO) & 0x00FFFFFFu;
    f.ebo = wl_rd_le32(rec + WL_APU_V_EBO) & 0x00FFFFFFu;

    // the guest moved the play position (VOICE_ON, SET_VOICE_BUF_CBO): start fetching there
    const uint32_t cbo = wl_rd_le32(rec + WL_APU_V_CBO) & 0x00FFFFFFu;
    if (cbo != vs->cbo_written) {
        vs->rd = cbo;
        vs->primed = false;
        vs->run_left = 0;
        vs->blk_next = 0xFFu;
    }

    bool off = false;
    const uint32_t ef = envelope(apu, rec, v, false, &off);
    const uint32_t ea = envelope(apu, rec, v, true, &off);
    if (off) {
        apu->stats.voices_ended++;
        wl_apu_voice_off(apu, v);
        return;
    }
    state = wl_rd_le32(rec + WL_APU_V_STATE);
    wl_wr_le32(rec + WL_APU_V_STATE, state & ~WL_APU_STATE_NEW);

    // pitch, with the pitch envelope's share (scale x 32 x its value)
    const int32_t pitch = (int16_t)(wl_rd_le32(rec + WL_APU_V_PITCH_LINK) >> 16);
    const int32_t scale = (int8_t)(wl_rd_le32(rec + WL_APU_V_ENV0) >> 24);
    const int32_t p = pitch + ((scale * 32 * (int32_t)ef) >> 16); // within 2^28
    if (p != vs->pitch_seen) {
        vs->pitch_seen = p;
        vs->step = pitch_step(p);
    }

    int32_t src[2][WL_XBOX_APU_FRAME];
    bool ended = false;
    if (f.fmt & WL_APU_FMT_MULTIPASS) {
        // a multipass voice plays a mixbin of this frame (the 2D and 3D lists have run)
        const uint32_t from = WL_APU_FMT_SPB(f.fmt);
        for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++) { // a 16-bit source, as voices are
            const int32_t s = apu->mix[from][i];
            src[0][i] = src[1][i] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        }
        f.channels = 1;
        if (f.fmt & WL_APU_FMT_CLEAR_MIX) {
            memset(apu->mix[from], 0, sizeof apu->mix[from]);
        }
    } else {
        ended = !voice_samples(apu, v, vs, &f, src);
    }

    // a silent frame (DirectSound keeps silent looping voices running) adds nothing to any bin
    int32_t any = 0;
    for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++) {
        any |= src[0][i] | (f.channels == 2 ? src[1][i] : 0);
    }

    // targets: six bins in VBIN, two in FMT; a 3D voice's first four go to the HRTF submixes
    const uint32_t vbin = wl_rd_le32(rec + WL_APU_V_VBIN);
    uint32_t bins[8] = {vbin & 31u,         (vbin >> 5) & 31u,  (vbin >> 10) & 31u,
                        (vbin >> 16) & 31u, (vbin >> 21) & 31u, (vbin >> 26) & 31u,
                        f.fmt & 31u,        (f.fmt >> 5) & 31u};
    if (list == 1 && v < 64u) {
        for (uint32_t i = 0; i < 4u; i++) {
            bins[i] = apu->hrtf_submix[i];
        }
    }
    const uint32_t va = wl_rd_le32(rec + WL_APU_V_VOLA), vb = wl_rd_le32(rec + WL_APU_V_VOLB),
                   vc = wl_rd_le32(rec + WL_APU_V_VOLC);
    const uint32_t vols[8] = {(va >> 4) & 0xFFFu,
                              va >> 20,
                              (vb >> 4) & 0xFFFu,
                              vb >> 20,
                              (vc >> 4) & 0xFFFu,
                              vc >> 20,
                              (va & 0xFu) | (vb & 0xFu) << 4 | (vc & 0xFu) << 8,
                              ((va >> 16) & 0xFu) | ((vb >> 16) & 0xFu) << 4 |
                                  ((vc >> 16) & 0xFu) << 8};
    for (uint32_t t = 0; t < 8u; t++) {
        const uint32_t g1 = ((volume_gain(vols[t]) >> 1) * (ea >> 1)) >> 15; // Q15
        const uint32_t g0 = vs->gain[t];
        vs->gain[t] = (uint16_t)g1;
        if ((g0 == 0 && g1 == 0) || any == 0) {
            continue;
        }
        const int32_t *s = src[t % f.channels];
        int32_t *bin = apu->mix[bins[t]];
        apu->mix_used |= 1u << bins[t];
        int32_t acc = (int32_t)g0 << 5; // Q20: the gain ramps over the frame
        const int32_t d = (int32_t)g1 - (int32_t)g0;
        for (uint32_t i = 0; i < WL_XBOX_APU_FRAME; i++) {
            bin[i] += (s[i] * (acc >> 5)) >> 15;
            acc += d;
        }
    }

    // the position the guest reads (DirectSound's GetCurrentPosition)
    const uint32_t pos = ended && !f.stream ? f.ebo : vs->pos;
    uint32_t cbo_word = wl_rd_le32(rec + WL_APU_V_CBO);
    wl_wr_le32(rec + WL_APU_V_CBO, (cbo_word & 0xFF000000u) | (pos & 0x00FFFFFFu));
    vs->cbo_written = pos & 0x00FFFFFFu;
    if (ended) {
        apu->stats.voices_ended++;
        wl_apu_voice_off(apu, v);
    }
}

// ---- The frame ---------------------------------------------------------------------------------

void wl_apu_vp_frame(WlXboxApu *apu) {
    wl_apu_clear_mix(apu);
    uint32_t voices = 0;
    for (uint32_t list = 0; list < 3u; list++) {
        const uint32_t regs = WL_APU_TVL2D + list * WL_APU_LIST_STRIDE;
        uint32_t v = wl_apu_reg(apu, regs) & 0xFFFFu;
        for (uint32_t n = 0; v != WL_APU_NO_VOICE; n++) {
            uint8_t *rec = wl_apu_voice_rec(apu, v);
            if (rec == NULL || n >= WL_XBOX_APU_VOICES) {
                if (rec != NULL) {
                    apu->stats.list_loops++;
                }
                break;
            }
            const uint32_t next = wl_rd_le32(rec + WL_APU_V_PITCH_LINK) & 0xFFFFu;
            wl_apu_set_reg(apu, regs + 4u, v);
            wl_apu_set_reg(apu, regs + 8u, next);
            if ((wl_rd_le32(rec + WL_APU_V_STATE) & WL_APU_STATE_ACTIVE) == 0) {
                wl_apu_idle_voice(apu, v);
            } else {
                voice_frame(apu, v, rec, list);
                voices++;
            }
            v = next;
        }
        wl_apu_set_reg(apu, regs + 4u, WL_APU_NO_VOICE);
    }
    apu->stats.voice_frames += voices;
    if (voices > apu->stats.voices_max) {
        apu->stats.voices_max = voices;
    }
}
