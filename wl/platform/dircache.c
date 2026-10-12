// SPDX-License-Identifier: MIT
// The directory listing cache (dircache.h).
#define _POSIX_C_SOURCE 200809L
#include "wl/platform/dircache.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    LIST_KEPT,      // names holds the entries
    LIST_MISSING,   // the directory was not there
    LIST_TOO_LARGE, // more entries than are kept: the caller looks into the folder each time
};

struct WlDirEntryList {
    char *directory;
    char *names; // the entries' names in the order the directory gave them, one after another, each with its end
    uint32_t count;
    int state;
};

typedef struct {
    char *buffer; // WL_DIRCACHE_MAX_NAME_BYTES
    uint32_t used;
    uint32_t count;
    bool too_large;
} ReadState;

static bool collect(const char *name, void *ctx) {
    ReadState *r = ctx;
    size_t length = strlen(name) + 1u;
    if (r->count >= WL_DIRCACHE_MAX_ENTRIES || r->used + length > WL_DIRCACHE_MAX_NAME_BYTES) {
        r->too_large = true;
        return false;
    }
    memcpy(r->buffer + r->used, name, length);
    r->used += (uint32_t)length;
    r->count++;
    return true;
}

static bool posix_lister(const char *directory, WlDirVisit visit, void *ctx, void *user) {
    (void)user;
    DIR *stream = opendir(directory);
    struct dirent *entry;
    if (stream == NULL) {
        return false;
    }
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!visit(entry->d_name, ctx)) {
            break;
        }
    }
    closedir(stream);
    return true;
}

void wl_dircache_init(WlDirCache *cache, WlDirLister lister, void *lister_user) {
    memset(cache, 0, sizeof *cache);
    pthread_mutex_init(&cache->lock, NULL);
    cache->lister = lister;
    cache->lister_user = lister_user;
}

static void list_free(WlDirEntryList *list) {
    free(list->directory);
    free(list->names);
    free(list);
}

void wl_dircache_clear(WlDirCache *cache) {
    pthread_mutex_lock(&cache->lock);
    for (uint32_t i = 0; i < cache->count; i++) {
        list_free(cache->lists[i]);
        cache->lists[i] = NULL;
    }
    cache->count = 0;
    pthread_mutex_unlock(&cache->lock);
}

void wl_dircache_forget(WlDirCache *cache, const char *directory) {
    pthread_mutex_lock(&cache->lock);
    for (uint32_t i = 0; i < cache->count; i++) {
        if (strcmp(cache->lists[i]->directory, directory) == 0) {
            list_free(cache->lists[i]);
            cache->lists[i] = cache->lists[--cache->count];
            cache->lists[cache->count] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&cache->lock);
}

// The listing of a directory, read when it is not kept yet; NULL when the table is full or memory is short.
// The lock is held.
static WlDirEntryList *list_get(WlDirCache *cache, const char *directory) {
    for (uint32_t i = 0; i < cache->count; i++) {
        if (strcmp(cache->lists[i]->directory, directory) == 0) {
            return cache->lists[i];
        }
    }
    if (cache->count >= WL_DIRCACHE_MAX_DIRECTORIES) {
        return NULL;
    }
    WlDirEntryList *list = calloc(1, sizeof *list);
    ReadState r = {0};
    if (list == NULL) {
        return NULL;
    }
    list->directory = strdup(directory);
    r.buffer = malloc(WL_DIRCACHE_MAX_NAME_BYTES);
    if (list->directory == NULL || r.buffer == NULL) {
        free(r.buffer);
        list_free(list);
        return NULL;
    }
    cache->reads++;
    WlDirLister lister = cache->lister != NULL ? cache->lister : posix_lister;
    if (!lister(directory, collect, &r, cache->lister_user)) {
        list->state = LIST_MISSING;
    } else if (r.too_large) {
        list->state = LIST_TOO_LARGE;
    } else {
        list->state = LIST_KEPT;
        if (r.used > 0) {
            list->names = malloc(r.used);
            if (list->names == NULL) {
                free(r.buffer);
                list_free(list);
                return NULL;
            }
            memcpy(list->names, r.buffer, r.used);
        }
        list->count = r.count;
    }
    free(r.buffer);
    cache->lists[cache->count++] = list;
    return list;
}

bool wl_dircache_spelling(WlDirCache *cache, const char *directory, const char *component, char *out, size_t size) {
    if (cache == NULL || directory == NULL || component == NULL || out == NULL || size == 0) {
        return false;
    }
    bool kept = false;
    pthread_mutex_lock(&cache->lock);
    WlDirEntryList *list = list_get(cache, directory);
    if (list != NULL && list->state != LIST_TOO_LARGE) {
        const char *match = NULL;
        const char *name = list->names;
        kept = true;
        for (uint32_t i = 0; i < list->count; i++) {
            if (strcmp(name, component) == 0) {
                match = name;
                break;
            }
            if (match == NULL && strcasecmp(name, component) == 0) {
                match = name;
            }
            name += strlen(name) + 1u;
        }
        snprintf(out, size, "%s", match != NULL ? match : component);
    }
    pthread_mutex_unlock(&cache->lock);
    return kept;
}
