// SPDX-License-Identifier: MIT
// The file half of platform.h on POSIX file descriptors, shared by the Wii backend (libfat's
// devoptab: "sd:/..." paths are opened as they are) and the host stub (the same logic against a
// directory, so host tests exercise what the console runs). Included by exactly one .c file that
// first defines
//   static WlResult fs_resolve(const char *path, char *out, size_t cap);
// which checks the device prefix and mount state and writes the path to hand to the C library.
// Unbuffered fds are used on purpose: stdio's 1 KiB default buffer would split large reads into
// many small SD transfers.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wl/platform/platform.h"

struct WlPlatformFile {
    atomic_bool busy;
    int fd;
    uint64_t size;
    const uint8_t *virt;        // a virtual file's bytes (fd is -1): read-only, see wl_platform_fs_virtual_set
    bool dirty;                 // something was written through this handle
    char wpath[WL_FS_PATH_MAX]; // the path of a file opened for writing while a written hook is set
};

static WlPlatformFile g_files[WL_FS_MAX_FILES];

// ---- virtual files and the written hook (platform.h) ---------------------------------------------

typedef struct {
    char path[WL_FS_PATH_MAX];
    uint8_t *data;
    uint32_t size;
} FsVirtual;

static FsVirtual g_virtual[WL_FS_VIRTUAL_MAX];
static atomic_uint g_virtual_count;
static WlFsWrittenFn g_written_fn;
static void *g_written_user;

WlResult wl_platform_fs_virtual_set(const char *path, const void *data, uint32_t size) {
    if (path == NULL || (data == NULL && size > 0u) || strlen(path) >= WL_FS_PATH_MAX ||
        (strncmp(path, "sd:/", 4) != 0 && strncmp(path, "usb:/", 5) != 0)) {
        return WL_ERR_ARG;
    }
    unsigned n = atomic_load(&g_virtual_count);
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(g_virtual[i].path, path) == 0) {
            return WL_ERR_STATE; // (set once: a reader may hold its bytes)
        }
    }
    if (n >= WL_FS_VIRTUAL_MAX) {
        return WL_ERR_LIMIT;
    }
    uint8_t *copy = malloc(size > 0u ? size : 1u);
    if (copy == NULL) {
        return WL_ERR_NOMEM;
    }
    if (size > 0u) {
        memcpy(copy, data, size);
    }
    strcpy(g_virtual[n].path, path);
    g_virtual[n].data = copy;
    g_virtual[n].size = size;
    atomic_store(&g_virtual_count, n + 1u); // (set from one thread, before the readers look)
    return WL_OK;
}

bool wl_platform_fs_virtual_get(const char *path, const void **data, uint32_t *size) {
    if (path == NULL) {
        return false;
    }
    unsigned n = atomic_load(&g_virtual_count);
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(g_virtual[i].path, path) == 0) {
            if (data != NULL) {
                *data = g_virtual[i].data;
            }
            if (size != NULL) {
                *size = g_virtual[i].size;
            }
            return true;
        }
    }
    return false;
}

void wl_platform_fs_set_written_hook(WlFsWrittenFn fn, void *user) {
    g_written_user = user;
    g_written_fn = fn; // (user first: set from the main thread before the files it watches)
}

void wl_platform_fs_notify_written(const char *path) {
    WlFsWrittenFn fn = g_written_fn;
    if (fn != NULL && path != NULL) {
        fn(g_written_user, path);
    }
}

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
    if (mode == WL_FILE_READ) {
        const void *bytes;
        uint32_t vsize;
        if (wl_platform_fs_virtual_get(path, &bytes, &vsize)) {
            WlPlatformFile *vf = fs_slot_claim();
            if (vf == NULL) {
                return WL_ERR_LIMIT;
            }
            vf->fd = -1;
            vf->size = vsize;
            vf->virt = bytes;
            vf->dirty = false;
            vf->wpath[0] = '\0';
            *out = vf;
            return WL_OK;
        }
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
    f->virt = NULL;
    f->dirty = false;
    f->wpath[0] = '\0';
    if (mode != WL_FILE_READ && g_written_fn != NULL) {
        snprintf(f->wpath, sizeof f->wpath, "%s", path);
    }
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
    if (file->virt != NULL) {
        memcpy(dst, file->virt + offset, size);
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
    if (file->virt != NULL) {
        return WL_ERR_DENIED;
    }
    file->dirty = true;
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
    return file->virt != NULL || fsync(file->fd) == 0 ? WL_OK : WL_ERR_IO;
}

uint64_t wl_platform_fs_size(const WlPlatformFile *file) {
    return file != NULL ? file->size : 0;
}

void wl_platform_fs_close(WlPlatformFile *file) {
    if (file == NULL || !atomic_load(&file->busy)) {
        return;
    }
    char written[WL_FS_PATH_MAX];
    bool notify = file->dirty && file->wpath[0] != '\0';
    if (notify) {
        memcpy(written, file->wpath, sizeof written);
    }
    if (file->fd >= 0) {
        close(file->fd);
    }
    file->fd = -1;
    file->virt = NULL;
    file->dirty = false;
    file->wpath[0] = '\0';
    atomic_store(&file->busy, false);
    if (notify) {
        wl_platform_fs_notify_written(written);
    }
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
        wl_platform_fs_notify_written(to);
        return WL_OK;
    }
    int e = errno;
    struct stat st;
    if (stat(a, &st) != 0) {
        return WL_ERR_NOT_FOUND; // the source is missing
    }
    // FAT cannot rename over an existing file: remove the target and try once more.
    if (stat(b, &st) == 0 && !S_ISDIR(st.st_mode) && unlink(b) == 0 && rename(a, b) == 0) {
        wl_platform_fs_notify_written(to);
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

bool wl_platform_fs_exists(const char *path) {
    char real[WL_FS_PATH_MAX + 64];
    struct stat st;
    if (wl_platform_fs_virtual_get(path, NULL, NULL)) {
        return true;
    }
    return fs_resolve(path, real, sizeof real) == WL_OK && stat(real, &st) == 0;
}

// ---- Files at offsets, sizes, directories ------------------------------------------------------

static WlResult fs_write_all(int fd, const uint8_t *p, uint32_t left) {
    while (left > 0) {
        ssize_t n = write(fd, p, left);
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
    return WL_OK;
}

WlResult wl_platform_fs_write_at(WlPlatformFile *file, uint64_t offset, const void *src, uint32_t size) {
    if (file == NULL || (src == NULL && size > 0)) {
        return WL_ERR_ARG;
    }
    if (offset > 0xFFFFFFFFull) {
        return WL_ERR_RANGE;
    }
    if (file->virt != NULL) {
        return WL_ERR_DENIED;
    }
    file->dirty = true;
    if (lseek(file->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        return WL_ERR_IO;
    }
    WlResult r = fs_write_all(file->fd, src, size);
    if (r == WL_OK && offset + size > file->size) {
        file->size = offset + size;
    }
    return r;
}

WlResult wl_platform_fs_read_some_at(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size,
                                     uint32_t *got) {
    if (file == NULL || got == NULL || (dst == NULL && size > 0)) {
        return WL_ERR_ARG;
    }
    *got = 0;
    if (offset >= file->size || size == 0) {
        return WL_OK;
    }
    if (offset + size > file->size) {
        size = (uint32_t)(file->size - offset);
    }
    if (file->virt != NULL) {
        memcpy(dst, file->virt + offset, size);
        *got = size;
        return WL_OK;
    }
    if (lseek(file->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        return WL_ERR_IO;
    }
    uint8_t *p = dst;
    while (*got < size) {
        ssize_t n = read(file->fd, p + *got, size - *got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return WL_ERR_IO;
        }
        if (n == 0) {
            break; // the file shrank under us: what was there
        }
        *got += (uint32_t)n;
    }
    return WL_OK;
}

WlResult wl_platform_fs_truncate(WlPlatformFile *file, uint64_t size) {
    if (file == NULL) {
        return WL_ERR_ARG;
    }
    if (size > 0xFFFFFFFFull) {
        return WL_ERR_RANGE;
    }
    if (file->virt != NULL) {
        return WL_ERR_DENIED;
    }
    file->dirty = true;
    if (ftruncate(file->fd, (off_t)size) != 0) {
        return WL_ERR_IO;
    }
    file->size = size;
    return WL_OK;
}

static void fs_stat_from(const struct stat *st, WlPlatformStat *out) {
    out->is_dir = S_ISDIR(st->st_mode);
    out->size = out->is_dir ? 0 : (uint64_t)st->st_size;
    out->mtime_us = st->st_mtime > 0 ? (uint64_t)st->st_mtime * 1000000ull : 0;
    out->read_only = (st->st_mode & S_IWUSR) == 0;
}

WlResult wl_platform_fs_stat(const char *path, WlPlatformStat *out) {
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    char real[WL_FS_PATH_MAX + 64];
    const void *vbytes;
    uint32_t vsize;
    if (wl_platform_fs_virtual_get(path, &vbytes, &vsize)) {
        memset(out, 0, sizeof *out);
        out->size = vsize;
        out->read_only = true;
        return WL_OK;
    }
    WlResult r = fs_resolve(path, real, sizeof real);
    if (r != WL_OK) {
        return r;
    }
    struct stat st;
    if (stat(real, &st) != 0) {
        return fs_errno_result(errno);
    }
    fs_stat_from(&st, out);
    return WL_OK;
}

WlResult wl_platform_fs_fstat(WlPlatformFile *file, WlPlatformStat *out) {
    if (file == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    if (file->virt != NULL) {
        memset(out, 0, sizeof *out);
        out->size = file->size;
        out->read_only = true;
        return WL_OK;
    }
    struct stat st;
    if (fstat(file->fd, &st) != 0) {
        return WL_ERR_IO;
    }
    fs_stat_from(&st, out);
    out->size = file->size; // what this handle wrote, even before the device has it
    return WL_OK;
}

WlResult wl_platform_fs_rmdir(const char *path) {
    char real[WL_FS_PATH_MAX + 64];
    WlResult r = fs_resolve(path, real, sizeof real);
    if (r != WL_OK) {
        return r;
    }
    if (rmdir(real) == 0) {
        return WL_OK;
    }
    int e = errno;
    // (libfat answers a directory with entries in it with EPERM, not ENOTEMPTY; FAT has no other
    // permission than the read-only attribute, which also means the directory stays)
    return (e == ENOTEMPTY || e == EEXIST || e == EBUSY || e == EPERM || e == EACCES) ? WL_ERR_DENIED
                                                                                    : fs_errno_result(e);
}

struct WlPlatformDir {
    atomic_bool busy;
    DIR *stream;
    char path[WL_FS_PATH_MAX + 64]; // the C library's path of the directory, for each entry's stat
};

static WlPlatformDir g_dirs[WL_FS_MAX_DIRS];

WlResult wl_platform_fs_dir_open(const char *path, WlPlatformDir **out) {
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    char real[WL_FS_PATH_MAX + 64];
    WlResult r = fs_resolve(path, real, sizeof real);
    if (r != WL_OK) {
        return r;
    }
    WlPlatformDir *d = NULL;
    for (uint32_t i = 0; i < WL_FS_MAX_DIRS && d == NULL; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&g_dirs[i].busy, &expected, true)) {
            d = &g_dirs[i];
        }
    }
    if (d == NULL) {
        return WL_ERR_LIMIT;
    }
    d->stream = opendir(real);
    if (d->stream == NULL) {
        int e = errno;
        atomic_store(&d->busy, false);
        return fs_errno_result(e);
    }
    size_t n = strlen(real);
    while (n > 1 && real[n - 1] == '/') {
        real[--n] = '\0';
    }
    memcpy(d->path, real, n + 1);
    *out = d;
    return WL_OK;
}

WlResult wl_platform_fs_dir_next(WlPlatformDir *dir, char *name, size_t name_cap, WlPlatformStat *st) {
    if (dir == NULL || name == NULL || name_cap == 0) {
        return WL_ERR_ARG;
    }
    for (;;) {
        struct dirent *e = readdir(dir->stream);
        if (e == NULL) {
            return WL_ERR_NOT_FOUND;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        size_t len = strlen(e->d_name);
        size_t keep = len < name_cap - 1 ? len : name_cap - 1;
        memcpy(name, e->d_name, keep);
        name[keep] = '\0';
        if (st != NULL) {
            char full[2 * (WL_FS_PATH_MAX + 64)];
            struct stat s;
            memset(st, 0, sizeof *st);
            int n = snprintf(full, sizeof full, "%s/%s", dir->path, e->d_name);
            if (n > 0 && (size_t)n < sizeof full && stat(full, &s) == 0) {
                fs_stat_from(&s, st);
            }
        }
        return WL_OK;
    }
}

void wl_platform_fs_dir_rewind(WlPlatformDir *dir) {
    if (dir != NULL) {
        rewinddir(dir->stream);
    }
}

void wl_platform_fs_dir_close(WlPlatformDir *dir) {
    if (dir == NULL || !atomic_load(&dir->busy)) {
        return;
    }
    closedir(dir->stream);
    dir->stream = NULL;
    atomic_store(&dir->busy, false);
}
