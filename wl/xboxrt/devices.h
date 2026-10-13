// SPDX-License-Identifier: MIT
// The Xbox's devices for a title that has to get through its XDK libraries' initialisation without
// sound, network or controllers yet: each answers as idle or absent hardware would, so the init
// loops end (and DirectSoundCreate, XNetStartup, the controller enumeration fail the way a title
// tolerates). And the GPU's place: the NV2A device and its vertical blank interrupt, wired to a
// model through WlXbGpuOps (wl/xbox's NV2A, or the idle stub here).
//
// Register numbers are from public descriptions of the hardware: the OHCI 1.0a specification
// (USB), Intel's AC'97 2.3 bus-master interface (the MCPX ACI follows it), envytools' NV register
// database (nv_mmio, nv03-nv50 PMC/PBUS/PFIFO/PTIMER/PFB/PGRAPH/PCRTC) for the NV2A.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XB_NV2A_BASE 0xFD000000u
#define WL_XB_NV2A_SIZE 0x01000000u
#define WL_XB_APU_BASE 0xFE800000u
#define WL_XB_APU_SIZE 0x00080000u
#define WL_XB_AC97_BASE 0xFEC00000u
#define WL_XB_AC97_SIZE 0x00001000u
#define WL_XB_USB0_BASE 0xFED00000u
#define WL_XB_USB1_BASE 0xFED08000u
#define WL_XB_USB_SIZE 0x00001000u
#define WL_XB_NIC_BASE 0xFEF00000u
#define WL_XB_NIC_SIZE 0x00000400u
#define WL_XB_FLASH_BASE 0xFF000000u
#define WL_XB_FLASH_SIZE 0x01000000u

// Registers the MCPX's idle devices: APU (registers kept, its sample counter running), AC'97 (no
// codec ready: DirectSoundCreate gives up; the bus master's reset bits clear themselves), two
// OHCI controllers (reset completes, frame number runs, ports powered with nothing plugged in),
// the NIC (registers kept, nothing happens) and the flash (reads zero). Each logs the first touch
// of a register it does not model, once.
WlResult wl_xb_devices_register_mcpx(void);

// ---- The GPU -----------------------------------------------------------------------------------

// A GPU model behind 0xFD000000 (offsets from it), its vertical blank, and its interrupt line.
typedef struct {
    uint32_t (*mmio_read)(void *gpu, uint32_t offset, unsigned size);
    void (*mmio_write)(void *gpu, uint32_t offset, uint32_t value, unsigned size);
    void (*vblank)(void *gpu);             // at each host vertical blank (lock held)
    bool (*irq_pending)(const void *gpu);  // the GPU's interrupt line (WL_XB_IRQ_GPU)
} WlXbGpuOps;

// Registers the device at 0xFD000000, the service's vblank hook and the level source of line 3.
WlResult wl_xb_gpu_attach(const WlXbGpuOps *ops, void *gpu);

// The idle stub: no drawing. A PUT write is consumed at once (GET = PUT on the channel, and on
// PFIFO's CACHE1 mirror), PFB's write-back flush bit clears itself, the FIFO and graph report idle
// and empty, PTIMER runs in nanoseconds, the framebuffer is 64 MB, interrupt status registers are
// write-one-to-clear, and each vblank sets PCRTC's interrupt (line 3 asserted while PCRTC and PMC
// enable it). Every other register keeps what was written.
typedef struct WlXbGpuStub WlXbGpuStub;
WlXbGpuStub *wl_xb_gpu_stub_create(void);
const WlXbGpuOps *wl_xb_gpu_stub_ops(void);
typedef struct {
    uint32_t puts;          // DMA_PUT writes
    uint32_t last_put;
    uint32_t flips;         // PCRTC_START writes
    uint32_t last_start;
    uint32_t vblanks;
    uint32_t irq_acks;      // PCRTC interrupt cleared by the guest
} WlXbGpuStubStats;
void wl_xb_gpu_stub_stats(const WlXbGpuStub *gpu, WlXbGpuStubStats *out);

// wl/xbox's NV2A model behind the same interface (nv2a_adapter.c, built when wl/xbox has it):
// creates the model with phys_ptr = the guest's window and attaches it. WL_ERR_UNSUPPORTED in a
// build without it.
WlResult wl_xb_gpu_attach_nv2a(void);
// Captures `frames` frames of the GPU's work (push buffers and the guest memory they read;
// wl/xbox/nv2a_capture.h) into `path`, starting `after_vblanks` vertical blanks from now, for
// tools/xbox_nv2a_replay. On the host the WL_XB_CAPTURE environment variable sets it too:
// "path[,frames[,after vblanks]]". Nothing when wl/xbox's NV2A model is not built in.
void wl_xb_gpu_capture(const char *path, uint32_t frames, uint32_t after_vblanks);

#ifdef __cplusplus
}
#endif
