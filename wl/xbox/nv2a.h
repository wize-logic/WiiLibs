// SPDX-License-Identifier: MIT
// The Xbox's GPU, the NV2A, as a device (nv2a.c): a title's own Direct3D runs unchanged, writes NV2A
// push buffers into guest RAM and kicks them through MMIO; this model answers the MMIO window
// (0xFD000000, 16 MiB) as an idle GPU would, executes each kicked push buffer synchronously (the
// DMA pusher's jumps, calls and returns, the puller's object binding and semaphores, the Kelvin 3D
// class's methods through the state tracker, nv2a_kelvin.h), writes the semaphores, notifiers and
// reports the title waits on into guest memory, latches the interrupts its ISR reads, and hands
// the draws, clears and flips to a backend: GX on the Wii (nv2a_gx.h), a recorder or a software
// rasterizer on the host (nv2a_record.h, nv2a_raster.h).
//
// Pure C11, no platform calls: guest memory is reached only through the caller's phys_ptr, which
// gives host pointers to guest-physical bytes (little-endian, contiguous for the length asked).
// Every read the model and its backends make of guest memory goes through wl_nv2a_phys, so a
// capture (nv2a_capture.h) sees all of it. One thread at a time (the guest lock).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/base/base.h"

typedef struct WlNv2a WlNv2a;
typedef struct WlNv2aKelvin WlNv2aKelvin;
typedef struct WlNv2aDraw WlNv2aDraw;

// What a backend is asked to do. Every call gets the state tracker's state as it is at that
// moment (nv2a_kelvin.h); a backend reads what it needs of it and keeps nothing it points at.
typedef struct WlNv2aBackend {
    void *user;
    // CLEAR_SURFACE: the colour and/or depth/stencil of the surface set up now, within the clear
    // rectangle (WL_NV2A_CLEAR_* flags; the values are in the state)
    void (*clear)(void *user, const WlNv2aKelvin *k, uint32_t flags);
    // a batch of vertices (a begin/end, or a part of a long one): returns the pixels it covered as
    // far as the backend knows (for the pixel-count reports while they are counted; 0: unknown)
    uint32_t (*draw)(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw);
    // FLIP_INCREMENT_WRITE: the title finished drawing the surface it was drawing into (its
    // back buffer, as the colour offset says)
    void (*frame_done)(void *user, const WlNv2aKelvin *k);
    // PCRTC_START written: the display shows the surface at this guest-physical address from the
    // next retrace (the vblank ISR's flip)
    void (*flip)(void *user, const WlNv2aKelvin *k, uint32_t phys);
} WlNv2aBackend;

// The host pointer of `bytes` guest-physical bytes at `phys` (little-endian), or NULL when they
// are not all guest RAM. The pointer stays valid while the device runs.
typedef void *(*WlNv2aPhysPtr)(void *user, uint32_t phys, uint32_t bytes);

typedef struct WlNv2aConfig {
    WlNv2aPhysPtr phys_ptr;
    void *user;
    // NULL: none (the state is tracked, nothing is drawn); copied
    const WlNv2aBackend *backend;
    // optional: the RAM's size (0: 64 MiB; a power of two), which PFB reports and whose last
    // megabyte is the instance memory PRAMIN shows
    uint32_t ram_bytes;
    // optional: the time PTIMER counts, in nanoseconds (NULL: a microsecond each time it is read)
    uint64_t (*time_ns)(void *user);
} WlNv2aConfig;

// A device, from the heap (it holds the push buffer decoder's batches: about 200 KiB). NULL when
// the configuration has no phys_ptr or there is no memory.
WlNv2a *wl_nv2a_create(const WlNv2aConfig *cfg);
void wl_nv2a_destroy(WlNv2a *dev);

// MMIO: `offset` from 0xFD000000, `size` 1, 2 or 4 bytes (little-endian lanes of the 32-bit
// registers). A write of a channel's DMA_PUT (0x800040 + chid * 0x10000) runs its push buffer from
// GET to PUT before it returns, then GET = PUT.
uint32_t wl_nv2a_mmio_read(WlNv2a *dev, uint32_t offset, unsigned size);
void wl_nv2a_mmio_write(WlNv2a *dev, uint32_t offset, uint32_t value, unsigned size);
// The host's vertical blank: PCRTC's interrupt (if the title enabled it), the frame counters.
void wl_nv2a_vblank(WlNv2a *dev);
// An interrupt is pending and enabled (PMC_INTR_0 with PMC_INTR_EN_0): the runtime runs the
// title's ISR, which reads and clears the status registers.
bool wl_nv2a_irq_pending(const WlNv2a *dev);

// ---- for the backends and tools ------------------------------------------------------------
// Guest-physical memory, through the configuration's phys_ptr (and the capture, when one runs):
// `phys` is masked into RAM as the GPU's address lines do.
void *wl_nv2a_phys(WlNv2a *dev, uint32_t phys, uint32_t bytes);
// The state tracker's state.
const WlNv2aKelvin *wl_nv2a_kelvin(const WlNv2a *dev);
// One method as the puller would hand it to the object bound to `subchannel` (methods below 0x100
// are the puller's own; handles of methods 0x180-0x1FC are looked up first). For tests and tools.
void wl_nv2a_method(WlNv2a *dev, unsigned subchannel, uint32_t method, uint32_t param);
// Binds an object of `class` to a subchannel without RAMHT (tests; a capture's replay).
void wl_nv2a_bind_class(WlNv2a *dev, unsigned subchannel, uint32_t class_id);
// The backend from now on (NULL: none).
void wl_nv2a_set_backend(WlNv2a *dev, const WlNv2aBackend *backend);

typedef struct WlNv2aStats {
    uint32_t kicks;           // DMA_PUT writes that ran a push buffer
    uint32_t words;           // push buffer words read
    uint32_t methods;         // methods handed to objects
    uint32_t jumps, calls, returns;
    uint32_t pusher_errors;   // invalid commands, a call in a call, a return outside one, bad memory
    uint32_t unhandled;       // methods the state tracker does not handle (logged once each)
    uint32_t draws, vertices, clears;
    uint32_t flips;           // PCRTC_START writes
    uint32_t frames;          // FLIP_INCREMENT_WRITE
    uint32_t vblanks;
    uint32_t semaphores;      // semaphore releases written
    uint32_t acquires_unmet;  // acquires whose value was not there (a synchronous GPU cannot wait)
    uint32_t notifies;        // notifiers written
    uint32_t reports;         // reports written
    uint32_t stencil_draws;   // draws with the stencil test or writes on, which GX cannot do
    uint32_t fixed_function_draws; // draws in the fixed-function transform mode
    uint32_t memory_misses;   // guest memory the model wanted that phys_ptr did not give
} WlNv2aStats;
void wl_nv2a_stats(const WlNv2a *dev, WlNv2aStats *out);
