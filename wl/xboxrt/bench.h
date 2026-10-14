// SPDX-License-Identifier: MIT
// The capture bench: a GPU capture (wl/xbox/nv2a_capture.h: what the title gave the NV2A for a
// few frames, written by the play harness's "capture" step, WL_XB_CAPTURE on the host or
// wl_xb_gpu_capture) replayed, several times, through the runtime's own GPU: a fresh NV2A model
// for each run over the guest memory the title runs in (the Wii's page table, paged, with its
// write watch), drawing through the GX backend the title draws with (the console's GX, its frames
// shown; the host's recorder elsewhere), with the textures it converted kept from one run to the
// next as they are from one frame to the next. Exact console numbers for a chosen spot of a
// title, in one short run.
//
// The boot code (boot.h) runs it in place of the title when the play setting (WlXbTitleConfig's
// play, WL_XB_PLAY on the host, the first line of <title folder>/play.cfg on the Wii) is
//     replay:<file> [runs]      the capture (relative to the title folder, or a platform path),
//                               replayed `runs` times (5 unless given)
// The boot sets up guest memory and the GPU as for the title, but loads no XBE, starts no kernel,
// devices, service thread or entry point; the bench streams the capture from the file (it is not
// held in memory: a capture of a few frames is tens of MB) and leaves with an exit code (the Wii's
// main then powers off where no loader started it).
//
// Every line it logs is tagged "xbbench":
//     run 1 frame 0: 512.3 ms (draws 401.2, present 70.1; 1465 draws)
//         each frame (each FLIP_INCREMENT_WRITE): the GPU's part of it, without reading the
//         capture and putting its memory in
//     run 1: 2 frames, 2950 draws, 498.0 ms a frame (pusher 40.1, draws 380.3, clears 2.1,
//            present 75.5; besides: memory 20.2, reading 110.3 ms a frame)
//         each run: the GPU's part a frame, and its phases: the model (the push buffers' methods
//         and its own reads), the backend's draws, clears and presents (a present waits for the
//         console's GPU to finish the frame); memory: the capture's guest memory compared and
//         written; reading: the file
//     summary: 5 runs of 2 frames: best 480.1 ms a frame, mean 495.3 ms a frame (...)
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

// Whether a play setting is the bench's ("replay:...").
bool wl_xb_bench_setting(const char *play);

// Runs the bench (the boot code calls it with guest memory up, the GPU attached and the guest lock
// held): 0 when every run replayed, else a negative WlResult (the capture missing or not one, no
// GPU). `base_dir` is where a relative capture path is looked for.
int wl_xb_bench_run(const char *setting, const char *base_dir);

#ifdef __cplusplus
}
#endif
