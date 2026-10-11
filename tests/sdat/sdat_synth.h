// SPDX-License-Identifier: MIT
// Synthetic SDAT builder shared by the SDAT tests: 2 sequences, a sequence archive with 2 sub-sequences,
// 1 bank holding every instrument type, 1 wave archive with PCM8/PCM16/ADPCM waves, 1 stream.
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *p;
    size_t len, cap;
} W;

static inline void wgrow(W *w, size_t n) {
    if (w->len + n > w->cap) {
        w->cap = (w->len + n) * 2 + 64;
        w->p = realloc(w->p, w->cap);
    }
}
static inline size_t wskip(W *w, size_t n) {
    wgrow(w, n);
    memset(w->p + w->len, 0, n);
    size_t at = w->len;
    w->len += n;
    return at;
}
static inline void p16(W *w, size_t at, unsigned v) {
    w->p[at] = (uint8_t)v;
    w->p[at + 1] = (uint8_t)(v >> 8);
}
static inline void p32(W *w, size_t at, uint32_t v) {
    p16(w, at, v & 0xFFFFu);
    p16(w, at + 2, v >> 16);
}
static inline void a8(W *w, unsigned v) {
    size_t at = wskip(w, 1);
    w->p[at] = (uint8_t)v;
}
static inline void a16(W *w, unsigned v) {
    p16(w, wskip(w, 2), v);
}
static inline void a32(W *w, uint32_t v) {
    p32(w, wskip(w, 4), v);
}
static inline void atag(W *w, const char *t) {
    size_t at = wskip(w, 4);
    memcpy(w->p + at, t, 4);
}
static inline void apad4(W *w) {
    while (w->len & 3u) {
        a8(w, 0);
    }
}
static inline void wappend(W *w, const W *o) {
    if (o->len == 0) {
        return;
    }
    size_t at = wskip(w, o->len);
    memcpy(w->p + at, o->p, o->len);
}

/* common sub-file header with one DATA block; returns after "DATA", size placeholder, data offset */
static inline void sub_begin(W *w, const char *tag) {
    atag(w, tag);
    a16(w, 0xFEFF);
    a16(w, 0x0100);
    a32(w, 0); /* file size */
    a16(w, 16);
    a16(w, 1);
    atag(w, "DATA");
    a32(w, 0); /* block size */
}
static inline void sub_end(W *w) {
    apad4(w);
    p32(w, 8, (uint32_t)w->len);
    p32(w, 20, (uint32_t)w->len - 16u);
}

static inline void build_sseq(W *w, unsigned tag_byte) {
    sub_begin(w, "SSEQ");
    a32(w, 0x1C);
    a8(w, 0xFE); a16(w, 0xFFFF); a8(w, 0x93); a8(w, 0); a8(w, 0xFF); a8(w, tag_byte);
    sub_end(w);
}

static inline void build_ssar(W *w) {
    sub_begin(w, "SSAR");
    a32(w, 0x30);
    a32(w, 2);
    for (unsigned i = 0; i < 2; i++) { /* offset, bank, vol, cpr, ppr, ply, pad */
        a32(w, 0x38u + i);
        a16(w, 0);
        a8(w, 100); a8(w, 64); a8(w, 32); a8(w, 1);
        a16(w, 0);
    }
    a8(w, 0xFF); a8(w, 0xFF); a8(w, 0xFF); a8(w, 0xFF); /* sequence bytes */
    sub_end(w);
}

/* note record: type, swav, swar, key, A, D, S, R, pan */
static inline void note(W *w, unsigned type, unsigned swav, unsigned slot) {
    a16(w, type); a16(w, swav); a16(w, slot);
    a8(w, 60); a8(w, 255); a8(w, 255); a8(w, 127); a8(w, 255); a8(w, 64);
}

static inline void build_sbnk(W *w) {
    sub_begin(w, "SBNK");
    wskip(w, 32);
    a32(w, 5);
    size_t tab = wskip(w, 5 * 4);
    /* 0: single PCM swav 0 slot 0 */
    p32(w, tab, 1u | ((uint32_t)w->len << 8));
    a16(w, 0); a16(w, 0); a8(w, 60); a8(w, 255); a8(w, 255); a8(w, 127); a8(w, 255); a8(w, 64);
    /* 1: PSG duty 3 */
    p32(w, tab + 4, 2u | ((uint32_t)w->len << 8));
    a16(w, 3); a16(w, 0); a8(w, 69); a8(w, 255); a8(w, 255); a8(w, 127); a8(w, 255); a8(w, 64);
    /* 2: noise */
    p32(w, tab + 8, 3u | ((uint32_t)w->len << 8));
    a16(w, 0); a16(w, 0); a8(w, 60); a8(w, 255); a8(w, 255); a8(w, 127); a8(w, 255); a8(w, 64);
    /* 3: drum set keys 60..61 -> swav 1, 2 */
    p32(w, tab + 12, 16u | ((uint32_t)w->len << 8));
    a8(w, 60); a8(w, 61);
    note(w, 1, 1, 0);
    note(w, 1, 2, 0);
    /* 4: key split, two regions (keys <= 63, <= 127) */
    p32(w, tab + 16, 17u | ((uint32_t)w->len << 8));
    a8(w, 63); a8(w, 127); for (int i = 0; i < 6; i++) { a8(w, 0); }
    note(w, 1, 0, 0);
    note(w, 1, 2, 0);
    sub_end(w);
}

static inline void build_swar(W *w, unsigned pcm8_bytes) {
    sub_begin(w, "SWAR");
    wskip(w, 32);
    a32(w, 3);
    size_t tab = wskip(w, 3 * 4);
    /* header: type, loop, rate, time, loop start (words), length (words) */
    struct { unsigned type, loop, rate, ls, ll; } t[3] = {
        {0, 0, 8000, 0, pcm8_bytes / 4}, {1, 1, 32728, 2, 6}, {2, 1, 22050, 3, 5}};
    for (int i = 0; i < 3; i++) {
        p32(w, tab + 4u * (unsigned)i, (uint32_t)w->len);
        a8(w, t[i].type); a8(w, t[i].loop); a16(w, t[i].rate); a16(w, 0);
        a16(w, t[i].ls); a32(w, t[i].ll);
        wskip(w, (t[i].ls + t[i].ll) * 4u);
    }
    sub_end(w);
}

typedef struct {
    int symbols;
    unsigned seq_bank;  /* the bank sequence 1 names */
    unsigned pcm8_bytes;
} Opt;

/* INFO / SYMB list helper: sets the kind's list offset, count, and returns the table position */
static inline size_t list_begin(W *b, unsigned kind, uint32_t count) {
    p32(b, 8u + 4u * kind, (uint32_t)b->len);
    a32(b, count);
    return wskip(b, 4u * count);
}

static inline void build_sdat(W *out, const Opt *o) {
    W f[6] = {{0}};
    build_sseq(&f[0], 0x11);
    build_sseq(&f[1], 0x22);
    build_sbnk(&f[2]);
    build_swar(&f[3], o->pcm8_bytes);
    build_ssar(&f[4]);
    atag(&f[5], "STRM"); a16(&f[5], 0xFEFF); a16(&f[5], 0x0100); a32(&f[5], 32); wskip(&f[5], 20);

    /* INFO */
    W info = {0};
    atag(&info, "INFO"); a32(&info, 0); wskip(&info, 32);
    size_t t = list_begin(&info, WL_SDAT_SEQ, 2);
    for (unsigned i = 0; i < 2; i++) {
        p32(&info, t + 4 * i, (uint32_t)info.len);
        a16(&info, i); a16(&info, 0); a16(&info, i == 0 ? 0 : o->seq_bank);
        a8(&info, 90 + i); a8(&info, 64); a8(&info, 32); a8(&info, 1); a16(&info, 0);
    }
    t = list_begin(&info, WL_SDAT_SEQARC, 1);
    p32(&info, t, (uint32_t)info.len); a16(&info, 4); a16(&info, 0);
    t = list_begin(&info, WL_SDAT_BANK, 1);
    p32(&info, t, (uint32_t)info.len);
    a16(&info, 2); a16(&info, 0); a16(&info, 0); a16(&info, 0xFFFF); a16(&info, 0xFFFF); a16(&info, 0xFFFF);
    t = list_begin(&info, WL_SDAT_WAVEARC, 1);
    p32(&info, t, (uint32_t)info.len); a16(&info, 3); a16(&info, 0);
    t = list_begin(&info, WL_SDAT_PLAYER, 1);
    p32(&info, t, (uint32_t)info.len); a16(&info, 4); a16(&info, 0); a32(&info, 0xFFFF); a32(&info, 0);
    t = list_begin(&info, WL_SDAT_GROUP, 1);
    p32(&info, t, (uint32_t)info.len);
    a32(&info, 2);
    a8(&info, 0); a8(&info, 0); a16(&info, 3); a32(&info, 0);
    a8(&info, 2); a8(&info, 0); a16(&info, 3); a32(&info, 0);
    t = list_begin(&info, WL_SDAT_PLAYER2, 1);
    p32(&info, t, (uint32_t)info.len); a8(&info, 16); wskip(&info, 16 + 7);
    t = list_begin(&info, WL_SDAT_STRM, 1);
    p32(&info, t, (uint32_t)info.len);
    a16(&info, 5); a16(&info, 0); a8(&info, 127); a8(&info, 64); a8(&info, 1); wskip(&info, 5);
    apad4(&info);
    p32(&info, 4, (uint32_t)info.len);

    /* SYMB */
    W sy = {0};
    if (o->symbols) {
        atag(&sy, "SYMB"); a32(&sy, 0); wskip(&sy, 32);
        const char *seq[2] = {"SEQ_ONE", "SEQ_TWO"};
        size_t ta = list_begin(&sy, WL_SDAT_SEQ, 2);
        for (unsigned i = 0; i < 2; i++) {
            p32(&sy, ta + 4 * i, (uint32_t)sy.len);
            size_t at = wskip(&sy, strlen(seq[i]) + 1);
            memcpy(sy.p + at, seq[i], strlen(seq[i]));
        }
        ta = list_begin(&sy, WL_SDAT_SEQARC, 1);
        size_t pair = wskip(&sy, 8);
        p32(&sy, ta, (uint32_t)pair);
        p32(&sy, pair, (uint32_t)sy.len);
        size_t at = wskip(&sy, 8); memcpy(sy.p + at, "SSAR_ONE", 8); a8(&sy, 0);
        p32(&sy, pair + 4, (uint32_t)sy.len);
        a32(&sy, 2);
        size_t sub = wskip(&sy, 8);
        const char *sn[2] = {"SUB_A", "SUB_B"};
        for (int i = 0; i < 2; i++) {
            p32(&sy, sub + 4u * (unsigned)i, (uint32_t)sy.len);
            at = wskip(&sy, strlen(sn[i]) + 1);
            memcpy(sy.p + at, sn[i], strlen(sn[i]));
        }
        ta = list_begin(&sy, WL_SDAT_BANK, 1);
        p32(&sy, ta, (uint32_t)sy.len);
        at = wskip(&sy, 11); memcpy(sy.p + at, "BANK_SE001", 10);
        ta = list_begin(&sy, WL_SDAT_WAVEARC, 1);
        p32(&sy, ta, (uint32_t)sy.len);
        at = wskip(&sy, 14); memcpy(sy.p + at, "WAVE_ARC_SE01", 13);
        list_begin(&sy, WL_SDAT_PLAYER, 0);
        list_begin(&sy, WL_SDAT_GROUP, 0);
        list_begin(&sy, WL_SDAT_PLAYER2, 0);
        list_begin(&sy, WL_SDAT_STRM, 0);
        apad4(&sy);
        p32(&sy, 4, (uint32_t)sy.len);
    }

    /* FAT + FILE */
    W fat = {0}, file = {0};
    atag(&fat, "FAT "); a32(&fat, 0); a32(&fat, 6);
    atag(&file, "FILE"); a32(&file, 0); a32(&file, 6); a32(&file, 0);
    size_t fat_blk = 12u + 6u * 16u, file_hdr = 16;
    size_t base = 0x40u + sy.len + info.len + fat_blk + file_hdr;
    size_t cur = base;
    for (int i = 0; i < 6; i++) {
        a32(&fat, (uint32_t)cur); a32(&fat, (uint32_t)f[i].len); wskip(&fat, 8);
        wappend(&file, &f[i]);
        cur += f[i].len;
    }
    p32(&fat, 4, (uint32_t)fat.len);
    p32(&file, 4, (uint32_t)file.len);

    atag(out, "SDAT"); a16(out, 0xFEFF); a16(out, 0x0100);
    a32(out, (uint32_t)(0x40 + sy.len + info.len + fat.len + file.len));
    a16(out, 0x40); a16(out, 4);
    size_t at = 0x40u;
    a32(out, sy.len ? (uint32_t)at : 0); a32(out, (uint32_t)sy.len); at += sy.len;
    a32(out, (uint32_t)at); a32(out, (uint32_t)info.len); at += info.len;
    a32(out, (uint32_t)at); a32(out, (uint32_t)fat.len); at += fat.len;
    a32(out, (uint32_t)at); a32(out, (uint32_t)file.len);
    wskip(out, 0x40 - out->len);
    wappend(out, &sy); wappend(out, &info); wappend(out, &fat); wappend(out, &file);
    for (int i = 0; i < 6; i++) free(f[i].p);
    free(info.p); free(sy.p); free(fat.p); free(file.p);
}

static const Opt GOOD = {1, 0, 16};
