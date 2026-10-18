// SPDX-License-Identifier: MIT
// A FAT volume's geometry and a file's runs (wl/platform/fat_map.h), over synthetic volumes made sector by
// sector as they are read: a FAT32 one behind an MBR, a FAT16 one in sector 0, a fragmented file whose bytes
// are a function of their offset; reads at every alignment of offset, length and buffer.
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/base/endian.h"
#include "wl/platform/fat_map.h"

typedef struct {
    bool mbr;          // a partition table in sector 0 (the volume at `base`)
    uint32_t base;     // the volume's first sector
    uint32_t spc;      // sectors per cluster
    uint32_t rsvd;     // reserved sectors
    uint32_t fat_size; // sectors per FAT (two FATs)
    uint32_t root_entries;
    uint32_t clusters;  // the volume's
    uint32_t *next;     // the FAT: next[c] (0x0FFFFFFF: the end)
    uint32_t *file_of;  // the file's cluster index + 1 of a volume cluster (0: not the file's)
    uint32_t reads;     // calls
    uint32_t sectors;   // sectors read
    bool misaligned;    // a read into a buffer not aligned to WL_FAT_ALIGN
    uint32_t fail_from; // reads of sectors at or above this fail (0: none)
} Vol;

static uint8_t pattern(uint64_t offset) {
    return (uint8_t)((offset * 2654435761u) >> 13);
}

static uint32_t vol_fat(const Vol *v) {
    return v->base + v->rsvd;
}

static uint32_t vol_root(const Vol *v) {
    return (v->root_entries * 32u + 511u) / 512u;
}

static uint32_t vol_data(const Vol *v) {
    return vol_fat(v) + 2u * v->fat_size + vol_root(v);
}

static bool fat32(const Vol *v) {
    return v->clusters > 0xFFF5u;
}

static void put_vbr(const Vol *v, uint8_t *s) {
    s[0] = 0xEB;
    s[11] = 0x00, s[12] = 0x02; // 512 bytes a sector
    s[13] = (uint8_t)v->spc;
    s[14] = (uint8_t)v->rsvd, s[15] = (uint8_t)(v->rsvd >> 8);
    s[16] = 2;
    s[17] = (uint8_t)v->root_entries, s[18] = (uint8_t)(v->root_entries >> 8);
    uint32_t total = vol_data(v) - v->base + v->clusters * v->spc;
    s[32] = (uint8_t)total, s[33] = (uint8_t)(total >> 8), s[34] = (uint8_t)(total >> 16), s[35] = (uint8_t)(total >> 24);
    if (fat32(v)) {
        s[36] = (uint8_t)v->fat_size, s[37] = (uint8_t)(v->fat_size >> 8), s[38] = (uint8_t)(v->fat_size >> 16);
        s[44] = 2; // root cluster
    } else {
        s[22] = (uint8_t)v->fat_size, s[23] = (uint8_t)(v->fat_size >> 8);
    }
    s[510] = 0x55, s[511] = 0xAA;
}

static void vol_sector(Vol *v, uint32_t sector, uint8_t *s) {
    memset(s, 0, 512);
    if (sector == 0u && v->mbr) {
        s[446 + 16 + 8] = (uint8_t)v->base, s[446 + 16 + 9] = (uint8_t)(v->base >> 8); // the second entry
        s[510] = 0x55, s[511] = 0xAA;
        return;
    }
    if (sector == v->base) {
        put_vbr(v, s);
        return;
    }
    uint32_t fat = vol_fat(v), data = vol_data(v);
    if (sector >= fat && sector < fat + 2u * v->fat_size) {
        uint32_t per = fat32(v) ? 128u : 256u, first = ((sector - fat) % v->fat_size) * per;
        for (uint32_t i = 0; i < per; i++) {
            uint32_t c = first + i, e = c < v->clusters + 2u ? v->next[c] : 0u;
            if (fat32(v)) {
                s[i * 4] = (uint8_t)e, s[i * 4 + 1] = (uint8_t)(e >> 8), s[i * 4 + 2] = (uint8_t)(e >> 16),
                       s[i * 4 + 3] = (uint8_t)(e >> 24);
            } else {
                s[i * 2] = (uint8_t)e, s[i * 2 + 1] = (uint8_t)(e >> 8);
            }
        }
        return;
    }
    if (sector >= data) {
        uint32_t c = (sector - data) / v->spc + 2u;
        if (c < v->clusters + 2u && v->file_of[c] != 0u) {
            uint64_t at = ((uint64_t)(v->file_of[c] - 1u) * v->spc + (sector - data) % v->spc) * 512u;
            for (uint32_t i = 0; i < 512; i++) {
                s[i] = pattern(at + i);
            }
        }
    }
}

static bool vol_read(void *user, uint32_t sector, uint32_t count, void *dst) {
    Vol *v = user;
    v->reads++;
    v->sectors += count;
    if (((uintptr_t)dst & (WL_FAT_ALIGN - 1u)) != 0u) {
        v->misaligned = true;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (v->fail_from != 0u && sector + i >= v->fail_from) {
            return false;
        }
        vol_sector(v, sector + i, (uint8_t *)dst + i * 512u);
    }
    return true;
}

// A volume of `clusters` clusters with a file in `runs` runs: (first cluster, count) pairs.
static void vol_make(Vol *v, bool mbr, uint32_t base, uint32_t spc, uint32_t clusters, const uint32_t *runs,
                     uint32_t run_count) {
    memset(v, 0, sizeof *v);
    v->mbr = mbr;
    v->base = base;
    v->spc = spc;
    v->rsvd = 32;
    v->clusters = clusters;
    v->root_entries = clusters > 0xFFF5u ? 0u : 512u;
    uint32_t per = clusters > 0xFFF5u ? 128u : 256u;
    v->fat_size = (clusters + 2u + per - 1u) / per;
    v->next = calloc(clusters + 2u, sizeof *v->next);
    v->file_of = calloc(clusters + 2u, sizeof *v->file_of);
    uint32_t prev = 0, index = 0, end = clusters > 0xFFF5u ? 0x0FFFFFFFu : 0xFFFFu;
    for (uint32_t r = 0; r < run_count; r++) {
        for (uint32_t k = 0; k < runs[2 * r + 1]; k++) {
            uint32_t c = runs[2 * r] + k;
            if (prev != 0u) {
                v->next[prev] = c;
            }
            v->file_of[c] = ++index;
            prev = c;
        }
    }
    v->next[prev] = end;
}

static void vol_free(Vol *v) {
    free(v->next);
    free(v->file_of);
}

static _Alignas(32) uint8_t g_scratch[WL_FAT_SCRATCH];
static _Alignas(32) uint8_t g_bounce[512];

// 70000 clusters of 4 KB (FAT32), a file in four runs out of the volume's order
static const uint32_t k_runs32[] = {100, 10, 5000, 3, 4000, 20, 69000, 7};

TEST a_fat32_volume_is_found_behind_its_partition_table(void) {
    Vol v;
    vol_make(&v, true, 2048, 8, 70000, k_runs32, 4);
    WlFatVolume fv;
    ASSERT_EQ(WL_OK, wl_fat_volume_probe(vol_read, &v, g_scratch, &fv));
    ASSERT_EQ(2048u, fv.base);
    ASSERT_EQ(2048u + 32u, fv.fat);
    ASSERT_EQ(vol_data(&v), fv.data);
    ASSERT_EQ(70002u, fv.entries);
    ASSERT_EQ(8u, fv.cluster_sectors);
    ASSERT_EQ(512u, fv.sector_bytes);
    ASSERT_EQ(32, fv.bits);
    vol_free(&v);
    PASS();
}

TEST a_fragmented_file_is_mapped_in_its_runs(void) {
    Vol v;
    vol_make(&v, true, 2048, 8, 70000, k_runs32, 4);
    WlFatVolume fv;
    ASSERT_EQ(WL_OK, wl_fat_volume_probe(vol_read, &v, g_scratch, &fv));
    WlFatMap m = {0};
    uint64_t size = 39u * 4096u + 100u; // the 40th cluster in part
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 100, size, g_scratch, &m));
    ASSERT_EQ(4u, m.count);
    ASSERT_EQ(40u, m.clusters);
    ASSERT_EQ(100u, m.runs[0].disk);
    ASSERT_EQ(10u, m.runs[0].count);
    ASSERT_EQ(13u, m.runs[2].cluster);
    ASSERT_EQ(4000u, m.runs[2].disk);
    uint32_t sector;
    uint64_t run;
    ASSERT(wl_fat_map_find(&fv, &m, 13u * 4096u + 1000u, &sector, &run));
    ASSERT_EQ(fv.data + (4000u - 2u) * 8u + 1u, sector);
    ASSERT_EQ(20u * 4096u - 1000u, run);
    ASSERT_FALSE(wl_fat_map_find(&fv, &m, 40u * 4096u, &sector, &run));
    wl_fat_map_free(&m);
    vol_free(&v);
    PASS();
}

TEST reads_at_every_alignment_give_the_files_bytes(void) {
    Vol v;
    vol_make(&v, true, 2048, 8, 70000, k_runs32, 4);
    WlFatVolume fv;
    ASSERT_EQ(WL_OK, wl_fat_volume_probe(vol_read, &v, g_scratch, &fv));
    WlFatMap m = {0};
    uint64_t size = 40u * 4096u;
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 100, size, g_scratch, &m));
    uint8_t *buf = aligned_alloc(32, 64u * 1024u + 64u);
    uint32_t seed = 12345;
    for (int i = 0; i < 400; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t shift = (seed >> 8) % 32u;
        uint64_t off = (seed >> 3) % size;
        seed = seed * 1103515245u + 12345u;
        uint32_t len = (seed >> 9) % (60u * 1024u) + 1u;
        if (i % 4 == 0) {
            off &= ~511ull; // whole sectors too
            len = (len + 511u) & ~511u;
        }
        if (off + len > size) {
            len = (uint32_t)(size - off);
        }
        memset(buf, 0xEE, 64u * 1024u + 64u);
        WlFatReader rd = {.whole = vol_read, .whole_user = &v, .part = vol_read, .part_user = &v,
                          .bounce = g_bounce, .max_sectors = 1u + (uint32_t)i % 9u};
        v.misaligned = false;
        ASSERT_EQ(WL_OK, wl_fat_read(&fv, &m, &rd, off, buf + shift, len));
        ASSERT_FALSE(v.misaligned);
        for (uint32_t k = 0; k < len; k++) {
            if (buf[shift + k] != pattern(off + k)) {
                FAILm("a byte differs");
            }
        }
        ASSERT_EQ(0xEE, buf[shift + len]); // nothing past the end
        if (shift > 0u) {
            ASSERT_EQ(0xEE, buf[shift - 1u]); // nor before
        }
    }
    free(buf);
    wl_fat_map_free(&m);
    vol_free(&v);
    PASS();
}

TEST a_whole_run_is_one_transfer(void) {
    Vol v;
    vol_make(&v, true, 2048, 8, 70000, k_runs32, 4);
    WlFatVolume fv;
    ASSERT_EQ(WL_OK, wl_fat_volume_probe(vol_read, &v, g_scratch, &fv));
    WlFatMap m = {0};
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 100, 40u * 4096u, g_scratch, &m));
    _Alignas(32) static uint8_t buf[20u * 4096u];
    WlFatReader rd = {.whole = vol_read, .whole_user = &v, .part = vol_read, .part_user = &v, .bounce = g_bounce,
                      .max_sectors = 256};
    v.reads = 0;
    ASSERT_EQ(WL_OK, wl_fat_read(&fv, &m, &rd, 13u * 4096u, buf, 20u * 4096u)); // the third run
    ASSERT_EQ(1u, v.reads);
    ASSERT_EQ(1u, rd.transfers);
    // across the first two runs, with a part sector at each end: four
    v.reads = 0;
    rd.transfers = 0;
    ASSERT_EQ(WL_OK, wl_fat_read(&fv, &m, &rd, 9u * 4096u + 100u, buf, 4096u + 2000u));
    ASSERT_EQ(4u, v.reads);
    wl_fat_map_free(&m);
    vol_free(&v);
    PASS();
}

TEST a_file_that_grew_is_mapped_further(void) {
    Vol v;
    vol_make(&v, true, 2048, 8, 70000, k_runs32, 4);
    WlFatVolume fv;
    ASSERT_EQ(WL_OK, wl_fat_volume_probe(vol_read, &v, g_scratch, &fv));
    WlFatMap m = {0};
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 100, 11u * 4096u, g_scratch, &m));
    ASSERT_EQ(2u, m.count);
    ASSERT_EQ(11u, m.clusters);
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 100, 40u * 4096u, g_scratch, &m));
    ASSERT_EQ(4u, m.count);
    ASSERT_EQ(40u, m.clusters);
    ASSERT_EQ(3u, m.runs[1].count);
    // cut: made again
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 100, 5u * 4096u, g_scratch, &m));
    ASSERT_EQ(1u, m.count);
    ASSERT_EQ(5u, m.clusters);
    // another first cluster: made again
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 4000, 4096u, g_scratch, &m));
    ASSERT_EQ(1u, m.count);
    ASSERT_EQ(4000u, m.runs[0].disk);
    wl_fat_map_free(&m);
    vol_free(&v);
    PASS();
}

TEST a_broken_chain_or_device_is_refused(void) {
    Vol v;
    vol_make(&v, true, 2048, 8, 70000, k_runs32, 4);
    WlFatVolume fv;
    ASSERT_EQ(WL_OK, wl_fat_volume_probe(vol_read, &v, g_scratch, &fv));
    WlFatMap m = {0};
    // longer than its chain
    ASSERT_EQ(WL_ERR_FORMAT, wl_fat_map_make(&fv, vol_read, &v, 100, 41u * 4096u, g_scratch, &m));
    ASSERT_EQ(0u, m.clusters);
    // a free cluster inside it
    v.next[4005] = 0;
    ASSERT_EQ(WL_ERR_FORMAT, wl_fat_map_make(&fv, vol_read, &v, 100, 40u * 4096u, g_scratch, &m));
    // a number off the volume
    v.next[4005] = 90000;
    ASSERT_EQ(WL_ERR_FORMAT, wl_fat_map_make(&fv, vol_read, &v, 100, 40u * 4096u, g_scratch, &m));
    v.next[4005] = 4006;
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 100, 40u * 4096u, g_scratch, &m));
    // past the map, and a device error
    _Alignas(32) static uint8_t buf[8192];
    WlFatReader rd = {.whole = vol_read, .whole_user = &v, .part = vol_read, .part_user = &v, .bounce = g_bounce,
                      .max_sectors = 16};
    ASSERT_EQ(WL_ERR_RANGE, wl_fat_read(&fv, &m, &rd, 40u * 4096u - 10u, buf, 20));
    v.fail_from = fv.data;
    ASSERT_EQ(WL_ERR_IO, wl_fat_read(&fv, &m, &rd, 0, buf, 8192));
    v.fail_from = 0;
    // no FAT volume: a blank card
    Vol blank;
    vol_make(&blank, false, 0, 8, 70000, k_runs32, 4);
    blank.base = 5; // (its boot record is not where sector 0 says)
    ASSERT_EQ(WL_ERR_FORMAT, wl_fat_volume_probe(vol_read, &blank, g_scratch, &fv));
    vol_free(&blank);
    wl_fat_map_free(&m);
    vol_free(&v);
    PASS();
}

TEST a_fat16_volume_in_sector_0_is_read(void) {
    static const uint32_t runs[] = {3000, 4, 10, 2, 3004, 5};
    Vol v;
    vol_make(&v, false, 0, 2, 20000, runs, 3);
    WlFatVolume fv;
    ASSERT_EQ(WL_OK, wl_fat_volume_probe(vol_read, &v, g_scratch, &fv));
    ASSERT_EQ(16, fv.bits);
    ASSERT_EQ(0u, fv.base);
    ASSERT_EQ(vol_data(&v), fv.data);
    WlFatMap m = {0};
    ASSERT_EQ(WL_OK, wl_fat_map_make(&fv, vol_read, &v, 3000, 11u * 1024u - 3u, g_scratch, &m));
    ASSERT_EQ(3u, m.count); // (3004 follows 11 in the chain, not 3003)
    _Alignas(32) static uint8_t buf[12 * 1024];
    WlFatReader rd = {.whole = vol_read, .whole_user = &v, .part = vol_read, .part_user = &v, .bounce = g_bounce,
                      .max_sectors = 64};
    ASSERT_EQ(WL_OK, wl_fat_read(&fv, &m, &rd, 7, buf + 3, 11u * 1024u - 10u));
    for (uint32_t k = 0; k < 11u * 1024u - 10u; k++) {
        if (buf[3 + k] != pattern(7 + k)) {
            FAILm("a byte differs");
        }
    }
    wl_fat_map_free(&m);
    vol_free(&v);
    PASS();
}

SUITE(fat_map) {
    RUN_TEST(a_fat32_volume_is_found_behind_its_partition_table);
    RUN_TEST(a_fragmented_file_is_mapped_in_its_runs);
    RUN_TEST(reads_at_every_alignment_give_the_files_bytes);
    RUN_TEST(a_whole_run_is_one_transfer);
    RUN_TEST(a_file_that_grew_is_mapped_further);
    RUN_TEST(a_broken_chain_or_device_is_refused);
    RUN_TEST(a_fat16_volume_in_sector_0_is_read);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(fat_map);
    GREATEST_MAIN_END();
}
