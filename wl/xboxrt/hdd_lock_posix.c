// SPDX-License-Identifier: MIT
// The hard disk's folder held by one run at a time on the host (boot.h, wl_xb_hdd_lock): flock on
// the folder itself, so nothing is written into it and the lock goes with the process.
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "wl/xboxrt/boot.h"

WlResult wl_xb_hdd_lock(const char *host_dir, int *out_fd) {
    if (out_fd == NULL) {
        return WL_ERR_ARG;
    }
    *out_fd = -1;
    if (host_dir == NULL || host_dir[0] == '\0') {
        return WL_ERR_ARG;
    }
    int fd = open(host_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT || errno == ENOTDIR ? WL_ERR_NOT_FOUND : WL_ERR_IO;
    }
    while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        if (e == EINTR) {
            continue;
        }
        close(fd);
        return e == EWOULDBLOCK ? WL_ERR_DENIED : WL_ERR_UNSUPPORTED;
    }
    *out_fd = fd;
    return WL_OK;
}

void wl_xb_hdd_unlock(int fd) {
    if (fd >= 0) {
        close(fd); // (the lock goes with the last descriptor of its open file)
    }
}
