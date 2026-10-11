# SPDX-License-Identifier: MIT
# wl_audio_core: the integer-only, platform-independent parts (the IMA-ADPCM decoder, DSP-ADPCM via wl_codec,
# the software mixer, the PWST/PWSF readers, the SSEQ/SBNK/SWAR sequence synthesiser). Builds
# everywhere, bit-identical output on little- and big-endian targets.
add_library(wl_audio_core STATIC
    adpcm.c mixer.c fmt.c synth_data.c synth_seq.c synth_voice.c)
wl_target_defaults(wl_audio_core DETERMINISTIC)
target_link_libraries(wl_audio_core PUBLIC wl_codec wl_base)
add_library(WiiLibs::audio_core ALIAS wl_audio_core)

# wl_audio: the engine (command ring to the audio thread, sample player, stream player, sequence
# voices, music track engine) over wl_platform's audio output, IO thread and memory system.
if(TARGET wl_platform)
    add_library(wl_audio STATIC audio.c stream.c music_engine.c)
    wl_target_defaults(wl_audio)
    target_link_libraries(wl_audio PUBLIC wl_audio_core wl_platform wl_base)
    add_library(WiiLibs::audio ALIAS wl_audio)
endif()
