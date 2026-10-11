// SPDX-License-Identifier: MIT
// The file suite: SD (and USB when present) mount, path and error rules,
// create/write/read-back/rename/remove, async reads through the IO thread, queue limits, and the
// read-speed measurement. Platform-independent: test_platform_fs.c runs it against the host stub
// (sd:/ mapped to a temp directory); an on-console runner can run the same file. USB absent
// (Dolphin) is reported, never a failure. Speed lines (`INFO fs_speed ...`) are what to read on
// hardware; an extra file at <dev>:/wltest/speed.bin (any size, put there by hand) is measured too.
#include <stdio.h>
#include <string.h>

#include "wl/platform/fs.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "tests/platform/check.h"

#define DIR_NAME "wltest"
#define BIG_BYTES (4u * 1024u * 1024u)
#define WRITE_CHUNK (32u * 1024u)
#define SCRATCH (64u * 1024u)
#define VERIFY_CHUNK (128u * 1024u)

static const char *dev_prefix(WlDevice d) {
    return d == WL_DEVICE_SD ? "sd:/" : "usb:/";
}

static uint8_t pat(uint32_t i) {
    return (uint8_t)((i * 2654435761u) >> 24);
}

static void fill(uint8_t *p, uint32_t first, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        p[i] = pat(first + i);
    }
}

static bool matches(const uint8_t *p, uint32_t first, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != pat(first + i)) {
            return false;
        }
    }
    return true;
}

static void sleep_poll(void) {
    wl_platform_thread_sleep_ms(1);
}

// Polls until `want` completions arrived (1 minute cap); fills `out` in completion order.
static uint32_t collect(WlFsDone *out, uint32_t want) {
    uint32_t got = 0;
    for (uint32_t spins = 0; got < want && spins < 60000u; spins++) {
        if (wl_fs_poll(&out[got])) {
            got++;
        } else {
            sleep_poll();
        }
    }
    return got;
}

static void check_paths(void) {
    WlPlatformFile *f = NULL;
    wlt_check("fs_open_no_prefix", wl_platform_fs_open("nodevice.bin", WL_FILE_READ, &f) == WL_ERR_ARG,
              "path without sd:/ or usb:/ refused");
    wlt_check("fs_open_null", wl_platform_fs_open(NULL, WL_FILE_READ, &f) == WL_ERR_ARG, "NULL path");
    char longp[WL_FS_PATH_MAX + 32];
    memset(longp, 'a', sizeof longp);
    memcpy(longp, "sd:/", 4);
    longp[sizeof longp - 1] = '\0';
    wlt_check("fs_open_too_long", wl_platform_fs_open(longp, WL_FILE_READ, &f) == WL_ERR_ARG,
              "over-long path refused");
    wlt_check("fs_open_missing",
              wl_platform_fs_open("sd:/" DIR_NAME "/does_not_exist.bin", WL_FILE_READ, &f) ==
                  WL_ERR_NOT_FOUND,
              "missing file is WL_ERR_NOT_FOUND");
    wlt_check("fs_open_missing_dir",
              wl_platform_fs_open("sd:/no_such_dir_wl/x.bin", WL_FILE_READ, &f) == WL_ERR_NOT_FOUND,
              "missing directory is WL_ERR_NOT_FOUND");
    wlt_check("fs_remove_missing",
              wl_platform_fs_remove("sd:/" DIR_NAME "/does_not_exist.bin") == WL_ERR_NOT_FOUND,
              "remove of a missing file");
    uint64_t sz;
    wlt_check("fs_size_missing",
              wl_fs_file_size("sd:/" DIR_NAME "/does_not_exist.bin", &sz) == WL_ERR_NOT_FOUND,
              "wl_fs_file_size of a missing file");
    WlDevice d;
    wlt_check("fs_path_device", wl_fs_path_device("usb:/a", &d) && d == WL_DEVICE_USB &&
                                    wl_fs_path_device("sd:/a", &d) && d == WL_DEVICE_SD &&
                                    !wl_fs_path_device("a:/a", &d) && !wl_fs_path_device(NULL, &d),
              "prefix parsing");
}

static void check_basic(void) {
    wlt_check("fs_mkdir", wl_platform_fs_mkdir("sd:/" DIR_NAME) == WL_OK, "create or reuse sd:/wltest");
    wlt_check("fs_mkdir_again", wl_platform_fs_mkdir("sd:/" DIR_NAME) == WL_OK,
              "existing directory is WL_OK");
    wlt_check("fs_mkdir_sub", wl_platform_fs_mkdir("sd:/" DIR_NAME "/fs") == WL_OK, "subdirectory");

    enum { N = 100003 }; // not a multiple of any sector size
    static uint8_t buf[N];
    fill(buf, 0, N);
    const char *p = "sd:/" DIR_NAME "/fs/a.tmp";
    WlPlatformFile *f = NULL;
    WlResult r = wl_platform_fs_open(p, WL_FILE_WRITE_NEW, &f);
    wlt_check("fs_create", r == WL_OK && f != NULL, "open for writing");
    if (f == NULL) {
        return;
    }
    bool wok = wl_platform_fs_write(f, buf, 1000) == WL_OK && wl_platform_fs_write(f, buf + 1000, N - 1000) == WL_OK;
    wlt_check("fs_write", wok, "two writes");
    wlt_check("fs_write_size", wl_platform_fs_size(f) == N, "size tracks writes");
    wlt_check("fs_flush", wl_platform_fs_flush(f) == WL_OK, "flush");
    wl_platform_fs_close(f);

    f = NULL;
    r = wl_platform_fs_open(p, WL_FILE_READ, &f);
    wlt_check("fs_reopen", r == WL_OK && f != NULL && wl_platform_fs_size(f) == N, "size after reopen");
    if (f == NULL) {
        return;
    }
    static uint8_t rd[N];
    memset(rd, 0xEE, sizeof rd);
    wlt_check("fs_read_all", wl_platform_fs_read_at(f, 0, rd, N) == WL_OK && memcmp(rd, buf, N) == 0,
              "whole file");
    memset(rd, 0xEE, 4096);
    wlt_check("fs_read_unaligned",
              wl_platform_fs_read_at(f, 12345, rd, 777) == WL_OK && matches(rd, 12345, 777),
              "unaligned offset and length");
    wlt_check("fs_read_tail", wl_platform_fs_read_at(f, N - 5, rd, 5) == WL_OK && matches(rd, N - 5, 5),
              "last bytes");
    wlt_check("fs_read_past_end", wl_platform_fs_read_at(f, N - 5, rd, 6) == WL_ERR_RANGE,
              "read crossing the end is WL_ERR_RANGE");
    wlt_check("fs_read_zero", wl_platform_fs_read_at(f, 0, rd, 0) == WL_OK, "zero-byte read");
    wlt_check("fs_read_back_forth",
              wl_platform_fs_read_at(f, 50000, rd, 100) == WL_OK && matches(rd, 50000, 100) &&
                  wl_platform_fs_read_at(f, 10, rd, 100) == WL_OK && matches(rd, 10, 100),
              "random access, both directions");
    wl_platform_fs_close(f);

    // Rename replaces an existing target.
    const char *q = "sd:/" DIR_NAME "/fs/b.tmp";
    WlPlatformFile *g = NULL;
    if (wl_platform_fs_open(q, WL_FILE_WRITE_NEW, &g) == WL_OK) {
        wl_platform_fs_write(g, "old", 3);
        wl_platform_fs_close(g);
    }
    wlt_check("fs_rename_replace", wl_platform_fs_rename(p, q) == WL_OK, "rename over an existing file");
    uint64_t sz = 0;
    wlt_check("fs_rename_result", wl_fs_file_size(q, &sz) == WL_OK && sz == N &&
                                      wl_fs_file_size(p, &sz) == WL_ERR_NOT_FOUND,
              "target has the new content, source is gone");
    wlt_check("fs_rename_missing", wl_platform_fs_rename(p, q) == WL_ERR_NOT_FOUND, "missing source");

    // READ_WRITE keeps the contents and allows read-after-write.
    f = NULL;
    r = wl_platform_fs_open(q, WL_FILE_READ_WRITE, &f);
    wlt_check("fs_read_write_open", r == WL_OK && f != NULL && wl_platform_fs_size(f) == N, "keeps contents");
    if (f != NULL) {
        wl_platform_fs_close(f);
    }
    wlt_check("fs_remove", wl_platform_fs_remove(q) == WL_OK && wl_fs_file_size(q, &sz) == WL_ERR_NOT_FOUND,
              "remove");

    // The handle pool: exhaustion is WL_ERR_LIMIT, and closing frees a slot.
    WlPlatformFile *h[WL_FS_MAX_FILES + 1];
    uint32_t opened = 0;
    WlResult last = WL_OK;
    for (uint32_t i = 0; i < WL_FS_MAX_FILES + 1u; i++) {
        last = wl_platform_fs_open("sd:/" DIR_NAME "/fs/pool.tmp", WL_FILE_READ_WRITE, &h[i]);
        if (last != WL_OK) {
            break;
        }
        opened++;
    }
    wlt_check("fs_pool_limit", opened == WL_FS_MAX_FILES && last == WL_ERR_LIMIT, "handle pool is fixed");
    for (uint32_t i = 0; i < opened; i++) {
        wl_platform_fs_close(h[i]);
    }
    wlt_check("fs_pool_reuse", wl_platform_fs_open("sd:/" DIR_NAME "/fs/pool.tmp", WL_FILE_READ, &h[0]) == WL_OK,
              "slot free again");
    wl_platform_fs_close(h[0]);
    wl_platform_fs_remove("sd:/" DIR_NAME "/fs/pool.tmp");
}

static void check_async(void) {
    enum { N = 70001 };
    static uint8_t src[N];
    fill(src, 7, N);
    const char *p = "sd:/" DIR_NAME "/fs/async.tmp";
    WlPlatformFile *f = NULL;
    bool made = wl_platform_fs_open(p, WL_FILE_WRITE_NEW, &f) == WL_OK && f != NULL &&
                wl_platform_fs_write(f, src, N) == WL_OK;
    if (f != NULL) {
        wl_platform_fs_close(f);
    }
    wlt_check("fs_async_fixture", made, "test file written");
    if (!made) {
        return;
    }

    wlt_check("fs_async_not_started", wl_fs_read_async(p, 0, src, 1, NULL, NULL) == WL_ERR_STATE,
              "needs the IO thread");
    wlt_check("fs_start", wl_fs_start() == WL_OK && wl_fs_running(), "IO thread started");

    static uint8_t a[4096];
    static uint8_t b[N];
    static uint8_t c[512];
    int tag_a = 1, tag_b = 2, tag_c = 3, tag_m = 4;
    uint32_t ida = 0, idb = 0, idc = 0, idm = 0;
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    WlResult ra = wl_fs_read_async(p, 1000, a, sizeof a, &tag_a, &ida);
    WlResult rb = wl_fs_read_async(p, 0, b, N, &tag_b, &idb);
    WlResult rc = wl_fs_read_async(p, 3, c, sizeof c, &tag_c, &idc); // fine
    WlResult rm = wl_fs_read_async("sd:/" DIR_NAME "/fs/missing.tmp", 0, c, 16, &tag_m, &idm);
    wlt_check("fs_async_submit", ra == WL_OK && rb == WL_OK && rc == WL_OK && rm == WL_OK &&
                                     ida != 0 && idb == ida + 1u && idc == idb + 1u,
              "four jobs queued with increasing ids");
    wlt_check("fs_async_bad_args",
              wl_fs_read_async(NULL, 0, a, 4, NULL, NULL) == WL_ERR_ARG &&
                  wl_fs_read_async("bad.bin", 0, a, 4, NULL, NULL) == WL_ERR_ARG &&
                  wl_fs_read_async(p, 0, NULL, 4, NULL, NULL) == WL_ERR_ARG,
              "NULL path, no device prefix, NULL buffer");

    WlFsDone d[4];
    uint32_t got = collect(d, 4);
    wlt_check("fs_async_complete", got == 4, "all four completions arrived");
    if (got == 4) {
        wlt_check("fs_async_order", d[0].id == ida && d[1].id == idb && d[2].id == idc && d[3].id == idm,
                  "completions in submission order");
        wlt_check("fs_async_user", d[0].user == &tag_a && d[1].user == &tag_b && d[3].user == &tag_m,
                  "user pointers come back");
        wlt_check("fs_async_data_a", d[0].result == WL_OK && d[0].bytes == sizeof a && matches(a, 7 + 1000, sizeof a),
                  "offset read");
        wlt_check("fs_async_data_b", d[1].result == WL_OK && d[1].bytes == N && memcmp(b, src, N) == 0,
                  "whole-file read");
        wlt_check("fs_async_data_c", d[2].result == WL_OK && matches(c, 7 + 3, sizeof c), "unaligned offset");
        wlt_check("fs_async_missing", d[3].result == WL_ERR_NOT_FOUND && d[3].bytes == 0,
                  "missing file errors cleanly");
    }
    WlFsDone none;
    wlt_check("fs_async_poll_empty", !wl_fs_poll(&none), "nothing left");

    // A read past the end fails whole.
    uint32_t id = 0;
    wl_fs_read_async(p, N - 10, c, 11, NULL, &id);
    wlt_check("fs_async_past_end", collect(d, 1) == 1 && d[0].result == WL_ERR_RANGE && d[0].bytes == 0,
              "range error");

    // Queue limit: WL_FS_QUEUE_DEPTH jobs fit, one more is WL_ERR_LIMIT, polling frees room.
    static uint8_t small[WL_FS_QUEUE_DEPTH][16];
    uint32_t queued = 0;
    for (uint32_t i = 0; i < WL_FS_QUEUE_DEPTH; i++) {
        if (wl_fs_read_async(p, i, small[i], 16, &small[i], NULL) == WL_OK) {
            queued++;
        }
    }
    WlResult over = wl_fs_read_async(p, 0, small[0], 16, NULL, NULL);
    wlt_check("fs_async_queue_limit", queued == WL_FS_QUEUE_DEPTH && over == WL_ERR_LIMIT,
              "bounded queue");
    static WlFsDone many[WL_FS_QUEUE_DEPTH];
    got = collect(many, WL_FS_QUEUE_DEPTH);
    bool all_ok = got == WL_FS_QUEUE_DEPTH;
    for (uint32_t i = 0; all_ok && i < got; i++) {
        uint8_t *dst = many[i].user;
        all_ok = many[i].result == WL_OK && matches(dst, 7 + i, 16);
    }
    wlt_check("fs_async_queue_drain", all_ok, "every queued read completed with the right data");
    wlt_check("fs_async_after_drain", wl_fs_read_async(p, 0, small[0], 16, NULL, NULL) == WL_OK &&
                                          collect(d, 1) == 1,
              "usable again after polling");

    // The IO thread must not stop the caller: submit does not wait for the device.
    WlFsStats st;
    wl_fs_stats(&st);
    wlt_check("fs_stats", st.reads >= 70 && st.errors >= 2 && st.bytes > 0, "counters moved");
    wl_fs_stop();
    wlt_check("fs_stop", !wl_fs_running(), "IO thread stopped");
    wl_platform_fs_remove(p);
}

// Writes BIG_BYTES, then measures the read speed three ways (sync, async, async with a different
// chunk size) and verifies the data.
static void check_speed(WlDevice dev) {
    const char *name = dev == WL_DEVICE_SD ? "sd" : "usb";
    char path[64];
    char label[48];
    snprintf(path, sizeof path, "%s" DIR_NAME "/speed_tmp.bin", dev_prefix(dev));
    snprintf(label, sizeof label, "fs_%s_", name);
    char nm[64];

    char dir[48];
    snprintf(dir, sizeof dir, "%s" DIR_NAME, dev_prefix(dev));
    wl_platform_fs_mkdir(dir);

    uint8_t *scratch = WL_MEM_ALLOC_GPU(WL_TAG_IO, SCRATCH);
    uint8_t *vbuf = WL_MEM_ALLOC_GPU(WL_TAG_IO, VERIFY_CHUNK);
    snprintf(nm, sizeof nm, "%smem", label);
    wlt_check(nm, scratch != NULL && vbuf != NULL, "IO buffers allocated");
    if (scratch == NULL || vbuf == NULL) {
        wl_mem_free(scratch);
        wl_mem_free(vbuf);
        return;
    }

    WlPlatformFile *f = NULL;
    uint64_t t0 = wl_platform_fs_clock_us();
    bool wrote = wl_platform_fs_open(path, WL_FILE_WRITE_NEW, &f) == WL_OK && f != NULL;
    for (uint32_t off = 0; wrote && off < BIG_BYTES; off += WRITE_CHUNK) {
        fill(scratch, off, WRITE_CHUNK);
        wrote = wl_platform_fs_write(f, scratch, WRITE_CHUNK) == WL_OK;
    }
    if (f != NULL) {
        wrote = wrote && wl_platform_fs_flush(f) == WL_OK;
        wl_platform_fs_close(f);
    }
    uint64_t wus = wl_platform_fs_clock_us() - t0;
    snprintf(nm, sizeof nm, "%swrite_big", label);
    wlt_check(nm, wrote, "4 MiB written");
    if (!wrote) {
        goto out;
    }
    wlt_report("INFO fs_speed %s write %u KiB/s (%u bytes in %u ms)", name,
               (unsigned)(((uint64_t)BIG_BYTES * 1000000ull) / ((wus ? wus : 1u) * 1024ull)),
               (unsigned)BIG_BYTES, (unsigned)(wus / 1000u));

    WlFsSpeed s;
    WlResult r = wl_fs_measure_read(path, scratch, SCRATCH, 0, &s);
    snprintf(nm, sizeof nm, "%smeasure_sync", label);
    wlt_check(nm, r == WL_OK && s.bytes == BIG_BYTES && s.kib_per_s > 0 && s.device == dev,
              "sync measurement covers the whole file");
    wlt_report("INFO fs_speed %s read_sync %u KiB/s (%u bytes in %u ms, %u byte chunks)", name,
               (unsigned)s.kib_per_s, (unsigned)s.bytes, (unsigned)(s.elapsed_us / 1000u),
               (unsigned)s.chunk);
    WlFsSpeed last;
    snprintf(nm, sizeof nm, "%slast_speed", label);
    wlt_check(nm, wl_fs_last_speed(dev, &last) && last.kib_per_s == s.kib_per_s && last.bytes == s.bytes,
              "last speed exposed for the overlay");

    // Through the IO thread: a measurement plus a verifying read run while this thread polls.
    snprintf(nm, sizeof nm, "%sstart", label);
    wlt_check(nm, wl_fs_start() == WL_OK, "IO thread started");
    uint32_t idm = 0;
    WlResult rm = wl_fs_measure_async(path, scratch, SCRATCH, 1024u * 1024u, NULL, &idm);
    WlFsDone d;
    uint32_t polls = 0;
    bool done = false;
    for (uint32_t spins = 0; !done && spins < 120000u; spins++) {
        done = wl_fs_poll(&d);
        if (!done) {
            polls++; // the main thread stays free while the card works
            sleep_poll();
        }
    }
    snprintf(nm, sizeof nm, "%smeasure_async", label);
    wlt_check(nm, rm == WL_OK && done && d.id == idm && d.result == WL_OK && d.bytes == 1024u * 1024u &&
                      d.op == WL_FS_OP_MEASURE,
              "async measurement of the first MiB");
    if (done) {
        WlFsSpeed a;
        if (wl_fs_last_speed(dev, &a)) {
            wlt_report("INFO fs_speed %s read_async %u KiB/s (%u bytes in %u ms, main polled %u times)",
                       name, (unsigned)a.kib_per_s, (unsigned)a.bytes, (unsigned)(a.elapsed_us / 1000u),
                       (unsigned)polls);
        }
    }

    bool verified = true;
    uint32_t nreads = 0;
    for (uint32_t off = 0; verified && off < BIG_BYTES; off += VERIFY_CHUNK) {
        uint32_t id2 = 0;
        verified = wl_fs_read_async(path, off, vbuf, VERIFY_CHUNK, NULL, &id2) == WL_OK &&
                   collect(&d, 1) == 1 && d.result == WL_OK && d.id == id2 &&
                   matches(vbuf, off, VERIFY_CHUNK);
        nreads++;
    }
    snprintf(nm, sizeof nm, "%sverify_big", label);
    wlt_check(nm, verified && nreads == BIG_BYTES / VERIFY_CHUNK, "4 MiB read back through the IO thread, byte-exact");
    wl_fs_stop();

    // The hand-placed file, when there is one.
    char hand[64];
    snprintf(hand, sizeof hand, "%s" DIR_NAME "/speed.bin", dev_prefix(dev));
    WlFsSpeed hs;
    if (wl_fs_measure_read(hand, scratch, SCRATCH, 16u * 1024u * 1024u, &hs) == WL_OK) {
        wlt_report("INFO fs_speed %s speed.bin %u KiB/s (%u bytes in %u ms)", name,
                   (unsigned)hs.kib_per_s, (unsigned)hs.bytes, (unsigned)(hs.elapsed_us / 1000u));
    }
out:
    wl_platform_fs_remove(path);
    wl_mem_free(scratch);
    wl_mem_free(vbuf);
}

void wlt_suite_fs(void);

void wlt_suite_fs(void) {
    if (!wl_mem_ready()) {
        wlt_check("mem_init", wl_mem_init(NULL) == WL_OK, "wl_mem_init");
    }
    uint32_t ios = 0;
    WlResult ir = wl_platform_fs_ios_init(&ios);
    wlt_report("INFO fs_ios version=%u usb2=%d", (unsigned)ios, ir == WL_OK);

    wlt_check("fs_sd_mount",
              wl_platform_fs_mounted(WL_DEVICE_SD) || wl_platform_fs_mount(WL_DEVICE_SD) == WL_OK,
              "SD mounted");
    if (!wl_platform_fs_mounted(WL_DEVICE_SD)) {
        return;
    }
    wlt_check("fs_mount_idempotent", wl_platform_fs_mount(WL_DEVICE_SD) == WL_OK && wl_platform_fs_mounted(WL_DEVICE_SD),
              "second mount is a no-op");

    WlResult ur = wl_platform_fs_mount(WL_DEVICE_USB);
    bool usb = ur == WL_OK && wl_platform_fs_mounted(WL_DEVICE_USB);
    wlt_report("INFO fs_usb %s", usb ? "mounted" : "not mounted");
    wlt_check("fs_usb_clean", usb || (ur == WL_ERR_NOT_FOUND && !wl_platform_fs_mounted(WL_DEVICE_USB)),
              "USB absent is reported as not mounted, not as an error");
    if (!usb) {
        WlPlatformFile *f = NULL;
        wlt_check("fs_usb_open_unmounted",
                  wl_platform_fs_open("usb:/" DIR_NAME "/x.bin", WL_FILE_READ, &f) == WL_ERR_STATE,
                  "usb:/ paths fail with WL_ERR_STATE while unmounted");
    }

    check_paths();
    wl_platform_fs_mkdir("sd:/" DIR_NAME);
    check_basic();
    check_async();
    check_speed(WL_DEVICE_SD);
    if (usb) {
        check_speed(WL_DEVICE_USB);
    }
}
