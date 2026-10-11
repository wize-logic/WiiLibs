// SPDX-License-Identifier: MIT
// Async file reads over the IO thread and the read-speed measurement. See fs.h.
#include "wl/platform/fs.h"

#include <stdatomic.h>
#include <string.h>

#define MEASURE_MIN_CHUNK 512u // one SD sector

typedef struct {
    bool used;           // main thread only
    char path[WL_FS_PATH_MAX];
    uint64_t offset;
    uint32_t max_bytes;  // measure: byte limit (0 = whole file)
    void *user;
    uint32_t bytes;      // written by the IO thread before the completion is pushed
    uint32_t elapsed_us;
} FsSlot;

static struct {
    FsSlot slots[WL_FS_QUEUE_DEPTH];
    WlIoHandler fallback;
    void *fallback_user;
    // Last speed per device: a seqlock (odd generation = being written) so the main thread can
    // read what the IO thread measured without a mutex.
    atomic_uint gen[2];
    WlFsSpeed speed[2];
    atomic_uint reads;
    atomic_uint errors;
    atomic_uint bytes_lo; // 64-bit atomics do not exist on the 750: carry into bytes_hi by hand
    atomic_uint bytes_hi;
} g;

bool wl_fs_path_device(const char *path, WlDevice *out) {
    if (path == NULL) {
        return false;
    }
    if (strncmp(path, "sd:/", 4) == 0) {
        *out = WL_DEVICE_SD;
        return true;
    }
    if (strncmp(path, "usb:/", 5) == 0) {
        *out = WL_DEVICE_USB;
        return true;
    }
    return false;
}

static void count_read(WlResult r, uint32_t bytes) {
    atomic_fetch_add(&g.reads, 1u);
    if (r != WL_OK) {
        atomic_fetch_add(&g.errors, 1u);
    } else {
        unsigned old = atomic_fetch_add(&g.bytes_lo, bytes);
        if (old + bytes < old) {
            atomic_fetch_add(&g.bytes_hi, 1u);
        }
    }
}

void wl_fs_stats(WlFsStats *out) {
    out->reads = atomic_load(&g.reads);
    out->errors = atomic_load(&g.errors);
    uint32_t hi = atomic_load(&g.bytes_hi);
    out->bytes = ((uint64_t)hi << 32) | atomic_load(&g.bytes_lo);
}

WlResult wl_fs_file_size(const char *path, uint64_t *out_size) {
    WlPlatformFile *f;
    WlResult r = wl_platform_fs_open(path, WL_FILE_READ, &f);
    if (r != WL_OK) {
        return r;
    }
    *out_size = wl_platform_fs_size(f);
    wl_platform_fs_close(f);
    return WL_OK;
}

static WlResult read_whole(const char *path, uint64_t offset, void *dst, uint32_t size) {
    WlPlatformFile *f;
    WlResult r = wl_platform_fs_open(path, WL_FILE_READ, &f);
    if (r != WL_OK) {
        return r;
    }
    r = wl_platform_fs_read_at(f, offset, dst, size);
    wl_platform_fs_close(f);
    return r;
}

WlResult wl_fs_read_sync(const char *path, uint64_t offset, void *dst, uint32_t size) {
    WlResult r = read_whole(path, offset, dst, size);
    count_read(r, size);
    return r;
}

static void speed_store(WlDevice dev, const WlFsSpeed *s) {
    unsigned n = atomic_load_explicit(&g.gen[dev], memory_order_relaxed);
    atomic_store_explicit(&g.gen[dev], n + 1u, memory_order_release); // odd: writing
    g.speed[dev] = *s;
    atomic_store_explicit(&g.gen[dev], n + 2u, memory_order_release);
}

bool wl_fs_last_speed(WlDevice device, WlFsSpeed *out) {
    if ((unsigned)device > 1u || out == NULL) {
        return false;
    }
    for (;;) {
        unsigned a = atomic_load_explicit(&g.gen[device], memory_order_acquire);
        if (a == 0) {
            return false;
        }
        if ((a & 1u) != 0) {
            wl_platform_thread_yield();
            continue;
        }
        WlFsSpeed copy = g.speed[device];
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g.gen[device], memory_order_relaxed) == a) {
            *out = copy;
            return true;
        }
    }
}

WlResult wl_fs_measure_read(const char *path, void *scratch, uint32_t scratch_size,
                            uint32_t max_bytes, WlFsSpeed *out) {
    WlDevice dev;
    if (scratch == NULL || scratch_size < MEASURE_MIN_CHUNK || !wl_fs_path_device(path, &dev)) {
        return WL_ERR_ARG;
    }
    WlPlatformFile *f;
    WlResult r = wl_platform_fs_open(path, WL_FILE_READ, &f);
    if (r != WL_OK) {
        return r;
    }
    uint32_t chunk = scratch_size & ~(MEASURE_MIN_CHUNK - 1u);
    uint64_t total = wl_platform_fs_size(f);
    if (max_bytes != 0 && total > max_bytes) {
        total = max_bytes;
    }
    if (total > 0xFFFFFFFFull) {
        total = 0xFFFFFFFFull;
    }
    uint64_t t0 = wl_platform_fs_clock_us();
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = (uint64_t)chunk < total - done ? chunk : (uint32_t)(total - done);
        r = wl_platform_fs_read_at(f, done, scratch, n);
        if (r != WL_OK) {
            break;
        }
        done += n;
    }
    uint64_t dt = wl_platform_fs_clock_us() - t0;
    wl_platform_fs_close(f);
    count_read(r, done);
    if (r != WL_OK) {
        return r;
    }
    if (dt == 0) {
        dt = 1;
    }
    if (dt > 0xFFFFFFFFull) {
        dt = 0xFFFFFFFFull;
    }
    WlFsSpeed s;
    s.device = dev;
    s.bytes = done;
    s.elapsed_us = (uint32_t)dt;
    s.chunk = chunk;
    // done / 1024 / (dt / 1e6), in 64-bit integers so a 4 GiB read cannot overflow.
    s.kib_per_s = (uint32_t)(((uint64_t)done * 1000000ull) / (dt * 1024ull));
    speed_store(dev, &s);
    WL_LOG_I("FS", "read speed %s: %u KiB/s (%u bytes in %u ms, %u byte chunks)",
             dev == WL_DEVICE_SD ? "sd" : "usb", (unsigned)s.kib_per_s, (unsigned)s.bytes,
             (unsigned)(s.elapsed_us / 1000u), (unsigned)s.chunk);
    if (out != NULL) {
        *out = s;
    }
    return WL_OK;
}

// Runs on the IO thread.
static void fs_handler(void *user, WlIoJob *job) {
    (void)user;
    FsSlot *slot = job->ptr;
    if (job->op == WL_FS_OP_READ) {
        uint64_t t0 = wl_platform_fs_clock_us();
        job->result = read_whole(slot->path, slot->offset, job->ptr2, job->size);
        slot->elapsed_us = (uint32_t)(wl_platform_fs_clock_us() - t0);
        slot->bytes = job->result == WL_OK ? job->size : 0;
        count_read(job->result, slot->bytes);
    } else if (job->op == WL_FS_OP_MEASURE) {
        WlFsSpeed s;
        job->result = wl_fs_measure_read(slot->path, job->ptr2, job->size, slot->max_bytes, &s);
        slot->bytes = job->result == WL_OK ? s.bytes : 0;
        slot->elapsed_us = job->result == WL_OK ? s.elapsed_us : 0;
    } else if (job->op >= WL_FS_OP_USER && g.fallback != NULL) {
        g.fallback(g.fallback_user, job);
    } else {
        job->result = WL_ERR_UNSUPPORTED;
    }
}

WlResult wl_fs_start(void) {
    memset(g.slots, 0, sizeof g.slots);
    return wl_io_start(fs_handler, NULL);
}

void wl_fs_stop(void) {
    wl_io_stop();
}

bool wl_fs_running(void) {
    return wl_io_running();
}

void wl_fs_set_fallback_handler(WlIoHandler handler, void *user) {
    g.fallback_user = user;
    g.fallback = handler;
}

static FsSlot *slot_alloc(const char *path) {
    if (strlen(path) >= WL_FS_PATH_MAX) {
        return NULL;
    }
    for (uint32_t i = 0; i < WL_FS_QUEUE_DEPTH; i++) {
        if (!g.slots[i].used) {
            g.slots[i].used = true;
            strcpy(g.slots[i].path, path);
            g.slots[i].bytes = 0;
            g.slots[i].elapsed_us = 0;
            return &g.slots[i];
        }
    }
    return NULL;
}

static WlResult submit(uint32_t op, const char *path, uint64_t offset, uint32_t max_bytes,
                       void *buf, uint32_t size, void *user, uint32_t *out_id) {
    if (path == NULL || buf == NULL || size == 0) {
        return WL_ERR_ARG;
    }
    if (!wl_io_running()) {
        return WL_ERR_STATE;
    }
    WlDevice dev;
    if (!wl_fs_path_device(path, &dev) || strlen(path) >= WL_FS_PATH_MAX) {
        return WL_ERR_ARG;
    }
    FsSlot *slot = slot_alloc(path);
    if (slot == NULL) {
        return WL_ERR_LIMIT;
    }
    slot->offset = offset;
    slot->max_bytes = max_bytes;
    slot->user = user;
    WlIoJob job;
    memset(&job, 0, sizeof job);
    job.op = op;
    job.ptr = slot;
    job.ptr2 = buf;
    job.size = size;
    WlResult r = wl_io_submit(&job);
    if (r != WL_OK) {
        slot->used = false;
        return r;
    }
    if (out_id != NULL) {
        *out_id = job.id;
    }
    return WL_OK;
}

WlResult wl_fs_read_async(const char *path, uint64_t offset, void *dst, uint32_t size, void *user,
                          uint32_t *out_id) {
    return submit(WL_FS_OP_READ, path, offset, 0, dst, size, user, out_id);
}

WlResult wl_fs_measure_async(const char *path, void *scratch, uint32_t scratch_size,
                             uint32_t max_bytes, void *user, uint32_t *out_id) {
    return submit(WL_FS_OP_MEASURE, path, 0, max_bytes, scratch, scratch_size, user, out_id);
}

bool wl_fs_poll(WlFsDone *out) {
    WlIoJob job;
    if (!wl_io_poll(&job)) {
        return false;
    }
    out->id = job.id;
    out->op = job.op;
    out->result = job.result;
    out->raw = job;
    FsSlot *slot = job.ptr;
    if (job.op < WL_FS_OP_USER && slot != NULL) {
        out->bytes = slot->bytes;
        out->elapsed_us = slot->elapsed_us;
        out->user = slot->user;
        slot->used = false;
    } else {
        out->bytes = 0;
        out->elapsed_us = 0;
        out->user = NULL;
    }
    return true;
}
