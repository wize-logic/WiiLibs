// SPDX-License-Identifier: MIT
// Wii file backend: the one owner of the SD and USB mounts, plus the IOS 58 reload
// for USB 2.0. libfat (dvm) mounts a block device under a name; "sd:/..." and "usb:/..." then
// work through the newlib file API, which the shared implementation (fs_posix.inc) uses. Only
// this directory may include <ogc/*.h>.
#include <fat.h>
#include <ogc/ios.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "wl/platform/platform.h"

#define WL_USB_IOS 58

static bool g_mounted[2];
static bool g_ios_done;

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

WlResult wl_platform_fs_mount(WlDevice device) {
    if ((unsigned)device > 1u) {
        return WL_ERR_ARG;
    }
    if (g_mounted[device]) {
        return WL_OK;
    }
    const DISC_INTERFACE *iface = device == WL_DEVICE_SD ? &__io_wiisd : &__io_usbstorage;
    if (!fatMountSimple(device_name(device), iface)) {
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
        fatUnmount(device_name(device));
        g_mounted[device] = false;
    }
}

uint64_t wl_platform_time_us(void) {
    return wl_platform_fs_clock_us();
}

uint64_t wl_platform_fs_clock_us(void) {
    return (uint64_t)ticks_to_microsecs(gettime());
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

#include "wl/platform/fs_posix.inc"
