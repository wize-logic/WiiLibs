// SPDX-License-Identifier: MIT
// Booting a recompiled Xbox title: platform, log, mounts, guest memory, the XBE from the title's
// folder, the kernel (wl/xboxkrnl), devices and the GPU, the service thread, and the entry point on
// a guest thread of its own; then it waits for the title to leave (HalReturnToFirmware, or its last
// thread ending) and returns the code. A title's main fills a WlXbTitleConfig and calls
// wl_xb_boot_run; wl_xboxrt_main is a main that does only that (a host path argument; on the Wii
// sd:/xbox/<title>/default.xbe).
#pragma once

#include "wl/xboxkrnl/xboxkrnl.h"
#include "wl/xboxrt/thunk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title_dir;       // the title's folder (D:): a wl/platform path ("sd:/xbox/halo")
    const char *xbe_name;        // "default.xbe"
    const char *data_dir;        // the hard disk's folder (writable); NULL: <title_dir>/hdd
    const char *log_dir;         // the Wii's log folder on SD; NULL: <title_dir>/logs ("" for none)
    const WlXkMount *mounts;     // more devices (they win over the default ones)
    uint32_t mount_count;
    uint32_t ram_mb;             // guest RAM: 64 (retail) or 128
    bool inline_first_thread;    // see WlXkConfig
    bool async_io_pending;       // see WlXkConfig
    const uint16_t *ordinal_remap; // a title built with another XDK: ordinal -> canonical (0: same)
    uint32_t ordinal_remap_count;
    bool thunk_look_behind;      // resolve a run of thunks just below the declared table
    WlXbManualLookup manual_lookup; // the title's hand-written functions (recomp_lookup_manual)
    bool gpu_stub;               // the idle NV2A even when wl/xbox's model is built in
    uint32_t frame_pool_bytes;   // the Wii: host memory for guest frames (0: as much as can be had,
                                 // leaving `keep_free_bytes`)
    uint32_t keep_free_bytes;    // the Wii: 14 MB by default
    uint32_t htab_bytes;         // the Wii's page table: 256 KB by default
    bool trace_kernel_calls;     // log every kernel call
    const char *play;            // the play harness's setting (play.h: "script:<file>", "bot:<seed>",
                                 // "look:<seed>"); NULL: none, or on the Wii the first line of
                                 // <title_dir>/play.cfg when there is one
    const char *play_out;        // where its screenshots go (NULL: beside the script)
    void (*kernel_config)(WlXkConfig *cfg, void *user); // edits the kernel's settings before init
    void (*before_entry)(void *user); // after the image is loaded and the kernel is up
    void *user;
} WlXbTitleConfig;

void wl_xb_title_config_default(WlXbTitleConfig *cfg);

// wl_xboxrt_main calls this before the boot with the configuration it made: a weak default that
// does nothing, which a title project's own source replaces to change the configuration and to
// register what it tells the play harness (play.h).
void wl_xb_title_setup(WlXbTitleConfig *cfg);

// Runs the title; returns its exit code (HalReturnToFirmware's routine, or 0), or a negative
// WlResult when it could not start (a missing XBE, no memory).
int wl_xb_boot_run(const WlXbTitleConfig *cfg);

#ifdef __cplusplus
}
#endif
