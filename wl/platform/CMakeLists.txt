# SPDX-License-Identifier: MIT
# wl_platform: the platform layer (platform.h) with its portable core (memory system, heap,
# SPSC ring, IO thread, async files, GX helpers, the display queue) and one backend per platform:
#   wii    - libogc (video, GX, threads, libfat, WPAD/PAD/keyboard, AESND/ASND, log sinks)
#   host   - dependency-free stubs over POSIX and pthreads, so application code runs in host tests
#   ppc-be - the host stubs too (big-endian Linux under qemu, for tests)
add_library(wl_platform STATIC
    mem_heap.c mem.c heap_overflow.c heap_sites.c ring.c io.c fs.c gx_common.c stack.c tev.c backtrace.c net.c
    posix.c present.c profile.c)
wl_target_defaults(wl_platform)
target_link_libraries(wl_platform PUBLIC wl_base)

if(WL_PLATFORM STREQUAL "wii")
    target_sources(wl_platform PRIVATE
        wii/atomic_wii.c wii/crash_wii.c wii/tls_wii.c wii/audio_wii.c wii/backtrace_wii.c wii/exit_wii.c wii/fs_wii.c wii/gx_wii.c wii/input_wii.c wii/log_wii.c
        wii/heap_wrap_wii.c wii/mem_wii.c wii/net_wii.c wii/posix_wii.c wii/profile_wii.c wii/tev_wii.c wii/thread_wii.c wii/trace_wii.c wii/video_wii.c)
    target_link_libraries(wl_platform PUBLIC fat wiikeyboard wiiuse bte aesnd asnd ogc m)
    # devkitPPC's sched_yield is an ENOSYS stub: thread_wii.c's __wrap_sched_yield yields.
    target_link_options(wl_platform INTERFACE "LINKER:--wrap=sched_yield")
else()
    find_package(Threads REQUIRED)
    target_sources(wl_platform PRIVATE
        host/audio_host.c host/backtrace_host.c host/fs_host.c host/gx_host.c host/input_host.c host/mem_host.c host/net_host.c host/profile_host.c
        host/system_host.c host/tev_host.c host/thread_host.c)
    target_link_libraries(wl_platform PUBLIC Threads::Threads)
endif()
add_library(WiiLibs::platform ALIAS wl_platform)
