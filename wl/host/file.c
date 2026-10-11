// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "wl/host/file.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "wl/host/sha1.h"

#define WL_PATH_MAX 1024
#define WL_TREE_DEPTH 16

WlResult wl_file_size(const char *path, uint64_t *size) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return WL_ERR_IO;
    }
    *size = (uint64_t)st.st_size;
    return WL_OK;
}

int wl_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int wl_file_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

WlResult wl_file_mkdirs(const char *path) {
    char buf[WL_PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) {
        return WL_ERR_ARG;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                return WL_ERR_IO;
            }
            buf[i] = saved;
        }
    }
    return wl_file_is_dir(path) ? WL_OK : WL_ERR_IO;
}

WlResult wl_file_mkdirs_for(const char *file) {
    char buf[WL_PATH_MAX];
    const char *slash = strrchr(file, '/');
    if (slash == NULL || slash == file) {
        return WL_OK;
    }
    size_t n = (size_t)(slash - file);
    if (n >= sizeof buf) {
        return WL_ERR_ARG;
    }
    memcpy(buf, file, n);
    buf[n] = '\0';
    return wl_file_mkdirs(buf);
}

WlResult wl_file_read_all(const char *path, size_t max, uint8_t **data, size_t *len) {
    uint64_t size;
    WlResult r = wl_file_size(path, &size);
    if (r != WL_OK) {
        return r;
    }
    if (size > max) {
        return WL_ERR_RANGE;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return WL_ERR_IO;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size + 1);
    if (buf == NULL) {
        (void)fclose(f);
        return WL_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return WL_ERR_IO;
    }
    buf[got] = 0;
    *data = buf;
    *len = got;
    return WL_OK;
}

WlResult wl_file_write_all(const char *path, const void *data, size_t len) {
    char tmp[WL_PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        return WL_ERR_ARG;
    }
    WlResult r = wl_file_mkdirs_for(path);
    if (r != WL_OK) {
        return r;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return WL_ERR_IO;
    }
    int ok = (len == 0 || fwrite(data, 1, len, f) == len);
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        (void)remove(tmp);
        return WL_ERR_IO;
    }
    return WL_OK;
}

WlResult wl_file_truncate(const char *path, uint64_t len) {
    return truncate(path, (off_t)len) == 0 ? WL_OK : WL_ERR_IO;
}

WlResult wl_file_sha1(const char *path, uint8_t digest[20], uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return WL_ERR_IO;
    }
    uint8_t *buf = (uint8_t *)malloc(1u << 20);
    if (buf == NULL) {
        (void)fclose(f);
        return WL_ERR_NOMEM;
    }
    WlSha1 ctx;
    wl_sha1_init(&ctx);
    uint64_t total = 0;
    size_t got;
    while ((got = fread(buf, 1, 1u << 20, f)) > 0) {
        wl_sha1_update(&ctx, buf, got);
        total += got;
    }
    int bad = ferror(f);
    (void)fclose(f);
    free(buf);
    if (bad) {
        return WL_ERR_IO;
    }
    wl_sha1_final(&ctx, digest);
    *size = total;
    return WL_OK;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

WlResult wl_file_list(const char *dir, char ***names, size_t *count) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return WL_ERR_IO;
    }
    char **list = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    WlResult r = WL_OK;
    while (r == WL_OK && (e = readdir(d)) != NULL) {
        char full[WL_PATH_MAX];
        struct stat st;
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full) {
            continue;
        }
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            char **grown = (char **)realloc(list, ncap * sizeof *list);
            if (grown == NULL) {
                r = WL_ERR_NOMEM;
                break;
            }
            list = grown;
            cap = ncap;
        }
        list[n] = wl_strdup(e->d_name);
        if (list[n] == NULL) {
            r = WL_ERR_NOMEM;
            break;
        }
        n++;
    }
    (void)closedir(d);
    if (r != WL_OK) {
        wl_file_list_free(list, n);
        return r;
    }
    if (n > 1) {
        qsort(list, n, sizeof *list, cmp_names);
    }
    *names = list;
    *count = n;
    return WL_OK;
}

void wl_file_list_free(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

WlResult wl_file_make_temp_dir(char *buf, size_t cap, const char *prefix) {
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    if (snprintf(buf, cap, "%s/%sXXXXXX", base, prefix) >= (int)cap) {
        return WL_ERR_ARG;
    }
    return mkdtemp(buf) != NULL ? WL_OK : WL_ERR_IO;
}

static WlResult remove_tree(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return WL_OK;
    }
    if (S_ISDIR(st.st_mode)) {
        if (depth >= WL_TREE_DEPTH) {
            return WL_ERR_RANGE;
        }
        DIR *d = opendir(path);
        if (d == NULL) {
            return WL_ERR_IO;
        }
        struct dirent *e;
        WlResult r = WL_OK;
        while (r == WL_OK && (e = readdir(d)) != NULL) {
            char full[WL_PATH_MAX];
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (snprintf(full, sizeof full, "%s/%s", path, e->d_name) >= (int)sizeof full) {
                r = WL_ERR_ARG;
                break;
            }
            r = remove_tree(full, depth + 1);
        }
        (void)closedir(d);
        if (r != WL_OK) {
            return r;
        }
        return rmdir(path) == 0 ? WL_OK : WL_ERR_IO;
    }
    return unlink(path) == 0 ? WL_OK : WL_ERR_IO;
}

WlResult wl_file_remove_tree(const char *path) {
    return remove_tree(path, 0);
}

WlResult wl_file_rename(const char *from, const char *to) {
    return rename(from, to) == 0 ? WL_OK : WL_ERR_IO;
}

#define WL_WALK_MAX_DEPTH 32

static WlResult walk_dir(const char *root, char *rel, size_t rel_len, int depth, WlFileWalkFn fn,
                         void *ud) {
    if (depth > WL_WALK_MAX_DEPTH) {
        return WL_ERR_RANGE;
    }
    char dirpath[WL_PATH_MAX];
    if (snprintf(dirpath, sizeof dirpath, "%s%s%.*s", root, rel_len ? "/" : "", (int)rel_len,
                 rel) >= (int)sizeof dirpath) {
        return WL_ERR_RANGE;
    }
    DIR *d = opendir(dirpath);
    if (d == NULL) {
        return WL_ERR_IO;
    }
    char **names = NULL;
    size_t n = 0, cap = 0;
    WlResult r = WL_OK;
    struct dirent *e;
    while (r == WL_OK && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            char **grown = (char **)realloc(names, ncap * sizeof *names);
            if (grown == NULL) {
                r = WL_ERR_NOMEM;
                break;
            }
            names = grown;
            cap = ncap;
        }
        names[n] = wl_strdup(e->d_name);
        if (names[n] == NULL) {
            r = WL_ERR_NOMEM;
            break;
        }
        n++;
    }
    (void)closedir(d);
    if (n > 1) {
        qsort(names, n, sizeof *names, cmp_names);
    }
    for (size_t i = 0; r == WL_OK && i < n; i++) {
        size_t nl = strlen(names[i]);
        if (rel_len + 1 + nl + 1 > WL_PATH_MAX) {
            r = WL_ERR_RANGE;
            break;
        }
        size_t at = rel_len;
        if (at > 0) {
            rel[at++] = '/';
        }
        memcpy(rel + at, names[i], nl + 1);
        char full[WL_PATH_MAX];
        struct stat st;
        if (snprintf(full, sizeof full, "%s/%s", root, rel) >= (int)sizeof full) {
            r = WL_ERR_RANGE;
        } else if (lstat(full, &st) != 0) {
            r = WL_ERR_IO;
        } else if (S_ISDIR(st.st_mode)) {
            r = fn(ud, rel, 1, 0);
            if (r == WL_OK) {
                r = walk_dir(root, rel, at + nl, depth + 1, fn, ud);
            }
        } else if (S_ISREG(st.st_mode)) {
            r = fn(ud, rel, 0, (uint64_t)st.st_size);
        }
        rel[rel_len] = '\0';
    }
    wl_file_list_free(names, n);
    return r;
}

WlResult wl_file_walk(const char *root, WlFileWalkFn fn, void *ud) {
    if (root == NULL || fn == NULL) {
        return WL_ERR_ARG;
    }
    char rel[WL_PATH_MAX];
    rel[0] = '\0';
    return walk_dir(root, rel, 0, 0, fn, ud);
}

unsigned wl_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1u;
}

struct WlFile {
    FILE *fp;
    uint64_t size;
};

WlResult wl_file_open_ro(const char *path, WlFile **out, uint64_t *size) {
    if (path == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    *out = NULL;
    uint64_t sz;
    if (wl_file_size(path, &sz) != WL_OK) {
        return WL_ERR_IO;
    }
    WlFile *f = (WlFile *)calloc(1, sizeof *f);
    if (f == NULL) {
        return WL_ERR_NOMEM;
    }
    f->fp = fopen(path, "rb");
    if (f->fp == NULL) {
        free(f);
        return WL_ERR_IO;
    }
    f->size = sz;
    *out = f;
    if (size != NULL) {
        *size = sz;
    }
    return WL_OK;
}

WlResult wl_file_read_at(WlFile *f, uint64_t off, void *buf, size_t n) {
    if (f == NULL || (buf == NULL && n != 0)) {
        return WL_ERR_ARG;
    }
    if (off > f->size || (uint64_t)n > f->size - off) {
        return WL_ERR_RANGE;
    }
    if (n == 0) {
        return WL_OK;
    }
    if (fseeko(f->fp, (off_t)off, SEEK_SET) != 0 || fread(buf, 1, n, f->fp) != n) {
        return WL_ERR_IO;
    }
    return WL_OK;
}

void wl_file_close(WlFile *f) {
    if (f != NULL) {
        (void)fclose(f->fp);
        free(f);
    }
}
