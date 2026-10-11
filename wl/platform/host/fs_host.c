// SPDX-License-Identifier: MIT
// Host backend of the file API: device prefixes map to directories (fs_host.h); the file
// operations are the shared POSIX implementation (wl/platform/fs_posix.inc).
#define _POSIX_C_SOURCE 200809L
#include "wl/platform/host/fs_host.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static char g_root[2][WL_FS_PATH_MAX];
static bool g_have_root[2];
static bool g_mounted[2];

void wl_host_fs_set_root(WlDevice device, const char *dir) {
    if ((unsigned)device > 1u) {
        return;
    }
    g_mounted[device] = false;
    g_have_root[device] = dir != NULL && strlen(dir) < sizeof g_root[0];
    if (g_have_root[device]) {
        strcpy(g_root[device], dir);
    }
}

WlResult wl_platform_fs_ios_init(uint32_t *out_ios_version) {
    if (out_ios_version != NULL) {
        *out_ios_version = 58;
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
    struct stat st;
    if (!g_have_root[device] || stat(g_root[device], &st) != 0 || !S_ISDIR(st.st_mode)) {
        return WL_ERR_NOT_FOUND;
    }
    g_mounted[device] = true;
    return WL_OK;
}

bool wl_platform_fs_mounted(WlDevice device) {
    return (unsigned)device <= 1u && g_mounted[device];
}

void wl_platform_fs_unmount(WlDevice device) {
    if ((unsigned)device <= 1u) {
        g_mounted[device] = false;
    }
}

uint64_t wl_platform_time_us(void) {
    return wl_platform_fs_clock_us();
}

uint64_t wl_platform_fs_clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
}

static WlResult fs_resolve(const char *path, char *out, size_t cap) {
    if (path == NULL) {
        return WL_ERR_ARG;
    }
    WlDevice dev;
    const char *rest;
    if (strncmp(path, "sd:/", 4) == 0) {
        dev = WL_DEVICE_SD;
        rest = path + 4;
    } else if (strncmp(path, "usb:/", 5) == 0) {
        dev = WL_DEVICE_USB;
        rest = path + 5;
    } else {
        return WL_ERR_ARG;
    }
    if (strlen(path) >= WL_FS_PATH_MAX || strstr(rest, "..") != NULL) {
        return WL_ERR_ARG;
    }
    if (!g_mounted[dev]) {
        return WL_ERR_STATE;
    }
    int n = snprintf(out, cap, "%s/%s", g_root[dev], rest);
    return (n > 0 && (size_t)n < cap) ? WL_OK : WL_ERR_ARG;
}

#include "wl/platform/fs_posix.inc"
