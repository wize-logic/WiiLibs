// SPDX-License-Identifier: MIT
// Canonical SDAT index (see index.h): counts, per-file lines with size and SHA-1, sequence
// closures, unresolved references.
#include "wl/sdat/index.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    char *p;
    size_t len, cap;
    int oom;
} Buf;

static void put(Buf *b, const char *fmt, ...) WL_PRINTF_FMT(2, 3);
static void put(Buf *b, const char *fmt, ...) {
    if (b->oom) {
        return;
    }
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        b->oom = 1;
        return;
    }
    if (b->len + (size_t)n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->len + (size_t)n + 1) {
            nc *= 2;
        }
        char *np = realloc(b->p, nc);
        if (np == NULL) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, tmp, (size_t)n + 1);
    b->len += (size_t)n;
}

/* Names are identifiers; anything unexpected is flattened so the line format stays parseable. */
static void name_of(const char *in, char out[96]) {
    if (in == NULL || in[0] == 0) {
        strcpy(out, "-");
        return;
    }
    size_t i = 0;
    for (; in[i] != 0 && i < 95; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (c > 0x20 && c < 0x7F) ? (char)c : '_';
    }
    out[i] = 0;
}

typedef struct {
    const WlSdat *s;
    uint8_t (*sha)[WL_SHA1_SIZE]; /* per file id, valid when have[id] */
    uint8_t *have;
} Ctx;

static void hex_of(Ctx *c, uint32_t id, const uint8_t *d, size_t n, char hex[WL_SHA1_HEX_SIZE]) {
    if (!c->have[id]) {
        wl_sha1_buf(d, n, c->sha[id]);
        c->have[id] = 1;
    }
    wl_sha1_to_hex(c->sha[id], hex);
}

typedef struct {
    int ok;
    WlSwar swar;
    uint32_t waves, by_enc[3];
} Arc;

typedef struct {
    int ok;
    WlSbnk bank;
} BankV;

WlResult wl_sdat_index_build(const WlSdat *s, WlSdatIndex *ix) {
    if (s == NULL || ix == NULL) {
        return WL_ERR_ARG;
    }
    memset(ix, 0, sizeof *ix);
    WlSdatSummary *sm = &ix->sum;
    Ctx c = {s, NULL, NULL};
    c.sha = calloc(s->file_count ? s->file_count : 1, WL_SHA1_SIZE);
    c.have = calloc(s->file_count ? s->file_count : 1, 1);
    uint32_t nw = s->counts[WL_SDAT_WAVEARC], nb = s->counts[WL_SDAT_BANK];
    Arc *arcs = calloc(nw ? nw : 1, sizeof *arcs);
    BankV *banks = calloc(nb ? nb : 1, sizeof *banks);
    WlSbnkInst *inst = malloc(sizeof *inst);
    Buf b = {0};
    if (c.sha == NULL || c.have == NULL || arcs == NULL || banks == NULL || inst == NULL) {
        free(c.sha); free(c.have); free(arcs); free(banks); free(inst);
        return WL_ERR_NOMEM;
    }
    char hex[WL_SHA1_HEX_SIZE], nm[96];
    const uint8_t *d;
    size_t n;

    sm->files = s->file_count;
    for (int k = 0; k < WL_SDAT_KINDS; k++) {
        uint32_t cnt = s->counts[k], pres = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            switch (k) {
            case WL_SDAT_SEQ: pres += (uint32_t)(s->seq[i].present != 0); break;
            case WL_SDAT_SEQARC: pres += (uint32_t)(s->seqarc[i].present != 0); break;
            case WL_SDAT_BANK: pres += (uint32_t)(s->bank[i].present != 0); break;
            case WL_SDAT_WAVEARC: pres += (uint32_t)(s->wavearc[i].present != 0); break;
            case WL_SDAT_PLAYER: pres += (uint32_t)(s->player[i].present != 0); break;
            case WL_SDAT_GROUP: pres += (uint32_t)(s->group[i].present != 0); break;
            case WL_SDAT_PLAYER2: pres += (uint32_t)(s->player2[i].present != 0); break;
            default: pres += (uint32_t)(s->strm[i].present != 0); break;
            }
        }
        uint32_t *dst[WL_SDAT_KINDS] = {&sm->seq, &sm->seqarc, &sm->bank, &sm->wavearc,
                                        &sm->player, &sm->group, &sm->player2, &sm->strm};
        *dst[k] = pres;
    }

    put(&b, "wlsdat-index 1\n");
    put(&b, "counts seq=%u seqarc=%u bank=%u wavearc=%u strm=%u player=%u group=%u player2=%u "
            "files=%u symbols=%d\n",
        sm->seq, sm->seqarc, sm->bank, sm->wavearc, sm->strm, sm->player, sm->group, sm->player2,
        sm->files, s->has_symbols);

    /* wave archives first: bank and closure lines need their wave counts */
    for (uint32_t i = 0; i < nw; i++) {
        const WlSdatWaveArc *w = &s->wavearc[i];
        if (!w->present) {
            continue;
        }
        name_of(w->name, nm);
        if (wl_sdat_file(s, w->file_id, &d, &n) != WL_OK || wl_swar_open(&arcs[i].swar, d, n) != WL_OK) {
            sm->unresolved++;
            put(&b, "wavearc %u %s UNRESOLVED file=%u\n", i, nm, w->file_id);
            continue;
        }
        arcs[i].ok = 1;
        hex_of(&c, w->file_id, d, n, hex);
        WlSwar *sw = &arcs[i].swar;
        for (uint32_t k = 0; k < sw->count; k++) {
            WlSwavInfo wi;
            if (wl_swar_wave(sw, k, &wi) != WL_OK) {
                sm->unresolved++;
                continue;
            }
            if (wi.present) {
                arcs[i].waves++;
                if (wi.encoding <= WL_SWAV_ADPCM) {
                    arcs[i].by_enc[wi.encoding]++;
                }
            }
        }
        sm->waves += arcs[i].waves;
        for (int e = 0; e < 3; e++) {
            sm->wave_by_enc[e] += arcs[i].by_enc[e];
        }
        sm->wave_bytes += n;
        put(&b, "wavearc %u %s file=%u size=%zu sha1=%s slots=%u waves=%u pcm8=%u pcm16=%u adpcm=%u\n",
            i, nm, w->file_id, n, hex, sw->count, arcs[i].waves, arcs[i].by_enc[0],
            arcs[i].by_enc[1], arcs[i].by_enc[2]);
    }

    for (uint32_t i = 0; i < nb; i++) {
        const WlSdatBank *bk = &s->bank[i];
        if (!bk->present) {
            continue;
        }
        name_of(bk->name, nm);
        if (wl_sdat_file(s, bk->file_id, &d, &n) != WL_OK || wl_sbnk_open(&banks[i].bank, d, n) != WL_OK) {
            sm->unresolved++;
            put(&b, "bank %u %s UNRESOLVED file=%u\n", i, nm, bk->file_id);
            continue;
        }
        banks[i].ok = 1;
        hex_of(&c, bk->file_id, d, n, hex);
        sm->bank_bytes += n;
        uint32_t refs = 0, miss = 0, nonempty = 0;
        for (uint32_t k = 0; k < banks[i].bank.count; k++) {
            if (wl_sbnk_inst(&banks[i].bank, k, inst) != WL_OK) {
                sm->unresolved++;
                continue;
            }
            nonempty += inst->type != WL_SBNK_EMPTY;
            for (uint32_t q = 0; q < inst->note_count; q++) {
                const WlSbnkNote *nt = &inst->notes[q];
                if (nt->type != WL_SBNK_PCM) {
                    continue;
                }
                refs++;
                uint16_t wa = nt->wave_slot < 4 ? bk->wavearc[nt->wave_slot] : WL_SDAT_NONE;
                WlSwavInfo wi;
                if (wa == WL_SDAT_NONE || wa >= nw || !arcs[wa].ok ||
                    wl_swar_wave(&arcs[wa].swar, nt->wave, &wi) != WL_OK || !wi.present) {
                    miss++;
                }
            }
        }
        sm->inst_unresolved += miss;
        put(&b, "bank %u %s file=%u size=%zu sha1=%s insts=%u used=%u refs=%u missing=%u arcs=%d,%d,%d,%d\n",
            i, nm, bk->file_id, n, hex, banks[i].bank.count, nonempty, refs, miss,
            bk->wavearc[0] == WL_SDAT_NONE ? -1 : bk->wavearc[0],
            bk->wavearc[1] == WL_SDAT_NONE ? -1 : bk->wavearc[1],
            bk->wavearc[2] == WL_SDAT_NONE ? -1 : bk->wavearc[2],
            bk->wavearc[3] == WL_SDAT_NONE ? -1 : bk->wavearc[3]);
    }

    /* closure of a bank: the wave archives it names; a missing archive is an unresolved ref */
#define CLOSURE(LABEL_FMT, ...)                                                                 \
    do {                                                                                        \
        uint32_t bk_ = bank_id;                                                                 \
        put(&b, "closure " LABEL_FMT " bank=%u arcs=", __VA_ARGS__, bk_);                       \
        uint64_t bytes_ = 0;                                                                    \
        int first_ = 1;                                                                         \
        if (bk_ >= nb || !s->bank[bk_].present || !banks[bk_].ok) {                             \
            sm->unresolved++;                                                                   \
            put(&b, "UNRESOLVED\n");                                                            \
        } else {                                                                                \
            size_t bsz_ = banks[bk_].bank.size;                                                 \
            bytes_ = bsz_;                                                                      \
            for (int w_ = 0; w_ < 4; w_++) {                                                    \
                uint16_t wa_ = s->bank[bk_].wavearc[w_];                                        \
                if (wa_ == WL_SDAT_NONE) {                                                      \
                    continue;                                                                   \
                }                                                                               \
                if (wa_ >= nw || !arcs[wa_].ok) {                                               \
                    sm->unresolved++;                                                           \
                    put(&b, "%sUNRESOLVED(%u)", first_ ? "" : ",", wa_);                        \
                } else {                                                                        \
                    put(&b, "%s%u", first_ ? "" : ",", wa_);                                    \
                    bytes_ += arcs[wa_].swar.size;                                              \
                }                                                                               \
                first_ = 0;                                                                     \
            }                                                                                   \
            put(&b, "%s bytes=%llu\n", first_ ? "-" : "", (unsigned long long)bytes_);          \
        }                                                                                       \
    } while (0)

    for (uint32_t i = 0; i < s->counts[WL_SDAT_SEQ]; i++) {
        const WlSdatSeq *q = &s->seq[i];
        if (!q->present) {
            continue;
        }
        name_of(q->name, nm);
        if (wl_sdat_file(s, q->file_id, &d, &n) != WL_OK) {
            sm->unresolved++;
            put(&b, "seq %u %s UNRESOLVED file=%u\n", i, nm, q->file_id);
            continue;
        }
        WlSseq sq;
        if (wl_sseq_open(&sq, d, n) != WL_OK) {
            sm->unresolved++;
            put(&b, "seq %u %s BADFILE file=%u\n", i, nm, q->file_id);
            continue;
        }
        hex_of(&c, q->file_id, d, n, hex);
        sm->seq_bytes += n;
        put(&b, "seq %u %s file=%u size=%zu sha1=%s bank=%u player=%u vol=%u cpr=%u ppr=%u\n", i, nm,
            q->file_id, n, hex, q->bank, q->player, q->volume, q->chan_prio, q->player_prio);
    }
    for (uint32_t i = 0; i < s->counts[WL_SDAT_SEQ]; i++) {
        if (s->seq[i].present) {
            uint32_t bank_id = s->seq[i].bank;
            CLOSURE("seq %u", i);
        }
    }

    for (uint32_t i = 0; i < s->counts[WL_SDAT_SEQARC]; i++) {
        const WlSdatSeqArc *a = &s->seqarc[i];
        if (!a->present) {
            continue;
        }
        name_of(a->name, nm);
        WlSsar ar;
        if (wl_sdat_file(s, a->file_id, &d, &n) != WL_OK || wl_ssar_open(&ar, d, n) != WL_OK) {
            sm->unresolved++;
            put(&b, "seqarc %u %s UNRESOLVED file=%u\n", i, nm, a->file_id);
            continue;
        }
        hex_of(&c, a->file_id, d, n, hex);
        sm->seq_bytes += n;
        sm->subseq += ar.count;
        put(&b, "seqarc %u %s file=%u size=%zu sha1=%s subseqs=%u\n", i, nm, a->file_id, n, hex,
            ar.count);
        for (uint32_t k = 0; k < ar.count; k++) {
            WlSsarEntry e;
            char sn[96];
            name_of(k < s->seqarc_sub_count[i] && s->seqarc_sub_names[i] != NULL
                        ? s->seqarc_sub_names[i][k] : NULL, sn);
            if (wl_ssar_entry(&ar, k, &e) != WL_OK) {
                sm->unresolved++;
                put(&b, "subseq %u.%u %s BADENTRY\n", i, k, sn);
                continue;
            }
            put(&b, "subseq %u.%u %s offset=%u bank=%u player=%u vol=%u\n", i, k, sn, e.offset,
                e.bank, e.player, e.volume);
            uint32_t bank_id = e.bank;
            CLOSURE("subseq %u.%u", i, k);
        }
    }

    for (uint32_t i = 0; i < s->counts[WL_SDAT_STRM]; i++) {
        const WlSdatStrm *st = &s->strm[i];
        if (!st->present) {
            continue;
        }
        name_of(st->name, nm);
        if (wl_sdat_file(s, st->file_id, &d, &n) != WL_OK || wl_strm_check(d, n) != WL_OK) {
            sm->unresolved++;
            put(&b, "strm %u %s UNRESOLVED file=%u\n", i, nm, st->file_id);
            continue;
        }
        hex_of(&c, st->file_id, d, n, hex);
        sm->strm_bytes += n;
        put(&b, "strm %u %s file=%u size=%zu sha1=%s player=%u vol=%u\n", i, nm, st->file_id, n, hex,
            st->player, st->volume);
    }

    put(&b, "end unresolved=%u inst_unresolved=%u\n", sm->unresolved, sm->inst_unresolved);

    free(c.sha); free(c.have); free(arcs); free(banks); free(inst);
    if (b.oom) {
        free(b.p);
        return WL_ERR_NOMEM;
    }
    ix->text = b.p;
    ix->text_len = b.len;
    wl_sha1_buf(b.p, b.len, ix->digest);
    return WL_OK;
}

void wl_sdat_index_free(WlSdatIndex *ix) {
    if (ix != NULL) {
        free(ix->text);
        memset(ix, 0, sizeof *ix);
    }
}
