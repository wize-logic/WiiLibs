# SPDX-License-Identifier: MIT
# wl_xboxapu: the original Xbox's audio hardware as device models: the MCPX APU (apu.h: registers,
# front end methods, the voice processor playing PCM and Xbox ADPCM voices into 32 mixbins at
# 48 kHz, notifiers and interrupts, a stereo mix in place of the GP/EP DSP programs) and the AC'97
# controller (ac97.h). Guest memory through a callback, no platform calls, integer only: the same
# output on every host. wl/xboxrt puts them behind 0xFE800000 and 0xFEC00000.
add_library(wl_xboxapu STATIC apu.c apu_vp.c ac97.c)
wl_target_defaults(wl_xboxapu DETERMINISTIC)
target_link_libraries(wl_xboxapu PUBLIC wl_audio_core wl_base)
add_library(WiiLibs::xboxapu ALIAS wl_xboxapu)
