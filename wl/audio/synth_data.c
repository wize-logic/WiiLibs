// SPDX-License-Identifier: MIT
// SSEQ synth data side - SSEQ, SBNK and SWAR views, instrument and wave lookup, and the
// level / pitch tables. All files are little-endian; every read is range checked (inputs are
// untrusted file contents) and goes through byte accessors.
#include <string.h>

#include "wl/audio/synth_internal.h"
#include "wl/base/endian.h"

// ---------------------------------------------------------------------------------------------
// Tables (generated once from the formulas in the comments; integer only at run time)
// ---------------------------------------------------------------------------------------------

// Level of a 0..127 volume or velocity in tenths of a decibel: 400 * log10(v / 127), at least -723
// (a "square law": the amplitude is (v / 127) squared).
static const int16_t k_db[128] = {
    -723, -723, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371,
    -360, -349, -339, -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245,
    -239, -234, -229, -224, -219, -214, -210, -205, -201, -196, -192, -188, -184, -180, -176, -173,
    -169, -165, -162, -158, -155, -152, -149, -145, -142, -139, -136, -133, -130, -127, -125, -122,
    -119, -116, -114, -111, -109, -106, -103, -101, -99,  -96,  -94,  -91,  -89,  -87,  -85,  -82,
    -80,  -78,  -76,  -74,  -72,  -70,  -68,  -66,  -64,  -62,  -60,  -58,  -56,  -54,  -52,  -50,
    -49,  -47,  -45,  -43,  -42,  -40,  -38,  -36,  -35,  -33,  -31,  -30,  -28,  -27,  -25,  -23,
    -22,  -20,  -19,  -17,  -16,  -14,  -13,  -11,  -10,  -8,   -7,   -6,   -4,   -3,   -1,   0,
};

// 65536 * 2^(-i/64), i = 0..64.
static const uint32_t k_exp2[65] = {
    65536, 64830, 64132, 63441, 62757, 62081, 61413, 60751, 60097, 59449, 58809, 58176, 57549,
    56929, 56316, 55709, 55109, 54515, 53928, 53347, 52773, 52204, 51642, 51085, 50535, 49991,
    49452, 48920, 48393, 47871, 47356, 46846, 46341, 45842, 45348, 44859, 44376, 43898, 43425,
    42958, 42495, 42037, 41584, 41136, 40693, 40255, 39821, 39392, 38968, 38548, 38133, 37722,
    37316, 36914, 36516, 36123, 35734, 35349, 34968, 34591, 34219, 33850, 33486, 33125, 32768,
};

// 65536 * 2^(k/12) and 65536 * 2^(f/768): a pitch in 1/64 semitones is octave, semitone (r >> 6)
// and fine (r & 63).
static const uint32_t k_semi[12] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
};
static const uint32_t k_fine[64] = {
    65536, 65595, 65654, 65714, 65773, 65832, 65892, 65951, 66011, 66071, 66130, 66190, 66250,
    66309, 66369, 66429, 66489, 66549, 66609, 66670, 66730, 66790, 66850, 66911, 66971, 67032,
    67092, 67153, 67213, 67274, 67335, 67395, 67456, 67517, 67578, 67639, 67700, 67761, 67823,
    67884, 67945, 68007, 68068, 68129, 68191, 68252, 68314, 68376, 68438, 68499, 68561, 68623,
    68685, 68747, 68809, 68871, 68933, 68996, 69058, 69120, 69183, 69245, 69308, 69370,
};

// round(127 * sin(pi/2 * i / 32)): a quarter of the LFO wave.
static const uint8_t k_sin[33] = {
    0,  6,  12,  19,  25,  31,  37,  43,  49,  54,  60,  65,  71,  76,  81,  85,  90,
    94, 98, 102, 106, 109, 112, 115, 117, 120, 122, 123, 125, 126, 126, 127, 127,
};

int32_t wl_syn_volume_db(uint32_t v) {
    return k_db[v > 127u ? 127u : v];
}

// One LFO sample, -127..127, for a phase of 0..127 over a whole cycle.
int32_t wl_syn_sin(uint32_t phase128) {
    const uint32_t p = phase128 & 127u;
    const uint32_t q = p & 31u;
    // Quarters: 0..31 rising, 32..63 falling, 64..95 negative falling, 96..127 negative rising.
    const int32_t v = (p & 32u) ? k_sin[32u - q] : k_sin[q];
    return (p & 64u) ? -v : v;
}

uint32_t wl_syn_level_gain(int32_t level_q7) {
    if (level_q7 >= 0) {
        return 65536u;
    }
    const uint32_t t = (uint32_t)(-(int64_t)level_q7);
    if (t >= (uint32_t)(-WL_SYN_ENV_MIN)) {
        return 0;
    }
    // 1 octave of amplitude = 20 * log10(2) = 6.0206 dB = 60.206 tenths = 7706.4 level units. x is
    // the number of 1/64 octaves in Q8.
    const uint32_t x = (uint32_t)(((uint64_t)t * 17415u) >> 13);
    const uint32_t ip = x >> 8;
    const uint32_t fr = x & 255u;
    const uint32_t idx = ip & 63u;
    const uint32_t oct = ip >> 6;
    const uint32_t a = k_exp2[idx];
    const uint32_t b = k_exp2[idx + 1u];
    const uint32_t g = a - (((a - b) * fr) >> 8);
    return g >> oct;
}

uint32_t wl_syn_step(uint32_t rate, int32_t pitch64) {
    if (pitch64 > 768 * 8) {
        pitch64 = 768 * 8;
    } else if (pitch64 < -768 * 8) {
        pitch64 = -768 * 8;
    }
    int32_t oct = pitch64 / 768;
    int32_t r = pitch64 - oct * 768;
    if (r < 0) {
        r += 768;
        oct--;
    }
    const uint64_t mult = ((uint64_t)k_semi[r >> 6] * k_fine[r & 63]) >> 16; // Q16, 1..2
    const uint64_t base = (((uint64_t)rate << 16) + WL_SYN_RATE / 2u) / WL_SYN_RATE;
    uint64_t step = (base * mult) >> 16;
    if (oct >= 0) {
        step <<= (uint32_t)oct;
    } else {
        step >>= (uint32_t)(-oct);
    }
    if (step < 1024u) {
        step = 1024u;
    } else if (step > (16u << 16)) {
        step = 16u << 16;
    }
    return (uint32_t)step;
}

// ---------------------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------------------

static bool tag_is(const uint8_t *p, const char *t) {
    return memcmp(p, t, 4) == 0;
}

static bool range_ok(uint64_t off, uint64_t len, uint64_t size) {
    return off <= size && len <= size - off;
}

// The common sub-file header: tag, BOM, version, size, header size, block count; then "DATA".
static WlResult sub_header(const uint8_t *f, size_t n, const char *tag, size_t *fsize) {
    if (f == NULL || n < 0x14 || !tag_is(f, tag) || wl_rd_le16(f + 4) != 0xFEFFu) {
        return WL_ERR_FORMAT;
    }
    const size_t sz = wl_rd_le32(f + 8);
    if (sz < 0x14 || sz > n || wl_rd_le16(f + 12) < 16 || !tag_is(f + 16, "DATA")) {
        return WL_ERR_FORMAT;
    }
    *fsize = sz;
    return WL_OK;
}

WlResult wl_syn_seq_open(WlSynSeq *q, const uint8_t *sseq, size_t n) {
    if (q == NULL) {
        return WL_ERR_ARG;
    }
    size_t sz = 0;
    WlResult r = sub_header(sseq, n, "SSEQ", &sz);
    if (r != WL_OK) {
        return r;
    }
    if (sz < 0x1Cu) {
        return WL_ERR_FORMAT;
    }
    const uint32_t doff = wl_rd_le32(sseq + 24);
    if (doff < 0x1Cu || doff >= sz) {
        return WL_ERR_RANGE;
    }
    q->file = sseq;
    q->size = sz;
    q->data = sseq + doff;
    q->data_size = sz - doff;
    return WL_OK;
}

WlResult wl_syn_waves_open(WlSynWaveArc *w, const uint8_t *swar, size_t n) {
    if (w == NULL) {
        return WL_ERR_ARG;
    }
    size_t sz = 0;
    WlResult r = sub_header(swar, n, "SWAR", &sz);
    if (r != WL_OK) {
        return r;
    }
    if (sz < 0x3Cu) {
        return WL_ERR_FORMAT;
    }
    const uint32_t c = wl_rd_le32(swar + 0x38);
    if (!range_ok(0x3C, (uint64_t)c * 4u, sz)) {
        return WL_ERR_RANGE;
    }
    w->file = swar;
    w->size = sz;
    w->count = c;
    return WL_OK;
}

WlResult wl_syn_bank_open(WlSynBank *b, const uint8_t *sbnk, size_t n) {
    if (b == NULL) {
        return WL_ERR_ARG;
    }
    size_t sz = 0;
    WlResult r = sub_header(sbnk, n, "SBNK", &sz);
    if (r != WL_OK) {
        return r;
    }
    if (sz < 0x3Cu) {
        return WL_ERR_FORMAT;
    }
    const uint32_t c = wl_rd_le32(sbnk + 0x38);
    if (!range_ok(0x3C, (uint64_t)c * 4u, sz)) {
        return WL_ERR_RANGE;
    }
    memset(b, 0, sizeof *b);
    b->file = sbnk;
    b->size = sz;
    b->count = c;
    return WL_OK;
}

void wl_syn_bank_set_waves(WlSynBank *b, uint32_t slot, const WlSynWaveArc *w) {
    if (b == NULL || slot >= 4u) {
        return;
    }
    if (w == NULL) {
        memset(&b->war[slot], 0, sizeof b->war[slot]);
    } else {
        b->war[slot] = *w;
    }
}

// A note record without its leading type: wave, wave slot, key, A, D, S, R, pan (10 bytes).
static bool note_from(WlSynNote *nt, uint32_t type, const uint8_t *p) {
    if (type < 1u || type > 3u) {
        return false; // direct PCM and null instruments are not used by DS sound data in practice
    }
    nt->type = (uint8_t)type;
    nt->wave = wl_rd_le16(p);
    const uint32_t slot = wl_rd_le16(p + 2);
    nt->slot = (uint8_t)(slot & 3u);
    if (slot > 3u && type == WL_SYN_NOTE_PCM) {
        return false;
    }
    nt->key = p[4];
    nt->attack = (uint8_t)(p[5] & 127u);
    nt->decay = (uint8_t)(p[6] & 127u);
    nt->sustain = (uint8_t)(p[7] & 127u);
    nt->release = (uint8_t)(p[8] & 127u);
    nt->pan = (int8_t)((int32_t)(p[9] & 127u) - 64);
    return true;
}

bool wl_syn_bank_note(const WlSynBank *b, uint32_t prog, uint32_t key, WlSynNote *out) {
    if (b == NULL || b->file == NULL || out == NULL || prog >= b->count || key > 127u) {
        return false;
    }
    const uint8_t *rec = b->file + 0x3C + 4u * prog;
    const uint32_t type = rec[0];
    const uint32_t off = wl_rd_le16(rec + 1);
    memset(out, 0, sizeof *out);
    if (type >= 1u && type <= 5u) {
        return range_ok(off, 10, b->size) && note_from(out, type, b->file + off);
    }
    if (type == 16u) { // drum set: low, high, then 12-byte notes
        if (!range_ok(off, 2, b->size)) {
            return false;
        }
        const uint32_t lo = b->file[off];
        const uint32_t hi = b->file[off + 1];
        if (hi < lo || hi > 127u || key < lo || key > hi) {
            return false;
        }
        const uint32_t k = key - lo;
        if (!range_ok(off + 2u + 12u * k, 12, b->size)) {
            return false;
        }
        const uint8_t *p = b->file + off + 2u + 12u * k;
        return note_from(out, wl_rd_le16(p), p + 2);
    }
    if (type == 17u) { // key split: eight end keys (0 ends the list), then 12-byte notes
        if (!range_ok(off, 8, b->size)) {
            return false;
        }
        uint32_t region = 0;
        while (region < 8u && b->file[off + region] != 0 && key > b->file[off + region]) {
            region++;
        }
        if (region >= 8u || b->file[off + region] == 0) {
            return false;
        }
        if (!range_ok(off + 8u + 12u * region, 12, b->size)) {
            return false;
        }
        const uint8_t *p = b->file + off + 8u + 12u * region;
        return note_from(out, wl_rd_le16(p), p + 2);
    }
    return false;
}

bool wl_syn_wave_get(const WlSynWaveArc *w, uint32_t idx, WlSynWave *out) {
    if (w == NULL || w->file == NULL || out == NULL || idx >= w->count) {
        return false;
    }
    const uint32_t off = wl_rd_le32(w->file + 0x3C + 4u * idx);
    if (off == 0 || !range_ok(off, 12, w->size)) {
        return false;
    }
    const uint8_t *p = w->file + off;
    const uint32_t enc = p[0];
    const uint32_t loop_start_w = wl_rd_le16(p + 6);
    const uint32_t loop_len_w = wl_rd_le32(p + 8);
    const uint64_t words = (uint64_t)loop_start_w + loop_len_w;
    if (enc > 2u || !range_ok(off + 12u, words * 4u, w->size) || words == 0) {
        return false;
    }
    out->enc = (uint8_t)enc;
    out->rate = wl_rd_le16(p + 2);
    out->data = p + 12;
    uint32_t ns = 0, ls = 0;
    if (enc == 0) {
        ns = (uint32_t)words * 4u;
        ls = loop_start_w * 4u;
    } else if (enc == 1) {
        ns = (uint32_t)words * 2u;
        ls = loop_start_w * 2u;
    } else {
        if (words < 2u) {
            return false; // header only
        }
        ns = (uint32_t)words * 8u - 8u;
        ls = loop_start_w > 0 ? loop_start_w * 8u - 8u : 0;
    }
    out->nsamples = ns;
    out->loop_start = ls;
    out->loop = p[1] != 0 && loop_len_w != 0 && ls < ns;
    return out->rate != 0;
}

WlResult wl_syn_bank_check(const WlSynBank *b, WlSynBankCheck *out) {
    if (b == NULL || b->file == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    for (uint32_t prog = 0; prog < b->count; prog++) {
        const uint8_t *rec = b->file + 0x3C + 4u * prog;
        const uint32_t type = rec[0];
        if (type == 0) {
            continue;
        }
        out->instruments++;
        bool seen = false;
        WlSynNote prev;
        bool have_prev = false;
        for (uint32_t key = 0; key < 128u; key++) {
            WlSynNote nt;
            if (!wl_syn_bank_note(b, prog, key, &nt)) {
                have_prev = false;
                continue;
            }
            seen = true;
            // Neighbouring keys that resolve to the same record (one split region, a single
            // instrument) are one note definition.
            if (have_prev && memcmp(&prev, &nt, sizeof nt) == 0) {
                continue;
            }
            prev = nt;
            have_prev = true;
            out->notes++;
            if (nt.type == WL_SYN_NOTE_PCM) {
                out->pcm_notes++;
                WlSynWave wv;
                if (!wl_syn_wave_get(&b->war[nt.slot], nt.wave, &wv)) {
                    out->unresolved++;
                }
            }
        }
        if (!seen) {
            out->bad++;
        }
    }
    return WL_OK;
}
