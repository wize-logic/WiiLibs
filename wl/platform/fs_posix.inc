// SPDX-License-Identifier: MIT
// The file half of platform.h on POSIX file descriptors, shared by the Wii backend (libfat's
// devoptab: "sd:/..." paths are opened as they are) and the host stub (the same logic against a
// directory, so host tests exercise what the console runs). Included by exactly one .c file that
// first defines
//   static WlResult fs_resolve(const char *path, char *out, size_t cap);
// which checks the device prefix and mount state and writes the path to hand to the C library.
// Unbuffered fds are used on purpose: stdio's 1 KiB default buffer would split large reads into
// many small SD transfers.
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wl/platform/platform.h"

struct WlPlatformFile {
    atomic_bool busy;
    int fd;
    uint64_t size;
};

static WlPlatformFile g_files[WL_FS_MAX_FILES];

static WlResult fs_errno_result(int e) {
    return (e == ENOENT || e == ENOTDIR) ? WL_ERR_NOT_FOUND : WL_ERR_IO;
}

static WlPlatformFile *fs_slot_claim(void) {
    for (uint32_t i = 0; i < WL_FS_MAX_FILES; i++) {
        bool expected = false;
        // Lock-free: no mutex, so a preempted low-priority thread can never stall a higher one.
        if (atomic_compare_exchange_strong(&g_files[i].busy, &expected, true)) {
            return &g_files[i];
        }
    }
    return NULL;
}

WlResult wl_platform_fs_open(const char *path, WlPlatformFileMode mode, WlPlatformFile **out) {
    if (path == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    char real[WL_FS_PATH_MAX + 64];
    WlResult r = fs_resolve(path, real, sizeof real);
    if (r != WL_OK) {
        return r;
    }
    int flags = O_RDONLY;
    if (mode == WL_FILE_WRITE_NEW) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mode == WL_FILE_READ_WRITE) {
        flags = O_RDWR | O_CREAT;
    } else if (mode != WL_FILE_READ) {
        return WL_ERR_ARG;
    }
    WlPlatformFile *f = fs_slot_claim();
    if (f == NULL) {
        return WL_ERR_LIMIT;
    }
    int fd = open(real, flags, 0666);
    if (fd < 0) {
        int e = errno;
        atomic_store(&f->busy, false);
        return fs_errno_result(e);
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        atomic_store(&f->busy, false);
        return WL_ERR_IO;
    }
    f->fd = fd;
    f->size = (uint64_t)st.st_size;
    *out = f;
    return WL_OK;
}

WlResult wl_platform_fs_read_at(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size) {
    if (file == NULL || (dst == NULL && size > 0)) {
        return WL_ERR_ARG;
    }
    if (offset > 0xFFFFFFFFull) {
        return WL_ERR_RANGE;
    }
    if (offset + size > file->size) {
        return WL_ERR_RANGE;
    }
    if (size == 0) {
        return WL_OK;
    }
    if (lseek(file->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        return WL_ERR_IO;
    }
    uint8_t *p = dst;
    uint32_t left = size;
    while (left > 0) {
        ssize_t n = read(file->fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return WL_ERR_IO;
        }
        if (n == 0) {
            return WL_ERR_RANGE; // the file shrank under us
        }
        p += n;
        left -= (uint32_t)n;
    }
    return WL_OK;
}

WlResult wl_platform_fs_write(WlPlatformFile *file, const void *src, uint32_t size) {
    if (file == NULL || (src == NULL && size > 0)) {
        return WL_ERR_ARG;
    }
    const uint8_t *p = src;
    uint32_t left = size;
    while (left > 0) {
        ssize_t n = write(file->fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return WL_ERR_IO;
        }
        if (n == 0) {
            return WL_ERR_IO; // card full
        }
        p += n;
        left -= (uint32_t)n;
    }
    off_t pos = lseek(file->fd, 0, SEEK_CUR);
    if (pos >= 0 && (uint64_t)pos > file->size) {
        file->size = (uint64_t)pos;
    }
    return WL_OK;
}

WlResult wl_platform_fs_flush(WlPlatformFile *file) {
    if (file == NULL) {
        return WL_ERR_ARG;
    }
    return fsync(file->fd) == 0 ? WL_OK : WL_ERR_IO;
}

uint64_t wl_platform_fs_size(const WlPlatformFile *file) {
    return file != NULL ? file->size : 0;
}

void wl_platform_fs_close(WlPlatformFile *file) {
    if (file == NULL || !atomic_load(&file->busy)) {
        return;
    }
    close(file->fd);
    file->fd = -1;
    atomic_store(&file->busy, false);
}

WlResult wl_platform_fs_rename(const char *from, const char *to) {
    char a[WL_FS_PATH_MAX + 64];
    char b[WL_FS_PATH_MAX + 64];
    WlResult r = fs_resolve(from, a, sizeof a);
    if (r == WL_OK) {
        r = fs_resolve(to, b, sizeof b);
    }
    if (r != WL_OK) {
        return r;
    }
    if (rename(a, b) == 0) {
        return WL_OK;
    }
    int e = errno;
    struct stat st;
    if (stat(a, &st) != 0) {
        return WL_ERR_NOT_FOUND; // the source is missing
    }
    // FAT cannot rename over an existing file: remove the target and try once more.
    if (stat(b, &st) == 0 && !S_ISDIR(st.st_mode) && unlink(b) == 0 && rename(a, b) == 0) {
        return WL_OK;
    }
    return fs_errno_result(e);
}

WlResult wl_platform_fs_remove(const char *path) {
    char real[WL_FS_PATH_MAX + 64];
    WlResult r = fs_resolve(path, real, sizeof real);
    if (r != WL_OK) {
        return r;
    }
    return unlink(real) == 0 ? WL_OK : fs_errno_result(errno);
}

WlResult wl_platform_fs_mkdir(const char *path) {
    char real[WL_FS_PATH_MAX + 64];
    WlResult r = fs_resolve(path, real, sizeof real);
    if (r != WL_OK) {
        return r;
    }
    if (mkdir(real, 0777) == 0) {
        return WL_OK;
    }
    int e = errno;
    struct stat st;
    if (e == EEXIST && stat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
        return WL_OK;
    }
    return fs_errno_result(e);
}
