// SPDX-License-Identifier: MIT
// Between the Wii backend's files (not part of the platform API).
#pragma once

// Called by the video backend at every retrace, in the interrupt, once input has started: reads the
// GameCube pads (input_wii.c) so that a press shorter than a frame is seen. Does nothing before
// wl_platform_input_init.
void wl_input_wii_retrace(void);
