# SPDX-License-Identifier: MIT
# The on-console test runner (console_runner.c): the platform-independent suites on the real Wii
# backends, as wl_console_tests.dol. With a Dolphin to run it (WL_DOLPHIN, or
# Documents/Emulators/Dolphin-x64 in a Windows profile under WSL), `ctest -L dolphin` boots it
# there through tools/dolphin_run.py with an empty folder as the SD card.
add_executable(wl_console_tests
    console_runner.c
    ${PROJECT_SOURCE_DIR}/tests/platform/suite_threads.c
    ${PROJECT_SOURCE_DIR}/tests/platform/suite_fs.c
    ${PROJECT_SOURCE_DIR}/tests/platform/suite_posix.c
    ${PROJECT_SOURCE_DIR}/tests/audio/suite_sseq.c
    ${PROJECT_SOURCE_DIR}/tests/audio/synth_fixture.c
    ${PROJECT_SOURCE_DIR}/tests/gfx/suite_sqrt.c
    ${PROJECT_SOURCE_DIR}/tests/xbox/suite_nv2a.c)
wl_target_defaults(wl_console_tests)
target_link_libraries(wl_console_tests PRIVATE wl_audio wl_platform wl_gfx wl_xbox_gx)
# the synthetic vertex programs compiled ahead of time with their paired-single code against the
# interpreter (tests/xbox/suite_vsh_ps.c): tools/xbox_vsh_codegen.c built for the build machine, as
# wl_add_xbox_title builds it
if(NOT WL_HOST_C_COMPILER)
    find_program(WL_HOST_C_COMPILER NAMES cc gcc clang)
endif()
if(WL_HOST_C_COMPILER)
    set(vsh_tool ${CMAKE_CURRENT_BINARY_DIR}/xbox_vsh_codegen${CMAKE_HOST_EXECUTABLE_SUFFIX})
    set(vsh_ps_generated ${CMAKE_CURRENT_BINARY_DIR}/vsh_ps_programs.c)
    add_custom_command(OUTPUT ${vsh_tool}
        COMMAND ${WL_HOST_C_COMPILER} -O1 -std=c11 -ffp-contract=off -I${PROJECT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/tools/xbox_vsh_codegen.c ${PROJECT_SOURCE_DIR}/wl/gfx/gfx_sqrt.c -lm -o ${vsh_tool}
        DEPENDS ${PROJECT_SOURCE_DIR}/tools/xbox_vsh_codegen.c ${PROJECT_SOURCE_DIR}/wl/xbox/vsh.c
            ${PROJECT_SOURCE_DIR}/wl/xbox/vsh.h ${PROJECT_SOURCE_DIR}/wl/gfx/gfx_sqrt.c
        VERBATIM)
    add_custom_command(OUTPUT ${vsh_ps_generated}
        COMMAND ${vsh_tool} ${PROJECT_SOURCE_DIR}/tests/xbox/vsh_programs.inc ${vsh_ps_generated} test_vsh_ps_natives
        DEPENDS ${vsh_tool} ${PROJECT_SOURCE_DIR}/tests/xbox/vsh_programs.inc
        VERBATIM)
    # (as a title's: -O2, no contraction)
    set_source_files_properties(${vsh_ps_generated} PROPERTIES COMPILE_OPTIONS "-O2;-ffp-contract=off;-w")
    target_sources(wl_console_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/xbox/suite_vsh_ps.c ${vsh_ps_generated})
    target_include_directories(wl_console_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/xbox)
    target_compile_definitions(wl_console_tests PRIVATE WLT_VSH_PS=1)
endif()
# every way out of the runner is logged with where it came from (wii/trace_wii.c)
wl_platform_trace_exits(wl_console_tests)
wl_platform_heap_overflow(wl_console_tests)
ogc_create_dol(wl_console_tests)

# the Homebrew Channel package of the runner (wl_add_hbc_package; `ninja wl_console_tests_hbc`)
wl_add_hbc_package(wl_console_tests NAME "WiiLibs tests" CODER "WiiLibs" VERSION "${PROJECT_VERSION}"
    SHORT_DESCRIPTION "WiiLibs on-console tests"
    LONG_DESCRIPTION "Runs the WiiLibs test suites on the console and prints WLT lines to the log. The SD card's wltest folder is used for scratch files."
    ICON_TEXT "WIILIBS" "TESTS")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sd)
    add_test(NAME wl_console_tests_dolphin
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/dolphin_run.py
            ${CMAKE_CURRENT_BINARY_DIR}/wl_console_tests.dol
            --until "WLT DONE" --fail " FAIL " --seconds 180
            --sd ${CMAKE_CURRENT_BINARY_DIR}/sd
            --screenshot ${CMAKE_CURRENT_BINARY_DIR}/wl_console_tests.png)
    set_tests_properties(wl_console_tests_dolphin PROPERTIES LABELS dolphin TIMEOUT 240)
endif()

# wl/xboxrt's guest memory on the console's MMU (tests/xboxrt/wii_console.c) as
# wl_xboxrt_console.dol: the page table filled by first-touch DSIs, evictions, a pinned page that
# moves a user page, device accesses through the DSI trampoline. Dolphin runs it with its full MMU
# emulation (--mmu); it prints XBRT lines.
if(TARGET wl_xboxrt)
    add_executable(wl_xboxrt_console ${PROJECT_SOURCE_DIR}/tests/xboxrt/wii_console.c)
    wl_target_defaults(wl_xboxrt_console)
    target_link_libraries(wl_xboxrt_console PRIVATE wl_xboxrt wl_platform)
    ogc_create_dol(wl_xboxrt_console)
    if(Python3_FOUND)
        add_test(NAME wl_xboxrt_console_dolphin
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/dolphin_run.py
                ${CMAKE_CURRENT_BINARY_DIR}/wl_xboxrt_console.dol
                --mmu --until "XBRT DONE" --fail "XBRT FAIL" --seconds 120)
        set_tests_properties(wl_xboxrt_console_dolphin PROPERTIES LABELS dolphin TIMEOUT 180)
    endif()
endif()
