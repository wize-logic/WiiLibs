# SPDX-License-Identifier: MIT
# WiiLibs: reusable C11 libraries for Wii homebrew and the host tools around it.
# WL_PLATFORM selects host, wii or ppc-be (32-bit big-endian Linux under qemu-ppc, for tests).
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED WL_PLATFORM)
    if(CMAKE_TOOLCHAIN_FILE MATCHES "Wii\\.cmake$")
        set(WL_PLATFORM wii)
    elseif(CMAKE_TOOLCHAIN_FILE MATCHES "powerpc-linux-gnu\\.cmake$")
        set(WL_PLATFORM ppc-be)
    else()
        set(WL_PLATFORM host)
    endif()
endif()
set(WL_PLATFORM "${WL_PLATFORM}" CACHE STRING "Target platform: host, wii or ppc-be")
set_property(CACHE WL_PLATFORM PROPERTY STRINGS host wii ppc-be)
if(NOT WL_PLATFORM MATCHES "^(host|wii|ppc-be)$")
    message(FATAL_ERROR "WL_PLATFORM must be host, wii or ppc-be (got '${WL_PLATFORM}')")
endif()
if(WL_PLATFORM STREQUAL "wii" AND NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "WL_PLATFORM=wii needs the devkitPro toolchain: configure with "
        "-DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Wii.cmake")
endif()

project(WiiLibs VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Only warn-as-error when WiiLibs is the top-level project, not when a consumer adds it.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(WL_TOP_LEVEL ON)
else()
    set(WL_TOP_LEVEL OFF)
endif()
option(WL_WERROR "Treat warnings in WiiLibs code as errors" ${WL_TOP_LEVEL})
option(WL_BUILD_TESTS "Build the WiiLibs tests" ${WL_TOP_LEVEL})

include(cmake/WiiLibs.cmake)

add_subdirectory(third_party)

# Every module is wl/<name>/ with its own CMakeLists.txt; it decides which platforms it builds
# on. Order matters only for dependencies (base first).
set(WL_MODULES base codec host nds ctr gcm wiidisc gs brstm sdat gxtex gfx platform audio xbox)
foreach(m IN LISTS WL_MODULES)
    if(EXISTS ${PROJECT_SOURCE_DIR}/wl/${m}/CMakeLists.txt)
        add_subdirectory(wl/${m})
    endif()
endforeach()

if(WL_BUILD_TESTS AND NOT WL_PLATFORM STREQUAL "wii")
    include(CTest)
    foreach(m IN LISTS WL_MODULES)
        if(EXISTS ${PROJECT_SOURCE_DIR}/tests/${m}/CMakeLists.txt)
            add_subdirectory(tests/${m})
        endif()
    endforeach()
    # the Xbox recompilation toolchain (the runtime header, the correctness net)
    add_subdirectory(tests/xbox_recomp)
    if(WL_PLATFORM STREQUAL "host")
        # host tools (tools/*.py) and the CMake helpers over them; Python runs on the build machine,
        # so not under the ppc-be emulator
        add_subdirectory(tests/tools)
    endif()
elseif(WL_BUILD_TESTS)
    # the Wii: the on-console runner, run in Dolphin (ctest -L dolphin)
    include(CTest)
    add_subdirectory(tests/console)
endif()
