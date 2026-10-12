// SPDX-License-Identifier: MIT
// A cache of directory listings for resolving a name written in another case than the file system has it:
// a program built for a volume that did not tell the cases apart, whose paths must name each entry as stored
// (a case-sensitive host, or other code that compares names).
// Looking into a directory opens it and reads all its entries, and on libfat each of those searches the path
// from the root, so a program that translates hundreds of paths pays for the same folders again and again.
// This reads a folder once.
//
// The cache answers differently from a look into the folder only in the case of a letter: a file made since
// the folder was read is not in the listing (the name is taken as written), one removed since is (the name is
// taken as it was). Both name the same file on a volume that does not tell the cases apart. A folder that is
// not there when it is first looked into stays "not there", and one with more entries than the cache keeps is
// never kept (wl_dircache_spelling returns false and the caller looks into it itself).
//
// Limits: WL_DIRCACHE_MAX_DIRECTORIES folders, WL_DIRCACHE_MAX_ENTRIES entries and WL_DIRCACHE_MAX_NAME_BYTES
// bytes of names in a folder. Thread-safe: one lock, held while a folder is read (a folder's first look only).
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WL_DIRCACHE_MAX_DIRECTORIES 64u
#define WL_DIRCACHE_MAX_ENTRIES 512u
#define WL_DIRCACHE_MAX_NAME_BYTES 16384u

// Reads `directory`, calling visit(name, ctx) for each entry (not "." or ".."), stopping when it returns false.
// Returns false when the directory cannot be opened. The default (a NULL lister in the cache) is opendir and
// readdir.
typedef bool (*WlDirVisit)(const char *name, void *ctx);
typedef bool (*WlDirLister)(const char *directory, WlDirVisit visit, void *ctx, void *user);

typedef struct WlDirEntryList WlDirEntryList;

typedef struct WlDirCache {
    pthread_mutex_t lock;
    WlDirLister lister; // NULL: opendir and readdir
    void *lister_user;
    WlDirEntryList *lists[WL_DIRCACHE_MAX_DIRECTORIES];
    uint32_t count;
    uint32_t reads; // the folders read, for tests and logs
} WlDirCache;

// A cache that uses the default lister. (Or WlDirCache c; wl_dircache_init(&c, lister, user).)
#define WL_DIRCACHE_INIT {.lock = PTHREAD_MUTEX_INITIALIZER}

// `lister` NULL for the default.
void wl_dircache_init(WlDirCache *cache, WlDirLister lister, void *lister_user);
// Frees every listing (a program that changes folders behind the cache's back calls this).
void wl_dircache_clear(WlDirCache *cache);
// Forgets one folder's listing (a program that has just made or removed files in it).
void wl_dircache_forget(WlDirCache *cache, const char *directory);

// The spelling `component` has in `directory`: an entry written as it is, else the first entry that differs in
// the case only, else `component` as written. Written to `out` (`size` bytes, truncated to fit). Returns true
// when the folder is kept; false when it is not (more entries than the cache keeps, the table full, no memory)
// and `out` is not written: the caller looks into the folder itself.
bool wl_dircache_spelling(WlDirCache *cache, const char *directory, const char *component, char *out, size_t size);
