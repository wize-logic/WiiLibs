// SPDX-License-Identifier: MIT
// What gx_wii.c gives the other GX files of this directory (gx_target_wii.c). Not for applications.
#pragma once

#include <stdbool.h>

// The copy filter for EFB copies to textures: off (the display copy's deflicker filter blurs them),
// or the video mode's own for the copy to the XFB. Writes GX_SetCopyFilter.
void wl_gx_wii_copy_filter(bool display);
// GX commands were submitted behind the shadow's back (wl_gx_peek_rgba then syncs first).
void wl_gx_wii_note_commands(void);
