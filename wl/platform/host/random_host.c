// SPDX-License-Identifier: MIT
// Host entropy: the system's source (getrandom where there is one, /dev/urandom otherwise), which
// is cryptographic, unlike the Wii's. See platform.h.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "wl/platform/platform.h"

#if defined(__linux__)
#include <sys/random.h>
#endif

WlResult wl_platform_random_bytes(void *buf, size_t size) {
    unsigned char *out = buf;
    size_t done = 0;

    if (buf == NULL && size != 0) {
        return WL_ERR_ARG;
    }
#if defined(__linux__)
    while (done < size) {
        ssize_t n = getrandom(out + done, size - done, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; // ENOSYS on an old kernel: read the device
        }
        done += (size_t)n;
    }
#endif
    if (done < size) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

        if (fd < 0) {
            return WL_ERR_IO;
        }
        while (done < size) {
            ssize_t n = read(fd, out + done, size - done);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                close(fd);
                return WL_ERR_IO;
            }
            done += (size_t)n;
        }
        close(fd);
    }
    return WL_OK;
}
