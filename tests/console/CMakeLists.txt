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
    ${PROJECT_SOURCE_DIR}/tests/gfx/suite_sqrt.c)
wl_target_defaults(wl_console_tests)
target_link_libraries(wl_console_tests PRIVATE wl_audio wl_platform wl_gfx)
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
