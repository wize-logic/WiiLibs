// SPDX-License-Identifier: MIT
// A FAT16/FAT32 volume's geometry and a file's clusters as runs of contiguous sectors, for reading a
// file straight from the device: a read of a whole run is one transfer into the caller's buffer.
//
// The Wii's libfat (FatFs under libdvm) reads at most one cluster per transfer, goes through its
// 4-page cache whenever a transfer is not aligned to a 32 KB page, and finds the cluster of an
// offset by following the FAT from the file's first cluster (or from the current one when it moves
// forward): every jump back in a map file of hundreds of MB reads through thousands of FAT
// entries. A map of the file's runs, made once, answers an offset with a binary search.
//
// It reads the device through a function (sectors into a buffer aligned to WL_FAT_ALIGN bytes): the
// Wii's SD card on the console, a volume image in the host's tests. It knows nothing of files'
// names: the caller gives the first cluster and the size (libfat's open file has them).
// Not thread-safe: a map is read and remade by one thread at a time.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_FAT_ALIGN 32u
#define WL_FAT_SECTOR_MAX 4096u
// The FAT read per transfer while a map is made (the sectors of WL_FAT_SCRATCH bytes).
#define WL_FAT_SCRATCH 4096u
// A file in more runs than this is not mapped (WL_ERR_LIMIT): the caller reads it as before.
#define WL_FAT_MAX_RUNS 8192u

// Reads `count` sectors from `sector` (the device's numbering) into `dst` (aligned to WL_FAT_ALIGN).
typedef bool (*WlFatReadFn)(void *user, uint32_t sector, uint32_t count, void *dst);

typedef struct {
    uint32_t base;            // the volume's first sector
    uint32_t fat;             // the first FAT's first sector
    uint32_t data;            // cluster 2's first sector
    uint32_t entries;         // clusters + 2: a cluster number is valid in [2, entries)
    uint32_t cluster_sectors; // a power of two
    uint32_t sector_bytes;    // 512, 1024, 2048 or 4096
    uint8_t bits;             // 16 or 32
} WlFatVolume;

typedef struct {
    uint32_t cluster; // the run's first cluster in the file (0 = the file's first)
    uint32_t disk;    // its cluster number on the volume
    uint32_t count;   // contiguous clusters
} WlFatRun;

typedef struct {
    WlFatRun *runs;
    uint32_t count, cap;
    uint32_t clusters; // the file's clusters mapped (the runs' total)
    uint32_t first;    // the file's first cluster
} WlFatMap;

// The volume on a device: a FAT volume boot record in sector 0, or the first of the MBR's four
// partitions that holds one (as FatFs mounts it). `scratch`: WL_FAT_SECTOR_MAX bytes, aligned.
// WL_ERR_FORMAT for no FAT16/32 volume (FAT12 and exFAT are not read), WL_ERR_IO for a read error.
WlResult wl_fat_volume_probe(WlFatReadFn read, void *user, uint8_t *scratch, WlFatVolume *out);

// Maps the first ceil(size / cluster bytes) clusters of the chain from `first`. A map already made
// from the same first cluster is extended (a file that grew keeps its clusters); another is made
// again. WL_ERR_FORMAT for a chain that leaves the volume or ends early, WL_ERR_LIMIT for one in
// more than WL_FAT_MAX_RUNS runs, WL_ERR_NOMEM, WL_ERR_IO. `scratch`: WL_FAT_SCRATCH bytes, aligned.
WlResult wl_fat_map_make(const WlFatVolume *v, WlFatReadFn read, void *user, uint32_t first, uint64_t size,
                         uint8_t *scratch, WlFatMap *m);
void wl_fat_map_free(WlFatMap *m);

// The sector holding byte `offset` of the file and how many bytes from there are contiguous on the
// device (to the end of its run). False past the mapped clusters.
bool wl_fat_map_find(const WlFatVolume *v, const WlFatMap *m, uint64_t offset, uint32_t *sector, uint64_t *run_bytes);

typedef struct {
    WlFatReadFn whole; // whole sectors into the caller's buffer (the device itself)
    void *whole_user;
    WlFatReadFn part; // the sector of a read's first or last bytes (may be a cache)
    void *part_user;
    uint8_t *bounce;      // a sector, aligned
    uint32_t max_sectors; // per transfer of `whole`
    uint32_t transfers;   // counted up: calls of `whole` and `part`
} WlFatReader;

// Reads `len` bytes at `offset` of a mapped file into `dst`, which need not be aligned: whole
// sectors go straight into it in transfers of up to max_sectors (a run's), shifted down by a
// memmove when `dst` is not aligned there; a first or last part sector comes through `bounce`.
// WL_ERR_RANGE past the mapped clusters, WL_ERR_IO for a read error.
WlResult wl_fat_read(const WlFatVolume *v, const WlFatMap *m, WlFatReader *rd, uint64_t offset, void *dst, uint32_t len);
