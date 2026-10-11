// SPDX-License-Identifier: MIT
// FNT walker, written from the GBATEK description. See fnt.h.
#include "wl/nds/fnt.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

typedef struct {
    const uint8_t *fnt;
    size_t size;
    unsigned dir_count;
    uint8_t *visited;
    WlFntFileFn fn;
    void *ud;
    char path[WL_FNT_MAX_PATH];
} FntCtx;

static WlResult walk_dir(FntCtx *c, unsigned dir, size_t plen, int depth) {
    if (depth > WL_FNT_MAX_DEPTH || c->visited[dir]) {
        return WL_ERR_FORMAT;
    }
    c->visited[dir] = 1;
    const uint8_t *e = c->fnt + (size_t)dir * 8u;
    size_t p = wl_rd_le32(e);
    uint32_t next_file = wl_rd_le16(e + 4);
    if (p > c->size) {
        return WL_ERR_RANGE;
    }
    while (p < c->size) { /* a table that ends without a terminator is accepted */
        unsigned b = c->fnt[p++];
        if (b == 0) {
            break;
        }
        size_t len = b & 0x7Fu;
        int is_dir = (b & 0x80u) != 0;
        if (len == 0 || !wl_range_ok(p, len + (is_dir ? 2u : 0u), c->size)) {
            return WL_ERR_FORMAT;
        }
        size_t np = plen + (plen ? 1u : 0u) + len;
        if (np >= WL_FNT_MAX_PATH) {
            return WL_ERR_RANGE;
        }
        if (plen) {
            c->path[plen] = '/';
        }
        memcpy(c->path + plen + (plen ? 1u : 0u), c->fnt + p, len);
        c->path[np] = '\0';
        p += len;
        WlResult r;
        if (is_dir) {
            unsigned id = wl_rd_le16(c->fnt + p);
            p += 2;
            if (id < 0xF000u || id - 0xF000u >= c->dir_count) {
                return WL_ERR_FORMAT;
            }
            r = walk_dir(c, id - 0xF000u, np, depth + 1);
        } else {
            if (next_file > 0xFFFFu) {
                return WL_ERR_RANGE;
            }
            r = c->fn(c->ud, c->path, next_file++);
        }
        if (r != WL_OK) {
            return r;
        }
    }
    c->path[plen] = '\0';
    return WL_OK;
}

WlResult wl_fnt_walk(const uint8_t *fnt, size_t fnt_size, WlFntFileFn fn, void *ud) {
    if (fnt == NULL || fn == NULL) {
        return WL_ERR_ARG;
    }
    if (fnt_size < 8) {
        return WL_ERR_FORMAT;
    }
    unsigned dirs = wl_rd_le16(fnt + 6); /* root entry: total number of directories */
    if (dirs == 0 || dirs > 0x1000u || !wl_range_ok(0, (size_t)dirs * 8u, fnt_size)) {
        return WL_ERR_FORMAT;
    }
    FntCtx *c = (FntCtx *)calloc(1, sizeof *c);
    uint8_t *vis = (uint8_t *)calloc(dirs, 1);
    if (c == NULL || vis == NULL) {
        free(c);
        free(vis);
        return WL_ERR_NOMEM;
    }
    c->fnt = fnt;
    c->size = fnt_size;
    c->dir_count = dirs;
    c->visited = vis;
    c->fn = fn;
    c->ud = ud;
    WlResult r = walk_dir(c, 0, 0, 0);
    free(vis);
    free(c);
    return r;
}
