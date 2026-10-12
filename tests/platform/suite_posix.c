// SPDX-License-Identifier: MIT
// The C library's pread, pwrite on a file in a given folder, through <unistd.h> as a program calls
// them (and sysconf's memory answers, a pthread's priority and the entropy): on the Wii these are
// wii/posix_wii.c's weak definitions over wl_posix_*, on the host the C library's own. Platform-independent: test_platform_posix.c runs it against a temp file and the
// on-console runner against the SD card (libfat's devoptab, which seeks and reads like a file).
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tests/platform/check.h"
#include "wl/platform/platform.h"
#include "wl/platform/posix.h"

static uint8_t pat(uint32_t i) {
    return (uint8_t)((i * 2654435761u) >> 24);
}

void wlt_suite_posix(const char *path);

static atomic_bool g_worker_ran;

static void *worker(void *arg) {
    (void)arg;
    atomic_store(&g_worker_ran, true);
    return NULL;
}

// A pthread starts at the lowest priority on the Wii, so a thread that yields (as the game's
// waits do) never lets it run until the priority is raised to the yielding thread's.
static void priority_checks(void) {
    pthread_t thread;
    atomic_store(&g_worker_ran, false);
    if (pthread_create(&thread, NULL, worker, NULL) != 0) {
        wlt_check("posix_pthread_create", false, "pthread_create");
        return;
    }
    wlt_check("posix_set_priority", wl_posix_thread_set_relative_priority(thread, 0) == WL_OK, "main's priority");
    uint64_t start = wl_platform_time_us();
    while (!atomic_load(&g_worker_ran) && wl_platform_time_us() - start < 2000000u) {
        sched_yield();
    }
    wlt_check("posix_worker_runs_while_main_yields", atomic_load(&g_worker_ran), "the worker ran within 2 s");
    pthread_join(thread, NULL);
}

static void libc_answers_checks(void) {
    unsigned char a[16], b[16];
    wlt_check("posix_random", wl_platform_random_bytes(a, sizeof a) == WL_OK && wl_platform_random_bytes(b, sizeof b) == WL_OK &&
                                  memcmp(a, b, sizeof a) != 0,
              "two draws differ");
#ifdef __wii__
    wlt_check("posix_sysconf_page", sysconf(_SC_PAGESIZE) == 4096, "4096");
    wlt_check("posix_sysconf_phys", sysconf(_SC_PHYS_PAGES) == (88L << 20) / 4096, "88 MB in pages");
    long free_pages = sysconf(_SC_AVPHYS_PAGES);
    wlt_check("posix_sysconf_avail", free_pages > 0 && free_pages <= sysconf(_SC_PHYS_PAGES), "some memory is free");
#endif
}

void wlt_suite_posix(const char *path) {
    uint8_t data[1000], back[200];
    for (uint32_t i = 0; i < sizeof data; i++) {
        data[i] = pat(i);
    }
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    wlt_check("posix_open", fd >= 0, path);
    if (fd < 0) {
        return;
    }
    wlt_check("posix_write", write(fd, data, sizeof data) == (ssize_t)sizeof data, "write");
    wlt_check("posix_seek", lseek(fd, 17, SEEK_SET) == 17, "lseek to 17");

    memset(back, 0, sizeof back);
    wlt_check("posix_pread", pread(fd, back, sizeof back, 600) == (ssize_t)sizeof back, "pread 200 at 600");
    wlt_check("posix_pread_data", memcmp(back, data + 600, sizeof back) == 0, "bytes at 600");
    wlt_check("posix_pread_keeps_position", lseek(fd, 0, SEEK_CUR) == 17, "position stays 17");
    uint8_t one = 0;
    wlt_check("posix_read_continues", read(fd, &one, 1) == 1 && one == data[17], "read at 17");

    wlt_check("posix_pread_short", pread(fd, back, sizeof back, 900) == 100, "pread 200 at 900 gives 100");
    wlt_check("posix_pread_eof", pread(fd, back, sizeof back, 1000) == 0, "pread at the end gives 0");

    const uint8_t patch[4] = {0xde, 0xad, 0xbe, 0xef};
    wlt_check("posix_pwrite", pwrite(fd, patch, sizeof patch, 100) == (ssize_t)sizeof patch, "pwrite 4 at 100");
    wlt_check("posix_pwrite_keeps_position", lseek(fd, 0, SEEK_CUR) == 18, "position stays 18");
    uint8_t check[6];
    wlt_check("posix_pwrite_data",
              pread(fd, check, sizeof check, 99) == (ssize_t)sizeof check && check[0] == data[99] &&
                  memcmp(check + 1, patch, sizeof patch) == 0 && check[5] == data[104],
              "bytes around 100");
    close(fd);
    wlt_check("posix_unlink", unlink(path) == 0, "unlink");
    priority_checks();
    libc_answers_checks();
}
