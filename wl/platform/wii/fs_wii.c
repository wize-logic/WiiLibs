// SPDX-License-Identifier: MIT
// Wii file backend: the one owner of the SD and USB mounts, plus the IOS 58 reload
// for USB 2.0. libfat (dvm) mounts a block device under a name; "sd:/..." and "usb:/..." then
// work through the newlib file API, which the shared implementation (fs_posix.inc) uses. Only
// this directory may include <ogc/*.h>.
// A file's bytes on the SD card are read straight from the card (wl_platform_fs_fast_set): the SD
// is mounted as fatMountSimple mounts it, keeping its disc (libfat's cache over the card), whose
// FAT the file's map is made from (fat_map.h); the file's first cluster comes from libfat's open
// file (FatFs's FFFIL: its first 16 bytes, checked against a read through libfat).
#define _POSIX_C_SOURCE 200809L
#include <dvm.h>
#include <fat.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/iosupport.h>
#include <unistd.h>
#include <ogc/ios.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "wl/platform/fat_map.h"
#include "wl/platform/platform.h"
#include "wl/platform/timebase.h"

#define WL_USB_IOS 58

static bool g_mounted[2];
static bool g_ios_done;

// libfat's cache over the SD card as fatMountSimple makes it (libdvm's defaults: 4 pages of 64 sectors)
#define SD_CACHE_PAGES 4u
#define SD_PAGE_SECTORS 64u
// A transfer straight from the card: at most this many sectors (128 KB)
#define FAST_MAX_SECTORS 256u
// Reads below FAST_MIN bytes of a file of at most FAST_SMALL_FILE bytes stay libfat's (its cache's
// pages serve a small file read in small pieces; its FAT walks are short)
#define FAST_MIN 4096u
#define FAST_SMALL_FILE (1u << 20)

static DvmDisc *g_sd_disc;      // libfat's disc of the SD card (its cache), while mounted
static WlFatVolume g_sd_vol;    // the SD's FAT volume
static atomic_bool g_sd_fast;   // the volume was read and the reads straight from the card are good
static atomic_bool g_sd_failed; // a read straight from the card failed once (logged once)

WlResult wl_platform_fs_ios_init(uint32_t *out_ios_version) {
    WlResult r = WL_OK;
    if (!g_ios_done) {
        g_ios_done = true;
        s32 v = IOS_GetVersion();
        if (v != WL_USB_IOS) {
            WL_LOG_I("FS", "IOS %d running, reloading IOS %d for USB 2.0", (int)v, WL_USB_IOS);
            if (IOS_ReloadIOS(WL_USB_IOS) < 0) {
                WL_LOG_W("FS", "IOS %d reload failed, staying on IOS %d", WL_USB_IOS, (int)v);
                r = WL_ERR_FAILED;
            }
        }
    } else if (IOS_GetVersion() != WL_USB_IOS) {
        r = WL_ERR_FAILED;
    }
    s32 now = IOS_GetVersion();
    WL_LOG_I("FS", "IOS version %d rev %d", (int)now, (int)IOS_GetRevision());
    if (out_ios_version != NULL) {
        *out_ios_version = (uint32_t)now;
    }
    return r;
}

static const char *device_name(WlDevice device) {
    return device == WL_DEVICE_SD ? "sd" : "usb";
}

// Sectors of the SD card: through libfat's cache (what libfat itself reads: coherent with what it
// has not written back yet), or straight from the card into `dst` (aligned to 32 bytes).
static bool sd_cached_read(void *user, uint32_t sector, uint32_t count, void *dst) {
    (void)user;
    return dvmDiscReadSectors(g_sd_disc, dst, sector, count);
}

static bool sd_card_read(void *user, uint32_t sector, uint32_t count, void *dst) {
    (void)user;
    return __io_wiisd.readSectors(sector, count, dst);
}

// The SD mounted as fatMountSimple mounts it (fatMount: a disc, its cache, the volume), its disc
// kept; its FAT volume read for the reads straight from the card.
static WlResult sd_mount(void) {
    DvmDisc *disc = dvmDiscCreate(&__io_wiisd);
    if (disc != NULL) {
        disc = dvmDiscCacheCreate(disc, SD_CACHE_PAGES, SD_PAGE_SECTORS);
    }
    if (disc == NULL || !dvmMountVolume("sd", disc, 0, "vfat")) {
        if (disc != NULL) {
            disc->vt->destroy(disc);
        }
        WL_LOG_I("FS", "sd: not present");
        return WL_ERR_NOT_FOUND;
    }
    g_sd_disc = disc;
    g_mounted[WL_DEVICE_SD] = true;
    uint8_t *scratch = memalign(WL_FAT_ALIGN, WL_FAT_SECTOR_MAX);
    WlResult r = scratch != NULL ? wl_fat_volume_probe(sd_cached_read, NULL, scratch, &g_sd_vol) : WL_ERR_NOMEM;
    free(scratch);
    if (r == WL_OK && g_sd_vol.sector_bytes != 512u) {
        r = WL_ERR_UNSUPPORTED; // (the card's sectors are 512 bytes)
    }
    atomic_store(&g_sd_fast, r == WL_OK);
    atomic_store(&g_sd_failed, false);
    if (r == WL_OK) {
        WL_LOG_I("FS", "sd: mounted, FAT%u, %u KB clusters, data at sector %u (%s libfat's %u KB cache pages)",
                 (unsigned)g_sd_vol.bits, (unsigned)(g_sd_vol.cluster_sectors / 2u), (unsigned)g_sd_vol.data,
                 g_sd_vol.data % SD_PAGE_SECTORS == 0u ? "aligned to" : "not aligned to",
                 (unsigned)(SD_PAGE_SECTORS / 2u));
    } else {
        WL_LOG_W("FS", "sd: mounted; its volume not read (%s): files read through libfat alone", wl_result_name(r));
    }
    return WL_OK;
}

WlResult wl_platform_fs_mount(WlDevice device) {
    if ((unsigned)device > 1u) {
        return WL_ERR_ARG;
    }
    if (g_mounted[device]) {
        return WL_OK;
    }
    if (device == WL_DEVICE_SD) {
        return sd_mount();
    }
    if (!fatMountSimple(device_name(device), &__io_usbstorage)) {
        WL_LOG_I("FS", "%s: not present", device_name(device));
        return WL_ERR_NOT_FOUND;
    }
    g_mounted[device] = true;
    WL_LOG_I("FS", "%s: mounted", device_name(device));
    return WL_OK;
}

bool wl_platform_fs_mounted(WlDevice device) {
    return (unsigned)device <= 1u && g_mounted[device];
}

void wl_platform_fs_unmount(WlDevice device) {
    if ((unsigned)device <= 1u && g_mounted[device]) {
        if (device == WL_DEVICE_SD) {
            atomic_store(&g_sd_fast, false);
        }
        fatUnmount(device_name(device));
        g_mounted[device] = false;
        if (device == WL_DEVICE_SD) {
            g_sd_disc = NULL; // (the volume let it go)
        }
    }
}

uint64_t wl_platform_time_us(void) {
    return wl_platform_fs_clock_us();
}

uint64_t wl_platform_time_ms(void) {
    return wl_timebase_ms(wl_timebase_read());
}

// The console's clock (the RTC with the settings' bias: local time), read once through the C
// library's time() and then advanced by the time base.
uint64_t wl_platform_wall_time_us(void) {
    static uint64_t anchor_wall, anchor_mono;
    static bool anchored;
    if (!anchored) {
        time_t now = time(NULL);
        anchor_wall = now > 0 ? (uint64_t)now * 1000000ull : 0;
        anchor_mono = wl_platform_fs_clock_us();
        anchored = true;
    }
    return anchor_wall + (wl_platform_fs_clock_us() - anchor_mono);
}

uint64_t wl_platform_fs_clock_us(void) {
    return wl_timebase_us(wl_timebase_read());
}

static WlResult fs_resolve(const char *path, char *out, size_t cap) {
    if (path == NULL) {
        return WL_ERR_ARG;
    }
    WlDevice dev;
    if (strncmp(path, "sd:/", 4) == 0) {
        dev = WL_DEVICE_SD;
    } else if (strncmp(path, "usb:/", 5) == 0) {
        dev = WL_DEVICE_USB;
    } else {
        return WL_ERR_ARG;
    }
    size_t len = strlen(path);
    if (len >= WL_FS_PATH_MAX || len >= cap) {
        return WL_ERR_ARG;
    }
    if (!g_mounted[dev]) {
        return WL_ERR_STATE;
    }
    memcpy(out, path, len + 1u);
    return WL_OK;
}

static WlResult fs_fast_read(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size);
static void fs_fast_forget(WlPlatformFile *file);

#include "wl/platform/fs_posix.inc"

// A handle's map and its sector's bounce.
typedef struct {
    uint8_t bounce[512] __attribute__((aligned(WL_FAT_ALIGN)));
    WlFatMap map;
    bool checked; // its bytes compared once with libfat's
    bool off;     // libfat's reads for this handle
} FastFile;

static void fs_fast_forget(WlPlatformFile *file) {
    FastFile *ff = file->fast;
    if (ff != NULL) {
        wl_fat_map_free(&ff->map);
        free(ff);
        file->fast = NULL;
    }
}

// libfat's open file (FatFs's FFFIL): its first cluster and size, at bytes 8 and 12 of the object
// that begins it (FFOBJID: FATFS *fs, WORD id, BYTE attr, BYTE stat, DWORD sclust, FSIZE_t objsize,
// with FatFs's 32-bit sizes).
static bool fs_fffil(const WlPlatformFile *file, uint32_t *first, uint32_t *size) {
    __handle *h = __get_handle(file->fd);
    const uint8_t *fp = h != NULL ? h->fileStruct : NULL;
    if (fp == NULL) {
        return false;
    }
    void *fs;
    memcpy(&fs, fp, sizeof fs);
    memcpy(first, fp + 8, 4);
    memcpy(size, fp + 12, 4);
    return fs != NULL && (*size == 0u || (*first >= 2u && *first < g_sd_vol.entries));
}

// The handle's map made (again): its writes flushed first (FatFs keeps a FAT sector it changed in
// its window), then the bytes at its last cluster compared with libfat's once. False: libfat reads.
static bool fs_fast_map(WlPlatformFile *file, FastFile *ff, uint64_t end) {
    if (file->writer && file->dirty) {
        if (fsync(file->fd) != 0) {
            return false;
        }
        file->partial = false;
    }
    uint32_t first, size;
    if (!fs_fffil(file, &first, &size) || (!file->writer && size != file->size)) {
        ff->off = true;
        WL_LOG_W("FS", "%s: libfat's open file not as expected: read through libfat", file->path);
        return false;
    }
    if (size < end) {
        return false; // (another handle's writes: mapped again at the next read)
    }
    uint8_t *scratch = memalign(WL_FAT_ALIGN, WL_FAT_SCRATCH);
    WlResult r = scratch != NULL ? wl_fat_map_make(&g_sd_vol, sd_cached_read, NULL, first, size, scratch, &ff->map)
                                 : WL_ERR_NOMEM;
    free(scratch);
    if (r != WL_OK) {
        ff->off = r != WL_ERR_NOMEM;
        WL_LOG_W("FS", "%s: its clusters not mapped (%s): read through libfat", file->path, wl_result_name(r));
        return false;
    }
    if (!ff->checked && size > 0u) {
        uint64_t cluster_bytes = (uint64_t)g_sd_vol.cluster_sectors * 512u;
        uint64_t at = (uint64_t)(ff->map.clusters - 1u) * cluster_bytes;
        uint32_t n = size - at < 512u ? (uint32_t)(size - at) : 512u;
        uint8_t *a = memalign(WL_FAT_ALIGN, 1024);
        WlFatReader rd = {.whole = sd_card_read, .part = sd_cached_read, .bounce = ff->bounce, .max_sectors = 1};
        bool same = a != NULL && fs_read_c(file, at, a, n) == WL_OK &&
                    wl_fat_read(&g_sd_vol, &ff->map, &rd, at, a + 512, n) == WL_OK && memcmp(a, a + 512, n) == 0;
        free(a);
        if (!same) {
            atomic_store(&g_sd_fast, false);
            WL_LOG_E("FS", "%s: the card's bytes at %u differ from libfat's: every file read through libfat",
                     file->path, (unsigned)at);
            return false;
        }
        ff->checked = true;
    }
    return true;
}

static WlResult fs_fast_read(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size) {
    if (!atomic_load(&g_sd_fast) || strncmp(file->path, "sd:/", 4) != 0 ||
        (size < FAST_MIN && file->size <= FAST_SMALL_FILE)) {
        return WL_ERR_UNSUPPORTED;
    }
    FastFile *ff = file->fast;
    if (ff == NULL) {
        ff = memalign(WL_FAT_ALIGN, sizeof *ff);
        if (ff == NULL) {
            return WL_ERR_UNSUPPORTED;
        }
        memset(ff, 0, sizeof *ff);
        file->fast = ff;
    }
    if (ff->off) {
        return WL_ERR_UNSUPPORTED;
    }
    if (atomic_exchange(&file->stale, false) && !fs_fast_map(file, ff, offset + size)) {
        if (!ff->off) {
            atomic_store(&file->stale, true);
        }
        return WL_ERR_UNSUPPORTED;
    }
    if (file->writer && file->partial) {
        // a part sector it wrote is still in FatFs's window
        if (fsync(file->fd) != 0) {
            return WL_ERR_UNSUPPORTED;
        }
        file->partial = false;
    }
    // (a handle that writes reads through libfat's cache, which may hold what it wrote unwritten)
    WlFatReader rd = {.whole = file->writer ? sd_cached_read : sd_card_read,
                      .part = file->writer ? sd_cached_read : sd_card_read,
                      .bounce = ff->bounce,
                      .max_sectors = FAST_MAX_SECTORS};
    WlResult r = wl_fat_read(&g_sd_vol, &ff->map, &rd, offset, dst, size);
    fs_count_transfers(rd.transfers);
    if (r == WL_ERR_RANGE) {
        atomic_store(&file->stale, true); // (it grew past its map)
        return WL_ERR_UNSUPPORTED;
    }
    if (r != WL_OK) {
        if (!atomic_exchange(&g_sd_failed, true)) {
            WL_LOG_W("FS", "%s: a read straight from the card failed at %u (%u bytes): read through libfat",
                     file->path, (unsigned)offset, (unsigned)size);
        }
        return WL_ERR_UNSUPPORTED;
    }
    return WL_OK;
}
