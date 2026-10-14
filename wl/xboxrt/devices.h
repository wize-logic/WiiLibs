// SPDX-License-Identifier: MIT
// The Xbox's devices for a title that has to get through its XDK libraries' initialisation without
// sound or network yet: each answers as idle or absent hardware would, so the init loops end (and
// DirectSoundCreate and XNetStartup fail the way a title tolerates); the USB controllers and the
// gamepads behind them are modelled (usb.h). And the GPU's place: the NV2A device and its vertical
// blank interrupt, wired to a model through WlXbGpuOps (wl/xbox's NV2A, or the idle stub here).
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

// Registers the MCPX's devices: APU (registers kept, its sample counter running), AC'97 (no
// codec ready: DirectSoundCreate gives up; the bus master's reset bits clear themselves), the two
// OHCI controllers with the gamepads (usb.h, with `usb`'s settings; NULL: the defaults), the NIC
// (registers kept, nothing happens) and the flash (reads zero). The idle ones log the first touch
// of a register they do not model, once.
struct WlXbUsbConfig;
WlResult wl_xb_devices_register_mcpx(void);
WlResult wl_xb_devices_register_mcpx_usb(const struct WlXbUsbConfig *usb);

// ---- The audio hardware ------------------------------------------------------------------------

// wl/xboxapu's APU and AC'97 models in place of the idle ones (audio.c, built when wl/xboxapu is):
// the devices at 0xFE800000 and 0xFEC00000, their interrupt lines (5 and 6), a service tick that
// runs the APU's frames and moves the AC'97's bus masters, and the output: the platform's audio
// at 48 kHz (the Wii), a WAV file (the host). The host reads its settings from the environment:
// WL_XB_AUDIO_WAV=<file> writes everything played, WL_XB_AUDIO_TRACE=<file> logs every APU and
// AC'97 access with the translated function that made it, WL_XB_AUDIO=0 keeps the idle devices.
typedef struct {
    uint32_t tick_us;         // how often the frames run: 4000 (6 frames)
    uint32_t latency_frames;  // frames queued ahead of the platform output: 48 (32 ms)
    bool output;              // start the platform's audio output (true)
    bool off;                 // keep the idle devices: DirectSoundCreate fails, the title is silent
    const char *wav_path;     // the host: a WAV of the output (NULL: none)
    const char *trace_path;   // the host: the MMIO trace (NULL: none)
} WlXbAudioConfig;
void wl_xb_audio_config_default(WlXbAudioConfig *cfg);
// Call after wl_xb_devices_register_mcpx. WL_ERR_UNSUPPORTED in a build without wl/xboxapu.
WlResult wl_xb_audio_attach(const WlXbAudioConfig *cfg);
// One line: frames run, voices a frame, the time a frame costs, notifiers, traps, underruns (also
// logged every 10 s while the title runs).
void wl_xb_audio_log_status(void);

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
// creates the model with phys_ptr = the guest's window and attaches it, its converted textures
// kept in `texture_arena` (`texture_bytes`, 32-byte aligned; NULL: the heap, within
// `texture_bytes` or 8 MB) and checked through the write watch (mem.h). WL_ERR_UNSUPPORTED in a
// build without it.
WlResult wl_xb_gpu_attach_nv2a(void *texture_arena, uint32_t texture_bytes);
// Captures `frames` frames of the GPU's work (push buffers and the guest memory they read;
// wl/xbox/nv2a_capture.h) into `path`, starting `after_vblanks` vertical blanks from now, for
// tools/xbox_nv2a_replay. On the host the WL_XB_CAPTURE environment variable sets it too:
// "path[,frames[,after vblanks]]". Nothing when wl/xbox's NV2A model is not built in.
void wl_xb_gpu_capture(const char *path, uint32_t frames, uint32_t after_vblanks);
// The vertex programs the GPU decodes that the title does not have compiled ahead of time are
// appended to the file `path` (the boot code's: <hard disk folder>/vsh_programs.inc), as
// tools/xbox_vsh_codegen.c takes them, each once: the title's build compiles the list into C
// (wl_add_xbox_title, the title config's "vertex_programs"), which the runtime then runs in place
// of the interpreter. NULL or "": no list. Call it before wl_xb_gpu_attach_nv2a. Nothing when
// wl/xbox's NV2A model is not built in.
void wl_xb_gpu_set_program_list(const char *path);

#ifdef __cplusplus
}
#endif
