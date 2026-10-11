// SPDX-License-Identifier: MIT
// Canonical SDAT index over the synthetic archive of sdat_synth.h: counts, names, closures,
// stability of the digest, unresolved references, and robustness against corrupted headers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/sdat/index.h"

#include "sdat_synth.h"

static WlResult build_index(const uint8_t *d, size_t n, WlSdatIndex *ix) {
    WlSdat s;
    WlResult r = wl_sdat_open(&s, d, n);
    if (r != WL_OK) {
        return r;
    }
    r = wl_sdat_index_build(&s, ix);
    wl_sdat_close(&s);
    return r;
}

TEST index_symbols_and_stability(void) {
    W w = {0};
    build_sdat(&w, &GOOD);
    WlSdatIndex a, b;
    ASSERT_EQ(WL_OK, build_index(w.p, w.len, &a));
    ASSERT_EQ(WL_OK, build_index(w.p, w.len, &b));
    ASSERT_EQ(0, memcmp(a.digest, b.digest, WL_SHA1_SIZE));
    ASSERT_EQ(a.text_len, b.text_len);
    ASSERT_EQ(2u, a.sum.seq);
    ASSERT_EQ(1u, a.sum.seqarc);
    ASSERT_EQ(2u, a.sum.subseq);
    ASSERT_EQ(1u, a.sum.bank);
    ASSERT_EQ(1u, a.sum.wavearc);
    ASSERT_EQ(1u, a.sum.strm);
    ASSERT_EQ(3u, a.sum.waves);
    ASSERT_EQ(1u, a.sum.wave_by_enc[0]);
    ASSERT_EQ(1u, a.sum.wave_by_enc[1]);
    ASSERT_EQ(1u, a.sum.wave_by_enc[2]);
    ASSERT_EQ(0u, a.sum.unresolved);
    ASSERT_EQ(0u, a.sum.inst_unresolved);
    ASSERT(strstr(a.text, "seq 1 SEQ_TWO file=1") != NULL);
    ASSERT(strstr(a.text, "closure seq 0 bank=0 arcs=0 ") != NULL);
    ASSERT(strstr(a.text, "subseq 0.1 SUB_B") != NULL);
    ASSERT(strstr(a.text, "bank 0 BANK_SE001 file=2") != NULL);
    ASSERT(strstr(a.text, "wavearc 0 WAVE_ARC_SE01 file=3") != NULL);
    ASSERT(strncmp(a.text, "wlsdat-index 1\n", 15) == 0);
    ASSERT_EQ(strlen(a.text), a.text_len);
    wl_sdat_index_free(&b);
    /* flipping one byte of a referenced file changes the digest */
    w.p[w.len - 1] ^= 0x5A;
    ASSERT_EQ(WL_OK, build_index(w.p, w.len, &b));
    ASSERT(memcmp(a.digest, b.digest, WL_SHA1_SIZE) != 0);
    wl_sdat_index_free(&a);
    wl_sdat_index_free(&b);
    free(w.p);
    PASS();
}

TEST index_without_symbols(void) {
    W w = {0};
    Opt o = GOOD;
    o.symbols = 0;
    build_sdat(&w, &o);
    WlSdat s;
    ASSERT_EQ(WL_OK, wl_sdat_open(&s, w.p, w.len));
    ASSERT(!s.has_symbols);
    ASSERT(s.seq[0].name == NULL);
    wl_sdat_close(&s);
    WlSdatIndex ix;
    ASSERT_EQ(WL_OK, build_index(w.p, w.len, &ix));
    ASSERT_EQ(2u, ix.sum.seq);
    ASSERT_EQ(0u, ix.sum.unresolved);
    ASSERT(strstr(ix.text, "seq 0 - file=0") != NULL);
    wl_sdat_index_free(&ix);
    free(w.p);
    PASS();
}

TEST index_unresolved_refs(void) {
    W w = {0};
    Opt o = GOOD;
    o.seq_bank = 9; /* sequence 1 names a bank that does not exist */
    build_sdat(&w, &o);
    WlSdatIndex ix;
    ASSERT_EQ(WL_OK, build_index(w.p, w.len, &ix));
    ASSERT_EQ(1u, ix.sum.unresolved);
    ASSERT(strstr(ix.text, "closure seq 1 bank=9 arcs=UNRESOLVED") != NULL);
    wl_sdat_index_free(&ix);
    free(w.p);
    PASS();
}

TEST index_malformed(void) {
    W w = {0};
    build_sdat(&w, &GOOD);
    size_t n = w.len;
    uint8_t *c = malloc(n);
    /* FAT extent outside the file: opens, but the file is unreachable and the index flags it */
    memcpy(c, w.p, n);
    uint32_t fat_off = (uint32_t)(c[32] | (c[33] << 8) | (c[34] << 16));
    p32(&(W){c, n, n}, fat_off + 12 + 16 * 2, 0xFFFFFF00u);
    WlSdatIndex ix;
    ASSERT_EQ(WL_OK, build_index(c, n, &ix));
    ASSERT(ix.sum.unresolved > 0);
    ASSERT(strstr(ix.text, "bank 0 BANK_SE001 UNRESOLVED file=2") != NULL);
    wl_sdat_index_free(&ix);
    /* every single-byte mutation of the headers and tables must end in a result, not a crash */
    size_t lim = n < 0x300 ? n : 0x300;
    for (size_t i = 0; i < lim; i++) {
        for (unsigned v = 0; v < 2; v++) {
            memcpy(c, w.p, n);
            c[i] = v ? 0xFF : 0x00;
            WlSdatIndex mi;
            if (build_index(c, n, &mi) == WL_OK) {
                ASSERT_EQ(strlen(mi.text), mi.text_len);
                wl_sdat_index_free(&mi);
            }
        }
    }
    free(c);
    free(w.p);
    PASS();
}

SUITE(sdat_index_suite) {
    RUN_TEST(index_symbols_and_stability);
    RUN_TEST(index_without_symbols);
    RUN_TEST(index_unresolved_refs);
    RUN_TEST(index_malformed);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(sdat_index_suite);
    GREATEST_MAIN_END();
}
