// SPDX-License-Identifier: MIT
// pread, pwrite and pause over a file descriptor (posix.h).
#define _POSIX_C_SOURCE 200809L
#include "wl/platform/posix.h"

#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#if defined(__wii__)
#include "wl/platform/platform.h"
#endif

// newlib's <limits.h> has no SSIZE_MAX; ssize_t is as wide as size_t.
#define WL_SSIZE_MAX ((size_t)(SIZE_MAX >> 1))

// One lock for every descriptor: simple, and these calls are short next to the device's own
// latency. A static initialiser, so no setup is needed before the first call.
static pthread_mutex_t g_positioned_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t positioned(int fd, void *buf, size_t count, int64_t offset, bool writing) {
    if (offset < 0 || count > WL_SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    off_t target = (off_t)offset;
    if ((int64_t)target != offset) {
        errno = EOVERFLOW;
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    pthread_mutex_lock(&g_positioned_lock);
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved == (off_t)-1 || lseek(fd, target, SEEK_SET) == (off_t)-1) {
        int e = errno;
        pthread_mutex_unlock(&g_positioned_lock);
        errno = e;
        return -1;
    }
    ssize_t result = writing ? write(fd, buf, count) : read(fd, buf, count);
    int e = errno;
    // The position is put back whatever the transfer did; a failure here has no one to tell.
    (void)lseek(fd, saved, SEEK_SET);
    pthread_mutex_unlock(&g_positioned_lock);
    errno = e;
    return result;
}

ssize_t wl_posix_pread(int fd, void *buf, size_t count, int64_t offset) {
    return positioned(fd, buf, count, offset, false);
}

ssize_t wl_posix_pwrite(int fd, const void *buf, size_t count, int64_t offset) {
    return positioned(fd, (void *)(uintptr_t)buf, count, offset, true);
}

int wl_posix_pause(void) {
#if defined(__wii__)
    for (;;) {
        wl_platform_thread_sleep_ms(3600u * 1000u);
    }
#else
    return pause();
#endif
}
