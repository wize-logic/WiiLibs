// SPDX-License-Identifier: MIT
// The MCPX's AC'97 controller (ACI, 0xFEC00000) as a device model: the primary codec's mixer
// registers, the global control and status, the codec access semaphore, and the PCM-out and
// S/PDIF-out bus masters running through their buffer descriptor lists at 48 kHz, with their
// completion interrupts (the guest's vector 6). The audio itself is not read: the APU model's
// stereo mix is what plays (apu.h). Register layout from Intel's AC'97 2.3 bus master programming
// interface (the ICH's NAMBAR/NABMBAR registers), which the MCPX follows, plus the S/PDIF bus
// master at 0x170 that DirectSound programs the same way.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XBOX_AC97_SIZE 0x1000u

typedef struct WlXboxAc97 WlXboxAc97;

typedef struct {
    // As WlXboxApuConfig.phys_ptr: the buffer descriptor lists live in guest-physical memory.
    uint8_t *(*phys_ptr)(void *user, uint32_t phys, uint32_t bytes);
    void *user;
    bool codec_ready; // GLOB_STA's primary codec ready bit (false: DirectSound gives up)
} WlXboxAc97Config;

WlXboxAc97 *wl_xbox_ac97_create(const WlXboxAc97Config *cfg);
void wl_xbox_ac97_destroy(WlXboxAc97 *ac);

uint32_t wl_xbox_ac97_read(WlXboxAc97 *ac, uint32_t offset, unsigned size);
void wl_xbox_ac97_write(WlXboxAc97 *ac, uint32_t offset, uint32_t value, unsigned size);

// Moves the running bus masters on by `frames` stereo frames at 48 kHz.
void wl_xbox_ac97_advance(WlXboxAc97 *ac, uint32_t frames);
// The interrupt line: a bus master's enabled completion status is set.
bool wl_xbox_ac97_irq(const WlXboxAc97 *ac);

typedef struct {
    uint32_t buffers_done[2]; // PCM out, S/PDIF out
    uint32_t interrupts;
} WlXboxAc97Stats;
void wl_xbox_ac97_stats(const WlXboxAc97 *ac, WlXboxAc97Stats *out);

#ifdef __cplusplus
}
#endif
