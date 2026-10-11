// SPDX-License-Identifier: MIT
// Dev survey of a 3DS image: NCSD -> NCCH 0 -> ExeFS list and RomFS walk; optionally opens every
// GARC and trial-decodes every sub-file that looks like LZ11. Output is paths, sizes and counts.
#include <stdlib.h>
#include <string.h>

#include "wl/ctr/ctr.h"
#include "wl/nds/lz.h"

#define SURVEY_MAX_GARC ((size_t)1 << 30)
#define SURVEY_MAX_DECODED (64u * 1024u * 1024u)

typedef struct {
    const WlRomfs *r;
    WlCtrSurvey *s;
    int garcs;
    FILE *list;
} Ctx;

static void survey_garc(Ctx *c, const WlRomfsEntry *e) {
    WlCtrSurvey *s = c->s;
    uint8_t *buf;
    size_t len;
    if (wl_romfs_read_all(c->r, e, SURVEY_MAX_GARC, &buf, &len) != WL_OK) {
        s->failures++;
        return;
    }
    WlGarc g;
    if (wl_garc_open(&g, buf, len) != WL_OK) {
        s->failures++;
        free(buf);
        return;
    }
    s->garcs++;
    for (uint32_t i = 0; i < g.entries; i++) {
        uint32_t mask;
        if (wl_garc_mask(&g, i, &mask) != WL_OK) {
            s->failures++;
            continue;
        }
        s->garc_entries++;
        for (unsigned b = 0; b < 32; b++) {
            if ((mask & (1u << b)) == 0) {
                continue;
            }
            const uint8_t *d;
            size_t n;
            if (wl_garc_sub(&g, i, b, &d, &n) != WL_OK) {
                s->failures++;
                continue;
            }
            s->garc_subfiles++;
            if (wl_garc_is_lz11(d, n)) {
                uint8_t *out;
                size_t olen;
                if (wl_lz_decode(d, n, SURVEY_MAX_DECODED, &out, &olen) == WL_OK) {
                    s->lz11_ok++;
                    free(out);
                } else {
                    s->lz11_fail++;
                }
            }
        }
    }
    free(buf);
}

static WlResult visit(void *ud, const char *path, const WlRomfsEntry *e) {
    Ctx *c = (Ctx *)ud;
    WlCtrSurvey *s = c->s;
    if (e->is_dir) {
        s->romfs_dirs++;
        return WL_OK;
    }
    s->romfs_files++;
    s->romfs_bytes += e->size;
    if (c->list) {
        (void)fprintf(c->list, "%s %llu\n", path, (unsigned long long)e->size);
    }
    WlRomfsEntry f;
    if (wl_romfs_find_file(c->r, path, &f) != WL_OK || f.offset != e->offset || f.size != e->size) {
        s->lookup_fail++;
    }
    uint8_t magic[4];
    if (c->garcs && e->size >= 4 && wl_romfs_read(c->r, e, 0, magic, 4) == WL_OK &&
        wl_garc_is_garc(magic, 4)) {
        survey_garc(c, e);
    }
    return WL_OK;
}

WlResult wl_ctr_survey(WlCtrSrc *src, int garcs, FILE *list, WlCtrSurvey *s) {
    memset(s, 0, sizeof *s);
    WlNcsd n;
    WlResult r = wl_ncsd_open(&n, src);
    if (r != WL_OK) {
        return r;
    }
    WlNcch c;
    r = wl_ncch_open(&c, src, n.part_off[0], n.part_size[0]);
    if (r != WL_OK) {
        return r;
    }
    WlExefs ex;
    if (c.exefs_size != 0 && wl_exefs_open(&ex, &c) == WL_OK) {
        s->exefs_files = ex.count;
    }
    if (c.romfs_size == 0) {
        return WL_OK;
    }
    WlRomfs rf;
    r = wl_romfs_open(&rf, &c);
    if (r != WL_OK) {
        return r;
    }
    Ctx ctx = {&rf, s, garcs, list};
    r = wl_romfs_walk(&rf, visit, &ctx);
    wl_romfs_close(&rf);
    return r;
}
