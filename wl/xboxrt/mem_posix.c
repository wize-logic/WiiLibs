// SPDX-License-Identifier: MIT
// Guest memory on the host and ppc-be (mem_internal.h): a 512 MB PROT_NONE reservation is the EA
// span; the guest RAM is a shared memory object (memfd, or an unlinked temporary file) as large as
// the RAM, mapped once over the window EAs and again, page by page, under each committed user
// page, so both EAs of a physical page are the same memory. The object is sparse: a page takes
// host memory on its first touch and reads as zero before it, which is the lazy commit the Wii
// does by hand. Everything else in the span (uncommitted user pages, the devices) stays PROT_NONE
// and faults (fault_posix.c).
//
// With a frame limit (WlXbMemConfig.frame_limit_bytes) the host pages as the Wii does, through
// the same paging core (paging.c), so the clock and the swap file run in host and ppc-be tests:
// every guest EA starts PROT_NONE; a fault (wl_xb_backend_page_fault) gives the page a frame and
// opens the touched EA read-only, or read-write once the page is dirty, so a write to a clean
// page faults once more and marks it (the C bit); the clock's "clear R" closes a page's EAs
// again, and the next touch marks it used (the R bit). A page's bytes are always the object's;
// the runtime reaches them through a third mapping (`g_door`) whatever the guest EAs allow; a
// frame given back is punched out of the object, so the frames in use are what the host holds.
// The write watch (paging.c) closes a watched page's EAs that are open for writing back to
// read-only; its next write faults, which marks it written, and a watched page is only opened for
// writing by a write.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "wl/xboxrt/page_census.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/thread.h"

#define TAG "xbmem"

static uint8_t *g_base;
static int g_fd = -1;
static uint32_t g_ram;
static bool g_punch = true; // the object frees punched holes (else released pages are zeroed by hand)

// paging (a frame limit)
static bool g_paging;
static uint8_t *g_door;      // the RAM object once more, read-write, outside the span
static uint8_t *g_prot[2];   // [ram pages] what the page's user (0) and window (1) EA allow now
static uint32_t *g_user_va;  // [ram pages] the user VA the page is mapped at (0: none)
static uint32_t g_faults;

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
    uint32_t frames = cfg->frame_limit_bytes >> WL_XB_PAGE_SHIFT;
    if (cfg->frame_limit_bytes != 0 && (frames < 16u || frames > 0xFFFFu)) {
        return WL_ERR_ARG;
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
    g_paging = frames != 0;
    if (g_paging) {
        uint32_t pages = g_ram >> WL_XB_PAGE_SHIFT;
        g_prot[0] = calloc(pages, 1); // PROT_NONE is 0
        g_prot[1] = calloc(pages, 1);
        g_user_va = calloc(pages, sizeof *g_user_va);
        void *door = mmap(NULL, g_ram, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
        if (g_prot[0] == NULL || g_prot[1] == NULL || g_user_va == NULL || door == MAP_FAILED) {
            return WL_ERR_NOMEM;
        }
        g_door = door;
        WlResult r = wl_xb_pg_init(cfg, frames);
        if (r != WL_OK) {
            return r;
        }
    }
    void *window = mmap(g_base + WL_XB_EA_WINDOW, g_ram, g_paging ? PROT_NONE : PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FIXED, g_fd, 0);
    if (window == MAP_FAILED) {
        WL_LOG_E(TAG, "cannot map the window: %s", strerror(errno));
        return WL_ERR_NOMEM;
    }
    *base_out = (uintptr_t)g_base;
    return WL_OK;
}

// ---- Paging: the MMU the core asks ----------------------------------------------------------------

static void set_prot(uint32_t p, uint32_t a, uint8_t prot, bool force) {
    if (g_prot[a][p] == prot && !force) {
        return;
    }
    uint8_t *at = a == 1 ? g_base + WL_XB_EA_WINDOW + ((size_t)p << WL_XB_PAGE_SHIFT)
                         : (g_user_va[p] != 0 ? g_base + g_user_va[p] : NULL);
    if (at != NULL && mprotect(at, WL_XB_PAGE, prot) != 0) {
        WL_LOG_E(TAG, "mprotect of page 0x%x (%s): %s", (unsigned)p, a == 1 ? "window" : "user", strerror(errno));
        abort();
    }
    g_prot[a][p] = prot;
}

void wl_xb_mmu_harvest(uint32_t p, bool clear_ref) {
    // R and C are the fault handler's own (it sets the flags as it opens the EAs); clearing R closes
    // the EAs so that the next touch faults and sets it again
    if (clear_ref && (g_xb_pg_flags[p] & WL_XB_PG_REF) != 0) {
        set_prot(p, 0, PROT_NONE, false);
        set_prot(p, 1, PROT_NONE, false);
    }
}

bool wl_xb_mmu_watch(uint32_t p) {
    if ((g_xb_pg_flags[p] & WL_XB_PG_PINNED) != 0) {
        return false; // open for writing for good
    }
    for (uint32_t a = 0; a < 2u; a++) {
        if (g_prot[a][p] == (PROT_READ | PROT_WRITE)) {
            set_prot(p, a, PROT_READ, false);
        }
    }
    return true;
}

void wl_xb_mmu_unmap(uint32_t p) {
    set_prot(p, 0, PROT_NONE, false);
    set_prot(p, 1, PROT_NONE, false);
}

uint8_t *wl_xb_mmu_frame_ptr(uint32_t p, uint32_t f) {
    if (p == WL_XB_PG_NO_PAGE) {
        // a frame the core keeps out of the pages: memory of its own (outside the heap a run
        // imitates the Wii's with, as the Wii's comes from its pool), set aside on first use
        static uint8_t *spare;
        static uint32_t spare_pages;
        if (spare == NULL) {
            spare_pages = g_ram >> WL_XB_PAGE_SHIFT;
            void *m = mmap(NULL, g_ram, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m == MAP_FAILED) {
                return NULL;
            }
            spare = m;
        }
        return f >= 1u && f <= spare_pages ? spare + ((size_t)(f - 1u) << WL_XB_PAGE_SHIFT) : NULL;
    }
    (void)f;
    return g_door + ((size_t)p << WL_XB_PAGE_SHIFT);
}

void wl_xb_mmu_frame_zero(uint32_t p, uint32_t f) {
    (void)p;
    (void)f; // a page without a frame is a hole of the object (punched when its frame went): zero
}

static void punch(uint32_t p, uint32_t pages) {
    off_t off = (off_t)p << WL_XB_PAGE_SHIFT;
    off_t len = (off_t)pages << WL_XB_PAGE_SHIFT;
#if defined(FALLOC_FL_PUNCH_HOLE)
    if (g_punch && fallocate(g_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, len) == 0) {
        return;
    }
    g_punch = false;
#endif
    memset((g_door != NULL ? g_door : g_base + WL_XB_EA_WINDOW) + off, 0, (size_t)len);
}

void wl_xb_mmu_frame_drop(uint32_t p, uint32_t f) {
    (void)f;
    punch(p, 1);
}

void wl_xb_mmu_frame_move(uint32_t from, uint32_t to, uint32_t f) {
    (void)f;
    memcpy(g_door + ((size_t)to << WL_XB_PAGE_SHIFT), g_door + ((size_t)from << WL_XB_PAGE_SHIFT), WL_XB_PAGE);
    punch(from, 1);
}

void wl_xb_mmu_census_close(uint32_t p) {
    if ((g_xb_pg_flags[p] & WL_XB_PG_PINNED) == 0) {
        set_prot(p, 0, PROT_NONE, false);
        set_prot(p, 1, PROT_NONE, false);
    }
}

void wl_xb_mmu_pinned(uint32_t p) {
    set_prot(p, 0, PROT_READ | PROT_WRITE, false); // never closed again: the service thread writes
    set_prot(p, 1, PROT_READ | PROT_WRITE, false); // KeTickCount without the guest lock
}

uint32_t wl_xb_mmu_lock(void) {
    return 0;
}

void wl_xb_mmu_unlock(uint32_t level) {
    (void)level;
}

static uint32_t phys_of(uint32_t ea) {
    if (ea < WL_XB_EA_WINDOW) {
        return g_xb_va2phys[ea >> WL_XB_PAGE_SHIFT];
    }
    uint32_t off = ea - WL_XB_EA_WINDOW;
    return off < g_ram ? (off >> WL_XB_PAGE_SHIFT) + 1u : 0u;
}

WlResult wl_xb_backend_page_fault(uint32_t ea) {
    uint32_t p1 = g_paging ? phys_of(ea) : 0u;
    if (p1 == 0) {
        return WL_ERR_UNSUPPORTED; // not paged guest RAM: the fault handler's other cases
    }
    uint32_t p = p1 - 1u;
    uint32_t a = ea >= WL_XB_EA_WINDOW ? 1u : 0u;
    if ((g_xb_pg_flags[p] & WL_XB_PG_IO) != 0) {
        // read back by a thread that gave the lock up: waited for, then the access runs again
        return wl_xb_lock_held() ? wl_xb_pg_touch(p, 0) : WL_ERR_DENIED;
    }
    if (a == 0) {
        g_user_va[p] = ea & ~(WL_XB_PAGE - 1u); // (what map_user recorded; kept right whatever happened)
    }
    if (g_xb_pg_frame[p] == 0) {
        if (!wl_xb_lock_held()) {
            return WL_ERR_DENIED; // only the lock's holder may change the tables (no mmu lock here)
        }
        // as the Wii's DSI does: a zero page first through the exception-safe path (a free frame or
        // a clean page's), the rest through the slow one
        uint32_t f = g_xb_pg_slot[p] == 0 ? wl_xb_pg_frame_fast(p, 0) : 0u;
        if (f != 0) {
            wl_xb_mmu_frame_zero(p, f);
        } else {
            // (a read back gives the lock up: the tables may have changed, so the access runs again
            // and opens the page through a fault of its own)
            return wl_xb_pg_touch(p, 0);
        }
    }
    uint8_t fl = g_xb_pg_flags[p];
    uint8_t cur = g_prot[a][p];
    bool write = cur == PROT_READ; // open for reading, so this was a write: the C bit
    if (write) {
        fl |= WL_XB_PG_DIRTY;
        wl_xb_pg_stored(p);
    }
    // (a watched page opens for writing only on a write, so that each write after its watch was
    // armed is seen)
    bool rw = (fl & WL_XB_PG_PINNED) != 0 || write || ((fl & WL_XB_PG_DIRTY) != 0 && (fl & WL_XB_PG_WATCH) == 0);
    g_xb_pg_flags[p] = (uint8_t)(fl | WL_XB_PG_REF);
    wl_xb_census_seen(p, write);
    // (already open as wanted: the mapping was replaced behind the table's back; set it again)
    set_prot(p, a, rw ? PROT_READ | PROT_WRITE : PROT_READ, cur == (rw ? (PROT_READ | PROT_WRITE) : PROT_READ));
    g_faults++;
    return WL_OK;
}

// ---- Backend operations ----------------------------------------------------------------------------

void wl_xb_backend_map_user(uint32_t va, uint32_t phys, uint32_t pages) {
    void *at = mmap(g_base + va, (size_t)pages << WL_XB_PAGE_SHIFT, g_paging ? PROT_NONE : PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, g_fd, (off_t)phys << WL_XB_PAGE_SHIFT);
    if (at == MAP_FAILED) {
        WL_LOG_E(TAG, "map of user 0x%08x (%u pages) failed: %s", (unsigned)va, (unsigned)pages, strerror(errno));
        abort();
    }
    if (g_paging) {
        for (uint32_t k = 0; k < pages; k++) {
            g_user_va[phys + k] = va + (k << WL_XB_PAGE_SHIFT);
            g_prot[0][phys + k] = PROT_NONE;
            if ((g_xb_pg_flags[phys + k] & WL_XB_PG_PINNED) != 0) {
                set_prot(phys + k, 0, PROT_READ | PROT_WRITE, true);
            }
        }
    }
}

void wl_xb_backend_unmap_user(uint32_t va, uint32_t pages) {
    if (g_paging) {
        for (uint32_t k = 0; k < pages; k++) {
            uint32_t p1 = g_xb_va2phys[(va >> WL_XB_PAGE_SHIFT) + k];
            if (p1 != 0) {
                g_user_va[p1 - 1u] = 0;
                g_prot[0][p1 - 1u] = PROT_NONE;
            }
        }
    }
    void *at = mmap(g_base + va, (size_t)pages << WL_XB_PAGE_SHIFT, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (at == MAP_FAILED) {
        WL_LOG_E(TAG, "unmap of user 0x%08x failed: %s", (unsigned)va, strerror(errno));
        abort();
    }
}

void wl_xb_backend_release_phys(uint32_t phys, uint32_t pages) {
    if (g_paging) {
        wl_xb_pg_release(phys, pages); // unmapped, frames and slots back (each frame punched)
        return;
    }
    punch(phys, pages);
}

void wl_xb_backend_move_phys(uint32_t from, uint32_t to, uint32_t va) {
    if (g_paging) {
        wl_xb_pg_move(from, to); // the bytes follow the frame (wl_xb_mmu_frame_move)
        g_user_va[from] = 0;
        wl_xb_backend_map_user(va, to, 1);
        return;
    }
    memcpy(g_base + WL_XB_EA_WINDOW + ((size_t)to << WL_XB_PAGE_SHIFT),
           g_base + WL_XB_EA_WINDOW + ((size_t)from << WL_XB_PAGE_SHIFT), WL_XB_PAGE);
    wl_xb_backend_map_user(va, to, 1);
    wl_xb_backend_release_phys(from, 1);
}

void wl_xb_backend_stats(uint32_t *frames, uint32_t *faults, uint32_t *pte_evictions) {
    // resident pages of the RAM object: what the window maps that has been touched
    uint32_t resident = 0;
#if defined(__linux__)
    if (!g_paging) {
        uint32_t pages = g_ram >> WL_XB_PAGE_SHIFT;
        unsigned char *vec = malloc(pages);
        if (vec != NULL && mincore(g_base + WL_XB_EA_WINDOW, g_ram, vec) == 0) {
            for (uint32_t i = 0; i < pages; i++) {
                resident += vec[i] & 1u;
            }
        }
        free(vec);
    }
#endif
    *frames = resident; // (with paging the core counts them: mem.c)
    *faults = g_faults;
    *pte_evictions = 0;
}

void wl_xb_backend_host_bytes(size_t *tables, size_t *frames) {
    size_t pages = g_ram >> WL_XB_PAGE_SHIFT;
    *tables = g_user_va != NULL ? pages * (2u + sizeof *g_user_va) : 0u;
    *frames = 0; // (the RAM object is mapped, not had from the heap)
}
