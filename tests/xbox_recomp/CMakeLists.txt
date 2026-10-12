# SPDX-License-Identifier: MIT
# The Xbox recompilation toolchain: the runtime header generated code includes
# (wl/xboxrt/recomp/recomp_types.h), xboxrecomp's pipeline as WiiLibs patches it, the correctness net
# against the x86 CPU, and wl_add_xbox_title on a synthetic title.
set(recomp_include ${PROJECT_SOURCE_DIR}/wl/xboxrt/recomp)

if(NOT WL_PLATFORM STREQUAL "wii")
    # the header itself: little-endian guest memory, the address transform, atomics, SIMD lanes and
    # the helpers, on the host and big-endian (ppc-be)
    wl_add_test(test_xbox_recomp_header SOURCES test_recomp_header.c LABELS unit be)
    target_include_directories(test_xbox_recomp_header SYSTEM PRIVATE ${recomp_include})
    target_compile_options(test_xbox_recomp_header PRIVATE -ffp-contract=off -fno-strict-aliasing)
    target_link_libraries(test_xbox_recomp_header PRIVATE m)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    message(STATUS "Python 3 not found: the Xbox recompilation tests are skipped")
    return()
endif()
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import capstone" RESULT_VARIABLE no_capstone
                OUTPUT_QUIET ERROR_QUIET)

if(WL_PLATFORM STREQUAL "host")
    # Python on the build machine, so host only. Exit code 77 means a tool is missing: skipped.
    set(work ${CMAKE_CURRENT_BINARY_DIR})
    # the committed big-endian header is what the generator makes from the patched template
    add_test(NAME xbox_recomp_header_current
        COMMAND ${Python3_EXECUTABLE} ${recomp_include}/gen_recomp_types.py --check)
    # xboxrecomp's own pytest suite, on the patched copy
    add_test(NAME xboxrecomp_pytest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_pytest.py --build ${work}/pytest)
    # every conformance case lifted and run big-endian (and on the host) against the x86 CPU
    add_test(NAME xbox_recomp_conformance
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/conformance.py --build ${work}/conformance)
    # whole programs lifted function by function, against the x86 CPU
    add_test(NAME xbox_recomp_e2e
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e.py --build ${work}/e2e)
    set_tests_properties(xbox_recomp_header_current xboxrecomp_pytest xbox_recomp_conformance
        xbox_recomp_e2e PROPERTIES LABELS unit SKIP_RETURN_CODE 77 TIMEOUT 1500)
    if(no_capstone)
        set_tests_properties(xboxrecomp_pytest xbox_recomp_conformance xbox_recomp_e2e
            PROPERTIES DISABLED TRUE)
    endif()
endif()

if(no_capstone)
    message(STATUS "${Python3_EXECUTABLE} has no capstone: the synthetic Xbox title is skipped")
    return()
endif()

# wl_add_xbox_title on the synthetic XBE upstream's mkxbe.py writes (five leaf functions), linked
# with a stand-in runtime; run on the host and ppc-be, built for the Wii (with its .dol)
set(synth_xbe ${CMAKE_CURRENT_BINARY_DIR}/synth.xbe)
set(mkxbe ${PROJECT_SOURCE_DIR}/third_party/xboxrecomp/tools/conformance/mkxbe.py)
add_custom_command(OUTPUT ${synth_xbe}
    COMMAND ${Python3_EXECUTABLE} ${mkxbe} ${synth_xbe}
    DEPENDS ${mkxbe}
    COMMENT "Synthetic XBE (xboxrecomp's mkxbe.py)"
    VERBATIM)
add_library(xbox_recomp_stub_runtime STATIC title/stub_runtime.c)
target_include_directories(xbox_recomp_stub_runtime SYSTEM PRIVATE ${recomp_include})
if(WL_PLATFORM STREQUAL "wii")
    target_compile_definitions(xbox_recomp_stub_runtime PRIVATE WL_XB_EA_BASE_ZERO)
endif()
wl_add_xbox_title(xbox_synth_title XBE ${synth_xbe} CHUNKS 4
    SOURCES title/synth_main.c RUNTIME xbox_recomp_stub_runtime)
if(NOT WL_PLATFORM STREQUAL "wii")
    add_test(NAME xbox_recomp_synthetic_title COMMAND xbox_synth_title)
    set_tests_properties(xbox_recomp_synthetic_title PROPERTIES LABELS "unit;be")
endif()
