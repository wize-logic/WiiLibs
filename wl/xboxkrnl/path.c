// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026
// sp00nz and contributors.
// Xbox object names to host paths: the symbolic links (\??\D: and the rest, which XAPI creates with
// IoCreateSymbolicLink), the mount table (NT devices to wl/platform folders), and a case-blind walk
// of the host folders (FATX is case-insensitive; a host's file system need not be).
//
// The device layout follows xboxrecomp's kernel_path.c (MIT): CdRom0 is the title's folder,
// Harddisk0's Partition1 (E:) holds TDATA and UDATA, Partition2 is C:, Partitions 3-5 the cache
// drives behind X:, Y: and Z:.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "wl/xboxkrnl/nt_file.h"

#define MAX_LINKS 32u

typedef struct {
    char link[24];     // "\??\D:"
    char target[160];  // "\Device\CdRom0"
} Link;

static XkMount g_mounts[WL_XK_MAX_MOUNTS + 8u];
static uint32_t g_mount_count;
static Link g_links[MAX_LINKS];

static bool prefix_ci(const char *s, const char *prefix, size_t *len) {
    size_t i = 0;
    for (; prefix[i] != '\0'; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) {
            return false;
        }
    }
    *len = i;
    return true;
}

static void add_mount(const char *device, const char *host, bool writable, bool sparse) {
    for (uint32_t i = 0; i < g_mount_count; i++) {
        if (strcasecmp(g_mounts[i].device, device) == 0) {
            snprintf(g_mounts[i].host, sizeof g_mounts[i].host, "%s", host);
            g_mounts[i].writable = writable;
            g_mounts[i].sparse = sparse;
            return;
        }
    }
    if (g_mount_count < WL_ARRAY_COUNT(g_mounts)) {
        XkMount *m = &g_mounts[g_mount_count++];
        snprintf(m->device, sizeof m->device, "%s", device);
        snprintf(m->host, sizeof m->host, "%s", host);
        m->writable = writable;
        m->sparse = sparse;
    }
}

// Makes every level of a platform path ("sd:/a/b/c").
static void mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *colon = strstr(tmp, ":/");
    char *p = colon != NULL ? colon + 2 : tmp;
    for (; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            wl_platform_fs_mkdir(tmp);
            *p = '/';
        }
    }
    wl_platform_fs_mkdir(tmp);
}

void xk_paths_init(const WlXkConfig *cfg) {
    char host[192];
    g_mount_count = 0;
    add_mount("\\Device\\CdRom0", cfg->title_dir, false, false);
    static const struct {
        const char *device, *sub;
        bool sparse;
    } parts[] = {
        {"\\Device\\Harddisk0\\Partition1", "E", false}, {"\\Device\\Harddisk0\\Partition2", "C", false},
        {"\\Device\\Harddisk0\\Partition3", "X", true},  {"\\Device\\Harddisk0\\Partition4", "Y", true},
        {"\\Device\\Harddisk0\\Partition5", "Z", true},
    };
    for (size_t i = 0; i < WL_ARRAY_COUNT(parts); i++) {
        snprintf(host, sizeof host, "%s/%s", cfg->data_dir, parts[i].sub);
        mkdir_p(host);
        add_mount(parts[i].device, host, true, parts[i].sparse);
    }
    for (uint32_t i = 0; i < cfg->mount_count && cfg->mounts != NULL; i++) {
        const WlXkMount *m = &cfg->mounts[i];
        if (m->device != NULL && m->host_path != NULL) {
            if (m->writable) {
                mkdir_p(m->host_path);
            }
            add_mount(m->device, m->host_path, m->writable, m->sparse);
        }
    }
    for (uint32_t i = 0; i < g_mount_count; i++) {
        WL_LOG_I(XK_TAG, "mount %s -> %s%s%s", g_mounts[i].device, g_mounts[i].host, g_mounts[i].writable ? " (rw" : " (ro",
                 g_mounts[i].sparse ? ", sparse)" : ")");
    }
}

bool xk_link_create(const char *link, const char *target) {
    for (uint32_t i = 0; i < MAX_LINKS; i++) {
        if (g_links[i].link[0] != '\0' && strcasecmp(g_links[i].link, link) == 0) {
            snprintf(g_links[i].target, sizeof g_links[i].target, "%s", target); // replaced
            return true;
        }
    }
    for (uint32_t i = 0; i < MAX_LINKS; i++) {
        if (g_links[i].link[0] == '\0') {
            snprintf(g_links[i].link, sizeof g_links[i].link, "%s", link);
            snprintf(g_links[i].target, sizeof g_links[i].target, "%s", target);
            WL_LOG_I(XK_TAG, "link %s -> %s", link, target);
            return true;
        }
    }
    return false;
}

bool xk_link_delete(const char *link) {
    for (uint32_t i = 0; i < MAX_LINKS; i++) {
        if (g_links[i].link[0] != '\0' && strcasecmp(g_links[i].link, link) == 0) {
            g_links[i].link[0] = '\0';
            return true;
        }
    }
    return false;
}

const char *xk_link_lookup(const char *link) {
    for (uint32_t i = 0; i < MAX_LINKS; i++) {
        if (g_links[i].link[0] != '\0' && strcasecmp(g_links[i].link, link) == 0) {
            return g_links[i].target;
        }
    }
    return NULL;
}

// The device behind a drive letter when no link names it: what the console's XAPI would link.
static bool default_drive(char letter, char *out, size_t cap) {
    switch (toupper((unsigned char)letter)) {
    case 'D':
        snprintf(out, cap, "\\Device\\CdRom0");
        return true;
    case 'C':
        snprintf(out, cap, "\\Device\\Harddisk0\\Partition2");
        return true;
    case 'E':
        snprintf(out, cap, "\\Device\\Harddisk0\\Partition1");
        return true;
    case 'T':
        snprintf(out, cap, "\\Device\\Harddisk0\\Partition1\\TDATA\\%08x", (unsigned)g_xk_cfg.title_id);
        return true;
    case 'U':
        snprintf(out, cap, "\\Device\\Harddisk0\\Partition1\\UDATA\\%08x", (unsigned)g_xk_cfg.title_id);
        return true;
    case 'X':
        snprintf(out, cap, "\\Device\\Harddisk0\\Partition3");
        return true;
    case 'Y':
        snprintf(out, cap, "\\Device\\Harddisk0\\Partition4");
        return true;
    case 'Z':
        snprintf(out, cap, "\\Device\\Harddisk0\\Partition5");
        return true;
    default:
        return false;
    }
}

// Rewrites a drive-letter name ("X:\rest", "\??\X:\rest") to its device path. false for none.
static bool expand_drive(const char *name, char *out, size_t cap) {
    size_t n = 0;
    if (prefix_ci(name, "\\??\\", &n)) {
        name += n;
    }
    if (!isalpha((unsigned char)name[0]) || name[1] != ':') {
        snprintf(out, cap, "%s", name);
        return name[0] == '\\';
    }
    char link[8] = {'\\', '?', '?', '\\', (char)toupper((unsigned char)name[0]), ':', '\0'};
    const char *target = xk_link_lookup(link);
    char def[160];
    if (target == NULL) {
        if (!default_drive(name[0], def, sizeof def)) {
            return false;
        }
        target = def;
    }
    const char *rest = name + 2;
    while (*rest == '\\' || *rest == '/') {
        rest++;
    }
    size_t tl = strlen(target);
    int w = snprintf(out, cap, "%s%s%s", target, (tl > 0 && target[tl - 1] != '\\' && *rest != '\0') ? "\\" : "", rest);
    if (*rest == '\0' && name[2] != '\0') {
        // "X:\" names the root directory
        size_t len = strlen(out);
        if (len + 1u < cap && (len == 0 || out[len - 1] != '\\')) {
            out[len] = '\\';
            out[len + 1] = '\0';
        }
    }
    return w > 0 && (size_t)w < cap;
}

bool xk_resolve(const char *name, XkResolved *out) {
    memset(out, 0, sizeof *out);
    char dev[400];
    if (!expand_drive(name, dev, sizeof dev)) {
        return false;
    }
    snprintf(out->device_path, sizeof out->device_path, "%s", dev);
    // bare devices: the DVD drive and the disk's partitions, opened as devices
    size_t n = 0;
    if (strcasecmp(dev, "\\Device\\CdRom0") == 0) {
        out->kind = XK_RES_CDROM;
        return true;
    }
    if (prefix_ci(dev, "\\Device\\Harddisk0\\Partition", &n) && isdigit((unsigned char)dev[n]) && dev[n + 1] == '\0') {
        out->kind = XK_RES_PARTITION;
        out->partition = (uint32_t)(dev[n] - '0');
        return true;
    }
    if (strcasecmp(dev, "\\Device\\Harddisk0\\Partition0") == 0 || strcasecmp(dev, "\\Device\\Harddisk0") == 0) {
        out->kind = XK_RES_PARTITION;
        return true;
    }
    // the longest mount whose device is a prefix of the name, at a separator
    const XkMount *best = NULL;
    size_t best_len = 0;
    for (uint32_t i = 0; i < g_mount_count; i++) {
        size_t len = 0;
        if (prefix_ci(dev, g_mounts[i].device, &len) && (dev[len] == '\0' || dev[len] == '\\') && len > best_len) {
            best = &g_mounts[i];
            best_len = len;
        }
    }
    if (best == NULL) {
        return false;
    }
    out->kind = XK_RES_HOST;
    out->mount = *best;
    int w = snprintf(out->host, sizeof out->host, "%s", best->host);
    size_t at = w > 0 ? (size_t)w : 0;
    out->root_len = (uint32_t)at;
    const char *rest = dev + best_len;
    // components: "\" separators, "." skipped, ".." refused (nothing above a mount)
    while (*rest != '\0') {
        while (*rest == '\\' || *rest == '/') {
            rest++;
        }
        if (*rest == '\0') {
            break;
        }
        const char *end = rest;
        while (*end != '\0' && *end != '\\' && *end != '/') {
            end++;
        }
        size_t len = (size_t)(end - rest);
        if (len == 1 && rest[0] == '.') {
            rest = end;
            continue;
        }
        if (len == 2 && rest[0] == '.' && rest[1] == '.') {
            return false;
        }
        if (at + 1u + len + 1u > sizeof out->host) {
            return false;
        }
        out->host[at++] = '/';
        memcpy(out->host + at, rest, len);
        at += len;
        out->host[at] = '\0';
        rest = end;
    }
    return true;
}

// Finds `host`'s components in another case when the name as given does not exist (from the
// mount's root, which exists). Leaves the path as given for components that are not there.
void xk_resolve_case(char *host, uint32_t root_len) {
    if (wl_platform_fs_exists(host)) {
        return;
    }
    size_t at = root_len;
    while (host[at] == '/') {
        char *comp = host + at + 1;
        char *end = strchr(comp, '/');
        size_t len = end != NULL ? (size_t)(end - comp) : strlen(comp);
        char saved = comp[len];
        comp[len] = '\0';
        bool there = wl_platform_fs_exists(host);
        if (!there) {
            // list the parent and take a match that differs in case only
            host[at] = '\0';
            WlPlatformDir *d = NULL;
            if (wl_platform_fs_dir_open(at == 0 ? "/" : host, &d) == WL_OK) {
                char name[256];
                while (wl_platform_fs_dir_next(d, name, sizeof name, NULL) == WL_OK) {
                    if (strlen(name) == len && strncasecmp(name, comp, len) == 0) {
                        memcpy(comp, name, len);
                        there = true;
                        break;
                    }
                }
                wl_platform_fs_dir_close(d);
            }
            host[at] = '/';
        }
        comp[len] = saved;
        if (!there) {
            return; // the rest cannot exist either
        }
        at += 1u + len;
    }
}
