// SPDX-License-Identifier: MIT
// What gx_flush_wii.c gives gx_direct_wii.c: the transform unit's flush state, shared by the direct
// layer's own draws and a renderer's own vertex formats (gx_direct.h). Not for applications.
#pragma once

#include <stdbool.h>

// whether the flush is turned off (wl_gx_direct_set_xf_flush)
bool wl_gx_flush_wii_off(void);
// whether a flush is to be sent now (due and on); none is due after it
bool wl_gx_flush_wii_take(void);
// a flush was sent
void wl_gx_flush_wii_count(void);
