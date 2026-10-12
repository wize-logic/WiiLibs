// SPDX-License-Identifier: MIT
// Wii log sinks: installed as the wl/base log sink. One formatted line goes to
//   1. the in-memory ring (crash screen, overlay),
//   2. the EXI UART via SYS_Report (Dolphin OSREPORT log; always on, harmless on hardware),
//   3. the on-screen console via printf (debug builds only; needs CON_Init by the caller),
//   4. <sd_dir>/log-NN.log when wl_platform_log_init was given a directory (buffered; flushed
//      at frame end, on shutdown, and when the buffer fills). Without an SD card the file sink
//      silently stays off.
// NN is a run counter: the first free number in 00..99 (all taken: 00 is overwritten).
// Only this directory may include <ogc/*.h>.
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wl/platform/platform.h"

#define RING_LINES 32u
#define RING_LINE_LEN 160u
#define FILE_BUF 4096u

static char g_ring[RING_LINES][RING_LINE_LEN];
static uint32_t g_ring_count; // total lines ever written
static char g_buf[FILE_BUF];
static uint32_t g_buf_len;
static FILE *g_file;
static bool g_file_tried; // open_sd_file has run once (a failed or closed file is not opened again)
static mutex_t g_mutex;
static bool g_inited;
static bool g_atexit;

static void file_flush_locked(void) {
    if (g_file != NULL && g_buf_len > 0) {
        if (fwrite(g_buf, 1, g_buf_len, g_file) != g_buf_len) {
            fclose(g_file); // card removed or full: stop logging to SD
            g_file = NULL;
        }
    }
    g_buf_len = 0;
    if (g_file != NULL) {
        fflush(g_file);
    }
}

void wl_platform_log_sink(void *user, WlLogLevel level, const char *tag, const char *line) {
    static const char letters[] = "EWID";
    char text[RING_LINE_LEN + 8];
    (void)user;
    int n = snprintf(text, sizeof text, "[%c][%s] %s", letters[(unsigned)level & 3u], tag, line);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n < sizeof text - 1 ? (size_t)n : sizeof text - 2;

    if (g_inited) {
        LWP_MutexLock(g_mutex);
    }
    char *slot = g_ring[g_ring_count % RING_LINES];
    size_t keep = len < RING_LINE_LEN - 1u ? len : RING_LINE_LEN - 1u;
    memcpy(slot, text, keep);
    slot[keep] = '\0';
    g_ring_count++;

    SYS_Report("%s\n", text); // EXI UART -> Dolphin OSREPORT
#ifndef NDEBUG
    printf("%s\n", text); // on-screen console
#endif
    if (g_file != NULL) {
        if (g_buf_len + len + 1 > FILE_BUF) {
            file_flush_locked();
        }
        if (len + 1 <= FILE_BUF) {
            memcpy(g_buf + g_buf_len, text, len);
            g_buf[g_buf_len + len] = '\n';
            g_buf_len += (uint32_t)len + 1u;
        }
    }
    if (g_inited) {
        LWP_MutexUnlock(g_mutex);
    }
}

uint32_t wl_platform_log_tail(const char **lines, uint32_t max) {
    uint32_t have = g_ring_count < RING_LINES ? g_ring_count : RING_LINES;
    uint32_t n = have < max ? have : max;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (g_ring_count - n + i) % RING_LINES;
        lines[i] = g_ring[idx];
    }
    return n;
}

// Creates every directory level of `dir` ("sd:/a/b/c"); existing levels are fine.
static void make_dirs(const char *dir) {
    char tmp[WL_FS_PATH_MAX];
    size_t len = strlen(dir);
    if (len >= sizeof tmp) {
        return;
    }
    memcpy(tmp, dir, len + 1u);
    const char *root = strstr(tmp, ":/");
    size_t start = root != NULL ? (size_t)(root - tmp) + 2u : 0u;
    for (size_t i = start; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char c = tmp[i];
            tmp[i] = '\0';
            if (i > start) {
                mkdir(tmp, 0777);
            }
            tmp[i] = c;
        }
    }
}

static void open_sd_file(const char *dir) {
    if (dir != NULL) {
        g_file_tried = true;
    }
    if (dir == NULL || strncmp(dir, "sd:/", 4) != 0 || strlen(dir) + 16u > WL_FS_PATH_MAX) {
        return;
    }
    if (wl_platform_fs_mount(WL_DEVICE_SD) != WL_OK) {
        return; // no SD card (fs_wii.c owns the mount)
    }
    make_dirs(dir);
    char path[WL_FS_PATH_MAX];
    unsigned pick = 0;
    for (unsigned i = 0; i < 100; i++) {
        snprintf(path, sizeof path, "%s/log-%02u.log", dir, i);
        struct stat st;
        if (stat(path, &st) != 0) {
            pick = i;
            break;
        }
    }
    snprintf(path, sizeof path, "%s/log-%02u.log", dir, pick);
    g_file = fopen(path, "wb");
}

void wl_platform_log_init(const char *sd_dir) {
    if (g_inited) {
        // a program that must reload IOS (for USB 2.0) mounts the card after its first call, which
        // had no directory to give: the file sink is opened by the call that has one
        if (sd_dir != NULL && g_file == NULL && !g_file_tried) {
            open_sd_file(sd_dir);
        }
        return;
    }
    LWP_MutexInit(&g_mutex, false);
    g_inited = true;
    open_sd_file(sd_dir);
    wl_log_set_sink(wl_platform_log_sink, NULL);
    if (!g_atexit) {
        g_atexit = true;
        atexit(wl_platform_log_shutdown);
    }
}

void wl_platform_log_flush(void) {
    if (!g_inited) {
        return;
    }
    LWP_MutexLock(g_mutex);
    file_flush_locked();
    LWP_MutexUnlock(g_mutex);
}

void wl_platform_log_shutdown(void) {
    if (!g_inited) {
        return;
    }
    LWP_MutexLock(g_mutex);
    file_flush_locked();
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
    LWP_MutexUnlock(g_mutex);
}
