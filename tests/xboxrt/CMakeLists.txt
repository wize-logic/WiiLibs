# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xboxrt)
    return()
endif()
# Guest memory on the host / ppc-be backend, with the faults through the host's decoder.
wl_add_test(test_xboxrt_mem SOURCES test_xboxrt_mem.c LIBS wl_xboxrt LABELS unit be)
# wl_xb_range_ok's benchmark (tools/qemu_icount.py counts its instructions on ppc-be); the count of
# ranges that pass pins what it answers.
add_executable(bench_range_ok bench_range_ok.c)
wl_target_defaults(bench_range_ok)
target_link_libraries(bench_range_ok PRIVATE wl_xboxrt)
add_test(NAME bench_range_ok_passes COMMAND bench_range_ok 100000 98434)
set_tests_properties(bench_range_ok_passes PROPERTIES LABELS "unit;be")
# Demand paging under a frame limit (the Wii's model through SIGSEGV and mprotect): with a swap
# file, and without one.
wl_add_test(test_xboxrt_paging SOURCES test_xboxrt_paging.c LIBS wl_xboxrt LABELS unit be)
wl_add_test(test_xboxrt_paging_no_swap SOURCES test_xboxrt_paging.c LIBS wl_xboxrt LABELS unit be)
target_compile_definitions(test_xboxrt_paging_no_swap PRIVATE WL_XBRT_PAGING_NO_SWAP=1)
# The gamepad state, its reports, the Wii controllers' mappings and the ports (portable).
wl_add_test(test_xboxrt_pad SOURCES test_xboxrt_pad.c LIBS wl_xboxrt LABELS unit be)
# The USB host controllers and the gamepads, driven as a title's XAPI drives them.
wl_add_test(test_xboxrt_usb SOURCES test_xboxrt_usb.c LIBS wl_xboxrt LABELS unit be)
# The play harness: a script of every step against a pretend title, the parser, the bot.
wl_add_test(test_xboxrt_play SOURCES test_xboxrt_play.c LIBS wl_xboxrt LABELS unit be)
# The PowerPC and x86-64 decoders against a fake device (portable).
wl_add_test(test_xboxrt_decode SOURCES test_xboxrt_decode.c LIBS wl_xboxrt LABELS unit be)
# The capture bench (wl/xboxrt/bench.h): the capture test_xbox_nv2a_pixels writes, replayed through
# the runtime's guest memory and GX backend by a title with no code of its own (bench_title.c) and
# timed; then with 1 MB of guest frames, paged as the Wii pages, the textures checked through the
# write watch.
if(WL_PLATFORM STREQUAL "host" AND TARGET test_xbox_nv2a_pixels AND TARGET wl_xboxkrnl)
    add_executable(xboxrt_bench_title bench_title.c)
    wl_target_defaults(xboxrt_bench_title)
    target_link_libraries(xboxrt_bench_title PRIVATE wl_xboxrt_main)
    set(bench_dir ${CMAKE_CURRENT_BINARY_DIR}/bench_title)
    file(MAKE_DIRECTORY ${bench_dir}/hdd)
    set(nv2a_capture ${PROJECT_BINARY_DIR}/tests/xbox/nv2a_test.capture)
    add_test(NAME xboxrt_capture_bench COMMAND xboxrt_bench_title ${bench_dir})
    add_test(NAME xboxrt_capture_bench_paged COMMAND xboxrt_bench_title ${bench_dir})
    set_tests_properties(xboxrt_capture_bench PROPERTIES ENVIRONMENT "WL_XB_PLAY=replay:${nv2a_capture} 3"
        PASS_REGULAR_EXPRESSION "run 3: 1 frames, 2 draws.*summary: 3 runs of 1 frames")
    set_tests_properties(xboxrt_capture_bench_paged PROPERTIES
        ENVIRONMENT "WL_XB_PLAY=replay:${nv2a_capture} 2;WL_XB_FRAME_KB=1024"
        PASS_REGULAR_EXPRESSION "summary: 2 runs of 1 frames.*by the write watch")
    set_tests_properties(xboxrt_capture_bench xboxrt_capture_bench_paged PROPERTIES LABELS unit
        FIXTURES_REQUIRED nv2a_capture FAIL_REGULAR_EXPRESSION "\\[E\\]\\[xbbench\\]|replay failed")
endif()
# The generated code's big-endian header (wl/xboxrt/recomp/recomp_types.h when the toolchain is in
# the tree; another directory for trying one out).
set(WL_XBRT_RECOMP_HEADER_DIR "${PROJECT_SOURCE_DIR}/wl/xboxrt/recomp" CACHE PATH
    "Directory with the generated code's recomp_types.h (the ABI and e2e tests)")

# The register file and the runtime's functions against that header: a type that differs is a
# compile error (recomp_symbols.inc, thunk.h). The header is a system include: its own helpers are
# not held to this project's warnings.
if(EXISTS ${WL_XBRT_RECOMP_HEADER_DIR}/recomp_types.h)
    wl_add_test(test_xboxrt_abi SOURCES test_xboxrt_abi.c LIBS wl_xboxrt LABELS unit be)
    target_include_directories(test_xboxrt_abi SYSTEM PRIVATE ${WL_XBRT_RECOMP_HEADER_DIR})
endif()

# End to end: a synthetic title (e2e/title.c) built for i386, wrapped in an XBE, lifted by
# xboxrecomp's pipeline and run on the runtime and kernel. Needs the generated code's header
# (above), xboxrecomp's tools (WL_XBOXRECOMP_DIR: third_party/xboxrecomp when it is vendored, else
# ~/GitHub/xboxrecomp), Python and a compiler that makes i386 code (gcc -m32).
if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/xboxrecomp/tools/recomp)
    set(wl_xbr_default ${PROJECT_SOURCE_DIR}/third_party/xboxrecomp)
else()
    set(wl_xbr_default "$ENV{HOME}/GitHub/xboxrecomp")
endif()
set(WL_XBOXRECOMP_DIR "${wl_xbr_default}" CACHE PATH "xboxrecomp's checkout (its tools/) for the e2e test")
find_package(Python3 COMPONENTS Interpreter)
find_program(WL_E2E_I386_CC NAMES gcc)
find_program(WL_E2E_I386_LD NAMES ld)
set(e2e_ok OFF)
if(Python3_FOUND AND WL_E2E_I386_CC AND WL_E2E_I386_LD AND EXISTS ${WL_XBRT_RECOMP_HEADER_DIR}/recomp_types.h
   AND EXISTS ${WL_XBOXRECOMP_DIR}/tools/recomp)
    execute_process(COMMAND ${WL_E2E_I386_CC} -m32 -ffreestanding -fsyntax-only -x c -
                    INPUT_FILE /dev/null RESULT_VARIABLE m32_rc OUTPUT_QUIET ERROR_QUIET)
    if(m32_rc EQUAL 0)
        set(e2e_ok ON)
    endif()
endif()
if(e2e_ok)
    set(e2e_out ${CMAKE_CURRENT_BINARY_DIR}/e2e)
    set(e2e_src ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
    add_custom_command(
        OUTPUT ${e2e_out}/title.xbe ${e2e_out}/title_syms.h ${e2e_out}/gen/recomp_0000.c ${e2e_out}/gen/recomp_dispatch.c
        COMMAND ${Python3_EXECUTABLE} ${e2e_src}/build_title.py --cc ${WL_E2E_I386_CC} --ld ${WL_E2E_I386_LD}
                --src ${e2e_src} --out ${e2e_out} --xboxrecomp ${WL_XBOXRECOMP_DIR} --header ${WL_XBRT_RECOMP_HEADER_DIR}
        DEPENDS ${e2e_src}/title.c ${e2e_src}/title.ld ${e2e_src}/mkxbe_title.py ${e2e_src}/build_title.py
                ${WL_XBRT_RECOMP_HEADER_DIR}/recomp_types.h
        COMMENT "Building and lifting the e2e title"
        VERBATIM)
    # generated code is linked as objects: nothing would pull it out of a static archive
    add_library(wl_e2e_generated OBJECT ${e2e_out}/gen/recomp_0000.c ${e2e_out}/gen/recomp_dispatch.c)
    target_include_directories(wl_e2e_generated BEFORE PRIVATE ${e2e_out}/gen ${PROJECT_SOURCE_DIR})
    target_compile_options(wl_e2e_generated PRIVATE -w -fno-strict-aliasing -ffp-contract=off -std=gnu11)
    wl_add_test(test_xboxrt_e2e SOURCES e2e/test_xboxrt_e2e.c ${e2e_out}/title_syms.h $<TARGET_OBJECTS:wl_e2e_generated>
                LIBS wl_xboxrt_boot wl_xboxrt m LABELS unit be)
    target_include_directories(test_xboxrt_e2e PRIVATE ${e2e_out})
    set_tests_properties(test_xboxrt_e2e PROPERTIES ENVIRONMENT "WL_E2E_DIR=${e2e_out}" TIMEOUT 120)
else()
    message(STATUS "test_xboxrt_e2e: off (needs ${WL_XBRT_RECOMP_HEADER_DIR}/recomp_types.h, "
                   "${WL_XBOXRECOMP_DIR}/tools, Python and gcc -m32)")
endif()
