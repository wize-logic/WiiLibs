// SPDX-License-Identifier: MIT
// What gx_wii.c gives the other GX files of this directory (gx_target_wii.c). Not for applications.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The copy filter for EFB copies to textures: off (the display copy's deflicker filter blurs them),
// or the video mode's own for the copy to the XFB. Writes GX_SetCopyFilter.
void wl_gx_wii_copy_filter(bool display);
// GX commands were submitted behind the shadow's back (wl_gx_peek_rgba then syncs first).
void wl_gx_wii_note_commands(void);
// a texture copy out of the EFB made (WlGxStats.efb_copies)
void wl_gx_wii_note_efb_copy(void);
// Texture maps loaded behind gx_direct_wii.c's back (bit n: GX_TEXMAPn): what they hold is unknown
// to it, so its next wl_gx_direct_texture on them loads.
void wl_gx_wii_texmaps_forget(uint32_t maps);
// Texture maps given a texture behind its back (forgotten, as above, and counted as loaded by
// WL_GX_CHECK's empty-map finding).
void wl_gx_wii_texmaps_loaded(uint32_t maps);
// TLUT slot `slot` loaded behind wl_gx_tlut_load's back (by the direct layer): its next load sends.
void wl_gx_wii_tlut_forget(uint32_t slot);
// The texture memory's cache and TLUT regions set up as WlGxBisect.regions_libogc says (after GX_Init and
// whenever the switch changes): texture maps and TLUT slots forgotten, GX's texture cache invalidated.
void wl_gx_wii_regions_apply(void);
// A frame begun: the bisection's count of primitives (WlGxBisect.max_draws) starts again.
void wl_gx_wii_bisect_frame(void);
// A draw-sync token sent into the FIFO now (0 before wl_gx_init): hwcount.h's marks.
uint32_t wl_gx_wii_mark_token(void);

// The GPU hang report (gx_hang_wii.c): started by wl_gx_init; an event for its ring of the last
// GX events (kind a letter, gx_hang_wii.c's event_name; two numbers); and what gx_wii.c knows of
// the frame (safe in an interrupt: plain reads).
void wl_gx_wii_hang_init(void);
void wl_gx_wii_hang_event(char kind, uint32_t a, uint32_t b);
#ifdef WL_GX_CHECK
#include "wl/platform/tev.h"
// the TEV program last applied (wl_tev_apply), NULL before the first: WL_GX_CHECK builds only
const WlTevProgram *wl_gx_wii_tev_last(void);
#endif
void wl_gx_wii_hang_state(uint32_t *token_sent, uint32_t *frames_ended, bool *in_frame);
// The calling thread made libogc's GX thread (the one it suspends at the FIFO's high-water mark)
// when it is not already.
void wl_gx_wii_own_thread(void);
