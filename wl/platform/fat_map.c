// SPDX-License-Identifier: MIT
// A FAT volume's geometry and a file's runs of sectors (fat_map.h).
#include "wl/platform/fat_map.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

// FatFs's limits on a volume's clusters for each FAT type (ff.c: MAX_FAT12, MAX_FAT16, MAX_FAT32)
#define FAT12_MAX 0xFF5u
#define FAT16_MAX 0xFFF5u
#define FAT32_MAX 0x0FFFFFF5u

static bool pow2(uint32_t x) {
    return x != 0u && (x & (x - 1u)) == 0u;
}

// A volume boot record at `base`: its geometry, as FatFs reads it (check_fs, mount_volume).
static bool vbr_parse(const uint8_t *s, uint32_t base, WlFatVolume *out) {
    if (wl_rd_le16(s + 510) != 0xAA55u || (s[0] != 0xEBu && s[0] != 0xE9u && s[0] != 0xE8u)) {
        return false;
    }
    uint32_t bps = wl_rd_le16(s + 11), spc = s[13], rsvd = wl_rd_le16(s + 14), fats = s[16];
    uint32_t root_entries = wl_rd_le16(s + 17);
    uint32_t total = wl_rd_le16(s + 19) != 0u ? wl_rd_le16(s + 19) : wl_rd_le32(s + 32);
    uint32_t fat_size = wl_rd_le16(s + 22) != 0u ? wl_rd_le16(s + 22) : wl_rd_le32(s + 36);
    if (!pow2(bps) || bps < 512u || bps > WL_FAT_SECTOR_MAX || !pow2(spc) || spc > 128u || rsvd == 0u ||
        fats < 1u || fats > 2u || fat_size == 0u || (bps * 8u) % 32u != 0u) {
        return false;
    }
    uint64_t root = ((uint64_t)root_entries * 32u + bps - 1u) / bps;
    uint64_t system = rsvd + (uint64_t)fats * fat_size + root;
    if (total < system) {
        return false;
    }
    uint64_t clusters = (total - system) / spc;
    uint8_t bits;
    if (clusters <= FAT12_MAX || clusters > FAT32_MAX) {
        return false; // FAT12, or nothing FatFs mounts
    }
    bits = clusters <= FAT16_MAX ? 16u : 32u;
    if ((bits == 32u) != (root_entries == 0u)) {
        return false;
    }
    if ((uint64_t)fat_size * bps * 8u / bits < clusters + 2u || (uint64_t)base + total > 0xFFFFFFFFull) {
        return false; // a FAT too small for its clusters, or past 32-bit sector numbers
    }
    out->base = base;
    out->fat = base + rsvd;
    out->data = (uint32_t)(base + system);
    out->entries = (uint32_t)clusters + 2u;
    out->cluster_sectors = spc;
    out->sector_bytes = bps;
    out->bits = bits;
    return true;
}

WlResult wl_fat_volume_probe(WlFatReadFn read, void *user, uint8_t *scratch, WlFatVolume *out) {
    if (read == NULL || scratch == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    if (!read(user, 0, 1, scratch)) {
        return WL_ERR_IO;
    }
    if (vbr_parse(scratch, 0, out)) {
        return WL_OK;
    }
    if (wl_rd_le16(scratch + 510) != 0xAA55u) {
        return WL_ERR_FORMAT;
    }
    uint32_t starts[4];
    for (int i = 0; i < 4; i++) {
        starts[i] = wl_rd_le32(scratch + 446 + 16 * i + 8);
    }
    for (int i = 0; i < 4; i++) {
        if (starts[i] == 0u) {
            continue;
        }
        if (!read(user, starts[i], 1, scratch)) {
            return WL_ERR_IO;
        }
        if (vbr_parse(scratch, starts[i], out)) {
            return WL_OK;
        }
    }
    return WL_ERR_FORMAT;
}

// The FAT's sectors read WL_FAT_SCRATCH bytes at a time while a chain is followed.
typedef struct {
    const WlFatVolume *v;
    WlFatReadFn read;
    void *user;
    uint8_t *buf;
    uint32_t first, count; // the sectors in buf (count 0: none)
} FatWindow;

static bool fat_next(FatWindow *w, uint32_t cluster, uint32_t *next) {
    const WlFatVolume *v = w->v;
    uint64_t at = (uint64_t)cluster * (v->bits / 8u);
    uint32_t sector = v->fat + (uint32_t)(at / v->sector_bytes);
    if (w->count == 0u || sector < w->first || sector >= w->first + w->count) {
        uint32_t n = WL_FAT_SCRATCH / v->sector_bytes;
        if (!w->read(w->user, sector, n, w->buf)) {
            w->count = 0;
            return false;
        }
        w->first = sector;
        w->count = n;
    }
    const uint8_t *e = w->buf + (sector - w->first) * v->sector_bytes + at % v->sector_bytes;
    *next = v->bits == 16u ? wl_rd_le16(e) : wl_rd_le32(e) & 0x0FFFFFFFu;
    return true;
}

void wl_fat_map_free(WlFatMap *m) {
    if (m != NULL) {
        free(m->runs);
        memset(m, 0, sizeof *m);
    }
}

WlResult wl_fat_map_make(const WlFatVolume *v, WlFatReadFn read, void *user, uint32_t first, uint64_t size,
                         uint8_t *scratch, WlFatMap *m) {
    if (v == NULL || read == NULL || scratch == NULL || m == NULL) {
        return WL_ERR_ARG;
    }
    uint64_t cluster_bytes = (uint64_t)v->cluster_sectors * v->sector_bytes;
    uint64_t needed = (size + cluster_bytes - 1u) / cluster_bytes;
    if (needed > v->entries - 2u) {
        return WL_ERR_FORMAT;
    }
    if (m->first != first || m->clusters > needed) {
        m->count = 0;
        m->clusters = 0;
    }
    m->first = first;
    FatWindow w = {.v = v, .read = read, .user = user, .buf = scratch};
    uint32_t c = first;
    if (m->clusters != 0u && m->clusters < needed) {
        const WlFatRun *last = &m->runs[m->count - 1u];
        if (!fat_next(&w, last->disk + last->count - 1u, &c)) {
            return WL_ERR_IO;
        }
    }
    while (m->clusters < needed) {
        if (c < 2u || c >= v->entries) {
            m->count = 0; // (an end, a free or bad cluster, or a number off the volume, inside the file)
            m->clusters = 0;
            return WL_ERR_FORMAT;
        }
        WlFatRun *last = m->count != 0u ? &m->runs[m->count - 1u] : NULL;
        if (last != NULL && last->disk + last->count == c) {
            last->count++;
        } else {
            if (m->count == m->cap) {
                uint32_t cap = m->cap != 0u ? m->cap * 2u : 16u;
                if (m->cap >= WL_FAT_MAX_RUNS) {
                    m->count = 0;
                    m->clusters = 0;
                    return WL_ERR_LIMIT;
                }
                cap = cap > WL_FAT_MAX_RUNS ? WL_FAT_MAX_RUNS : cap;
                WlFatRun *runs = realloc(m->runs, cap * sizeof *runs);
                if (runs == NULL) {
                    m->count = 0;
                    m->clusters = 0;
                    return WL_ERR_NOMEM;
                }
                m->runs = runs;
                m->cap = cap;
            }
            m->runs[m->count++] = (WlFatRun){.cluster = m->clusters, .disk = c, .count = 1};
        }
        m->clusters++;
        if (m->clusters < needed && !fat_next(&w, c, &c)) {
            m->count = 0;
            m->clusters = 0;
            return WL_ERR_IO;
        }
    }
    return WL_OK;
}

bool wl_fat_map_find(const WlFatVolume *v, const WlFatMap *m, uint64_t offset, uint32_t *sector, uint64_t *run_bytes) {
    uint64_t cluster_bytes = (uint64_t)v->cluster_sectors * v->sector_bytes;
    uint64_t cl = offset / cluster_bytes;
    if (m->count == 0u || cl >= m->clusters) {
        return false;
    }
    uint32_t lo = 0, hi = m->count - 1u; // the last run that starts at or before cl
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1u) / 2u;
        if (m->runs[mid].cluster <= cl) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    const WlFatRun *r = &m->runs[lo];
    uint64_t in = offset % cluster_bytes;
    *sector = v->data + (r->disk + (uint32_t)(cl - r->cluster) - 2u) * v->cluster_sectors +
              (uint32_t)(in / v->sector_bytes);
    *run_bytes = ((uint64_t)r->cluster + r->count) * cluster_bytes - offset;
    return true;
}

WlResult wl_fat_read(const WlFatVolume *v, const WlFatMap *m, WlFatReader *rd, uint64_t offset, void *dst,
                     uint32_t len) {
    if (v == NULL || m == NULL || rd == NULL || rd->whole == NULL || rd->part == NULL || rd->bounce == NULL ||
        (dst == NULL && len > 0u)) {
        return WL_ERR_ARG;
    }
    const uint32_t bps = v->sector_bytes;
    const uint32_t most = rd->max_sectors != 0u ? rd->max_sectors : 1u;
    uint8_t *p = dst;
    while (len > 0u) {
        uint32_t sector, n;
        uint64_t run;
        if (!wl_fat_map_find(v, m, offset, &sector, &run)) {
            return WL_ERR_RANGE;
        }
        uint32_t in = (uint32_t)(offset % bps);
        if (in != 0u || len < bps) {
            // a part sector: through the bounce
            n = bps - in < len ? bps - in : len;
            rd->transfers++;
            if (!rd->part(rd->part_user, sector, 1, rd->bounce)) {
                return WL_ERR_IO;
            }
            memcpy(p, rd->bounce + in, n);
        } else {
            uint64_t secs = len / bps;
            secs = secs < run / bps ? secs : run / bps;
            secs = secs < most ? secs : most;
            uint32_t shift = (uint32_t)((WL_FAT_ALIGN - ((uintptr_t)p & (WL_FAT_ALIGN - 1u))) & (WL_FAT_ALIGN - 1u));
            if (shift != 0u) {
                // read aligned a little above, then moved down into place: room for it below len
                uint64_t room = (len - shift) / bps;
                secs = secs < room ? secs : room;
            }
            if (secs == 0u) {
                // one whole sector with no room to shift it: through the bounce
                rd->transfers++;
                if (!rd->whole(rd->whole_user, sector, 1, rd->bounce)) {
                    return WL_ERR_IO;
                }
                memcpy(p, rd->bounce, bps);
                n = bps;
            } else {
                n = (uint32_t)secs * bps;
                rd->transfers++;
                if (!rd->whole(rd->whole_user, sector, (uint32_t)secs, p + shift)) {
                    return WL_ERR_IO;
                }
                if (shift != 0u) {
                    memmove(p, p + shift, n);
                }
            }
        }
        p += n;
        offset += n;
        len -= n;
    }
    return WL_OK;
}
