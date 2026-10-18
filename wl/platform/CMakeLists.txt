# SPDX-License-Identifier: MIT
# wl_platform: the platform layer (platform.h) with its portable core (memory system, heap,
# SPSC ring, IO thread, async files, GX helpers, the display queue) and one backend per platform:
#   wii    - libogc (video, GX, threads, libfat, WPAD/PAD/keyboard, AESND/ASND, log sinks)
#   host   - dependency-free stubs over POSIX and pthreads, so application code runs in host tests
#   ppc-be - the host stubs too (big-endian Linux under qemu, for tests)
add_library(wl_platform STATIC
    mem_heap.c mem.c mem_census.c heap_overflow.c heap_sites.c ring.c io.c fs.c gx_common.c gx_shadow.c gx_target.c gx_vertex.c stack.c tev.c backtrace.c net.c net_stream.c
    posix.c fat_map.c fs_speed.c present.c video_mode.c profile.c profile_arm.c hwcount.c hwcount_app.c stop.c input_shape.c input_reports.c input_gesture.c shutdown.c gx_fifo.c audio_gain.c overlay.c file_replace.c dircache.c
    home_menu.c home_menu_draw.c strap_screen.c strap_screen_draw.c home_shell.c wii/memfast_wii.c)
wl_target_defaults(wl_platform)
wl_gpu_path(wl_platform)
target_link_libraries(wl_platform PUBLIC wl_base m)

if(WL_PLATFORM STREQUAL "wii")
    target_sources(wl_platform PRIVATE
        wii/atomic_wii.c wii/crash_wii.c wii/tls_wii.c wii/audio_wii.c wii/backtrace_wii.c wii/exit_wii.c wii/fs_wii.c wii/gx_wii.c wii/gx_direct_wii.c wii/gx_flush_wii.c wii/gx_hang_wii.c wii/gx_target_wii.c wii/input_wii.c wii/log_wii.c
        wii/heap_wrap_wii.c wii/hwcount_wii.c wii/mem_wii.c wii/net_wii.c wii/posix_wii.c wii/profile_wii.c wii/random_wii.c wii/stop_wii.c wii/tev_wii.c wii/thread_wii.c wii/trace_wii.c wii/video_wii.c)
    target_link_libraries(wl_platform PUBLIC fat wiikeyboard wiiuse bte aesnd asnd ogc m)
    # devkitPPC's sched_yield is an ENOSYS stub: thread_wii.c's __wrap_sched_yield yields.
    target_link_options(wl_platform INTERFACE "LINKER:--wrap=sched_yield")
else()
    find_package(Threads REQUIRED)
    target_sources(wl_platform PRIVATE
        host/audio_host.c host/backtrace_host.c host/fs_host.c host/gx_host.c host/gx_direct_host.c host/gx_target_host.c host/heap_host.c host/hwcount_host.c host/input_host.c host/mem_host.c host/net_host.c host/profile_host.c host/stop_host.c
        host/random_host.c host/system_host.c host/tev_host.c host/thread_host.c)
    target_link_libraries(wl_platform PUBLIC Threads::Threads)
    # host/heap_host.c replaces malloc for the census's cap, which a static link cannot do
    if(CMAKE_EXE_LINKER_FLAGS MATCHES "-static" OR CMAKE_EXE_LINKER_FLAGS_INIT MATCHES "-static")
        set_source_files_properties(host/heap_host.c PROPERTIES COMPILE_DEFINITIONS WL_HEAP_HOST_STATIC=1)
    endif()
endif()
add_library(WiiLibs::platform ALIAS wl_platform)
# memcpy, memmove, memset and memcmp tuned for Broadway (memfast.h): the same C on every target for
# the tests; the loops in it must not become calls of the functions it replaces.
set_source_files_properties(wii/memfast_wii.c PROPERTIES COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
# On the Wii every program that links wl_platform has them as its memcpy & co. (the libraries' and
# libogc's and newlib's calls too) through the linker's --wrap; -u pulls the wrappers out of the
# archive even when only libc's objects call them. On the host it is for a run that proves them in a
# title (dynamic glibc keeps its own internal calls). WL_MEMFAST_HIST counts the calls by length.
if(WL_PLATFORM STREQUAL "wii")
    set(WL_MEMFAST_WRAP_DEFAULT ON)
else()
    set(WL_MEMFAST_WRAP_DEFAULT OFF)
endif()
option(WL_MEMFAST_WRAP "memcpy, memmove, memset and memcmp are wl/platform/memfast.h's (--wrap)" ${WL_MEMFAST_WRAP_DEFAULT})
option(WL_MEMFAST_HIST "Count memcpy, memmove, memset and memcmp calls by length (wl_memfast_hist_write)" OFF)
if(WL_MEMFAST_WRAP)
    target_compile_definitions(wl_platform PRIVATE WL_MEMFAST_WRAP=1)
    target_link_options(wl_platform INTERFACE
        "LINKER:--wrap=memcpy,--wrap=memmove,--wrap=memset,--wrap=memcmp" "LINKER:-u,__wrap_memcpy")
endif()
if(WL_MEMFAST_HIST)
    target_compile_definitions(wl_platform PRIVATE WL_MEMFAST_HIST=1)
endif()
# Each direct-layer draw's GX state checked on the Wii (gx_direct.h, wl_gx_check_draw): findings
# logged under [GXCHECK]. Off: no cost.
option(WL_GX_CHECK "Check each GX draw's state on the Wii and log what the console's GPU cannot draw" OFF)
if(WL_GX_CHECK)
    target_compile_definitions(wl_platform PRIVATE WL_GX_CHECK=1)
endif()
