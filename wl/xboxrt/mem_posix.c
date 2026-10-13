// SPDX-License-Identifier: MIT
// Guest memory on the host and ppc-be (mem_internal.h): a 512 MB PROT_NONE reservation is the EA
// span; the guest RAM is a shared memory object (memfd, or an unlinked temporary file) as large as
// the RAM, mapped once over the window EAs and again, page by page, under each committed user
// page, so both EAs of a physical page are the same memory. The object is sparse: a page takes
// host memory on its first touch and reads as zero before it, which is the lazy commit the Wii
// does by hand. Everything else in the span (uncommitted user pages, the devices) stays PROT_NONE
// and faults (fault_posix.c).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "wl/xboxrt/mem_internal.h"

#define TAG "xbmem"

static uint8_t *g_base;
static int g_fd = -1;
static uint32_t g_ram;
static bool g_punch = true; // the object frees punched holes (else released pages are zeroed by hand)

static int open_ram_object(void) {
#if defined(SYS_memfd_create)
    int fd = (int)syscall(SYS_memfd_create, "wl_xboxrt_ram", 0u);
    if (fd >= 0) {
        return fd;
    }
#endif
    const char *dir = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/wl_xboxrt_ram_XXXXXX", dir != NULL && dir[0] != '\0' ? dir : "/tmp");
    int fd2 = mkstemp(path);
    if (fd2 >= 0) {
        unlink(path);
    }
    return fd2;
}

WlResult wl_xb_backend_init(const WlXbMemConfig *cfg, uintptr_t *base_out) {
    if (sysconf(_SC_PAGESIZE) != (long)WL_XB_PAGE) {
        WL_LOG_E(TAG, "host page size %ld: guest memory needs 4 KB pages", sysconf(_SC_PAGESIZE));
        return WL_ERR_UNSUPPORTED;
    }
    void *span = mmap(NULL, WL_XB_EA_SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (span == MAP_FAILED) {
        WL_LOG_E(TAG, "cannot reserve %u MB of address space: %s", WL_XB_EA_SPAN >> 20, strerror(errno));
        return WL_ERR_NOMEM;
    }
    g_fd = open_ram_object();
    if (g_fd < 0 || ftruncate(g_fd, (off_t)cfg->ram_bytes) != 0) {
        WL_LOG_E(TAG, "cannot make the RAM object: %s", strerror(errno));
        munmap(span, WL_XB_EA_SPAN);
        return WL_ERR_NOMEM;
    }
    g_base = span;
    g_ram = cfg->ram_bytes;
    void *window = mmap(g_base + WL_XB_EA_WINDOW, g_ram, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_fd, 0);
    if (window == MAP_FAILED) {
        WL_LOG_E(TAG, "cannot map the window: %s", strerror(errno));
        return WL_ERR_NOMEM;
    }
    *base_out = (uintptr_t)g_base;
    return WL_OK;
}

void wl_xb_backend_map_user(uint32_t va, uint32_t phys, uint32_t pages) {
    void *at = mmap(g_base + va, (size_t)pages << WL_XB_PAGE_SHIFT, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_fd,
                    (off_t)phys << WL_XB_PAGE_SHIFT);
    if (at == MAP_FAILED) {
        WL_LOG_E(TAG, "map of user 0x%08x (%u pages) failed: %s", (unsigned)va, (unsigned)pages, strerror(errno));
        abort();
    }
}

void wl_xb_backend_unmap_user(uint32_t va, uint32_t pages) {
    void *at = mmap(g_base + va, (size_t)pages << WL_XB_PAGE_SHIFT, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (at == MAP_FAILED) {
        WL_LOG_E(TAG, "unmap of user 0x%08x failed: %s", (unsigned)va, strerror(errno));
        abort();
    }
}

void wl_xb_backend_release_phys(uint32_t phys, uint32_t pages) {
    off_t off = (off_t)phys << WL_XB_PAGE_SHIFT;
    off_t len = (off_t)pages << WL_XB_PAGE_SHIFT;
#if defined(FALLOC_FL_PUNCH_HOLE)
    if (g_punch && fallocate(g_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, len) == 0) {
        return;
    }
    g_punch = false;
#endif
    memset(g_base + WL_XB_EA_WINDOW + off, 0, (size_t)len);
}

void wl_xb_backend_move_phys(uint32_t from, uint32_t to, uint32_t va) {
    memcpy(g_base + WL_XB_EA_WINDOW + ((size_t)to << WL_XB_PAGE_SHIFT),
           g_base + WL_XB_EA_WINDOW + ((size_t)from << WL_XB_PAGE_SHIFT), WL_XB_PAGE);
    wl_xb_backend_map_user(va, to, 1);
    wl_xb_backend_release_phys(from, 1);
}

void wl_xb_backend_stats(uint32_t *frames, uint32_t *faults, uint32_t *evictions) {
    // resident pages of the RAM object: what the window maps that has been touched
    uint32_t resident = 0;
#if defined(__linux__)
    uint32_t pages = g_ram >> WL_XB_PAGE_SHIFT;
    unsigned char *vec = malloc(pages);
    if (vec != NULL && mincore(g_base + WL_XB_EA_WINDOW, g_ram, vec) == 0) {
        for (uint32_t i = 0; i < pages; i++) {
            resident += vec[i] & 1u;
        }
    }
    free(vec);
#endif
    *frames = resident;
    *faults = 0;
    *evictions = 0;
}
