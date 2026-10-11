// SPDX-License-Identifier: MIT
// File-system shim: the only place in the host tools that touches POSIX. A console
// build can provide its own implementation of this header.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/host/base.h"

WlResult wl_file_size(const char *path, uint64_t *size); /* regular files only */
int wl_file_exists(const char *path);
int wl_file_is_dir(const char *path);
WlResult wl_file_mkdirs(const char *path);        /* mkdir -p */
WlResult wl_file_mkdirs_for(const char *file);    /* creates the parent directories of file */
/* Reads the whole file (at most max bytes) into a malloc'd buffer with one extra NUL byte. */
WlResult wl_file_read_all(const char *path, size_t max, uint8_t **data, size_t *len);
/* Writes through a temporary file and rename, so readers never see a partial file. */
WlResult wl_file_write_all(const char *path, const void *data, size_t len);
WlResult wl_file_truncate(const char *path, uint64_t len);
/* Streamed SHA-1 of a file; size receives the byte count. */
WlResult wl_file_sha1(const char *path, uint8_t digest[20], uint64_t *size);
/* Sorted names of the regular files directly inside dir. */
WlResult wl_file_list(const char *dir, char ***names, size_t *count);
void wl_file_list_free(char **names, size_t count);
WlResult wl_file_make_temp_dir(char *buf, size_t cap, const char *prefix);
WlResult wl_file_remove_tree(const char *path);
WlResult wl_file_rename(const char *from, const char *to);
/* Recursive walk of root in sorted order (entries of a directory sorted by name, a directory
 * reported before its children). relpath is relative to root and uses '/'. Symlinks and special
 * files are skipped. A non-WL_OK return from fn stops the walk and is returned. */
typedef WlResult (*WlFileWalkFn)(void *ud, const char *relpath, int is_dir, uint64_t size);
WlResult wl_file_walk(const char *root, WlFileWalkFn fn, void *ud);
unsigned wl_cpu_count(void);

/* Random-access read-only handle with 64-bit offsets, for images too big to load (3DS, GameCube). */
typedef struct WlFile WlFile;
WlResult wl_file_open_ro(const char *path, WlFile **out, uint64_t *size);
/* Reads exactly n bytes at off; WL_ERR_RANGE when the range passes the end of the file. */
WlResult wl_file_read_at(WlFile *f, uint64_t off, void *buf, size_t n);
void wl_file_close(WlFile *f);
