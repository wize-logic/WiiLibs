// SPDX-License-Identifier: MIT
// Asynchronous file reads and the read-speed measurement platform-independent, on
// top of the blocking file API in platform.h and the IO thread in io.h. The main thread submits
// reads and polls completions without ever waiting for the card; the data lands directly in the
// caller's buffer (which must stay valid until the completion is polled).
//
// Threading: wl_fs_start / _stop / _read_async / _measure_async / wl_fs_poll are main-thread
// only (they wrap the IO thread's single-producer queues). wl_fs_read_sync, wl_fs_measure_read,
// wl_fs_file_size and wl_fs_last_speed may be called from any thread.
// The IO thread owns the handler; other jobs (an archive reader, a decompressor) register through
// wl_fs_set_fallback_handler and come back from wl_fs_poll with their raw job.
#pragma once

#include "wl/platform/io.h"
#include "wl/platform/platform.h"

#define WL_FS_QUEUE_DEPTH WL_IO_QUEUE_DEPTH

// Starts the IO thread with the file handler (wl_mem_init first). WL_ERR_STATE if running.
WlResult wl_fs_start(void);
// Finishes the queued jobs and stops the thread. Unpolled completions are discarded.
void wl_fs_stop(void);
bool wl_fs_running(void);

// Jobs whose op is not a file op (>= WL_FS_OP_USER) go to `handler` on the IO thread. Set it
// before submitting such jobs; NULL removes it (such jobs then complete with WL_ERR_UNSUPPORTED).
#define WL_FS_OP_USER 0x100u
void wl_fs_set_fallback_handler(WlIoHandler handler, void *user);

// "sd:/..." or "usb:/..." -> device; false when the prefix is neither.
bool wl_fs_path_device(const char *path, WlDevice *out);

typedef enum { WL_FS_OP_READ = 1, WL_FS_OP_MEASURE = 2 } WlFsOp;

typedef struct {
    uint32_t id;         // returned by the submit call
    uint32_t op;         // WlFsOp, or the fallback handler's op
    int32_t result;      // WL_OK or a WL_ERR_* (WL_ERR_NOT_FOUND for a missing file)
    uint32_t bytes;      // bytes read (the whole `size` on success, 0 on error)
    uint32_t elapsed_us; // time spent on the device, open and close included for reads
    void *user;          // the pointer given at submit
    WlIoJob raw;         // the finished job (for fallback ops)
} WlFsDone;

// Reads exactly `size` bytes at `offset` of `path` into `dst`. WL_ERR_LIMIT when the queue is
// full (poll first); the path is copied, `dst` is not.
WlResult wl_fs_read_async(const char *path, uint64_t offset, void *dst, uint32_t size, void *user,
                          uint32_t *out_id);
// Sequentially reads up to `max_bytes` (0 = the whole file) of `path` in `scratch_size` chunks
// into `scratch`, timing only the reads; the result is recorded as the device's last speed.
WlResult wl_fs_measure_async(const char *path, void *scratch, uint32_t scratch_size,
                             uint32_t max_bytes, void *user, uint32_t *out_id);
// Next finished job in submission order; false when none is ready.
bool wl_fs_poll(WlFsDone *out);

// Blocking forms for boot code and tests (same semantics, on the calling thread).
WlResult wl_fs_read_sync(const char *path, uint64_t offset, void *dst, uint32_t size);
WlResult wl_fs_file_size(const char *path, uint64_t *out_size);

typedef struct {
    WlDevice device;
    uint32_t bytes;      // bytes read in the measurement
    uint32_t elapsed_us;
    uint32_t kib_per_s;  // bytes / 1024 per second; 0 = never measured
    uint32_t chunk;      // chunk size used
} WlFsSpeed;

WlResult wl_fs_measure_read(const char *path, void *scratch, uint32_t scratch_size,
                            uint32_t max_bytes, WlFsSpeed *out);
// The most recent measurement of `device` (what the debug overlay prints). False when none yet.
bool wl_fs_last_speed(WlDevice device, WlFsSpeed *out);

typedef struct {
    uint32_t reads;  // completed read jobs (async or sync)
    uint32_t errors; // ...that failed
    uint64_t bytes;  // total bytes delivered
} WlFsStats;
void wl_fs_stats(WlFsStats *out);
