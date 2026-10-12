# SPDX-License-Identifier: MIT
# The portable core (heap, memory system, ring, async files) on the host stubs; all of it is
# byte-order independent, so it also runs on the big-endian ppc-be build.
wl_add_test(test_platform_mem_heap SOURCES test_platform_mem_heap.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_mem SOURCES test_platform_mem.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_mem_fixed SOURCES test_platform_mem_fixed.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_mem_limit SOURCES test_platform_mem_limit.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_ring SOURCES test_platform_ring.c LIBS wl_platform LABELS unit be)
# The display queue (present.c) against a simulated video interface.
wl_add_test(test_platform_present SOURCES test_platform_present.c LIBS wl_platform LABELS unit be)
# The video mode the console's settings ask for (video_mode.c): every combination.
wl_add_test(test_platform_video_mode SOURCES test_platform_video_mode.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_fs SOURCES test_platform_fs.c suite_fs.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_backtrace SOURCES test_platform_backtrace.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_net SOURCES test_platform_net.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_posix SOURCES test_platform_posix.c suite_posix.c LIBS wl_platform LABELS unit be)
# Crash-safe file replacement and the sweep after a cut one; the directory listing cache (POSIX paths, a temp folder).
wl_add_test(test_platform_file_replace SOURCES test_platform_file_replace.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_dircache SOURCES test_platform_dircache.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_random SOURCES test_platform_random.c LIBS wl_platform LABELS unit be)
# The time base's conversions (timebase.h) against a bit-by-bit division: edges, wrap, random counts.
wl_add_test(test_platform_timebase SOURCES test_platform_timebase.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_host SOURCES test_platform_host.c LIBS wl_platform LABELS unit be)
# The render targets (the owner of the EFB, parking, resolving, clearing) and the ramp, on the host stub.
wl_add_test(test_platform_target SOURCES test_platform_target.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_thread SOURCES test_platform_thread.c suite_threads.c LIBS wl_platform LABELS unit be)

# TEV programs: the builder, validator and evaluator (portable, also on ppc-be) ...
wl_add_test(test_platform_tev SOURCES test_platform_tev.c LIBS wl_platform LABELS unit be)

# ... and the Wii apply step (wii/tev_wii.c) compiled against a recording stand-in for libogc's GX.
# It must not link wl_platform, whose host backend has its own wl_tev_apply_unchecked.
wl_add_test(test_platform_tev_gx
    SOURCES test_platform_tev_gx.c gx_stub/gx_stub.c
            ${PROJECT_SOURCE_DIR}/wl/platform/tev.c ${PROJECT_SOURCE_DIR}/wl/platform/wii/tev_wii.c
    LIBS wl_base LABELS unit be)
target_include_directories(test_platform_tev_gx BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gx_stub)

# The render targets' GPU half (wii/gx_target_wii.c) with their bookkeeping (gx_target.c): the exact GX calls,
# in order, against the same recording stand-in. Not wl_platform either (the stand-in provides the rest).
wl_add_test(test_platform_target_gx
    SOURCES test_platform_target_gx.c gx_stub/gx_stub.c
            ${PROJECT_SOURCE_DIR}/wl/platform/gx_target.c ${PROJECT_SOURCE_DIR}/wl/platform/gx_common.c
            ${PROJECT_SOURCE_DIR}/wl/platform/wii/gx_target_wii.c
    LIBS wl_base LABELS unit be)
target_include_directories(test_platform_target_gx BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gx_stub)

# The spill-over heap: the portable core against a fake primary allocator ...
wl_add_test(test_platform_heap_overflow SOURCES test_platform_heap_overflow.c LIBS wl_platform LABELS unit be)
# ... and the newlib wrap layer (wii/heap_wrap_wii.c, which includes no libogc header) against a fake newlib.
# (Host builds of wl_platform do not carry that file, so there is no second copy to clash with.)
wl_add_test(test_platform_heap_wrap SOURCES test_platform_heap_wrap.c
            ${PROJECT_SOURCE_DIR}/wl/platform/wii/heap_wrap_wii.c LIBS wl_platform LABELS unit be)
# Who holds the heap: the accounting by call site that the wrap layer feeds.
wl_add_test(test_platform_heap_sites SOURCES test_platform_heap_sites.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_profile SOURCES test_platform_profile.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_stop SOURCES test_platform_stop.c LIBS wl_platform LABELS unit be)
# Leaving the program: shutdown hooks, the system buttons' thread, the GX FIFO's size from its budget.
wl_add_test(test_platform_shutdown SOURCES test_platform_shutdown.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_profile_arm SOURCES test_platform_profile_arm.c LIBS wl_platform LABELS unit be)
# The hardware counters' engine (hwcount.c) on a fake machine: groups, windows, medians, the verdict, the log.
wl_add_test(test_platform_hwcount SOURCES test_platform_hwcount.c LIBS wl_platform LABELS unit be)

# Input shaping (sticks, triggers, the Remote's pointer): portable, also on ppc-be ...
wl_add_test(test_platform_input_shape SOURCES test_platform_input_shape.c LIBS wl_platform LABELS unit be)
# ... and the folding of a frame's reports into one (input_reports.c).
wl_add_test(test_platform_input_reports SOURCES test_platform_input_reports.c LIBS wl_platform LABELS unit be)

# ... and a Nunchuk's thrust found in synthetic acceleration traces (input_gesture.c).
wl_add_test(test_platform_input_gesture SOURCES test_platform_input_gesture.c LIBS wl_platform LABELS unit be)

# ... and the Wii input backend (wii/input_wii.c) compiled against stand-ins for libogc's WPAD, PAD
# and keyboard, which answer from what the test sets up. It links wl_base for the log only.
wl_add_test(test_platform_input_wii
    SOURCES test_platform_input_wii.c input_stub/input_stub.c
            ${PROJECT_SOURCE_DIR}/wl/platform/wii/input_wii.c ${PROJECT_SOURCE_DIR}/wl/platform/input_shape.c
            ${PROJECT_SOURCE_DIR}/wl/platform/input_reports.c
    LIBS wl_base LABELS unit be)
target_include_directories(test_platform_input_wii BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/input_stub)
target_link_libraries(test_platform_input_wii PRIVATE m)

# The frame overlay hook, its drawing helpers and font (overlay.c) on the host GX stub, and the audio
# output's master fade (audio_gain.c).
wl_add_test(test_platform_overlay SOURCES test_platform_overlay.c LIBS wl_platform LABELS unit be)
wl_add_test(test_platform_audio_fade SOURCES test_platform_audio_fade.c LIBS wl_platform LABELS unit be)
