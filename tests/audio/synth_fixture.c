// SPDX-License-Identifier: MIT
#include "tests/audio/synth_fixture.h"

#include <string.h>

#include "wl/audio/adpcm.h"
#include "wl/base/endian.h"

static void sub_header(uint8_t *f, const char *tag, size_t size, uint16_t blocks) {
    memcpy(f, tag, 4);
    wl_wr_le16(f + 4, 0xFEFFu);
    wl_wr_le16(f + 6, 0x0100u);
    wl_wr_le32(f + 8, (uint32_t)size);
    wl_wr_le16(f + 12, 16);
    wl_wr_le16(f + 14, blocks);
    memcpy(f + 16, "DATA", 4);
}

size_t synfix_sseq(uint8_t *out, size_t cap, const uint8_t *cmds, size_t n) {
    const size_t total = 0x1Cu + n;
    if (cap < total) {
        return 0;
    }
    memset(out, 0, 0x1C);
    sub_header(out, "SSEQ", total, 1);
    wl_wr_le32(out + 20, (uint32_t)(total - 16u)); // DATA block size
    wl_wr_le32(out + 24, 0x1C);
    memcpy(out + 0x1C, cmds, n);
    return total;
}

size_t synfix_swar(uint8_t *out, size_t cap, const SynfixWave *w, uint32_t n) {
    size_t off = 0x3Cu + 4u * n;
    if (cap < off) {
        return 0;
    }
    memset(out, 0, off);
    wl_wr_le32(out + 0x38, n);
    for (uint32_t i = 0; i < n; i++) {
        const size_t bytes = ((size_t)w[i].loop_start_w + w[i].loop_len_w) * 4u;
        if (cap < off + 12u + bytes) {
            return 0;
        }
        wl_wr_le32(out + 0x3C + 4u * i, (uint32_t)off);
        out[off] = w[i].enc;
        out[off + 1] = w[i].loop ? 1 : 0;
        wl_wr_le16(out + off + 2, w[i].rate);
        wl_wr_le16(out + off + 4, 0);
        wl_wr_le16(out + off + 6, (uint16_t)w[i].loop_start_w);
        wl_wr_le32(out + off + 8, w[i].loop_len_w);
        memcpy(out + off + 12, w[i].data, bytes);
        off += 12u + bytes;
    }
    sub_header(out, "SWAR", off, 1);
    return off;
}

static void put_note(uint8_t *p, const SynfixNote *nt, bool with_type) {
    if (with_type) {
        wl_wr_le16(p, nt->type);
        p += 2;
    }
    wl_wr_le16(p, nt->wave);
    wl_wr_le16(p + 2, nt->slot);
    p[4] = nt->key;
    p[5] = nt->attack;
    p[6] = nt->decay;
    p[7] = nt->sustain;
    p[8] = nt->release;
    p[9] = nt->pan;
}

size_t synfix_sbnk(uint8_t *out, size_t cap, const SynfixInst *inst, uint32_t n) {
    size_t off = 0x3Cu + 4u * n;
    if (cap < off) {
        return 0;
    }
    memset(out, 0, off);
    wl_wr_le32(out + 0x38, n);
    for (uint32_t i = 0; i < n; i++) {
        const SynfixInst *p = &inst[i];
        uint8_t *rec = out + 0x3C + 4u * i;
        size_t len = 0;
        if (p->kind == 1) {
            len = 10;
        } else if (p->kind == 16) {
            len = 2u + 12u * ((size_t)p->hi - p->lo + 1u);
        } else if (p->kind == 17) {
            len = 8u + 12u * p->count;
        }
        if (p->kind != 0 && cap < off + len) {
            return 0;
        }
        rec[0] = p->kind;
        if (p->kind == 0) {
            continue;
        }
        wl_wr_le16(rec + 1, (uint16_t)off);
        uint8_t *q = out + off;
        memset(q, 0, len);
        if (p->kind == 1) {
            put_note(q, &p->notes[0], false);
            rec[0] = p->notes[0].type; // a single instrument's table entry carries the note's type
        } else if (p->kind == 16) {
            q[0] = p->lo;
            q[1] = p->hi;
            for (uint32_t k = 0; k <= (uint32_t)p->hi - p->lo; k++) {
                put_note(q + 2 + 12u * k, &p->notes[k < p->count ? k : p->count - 1u], true);
            }
        } else {
            memcpy(q, p->split_end, 8);
            for (uint32_t k = 0; k < p->count; k++) {
                put_note(q + 8 + 12u * k, &p->notes[k], true);
            }
        }
        off += len;
    }
    sub_header(out, "SBNK", off, 1);
    return off;
}

SynfixInst synfix_single(uint8_t type, uint16_t wave, uint8_t key, uint8_t a, uint8_t d, uint8_t s,
                     uint8_t r) {
    SynfixInst i;
    memset(&i, 0, sizeof i);
    i.kind = 1;
    i.count = 1;
    i.notes[0].type = type;
    i.notes[0].wave = wave;
    i.notes[0].key = key;
    i.notes[0].attack = a;
    i.notes[0].decay = d;
    i.notes[0].sustain = s;
    i.notes[0].release = r;
    i.notes[0].pan = 64;
    return i;
}

void synfix_wave16(int16_t *out, uint32_t n, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 12345u;
    int32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        v += (int32_t)((x >> 20) & 0x7FFu) - 1024; // a random walk, bounded by the clamp
        v = v > 28000 ? 28000 : v < -28000 ? -28000 : v;
        out[i] = (int16_t)v;
    }
}

size_t synfix_ima_encode(const int16_t *pcm, uint32_t n, uint8_t *out, int16_t *ref) {
    WlImaState st;
    wl_ima_init(&st, 0, 0);
    wl_wr_le16(out, 0);
    wl_wr_le16(out + 2, 0);
    memset(out + 4, 0, n / 2u);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t best = 0;
        int32_t best_err = 1 << 30;
        for (uint32_t nib = 0; nib < 16u; nib++) {
            WlImaState t = st;
            const int32_t e = (int32_t)wl_ima_step(&t, nib) - pcm[i];
            const int32_t ae = e < 0 ? -e : e;
            if (ae < best_err) {
                best_err = ae;
                best = nib;
            }
        }
        const int16_t d = wl_ima_step(&st, best);
        if (ref != NULL) {
            ref[i] = d;
        }
        out[4u + (i >> 1)] = (uint8_t)(out[4u + (i >> 1)] | ((i & 1u) ? (best << 4) : best));
    }
    return 4u + n / 2u;
}
