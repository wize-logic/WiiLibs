# SPDX-License-Identifier: MIT
# Audio tests. Synthetic content only: dsp_fixture.c (test tone, reference DSP-ADPCM encoder),
# synth_fixture.c (hand-built SSEQ/SBNK/SWAR files), audio_synth.c (PWST/PWSF payloads).

# The integer core: decoders, mixer and the SSEQ synth checks, also on big-endian (ppc-be).
wl_add_test(test_audio_adpcm SOURCES test_audio_adpcm.c dsp_fixture.c LIBS wl_audio_core LABELS unit be)
wl_add_test(test_audio_mixer SOURCES test_audio_mixer.c LIBS wl_audio_core LABELS unit be)
wl_add_test(test_audio_synth_be SOURCES test_audio_synth_be.c suite_sseq.c synth_fixture.c
            LIBS wl_audio_core LABELS unit be)
target_compile_definitions(test_audio_synth_be PRIVATE WL_SSEQ_CORE_ONLY=1)
target_link_libraries(test_audio_synth_be PRIVATE m)

# The engine on the host stub of wl_platform (no sound device: the test renders blocks itself);
# ppc-be uses the same stubs.
if(TARGET wl_audio)
    wl_add_test(test_audio_engine SOURCES test_audio_engine.c audio_synth.c dsp_fixture.c
                LIBS wl_audio LABELS unit be)
    wl_add_test(test_audio_synth SOURCES test_audio_synth.c suite_sseq.c synth_fixture.c
                LIBS wl_audio LABELS unit be)
    target_compile_definitions(test_audio_synth PRIVATE WL_SSEQ_CORE_ONLY=1)
    target_link_libraries(test_audio_synth PRIVATE m)
    # The music engine backend on its own: prepare/play/stop/pause/gain/duck through the backend table over a
    # scripted loader and the real audio engine on the host stub; refusals, slow loads, voice and table limits,
    # eviction, ends and loops, shutdown.
    wl_add_test(test_audio_music_engine SOURCES test_audio_music_engine.c synth_fixture.c
                LIBS wl_audio LABELS unit be)
    target_link_libraries(test_audio_music_engine PRIVATE m)
endif()
