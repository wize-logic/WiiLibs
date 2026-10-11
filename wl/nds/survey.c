// SPDX-License-Identifier: MIT
// Dev survey of a DS image: walks every file, opens NARCs, decompresses ARM9 and overlays.
// Output carries only paths, sizes and counts.
#include <stdlib.h>

#include "wl/nds/lz.h"
#include "wl/nds/nds.h"

#define WL_NDS_MAX_DECODED (64u * 1024u * 1024u)

typedef struct {
    const WlNds *n;
    int open_narcs, decode;
    FILE *list;
    WlNdsSurvey *s;
} SurveyCtx;

static void try_lz(WlNdsSurvey *s, const uint8_t *d, size_t sz) {
    uint8_t *out;
    size_t olen;
    s->lz_candidates++;
    if (wl_lz_decode(d, sz, WL_NDS_MAX_DECODED, &out, &olen) == WL_OK) {
        s->lz_ok++;
        free(out);
    } else {
        s->lz_fail++;
    }
}

/* Members that look like LZ10/LZ11 streams are trial-decoded; a miss is informational only,
 * because arbitrary data can start with 0x10 or 0x11. */
static WlResult count_member(void *ud, const char *path, uint32_t i, const uint8_t *d, size_t sz) {
    (void)path; (void)i;
    if (ud != NULL && wl_lz_looks_valid(d, sz)) {
        try_lz((WlNdsSurvey *)ud, d, sz);
    }
    return WL_OK;
}

static WlResult survey_file(void *ud, const char *path, uint32_t id, const uint8_t *d, size_t sz) {
    SurveyCtx *c = (SurveyCtx *)ud;
    WlNdsSurvey *s = c->s;
    (void)id;
    s->files++;
    s->bytes += sz;
    if (c->list) {
        (void)fprintf(c->list, "%s %zu\n", path, sz);
    }
    if (c->open_narcs && wl_narc_is_narc(d, sz)) {
        WlNarc a;
        WlResult r = wl_narc_open(&a, d, sz);
        if (r == WL_OK) {
            s->narcs++;
            s->narc_members += a.count;
            r = wl_narc_walk(&a, count_member, NULL);
            for (uint32_t m = 0; c->decode && m < a.count; m++) {
                const uint8_t *md;
                size_t ms;
                if (wl_narc_member(&a, m, &md, &ms) == WL_OK) {
                    (void)count_member(s, NULL, m, md, ms);
                }
            }
        }
        if (r != WL_OK) {
            s->narc_failed++;
            s->failures++;
            if (c->list) {
                (void)fprintf(c->list, "  ! narc error %d: %s\n", r, path);
            }
        } else if (c->list) {
            (void)fprintf(c->list, "  narc members=%u names=%d\n", (unsigned)a.count, a.has_names);
        }
    } else if (c->decode && wl_lz_looks_valid(d, sz)) {
        try_lz(s, d, sz);
    }
    return WL_OK;
}

static void try_blz(WlNdsSurvey *s, FILE *list, const char *what, uint32_t idx, const uint8_t *d,
                    size_t sz, int required) {
    uint8_t *out;
    size_t olen;
    if (!required && !wl_blz_footer_ok(d, sz)) {
        return;
    }
    WlResult r = wl_blz_decode(d, sz, WL_NDS_MAX_DECODED, &out, &olen);
    if (r == WL_OK) {
        s->blz_ok++;
        free(out);
    } else {
        s->blz_fail++;
        s->failures++;
        if (list) {
            (void)fprintf(list, "  ! blz error %d: %s %u\n", r, what, (unsigned)idx);
        }
    }
}

WlResult wl_nds_survey(const WlNds *n, int open_narcs, int decode, FILE *list, WlNdsSurvey *s) {
    if (n == NULL || s == NULL) {
        return WL_ERR_ARG;
    }
    *s = (WlNdsSurvey){0};
    SurveyCtx c = {n, open_narcs, decode, list, s};
    WlResult r = wl_nds_walk(n, survey_file, &c);
    if (r != WL_OK) {
        s->failures++;
        if (list) {
            (void)fprintf(list, "! file system walk error %d\n", r);
        }
        return WL_ERR_FAILED;
    }
    if (decode) {
        const uint8_t *a9 = n->data + n->arm9_off;
        if (wl_blz_footer_ok(a9, n->arm9_size)) {
            s->arm9_blz = 1;
            try_blz(s, list, "arm9", 0, a9, n->arm9_size, 1);
        }
        for (int arm9 = 1; arm9 >= 0; arm9--) {
            uint32_t cnt = arm9 ? n->ov9_count : n->ov7_count;
            for (uint32_t i = 0; i < cnt; i++) {
                WlNdsOverlay ov;
                const uint8_t *d;
                size_t sz;
                if (wl_nds_overlay(n, arm9, i, &ov) != WL_OK ||
                    wl_nds_overlay_data(n, &ov, &d, &sz) != WL_OK) {
                    s->failures++;
                    if (list) {
                        (void)fprintf(list, "  ! overlay %u (arm%d) out of range\n", (unsigned)i, arm9 ? 9 : 7);
                    }
                    continue;
                }
                s->overlays++;
                if (ov.compressed) {
                    try_blz(s, list, arm9 ? "ovl9" : "ovl7", i, d, sz, 1);
                }
            }
        }
    }
    return s->failures ? WL_ERR_FAILED : WL_OK;
}
