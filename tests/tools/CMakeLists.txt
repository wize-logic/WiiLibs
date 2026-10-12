# SPDX-License-Identifier: MIT
# The Python tools (tools/*.py) and the CMake helpers over them. Host only: Python runs on the
# build machine.
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    message(STATUS "Python 3 not found: the tools' tests are skipped")
    return()
endif()

add_test(NAME test_wii_package
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_wii_package.py)
set_tests_properties(test_wii_package PROPERTIES LABELS unit)

# wl_add_hbc_package: one package from a .dol file and one from an executable target that has the
# .dol of ogc_create_dol (here a copy of the executable, with the same DKP_FILE property), both
# built with the tests and then checked.
set(fixture_dir ${CMAKE_CURRENT_BINARY_DIR})
add_custom_command(OUTPUT ${fixture_dir}/fixture.dol
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/test_wii_package.py ${fixture_dir}/fixture.dol
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_wii_package.py)
file(WRITE ${fixture_dir}/long.txt "A long description\nof two lines & a symbol.\n")
wl_add_hbc_package(wl_hbc_file NAME "Fixture & Co" DOL ${fixture_dir}/fixture.dol
    OUTPUT ${fixture_dir}/hbc_file.zip CODER "Me <me>" VERSION "1.0" SHORT_DESCRIPTION "short"
    LONG_DESCRIPTION_FILE ${fixture_dir}/long.txt ICON_TEXT "FIXTURE" "CO"
    EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/test_wii_package.py:notes/extra.py AHB_ACCESS
    RELEASE_DATE 20260101000000 ALL)

add_executable(wl_hbc_program hbc_program.c)
add_custom_command(TARGET wl_hbc_program POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:wl_hbc_program> ${fixture_dir}/wl_hbc_program.dol
    BYPRODUCTS ${fixture_dir}/wl_hbc_program.dol VERBATIM)
set_target_properties(wl_hbc_program PROPERTIES DKP_FILE ${fixture_dir}/wl_hbc_program.dol)
wl_add_hbc_package(wl_hbc_program NAME "Program" ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon_128x48.png ALL)

add_test(NAME test_hbc_packages
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_hbc_zip.py
        ${fixture_dir}/hbc_file.zip ${fixture_dir}/wl_hbc_program.zip)
set_tests_properties(test_hbc_packages PROPERTIES LABELS unit)

# the profiler's symbolizer (tools/wl_profile.py): the dump parser, stacks and tables on synthetic
# samples, and the command line over a small ELF linked here with the host compiler
add_test(NAME test_wl_profile
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_wl_profile.py)
set_tests_properties(test_wl_profile PROPERTIES LABELS unit)

# the instruction counter (tools/qemu_icount.py): its log parser on both of qemu's forms, and a loop
# counted under qemu-ppc where it and a PowerPC Linux compiler are installed (skipped otherwise)
add_test(NAME test_qemu_icount
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_qemu_icount.py)
set_tests_properties(test_qemu_icount PROPERTIES LABELS unit)

# the stub linker (tools/link_stubs.py): the symbols it finds, the C file it writes, and programs
# linked and run with the host compiler standing in for the Wii's; and, natively, against
# wl_platform's host backend with its stop screen as the stop function
if(CMAKE_CROSSCOMPILING)
    set(link_stubs_library_args "")
else()
    set(link_stubs_library_args
        $<TARGET_FILE:wl_platform> $<TARGET_FILE:wl_base> ${PROJECT_SOURCE_DIR})
endif()
add_test(NAME test_link_stubs
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_link_stubs.py ${link_stubs_library_args})
set_tests_properties(test_link_stubs PROPERTIES LABELS unit)
