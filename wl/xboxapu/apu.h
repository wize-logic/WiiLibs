// SPDX-License-Identifier: MIT
// The original Xbox's audio processing unit (the MCPX APU at 0xFE800000) as a device model: the
// registers, the front end's methods, the voice processor that plays the voices DirectSound sets
// up in guest memory into 32 mixbins at 48 kHz, its interrupts, and a stereo mix of the mixbins
// in place of the GP and EP DSPs' programs. Backend independent: guest memory through a callback,
// no platform calls, the same output on little- and big-endian hosts. See wl/xboxapu/README.md.
//
// A clean implementation: register offsets, method numbers and field layouts are hardware facts
// from public descriptions (xboxdevwiki's APU page, XboxDev's xboxpy register list) and from the
// traffic of a title's own DirectSound, cited in apu_regs.h.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XBOX_APU_SIZE 0x80000u // the register window
#define WL_XBOX_APU_RATE 48000u   // samples a second
#define WL_XBOX_APU_FRAME 32u     // samples a frame (1500 frames a second)
#define WL_XBOX_APU_VOICES 256u
#define WL_XBOX_APU_BINS 32u

typedef struct WlXboxApu WlXboxApu;

typedef struct {
    // The host address of `bytes` bytes of guest-physical memory at `phys` (little-endian bytes,
    // contiguous), or NULL outside guest RAM. Called with whatever lock the caller holds around
    // wl_xbox_apu_write / wl_xbox_apu_run.
    uint8_t *(*phys_ptr)(void *user, uint32_t phys, uint32_t bytes);
    void *user;
    // The GP DSP's command word that DirectSound's effects program clears when it has taken a
    // command: a byte offset into the GP's scratch memory (GPSADDR's pages). The model clears it
    // once a frame while the GP runs, as the program would. 0x810 for the XDK's DirectSound (the
    // command block at scratch 0x800); 0 turns it off.
    uint32_t gp_command_word;
    // The stereo mix of the mixbins: Q15 weights per bin (32768 = 1.0, the most) for the left and
    // the right output; bins with 0 in both are not read. wl_xbox_apu_config_default fills
    // DirectSound's speaker bins: front L/R 1.0, centre 0.707 to both, LFE 0.5 to both, rear L/R
    // 0.707, the four crosstalk-cancelled 3D bins (6..9) to their side. The effect sends (10..31)
    // are left out.
    uint16_t mix_left[WL_XBOX_APU_BINS];
    uint16_t mix_right[WL_XBOX_APU_BINS];
    // Called after each frame wl_xbox_apu_run makes, with its 32 mixbins (bin * 32 + sample) and
    // its stereo output (32 interleaved pairs; NULL when the run drops them): checksums, captures.
    // NULL: none.
    void (*on_frame)(void *user, const int32_t *bins, const int16_t *out);
    void *on_frame_user;
    // The voice processor's and the output mix's first, plainer code paths (the same output bit
    // for bit; slower): for an A/B of the faster ones. false by default.
    bool plain;
} WlXboxApuConfig;

void wl_xbox_apu_config_default(WlXboxApuConfig *cfg);

// NULL when out of memory (about 160 KB: the voices' decoder state, the frame's mix).
WlXboxApu *wl_xbox_apu_create(const WlXboxApuConfig *cfg);
void wl_xbox_apu_destroy(WlXboxApu *apu);

// The register window, offsets from 0xFE800000; sizes 1, 2 or 4. A write to the front end's
// method area (0x20000..0x2FFFF) runs the method at once.
uint32_t wl_xbox_apu_read(WlXboxApu *apu, uint32_t offset, unsigned size);
void wl_xbox_apu_write(WlXboxApu *apu, uint32_t offset, uint32_t value, unsigned size);

// The interrupt line (the guest's vector 5): an enabled status bit is set.
bool wl_xbox_apu_irq(const WlXboxApu *apu);
// The voice processor runs: the sample counter is on (SECTL) and the front end runs freely
// (FECTL neither halted nor trapped).
bool wl_xbox_apu_running(const WlXboxApu *apu);

// Runs up to `frames` frames and writes 32 stereo samples a frame to `out` (interleaved, host
// order; NULL drops them). While the voice processor does not run, a frame is silence. Returns
// the frames done: fewer when the front end traps in the middle, or when a stream played out a
// segment list (its notifier and FE_VOICE are up: the guest's interrupt handler answers, and
// gives the stream its next list, before the next frame), so the caller runs the rest after the
// interrupt.
uint32_t wl_xbox_apu_run(WlXboxApu *apu, uint32_t frames, int16_t *out);

// The 32 mixbins of the last frame (bin * 32 + sample, 16-bit scale), for tests.
const int32_t *wl_xbox_apu_mixbins(const WlXboxApu *apu);

typedef struct {
    uint64_t frames;        // frames run
    uint64_t frames_silent; // of which the voice processor did not run
    uint64_t voice_frames;  // voices processed, summed over frames
    uint32_t voices_max;    // most voices in one frame
    uint32_t methods;       // front end methods
    uint32_t voice_ons, voice_offs, voices_ended;
    uint32_t notifies;       // notifier writes
    uint32_t traps;          // idle-voice traps
    uint32_t gp_commands;    // GP command words cleared
    uint32_t stream_starved; // stream frames with no data
    uint32_t bad_memory;     // guest-physical reads outside RAM (each skipped)
    uint32_t clipped;        // output samples clipped
    uint32_t list_loops;     // voice lists cut after 256 voices
    // the voice the last idle-voice trap named, and the traps in a row that named it (the guest's
    // handler answered without taking it out of its list)
    uint32_t trap_voice, trap_repeats;
} WlXboxApuStats;
void wl_xbox_apu_stats(const WlXboxApu *apu, WlXboxApuStats *out);

#ifdef __cplusplus
}
#endif
