// SPDX-License-Identifier: MIT
// Host stub of the file part of platform.h: `sd:/` and `usb:/` map to directories
// chosen by the test, so the async layer and the path rules are testable without a console.
#pragma once

#include "wl/platform/platform.h"

// Points a device at a directory (it must exist for wl_platform_fs_mount to succeed); NULL
// forgets it and unmounts. The device stays unmounted until wl_platform_fs_mount.
void wl_host_fs_set_root(WlDevice device, const char *dir);
