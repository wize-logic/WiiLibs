# SPDX-License-Identifier: MIT
# wl_xboxrt: the runtime a recompiled Xbox title runs on: guest memory (the Wii's page table, a
# PROT_NONE reservation on the host and ppc-be), device faults and their decoders, the XBE loader,
# the register file the generated code reads, guest threads under one guest lock, interrupts and
# the vertical blank, the kernel thunk dispatcher, crash reports. The kernel itself is wl_xboxkrnl;
# the boot code that joins them is wl_xboxrt_boot (and wl_xboxrt_main, a default main).
add_library(wl_xboxrt STATIC
    mem.c paging.c mmio.c ppc_decode.c x86_decode.c regs.c thread.c irq.c thunk.c crash.c xbe.c devices.c gpu_stub.c
    pad.c input.c usb_ohci.c usb_xid.c play.c bench.c runopts.c)
wl_target_defaults(wl_xboxrt)
target_link_libraries(wl_xboxrt PUBLIC wl_platform wl_base)
if(WL_PLATFORM STREQUAL "wii")
    target_sources(wl_xboxrt PRIVATE mem_wii.c dsi_wii.c)
else()
    target_sources(wl_xboxrt PRIVATE mem_posix.c fault_posix.c)
endif()
# wl/xbox's NV2A model, when it is there, behind the GPU interface (devices.h)
if(EXISTS ${PROJECT_SOURCE_DIR}/wl/xbox/nv2a.h AND TARGET wl_xbox AND TARGET wl_xbox_gx)
    target_sources(wl_xboxrt PRIVATE nv2a_adapter.c)
    target_compile_definitions(wl_xboxrt PRIVATE WL_XBRT_HAVE_NV2A=1)
    target_link_libraries(wl_xboxrt PUBLIC wl_xbox wl_xbox_gx)
endif()
# the play harness's screenshots on the host: wl/gxsoft draws a frame, wl/gxtex writes the PNG
if(TARGET wl_gxsoft AND TARGET wl_gxtex)
    target_compile_definitions(wl_xboxrt PRIVATE WL_XBRT_HAVE_GXSOFT=1)
    target_link_libraries(wl_xboxrt PUBLIC wl_gxsoft wl_gxtex)
endif()
# wl/xboxapu's APU and AC'97 models, when they are there, as the audio devices (devices.h)
if(TARGET wl_xboxapu)
    target_sources(wl_xboxrt PRIVATE audio.c)
    target_compile_definitions(wl_xboxrt PRIVATE WL_XBRT_HAVE_APU=1)
    target_link_libraries(wl_xboxrt PUBLIC wl_xboxapu)
endif()
add_library(WiiLibs::xboxrt ALIAS wl_xboxrt)

# The boot code (boot.h) joins the runtime to the kernel; wl_xboxrt_main is a main over it.
add_library(wl_xboxrt_boot STATIC boot.c)
wl_target_defaults(wl_xboxrt_boot)
target_link_libraries(wl_xboxrt_boot PUBLIC wl_xboxrt wl_xboxkrnl wl_platform)
if(NOT WL_PLATFORM STREQUAL "wii")
    target_sources(wl_xboxrt_boot PRIVATE hdd_lock_posix.c) # one run at a time on a hard disk's folder
endif()
add_library(WiiLibs::xboxrt_boot ALIAS wl_xboxrt_boot)
add_library(wl_xboxrt_main STATIC boot_main.c)
wl_target_defaults(wl_xboxrt_main)
target_link_libraries(wl_xboxrt_main PUBLIC wl_xboxrt_boot)
add_library(WiiLibs::xboxrt_main ALIAS wl_xboxrt_main)
