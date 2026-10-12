// SPDX-License-Identifier: MIT
// The C library's pread, pwrite, pause and sysconf on the Wii: devkitPPC's newlib declares them
// (<unistd.h>) and does not define them, so a program that calls them fails to link. These are
// weak definitions over wl_posix_* (posix.h); a newlib that defines the calls itself wins. The
// object is part of wl_platform, which the linker takes members from only for names that are
// still undefined, so a program that does not call them gets none of it.
#define _POSIX_C_SOURCE 200809L
#include <malloc.h>
#include <unistd.h>

#include "wl/platform/heap_overflow.h"
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

// sysconf's memory and processor answers (wl_posix_sysconf): the Wii's 88 MB, and what malloc can
// still give. With the overflow heap linked in (wl_platform_heap_overflow) that is newlib's heap
// in MEM1 and what is left of the overflow heap in MEM2. The reference is weak so that a program
// without the heap_wrap_wii.c wraps links too, and not the member that needs them: it then gets
// the free blocks newlib's heap holds (its mallinfo), which understates what malloc can still
// extend.
extern void wl_heap_overflow_usage(WlOverflowUsage *out) __attribute__((weak));

__attribute__((weak)) long sysconf(int name) {
    size_t free_bytes;

    if (wl_heap_overflow_usage != NULL) {
        WlOverflowUsage usage;

        wl_heap_overflow_usage(&usage);
        free_bytes = usage.free_total;
    } else {
        free_bytes = (size_t)mallinfo().fordblks;
    }
    return wl_posix_sysconf(name, free_bytes);
}
