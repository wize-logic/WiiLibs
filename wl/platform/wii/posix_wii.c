// SPDX-License-Identifier: MIT
// The C library's pread, pwrite and pause on the Wii: devkitPPC's newlib declares them
// (<unistd.h>) and does not define them, so a program that calls them fails to link. These are
// weak definitions over wl_posix_* (posix.h); a newlib that defines the calls itself wins. The
// object is part of wl_platform, which the linker takes members from only for names that are
// still undefined, so a program that does not call them gets none of it.
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>

#include "wl/platform/posix.h"

__attribute__((weak)) ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    return wl_posix_pread(fd, buf, count, (int64_t)offset);
}

__attribute__((weak)) ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return wl_posix_pwrite(fd, buf, count, (int64_t)offset);
}

__attribute__((weak)) int pause(void) {
    return wl_posix_pause();
}
