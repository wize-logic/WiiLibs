# SPDX-License-Identifier: MIT
# Shared build helpers: warning flags for WiiLibs code and the wl_add_test helper.

set(WL_WARNING_FLAGS
    -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes
    -Wvla -Wundef)

# wl_target_defaults(<target> [DETERMINISTIC]): warnings and the include root (so headers are
# included as "wl/<module>/<file>.h"). DETERMINISTIC adds -ffp-contract=off, for code whose
# results must match bit for bit across CPUs.
function(wl_target_defaults target)
    cmake_parse_arguments(A "DETERMINISTIC" "" "" ${ARGN})
    target_compile_options(${target} PRIVATE ${WL_WARNING_FLAGS})
    if(WL_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(A_DETERMINISTIC)
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>)
endfunction()

# wl_add_test(<name> SOURCES <files...> [LIBS <targets...>] [LABELS <labels...>] [ARGS <args...>])
# Builds a greatest test executable and registers it with ctest. Labels: unit, be (also runs
# on the big-endian ppc-be build), dump (needs real game files, skipped without them).
function(wl_add_test name)
    cmake_parse_arguments(A "" "" "SOURCES;LIBS;LABELS;ARGS" ${ARGN})
    if(NOT A_LABELS)
        set(A_LABELS unit)
    endif()
    add_executable(${name} ${A_SOURCES})
    wl_target_defaults(${name})
    target_link_libraries(${name} PRIVATE greatest ${A_LIBS})
    add_test(NAME ${name} COMMAND ${name} ${A_ARGS})
    set_tests_properties(${name} PROPERTIES LABELS "${A_LABELS}")
endfunction()

# wl_platform_trace_exits(<executable>): the program logs every way out of itself ("exit(0) from
# 80001234 ...", wl/platform/backtrace.h) by wrapping exit, _exit, abort and SYS_ResetSystem with
# the functions in wl/platform/wii/trace_wii.c. Wii only; elsewhere it does nothing.
function(wl_platform_trace_exits target)
    if(WL_PLATFORM STREQUAL "wii")
        target_link_options(${target} PRIVATE
            "LINKER:--wrap=exit,--wrap=_exit,--wrap=abort,--wrap=SYS_ResetSystem")
    endif()
endfunction()

# wl_platform_heap_overflow(<executable>): malloc and its relatives spill from newlib's heap into a
# wl_mem heap (wl/platform/heap_overflow.h) once newlib's is full, by wrapping them with the
# functions in wl/platform/wii/heap_wrap_wii.c. Call wl_heap_overflow_enable(tag) after
# wl_mem_init to arm it. Wii only; elsewhere it does nothing. A build without CMake passes
# --wrap=<name> for each name in WL_HEAP_OVERFLOW_WRAPS and -u __wrap_malloc itself.
set(WL_HEAP_OVERFLOW_WRAPS malloc calloc realloc free memalign aligned_alloc malloc_usable_size
    _malloc_r _calloc_r _realloc_r _free_r _memalign_r _malloc_usable_size_r)
function(wl_platform_heap_overflow target)
    if(WL_PLATFORM STREQUAL "wii")
        set(flags "")
        foreach(name IN LISTS WL_HEAP_OVERFLOW_WRAPS)
            list(APPEND flags "--wrap=${name}")
        endforeach()
        list(JOIN flags "," joined)
        # -u pulls the wrappers out of libwl_platform.a even when only libc's objects call malloc
        target_link_options(${target} PRIVATE "LINKER:${joined}" "LINKER:-u,__wrap_malloc")
    endif()
endfunction()

# wl_add_hbc_package(<target> NAME <name> [DOL <file>] [OUTPUT <zip>] [APP_DIR <dir>] [CODER <c>]
#                    [VERSION <v>] [SHORT_DESCRIPTION <s>] [LONG_DESCRIPTION <l> |
#                    LONG_DESCRIPTION_FILE <file>] [ICON <128x48.png> | ICON_TEXT <line> [<line>]]
#                    [EXTRA <src[:dest]>...] [RELEASE_DATE <YYYYMMDDHHMMSS>] [AHB_ACCESS] [ALL])
# Zips a Wii program for the Homebrew Channel (tools/wii_package.py): apps/<APP_DIR>/boot.dol,
# meta.xml and icon.png, plus the EXTRA files (a folder is added with everything under it) at
# dest inside the app's folder. <target> is the program's executable target, already given its
# .dol with ogc_create_dol (call this after it): the zip is rebuilt when the program is. Without
# such a target, DOL names the .dol file (for a program built elsewhere). It adds the target
# <target>_hbc (part of ALL with the ALL keyword) whose output, <OUTPUT> (default
# ${CMAKE_CURRENT_BINARY_DIR}/<target>.zip), is also its HBC_ZIP property. APP_DIR defaults to
# NAME as lower case words joined by dashes (values cannot hold a semicolon, which CMake splits
# at: LONG_DESCRIPTION_FILE takes text from a file); without an icon one is drawn from NAME. The release
# date is the build's time (SOURCE_DATE_EPOCH, if set, for a reproducible zip) unless given.
function(wl_add_hbc_package target)
    cmake_parse_arguments(A "AHB_ACCESS;ALL"
        "NAME;DOL;OUTPUT;APP_DIR;CODER;VERSION;SHORT_DESCRIPTION;LONG_DESCRIPTION;LONG_DESCRIPTION_FILE;ICON;RELEASE_DATE"
        "ICON_TEXT;EXTRA" ${ARGN})
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "wl_add_hbc_package(${target}): unexpected arguments ${A_UNPARSED_ARGUMENTS} "
            "(a semicolon splits a value: put long text in LONG_DESCRIPTION_FILE)")
    endif()
    if(NOT A_NAME)
        message(FATAL_ERROR "wl_add_hbc_package(${target}): NAME is required")
    endif()
    if(A_ICON AND A_ICON_TEXT)
        message(FATAL_ERROR "wl_add_hbc_package(${target}): ICON and ICON_TEXT are alternatives")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(dol "${A_DOL}")
    set(deps "")
    if(TARGET ${target})
        list(APPEND deps ${target})
        if(NOT dol)
            get_target_property(dol ${target} DKP_FILE)
            if(NOT dol)
                message(FATAL_ERROR "wl_add_hbc_package(${target}): the target has no .dol "
                    "(call ogc_create_dol first, or give DOL)")
            endif()
        endif()
    elseif(dol)
        list(APPEND deps ${dol})
    else()
        message(FATAL_ERROR "wl_add_hbc_package(${target}): not a target; give DOL")
    endif()
    if(NOT A_OUTPUT)
        set(A_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}.zip")
    endif()
    set(cmd ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/wii_package.py
        --dol ${dol} --output ${A_OUTPUT} --name ${A_NAME})
    foreach(opt APP_DIR CODER VERSION SHORT_DESCRIPTION LONG_DESCRIPTION RELEASE_DATE)
        if(DEFINED A_${opt})
            string(TOLOWER "${opt}" lower)
            string(REPLACE "_" "-" flag "${lower}")
            list(APPEND cmd --${flag} "${A_${opt}}")
        endif()
    endforeach()
    if(A_LONG_DESCRIPTION_FILE)
        list(APPEND cmd --long-description-file ${A_LONG_DESCRIPTION_FILE})
        list(APPEND deps ${A_LONG_DESCRIPTION_FILE})
    endif()
    if(A_ICON)
        list(APPEND cmd --icon ${A_ICON})
        list(APPEND deps ${A_ICON})
    elseif(A_ICON_TEXT)
        list(APPEND cmd --icon-text ${A_ICON_TEXT})
    endif()
    foreach(spec IN LISTS A_EXTRA)
        list(APPEND cmd --extra ${spec})
        string(REGEX REPLACE ":[^:]*$" "" source "${spec}")
        if(EXISTS "${source}" AND NOT IS_DIRECTORY "${source}")
            list(APPEND deps ${source})
        endif()
    endforeach()
    if(A_AHB_ACCESS)
        list(APPEND cmd --ahb-access)
    endif()
    add_custom_command(OUTPUT ${A_OUTPUT}
        COMMAND ${cmd}
        DEPENDS ${deps} ${PROJECT_SOURCE_DIR}/tools/wii_package.py
        COMMENT "Homebrew Channel package ${A_OUTPUT}"
        VERBATIM)
    if(A_ALL)
        add_custom_target(${target}_hbc ALL DEPENDS ${A_OUTPUT})
    else()
        add_custom_target(${target}_hbc DEPENDS ${A_OUTPUT})
    endif()
    set_target_properties(${target}_hbc PROPERTIES HBC_ZIP "${A_OUTPUT}")
endfunction()
