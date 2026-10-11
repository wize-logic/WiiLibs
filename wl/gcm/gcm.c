// SPDX-License-Identifier: MIT
#include "wl/gcm/gcm.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

#define TAG "gcm"
#define GCM_MAX_FST ((uint32_t)8u << 20)
#define GCM_DOL_MAX ((size_t)16u << 20)
#define GCM_DOL_HDR 0x100u
#define GCM_NAME_MAX 255u

WlResult wl_gcm_read(const WlGcm *g, uint64_t off, void *buf, size_t n) {
    if (g == NULL || g->read == NULL) {
        return WL_ERR_ARG;
    }
    if (!wl_range_ok(off, n, g->size)) {
        return WL_ERR_RANGE;
    }
    return g->read(g->ctx, off, buf, n);
}

void wl_gcm_close(WlGcm *g) {
    if (g == NULL) {
        return;
    }
    if (g->close != NULL) {
        g->close(g->ctx);
    }
    free(g->fst);
    memset(g, 0, sizeof *g);
}

static WlResult open_inner(WlGcm *g) {
    uint8_t b[WL_GCM_BOOT_SIZE];
    if (g->size < WL_GCM_APPLOADER_OFF + 0x20u) {
        return WL_ERR_FORMAT;
    }
    WlResult r = wl_gcm_read(g, 0, b, sizeof b);
    if (r != WL_OK) {
        return r;
    }
    if (wl_rd_be32(b + 0x1C) != WL_GCM_MAGIC) {
        if (wl_rd_be32(b + 0x18) == WL_GCM_WII_MAGIC) {
            WL_LOG_E(TAG, "this is a Wii disc; open it through wl/wiidisc (dolphin-tool)");
        }
        return WL_ERR_FORMAT;
    }
    memcpy(g->game_code, b, 4);
    memcpy(g->maker, b + 4, 2);
    g->disc_number = b[6];
    g->version = b[7];
    memcpy(g->title, b + 0x20, 64);
    g->title[64] = '\0';
    g->dol_off = wl_rd_be32(b + 0x420);
    g->fst_off = wl_rd_be32(b + 0x424);
    g->fst_size = wl_rd_be32(b + 0x428);
    g->fst_max = wl_rd_be32(b + 0x42C);
    if (g->dol_off < WL_GCM_APPLOADER_OFF || g->dol_off >= g->size) {
        return WL_ERR_RANGE;
    }
    if (g->fst_size < 12u || g->fst_size > GCM_MAX_FST ||
        !wl_range_ok(g->fst_off, g->fst_size, g->size)) {
        return WL_ERR_RANGE;
    }
    /* Apploader header: date[16], entry, size, trailer size, then the body. */
    uint8_t ah[0x20];
    r = wl_gcm_read(g, WL_GCM_APPLOADER_OFF, ah, sizeof ah);
    if (r != WL_OK) {
        return r;
    }
    uint64_t asz = (uint64_t)0x20 + wl_rd_be32(ah + 0x14) + wl_rd_be32(ah + 0x18);
    g->apploader_size = wl_range_ok(WL_GCM_APPLOADER_OFF, asz, g->size) ? (uint32_t)asz : 0u;

    g->fst = (uint8_t *)malloc(g->fst_size);
    if (g->fst == NULL) {
        return WL_ERR_NOMEM;
    }
    r = wl_gcm_read(g, g->fst_off, g->fst, g->fst_size);
    if (r != WL_OK) {
        return r;
    }
    /* Root entry: must be a directory; its size field is the number of entries. */
    if (g->fst[0] != 1u) {
        return WL_ERR_FORMAT;
    }
    g->entry_count = wl_rd_be32(g->fst + 8);
    if (g->entry_count < 1u || (uint64_t)g->entry_count * 12u > g->fst_size) {
        return WL_ERR_RANGE;
    }
    return WL_OK;
}

WlResult wl_gcm_open_src(WlGcm *g, WlGcmReadFn read, WlGcmCloseFn close, void *ctx,
                         uint64_t size) {
    if (g == NULL) {
        return WL_ERR_ARG;
    }
    memset(g, 0, sizeof *g);
    if (read == NULL) {
        if (close != NULL) {
            close(ctx);
        }
        return WL_ERR_ARG;
    }
    g->read = read;
    g->close = close;
    g->ctx = ctx;
    g->size = size;
    WlResult r = open_inner(g);
    if (r != WL_OK) {
        wl_gcm_close(g);
    }
    return r;
}

static WlResult mem_read(void *ctx, uint64_t off, void *buf, size_t n) {
    memcpy(buf, (const uint8_t *)ctx + off, n);
    return WL_OK;
}

WlResult wl_gcm_open_mem(WlGcm *g, const uint8_t *data, size_t size) {
    if (g == NULL || (data == NULL && size != 0)) {
        return WL_ERR_ARG;
    }
    return wl_gcm_open_src(g, mem_read, NULL, (void *)(uintptr_t)data, size);
}

typedef struct {
    uint32_t dir_index; /* index of the directory that owns this frame */
    uint32_t end;       /* first entry index after the directory */
    size_t path_len;    /* length of the directory's path (no trailing slash) */
} Frame;

WlResult wl_gcm_walk(const WlGcm *g, WlGcmWalkFn fn, void *ud) {
    if (g == NULL || g->fst == NULL || fn == NULL) {
        return WL_ERR_ARG;
    }
    const uint8_t *fst = g->fst;
    const uint32_t count = g->entry_count;
    const uint8_t *names = fst + (size_t)count * 12u;
    const size_t names_len = g->fst_size - (size_t)count * 12u;
    Frame stack[WL_GCM_MAX_DEPTH + 1];
    unsigned depth = 0;
    char path[WL_GCM_PATH_MAX];
    stack[0].dir_index = 0;
    stack[0].end = count;
    stack[0].path_len = 0;
    for (uint32_t i = 1; i < count; i++) {
        while (depth > 0 && i >= stack[depth].end) {
            depth--;
        }
        const uint8_t *e = fst + (size_t)i * 12u;
        uint32_t name_off = wl_rd_be24(e + 1);
        uint32_t a = wl_rd_be32(e + 4);
        uint32_t b = wl_rd_be32(e + 8);
        if (e[0] > 1u || name_off >= names_len) {
            return WL_ERR_FORMAT;
        }
        const uint8_t *nm = names + name_off;
        size_t nl = 0;
        while (name_off + nl < names_len && nm[nl] != 0u) {
            nl++;
        }
        if (name_off + nl >= names_len || nl == 0 || nl > GCM_NAME_MAX ||
            memchr(nm, '/', nl) != NULL) {
            return WL_ERR_FORMAT;
        }
        size_t base = stack[depth].path_len;
        size_t sep = base > 0 ? 1u : 0u;
        if (base + sep + nl + 1u > sizeof path) {
            return WL_ERR_RANGE;
        }
        if (sep) {
            path[base] = '/';
        }
        memcpy(path + base + sep, nm, nl);
        path[base + sep + nl] = '\0';
        WlResult r;
        if (e[0] == 1u) {
            /* a = parent index, b = index after the last entry of this directory */
            if (a != stack[depth].dir_index || b <= i || b > stack[depth].end ||
                depth >= WL_GCM_MAX_DEPTH) {
                return WL_ERR_FORMAT;
            }
            r = fn(ud, path, 1, 0, 0);
            if (r != WL_OK) {
                return r;
            }
            depth++;
            stack[depth].dir_index = i;
            stack[depth].end = b;
            stack[depth].path_len = base + sep + nl;
        } else {
            if (!wl_range_ok(a, b, g->size)) {
                return WL_ERR_RANGE;
            }
            r = fn(ud, path, 0, a, b);
            if (r != WL_OK) {
                return r;
            }
        }
    }
    return WL_OK;
}

static WlResult count_cb(void *ud, const char *path, int is_dir, uint64_t off, uint64_t size) {
    WlGcmCounts *c = (WlGcmCounts *)ud;
    (void)path;
    (void)off;
    if (is_dir) {
        c->dirs++;
    } else {
        c->files++;
        c->bytes += size;
    }
    return WL_OK;
}

WlResult wl_gcm_count(const WlGcm *g, WlGcmCounts *c) {
    if (c == NULL) {
        return WL_ERR_ARG;
    }
    memset(c, 0, sizeof *c);
    return wl_gcm_walk(g, count_cb, c);
}

typedef struct {
    const char *want;
    uint64_t off, size;
    int found;
} FindCtx;

#define WL_GCM_STOP 1 /* internal: positive value, never escapes wl_gcm_find */

static WlResult find_cb(void *ud, const char *path, int is_dir, uint64_t off, uint64_t size) {
    FindCtx *f = (FindCtx *)ud;
    if (!is_dir && strcmp(path, f->want) == 0) {
        f->off = off;
        f->size = size;
        f->found = 1;
        return WL_GCM_STOP;
    }
    return WL_OK;
}

WlResult wl_gcm_find(const WlGcm *g, const char *path, uint64_t *off, uint64_t *size) {
    if (path == NULL || off == NULL || size == NULL) {
        return WL_ERR_ARG;
    }
    FindCtx f = {path, 0, 0, 0};
    WlResult r = wl_gcm_walk(g, find_cb, &f);
    if (r != WL_OK && r != WL_GCM_STOP) {
        return r;
    }
    if (!f.found) {
        return WL_ERR_NOT_FOUND;
    }
    *off = f.off;
    *size = f.size;
    return WL_OK;
}

WlResult wl_gcm_read_range(const WlGcm *g, uint64_t off, uint64_t size, size_t max, uint8_t **data,
                           size_t *len) {
    if (data == NULL || len == NULL) {
        return WL_ERR_ARG;
    }
    if (size > max) {
        return WL_ERR_RANGE;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_gcm_read(g, off, buf, (size_t)size);
    if (r != WL_OK) {
        free(buf);
        return r;
    }
    buf[size] = 0;
    *data = buf;
    *len = (size_t)size;
    return WL_OK;
}

WlResult wl_gcm_dol_size(const WlGcm *g, uint32_t *size) {
    uint8_t h[GCM_DOL_HDR];
    if (size == NULL) {
        return WL_ERR_ARG;
    }
    WlResult r = wl_gcm_read(g, g->dol_off, h, sizeof h);
    if (r != WL_OK) {
        return r;
    }
    /* 7 text + 11 data sections: file offsets at 0x00, sizes at 0x90. */
    uint64_t end = GCM_DOL_HDR;
    for (unsigned i = 0; i < 18; i++) {
        uint32_t off = wl_rd_be32(h + 4u * i);
        uint32_t sz = wl_rd_be32(h + 0x90 + 4u * i);
        if (sz == 0) {
            continue;
        }
        if (off < GCM_DOL_HDR || (uint64_t)off + sz > GCM_DOL_MAX) {
            return WL_ERR_FORMAT;
        }
        if ((uint64_t)off + sz > end) {
            end = (uint64_t)off + sz;
        }
    }
    if (!wl_range_ok(g->dol_off, end, g->size)) {
        return WL_ERR_RANGE;
    }
    *size = (uint32_t)end;
    return WL_OK;
}

WlResult wl_gcm_read_dol(const WlGcm *g, uint8_t **data, size_t *len) {
    uint32_t sz;
    WlResult r = wl_gcm_dol_size(g, &sz);
    if (r != WL_OK) {
        return r;
    }
    return wl_gcm_read_range(g, g->dol_off, sz, GCM_DOL_MAX, data, len);
}
