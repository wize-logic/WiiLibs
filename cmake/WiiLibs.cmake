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

# wl_gfx_wrap_sqrtf(<executable>): every call of sqrtf in the program, the libraries' too, is
# wl_sqrtf_exact (wl/gfx/gfx_sqrt.h): the correctly rounded root, bit for bit newlib's, without
# newlib's loop of a bit a step (about 60 instructions on Broadway against about 290). The program
# links wl_gfx, which holds __wrap_sqrtf. On any platform with GNU ld; a build without CMake passes
# -Wl,--wrap=sqrtf -Wl,-u,__wrap_sqrtf and links libwl_gfx.a.
function(wl_gfx_wrap_sqrtf target)
    target_link_libraries(${target} PRIVATE wl_gfx)
    # -u pulls __wrap_sqrtf out of libwl_gfx.a even when only libc's objects call sqrtf
    target_link_options(${target} PRIVATE "LINKER:--wrap=sqrtf" "LINKER:-u,__wrap_sqrtf")
endfunction()

# wl_gfx_wrap_sqrt(<executable>): the same for sqrt, double: every call is wl_sqrt_exact (wl/gfx/gfx_sqrt.h),
# the bits newlib's sqrt gives, without its bit-a-step loop.
function(wl_gfx_wrap_sqrt target)
    target_link_libraries(${target} PRIVATE wl_gfx)
    target_link_options(${target} PRIVATE "LINKER:--wrap=sqrt" "LINKER:-u,__wrap_sqrt")
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

# wl_add_fma_check(<target> [LINKED] [OBJDUMP <objdump>] [EXCLUDE <pattern>...]
#                  [EXCLUDE_FUNCTION <pattern>...] [INCLUDE <pattern>...]
#                  [INCLUDE_FUNCTION <pattern>...] [DISASSEMBLER_OPTIONS <options>])
# Fails <target>'s build if its code has a fused multiply-add (tools/fma_check.py), as a step after
# it links: for a program whose results must match another machine's bit for bit (build it with
# wl_target_defaults(... DETERMINISTIC), which compiles with -ffp-contract=off; the check shows
# where a compiler fused one regardless). It reads the target's own object files; with LINKED, the
# linked file instead (the ELF, which has the libraries' code too: EXCLUDE_FUNCTION and
# INCLUDE_FUNCTION choose among their functions). The patterns are the tool's (shell patterns on an
# object's path or an archive member, and on function names). OBJDUMP defaults to CMAKE_OBJDUMP when
# cross compiling, else to the tool's own search (devkitPPC's, then PowerPC Linux's).
function(wl_add_fma_check target)
    cmake_parse_arguments(A "LINKED" "OBJDUMP;DISASSEMBLER_OPTIONS"
        "EXCLUDE;EXCLUDE_FUNCTION;INCLUDE;INCLUDE_FUNCTION" ${ARGN})
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "wl_add_fma_check(${target}): unexpected arguments ${A_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "wl_add_fma_check(${target}): not a target")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # (found from this file, not PROJECT_SOURCE_DIR, which is the caller's when WiiLibs is a
    # subdirectory of another project)
    get_filename_component(tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/fma_check.py" ABSOLUTE)
    set(cmd ${Python3_EXECUTABLE} ${tool} --quiet)
    if(A_OBJDUMP)
        list(APPEND cmd --objdump ${A_OBJDUMP})
    elseif(CMAKE_OBJDUMP AND NOT WL_PLATFORM STREQUAL "host")
        list(APPEND cmd --objdump ${CMAKE_OBJDUMP})
    endif()
    if(DEFINED A_DISASSEMBLER_OPTIONS)
        list(APPEND cmd --disassembler-options "${A_DISASSEMBLER_OPTIONS}")
    endif()
    foreach(kind EXCLUDE EXCLUDE_FUNCTION INCLUDE INCLUDE_FUNCTION)
        string(TOLOWER "${kind}" lower)
        string(REPLACE "_" "-" flag "${lower}")
        foreach(pattern IN LISTS A_${kind})
            list(APPEND cmd --${flag} "${pattern}")
        endforeach()
    endforeach()
    if(A_LINKED)
        list(APPEND cmd $<TARGET_FILE:${target}>)
    else()
        list(APPEND cmd $<TARGET_OBJECTS:${target}>)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${cmd}
        COMMENT "Checking ${target} for fused multiply-adds"
        COMMAND_EXPAND_LISTS VERBATIM)
endfunction()

# wl_add_xbox_title(<target> XBE <default.xbe> [TITLE_CONFIG <json>] [OPT <-Os|-O2|...>]
#                   [SOURCES <files>...] [RUNTIME <targets>...] [CHUNKS <n>] [NO_EXECUTABLE])
# Recompiles an Xbox title for this platform. At build time tools/xbox_recomp.py runs xboxrecomp's
# pipeline (vendored in third_party/xboxrecomp, WiiLibs' patches applied in the build tree) on the
# XBE: parse, disassemble every executable section, identify, recover the calling conventions,
# lift to C. Its output stays in the build tree (${CMAKE_CURRENT_BINARY_DIR}/<target>_recomp/:
# derived from the title's code, never commit it); each stage reruns only when its inputs change.
# The C is compiled into the object library <target>_code with the flags it needs
# (-ffp-contract=off -fno-strict-aliasing -w, OPT, default -Os; on the Wii -msdata=eabi and
# WL_XB_EA_BASE_ZERO) against WiiLibs' big-endian runtime header (wl/xboxrt/recomp/recomp_types.h,
# README.md there lists the symbols a runtime must define). Then the executable <target>: SOURCES
# (the title project's own code: hand-written overrides, data) plus the generated code, linked with
# RUNTIME (default WiiLibs::xboxrt). On the Wii it also gets its .dol (ogc_create_dol).
# TITLE_CONFIG is the title's JSON (tools/xbox_recomp.py documents the keys: manual functions,
# seeds, extra options). CHUNKS (default 32) is how many translation units the code is spread over.
# The functions the config's "hot_functions" names (or the file in the cache variable
# WL_XBOX_HOT_FUNCTIONS: sub_XXXXXXXX names, a profile report cut to them will do) are compiled at
# WL_XBOX_HOT_OPT (default -O2) in a unit of their own, tu/recomp_tu_hot.c.
# NO_EXECUTABLE makes only <target>_code. The driver's Python needs capstone; set
# WL_XBOX_RECOMP_PYTHON to choose the interpreter.
# The title config's "vertex_programs" names the list of the title's NV2A vertex programs (relative
# to the config; the runtime appends each program it draws with that is not compiled in to
# <hard disk folder>/vsh_programs.inc, so a config gives "hdd/vsh_programs.inc"): at build time
# tools/xbox_vsh_codegen.c (built for the build machine with WL_HOST_C_COMPILER, default cc) makes
# it C, ${CMAKE_CURRENT_BINARY_DIR}/<target>_recomp/vsh/vsh_natives.c (derived from the title: never
# commit it or the list), compiled into the executable (-O2, no fused multiply-adds: the same bits
# as the interpreter), whose wl_xb_title_vsh_natives the runtime finds them by. A list that is not
# there yet is an empty table. The executable also gets sqrtf and sqrt as wl_sqrtf_exact and
# wl_sqrt_exact (wl_gfx_wrap_sqrtf, wl_gfx_wrap_sqrt: the same bits, faster on the Wii).
# (a cache entry: a project that adds WiiLibs with add_subdirectory calls the functions below from its
# own scope, where a plain variable of this directory is not seen)
set(WL_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE INTERNAL "WiiLibs' cmake directory")
function(wl_add_xbox_title target)
    cmake_parse_arguments(A "NO_EXECUTABLE" "XBE;TITLE_CONFIG;OPT;CHUNKS" "SOURCES;RUNTIME" ${ARGN})
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "wl_add_xbox_title(${target}): unexpected arguments ${A_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT A_XBE)
        message(FATAL_ERROR "wl_add_xbox_title(${target}): XBE is required")
    endif()
    get_filename_component(xbe "${A_XBE}" ABSOLUTE)
    if(NOT A_OPT)
        set(A_OPT -Os)
    endif()
    if(NOT A_CHUNKS)
        set(A_CHUNKS 32)
    endif()
    get_filename_component(wl_root "${WL_CMAKE_DIR}/.." ABSOLUTE)
    set(driver ${wl_root}/tools/xbox_recomp.py)
    if(WL_XBOX_RECOMP_PYTHON)
        set(python ${WL_XBOX_RECOMP_PYTHON})
    else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(python ${Python3_EXECUTABLE})
    endif()
    execute_process(COMMAND ${python} -c "import capstone" RESULT_VARIABLE no_capstone
                    OUTPUT_QUIET ERROR_QUIET)
    if(no_capstone)
        message(FATAL_ERROR "wl_add_xbox_title(${target}): ${python} has no capstone module "
            "(pip install capstone), which xboxrecomp's disassembler needs; or set WL_XBOX_RECOMP_PYTHON")
    endif()

    set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}_recomp)
    set(stamp ${out}/xbox_recomp.stamp)
    set(units "")
    math(EXPR last "${A_CHUNKS} - 1")
    foreach(i RANGE ${last})
        if(i LESS 10)
            set(i "0${i}")
        endif()
        list(APPEND units ${out}/tu/recomp_tu_${i}.c)
    endforeach()
    list(APPEND units ${out}/tu/recomp_tu_dispatch.c ${out}/tu/recomp_tu_stubs.c ${out}/tu/recomp_tu_patches.c
         ${out}/tu/recomp_tu_hot.c)
    # rerun when the pipeline, its patches or the driver change (the driver's own stage cache
    # decides what actually reruns)
    file(GLOB_RECURSE pipeline_files CONFIGURE_DEPENDS
        ${wl_root}/third_party/xboxrecomp/tools/*.py ${wl_root}/third_party/xboxrecomp/patches/*.patch)
    set(cmd ${python} ${driver} run ${xbe} -o ${out} --chunks ${A_CHUNKS} --stamp ${stamp} --quiet)
    set(deps ${xbe} ${driver} ${wl_root}/third_party/xboxrecomp/UPSTREAM.json ${pipeline_files})
    if(WL_XBOX_HOT_FUNCTIONS)
        get_filename_component(hot_list "${WL_XBOX_HOT_FUNCTIONS}" ABSOLUTE)
        list(APPEND cmd --hot-functions ${hot_list})
        list(APPEND deps ${hot_list})
    endif()
    if(A_TITLE_CONFIG)
        get_filename_component(config "${A_TITLE_CONFIG}" ABSOLUTE)
        list(APPEND cmd --config ${config})
        list(APPEND deps ${config})
        # the files the config names are inputs too: a seed file the runtime added a function to
        # must rerun the pipeline (relative paths are relative to the config)
        get_filename_component(config_dir "${config}" DIRECTORY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${config})
        file(READ ${config} config_text)
        foreach(key seed_functions coalesce_functions manual_functions trace_functions icall_sites hot_functions)
            string(JSON kind ERROR_VARIABLE no_key TYPE "${config_text}" ${key})
            if(no_key)
                continue()
            endif()
            set(items "")
            if(kind STREQUAL "ARRAY")
                string(JSON n LENGTH "${config_text}" ${key})
                if(n GREATER 0)
                    math(EXPR last_item "${n} - 1")
                    foreach(k RANGE ${last_item})
                        string(JSON item GET "${config_text}" ${key} ${k})
                        list(APPEND items "${item}")
                    endforeach()
                endif()
            elseif(kind STREQUAL "STRING")
                string(JSON item GET "${config_text}" ${key})
                list(APPEND items "${item}")
            endif()
            foreach(item IN LISTS items)
                if(item MATCHES "^0[xX][0-9a-fA-F]+$" OR item MATCHES "^[0-9]+$")
                    continue() # an address, not a file
                endif()
                if(NOT IS_ABSOLUTE "${item}")
                    set(item "${config_dir}/${item}")
                endif()
                if(EXISTS "${item}")
                    list(APPEND deps "${item}")
                endif()
            endforeach()
        endforeach()
    endif()
    add_custom_command(OUTPUT ${stamp}
        BYPRODUCTS ${units}
        COMMAND ${cmd}
        DEPENDS ${deps}
        COMMENT "Recompiling ${xbe} (xboxrecomp pipeline)"
        VERBATIM)
    add_custom_target(${target}_recomp DEPENDS ${stamp})

    add_library(${target}_code OBJECT ${units})
    add_dependencies(${target}_code ${target}_recomp)
    set_source_files_properties(${units} PROPERTIES GENERATED TRUE)
    if(NOT WL_XBOX_HOT_OPT)
        set(WL_XBOX_HOT_OPT -O2)
    endif()
    # after the target's ${A_OPT}: the last -O wins
    set_source_files_properties(${out}/tu/recomp_tu_hot.c PROPERTIES COMPILE_OPTIONS "${WL_XBOX_HOT_OPT}")
    set_target_properties(${target}_code PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_include_directories(${target}_code PUBLIC ${wl_root}/wl/xboxrt/recomp ${out}/gen)
    target_compile_options(${target}_code PRIVATE ${A_OPT} -ffp-contract=off -fno-strict-aliasing -w)
    if(WL_PLATFORM STREQUAL "wii")
        # guest memory is mapped at effective address 0 (the runtime's MMU setup), and the register
        # globals are reached through r13 (devkitPPC's crt0 sets it; libogc's own code uses data)
        target_compile_definitions(${target}_code PUBLIC WL_XB_EA_BASE_ZERO)
        target_compile_options(${target}_code PRIVATE -msdata=eabi)
    endif()
    if(A_NO_EXECUTABLE)
        return()
    endif()

    add_executable(${target} ${A_SOURCES})
    target_link_libraries(${target} PRIVATE ${target}_code)
    if(A_RUNTIME)
        target_link_libraries(${target} PRIVATE ${A_RUNTIME})
    elseif(TARGET WiiLibs::xboxrt)
        target_link_libraries(${target} PRIVATE WiiLibs::xboxrt)
    else()
        # TODO(xboxrt): WiiLibs' Xbox runtime (wl/xboxrt, target WiiLibs::xboxrt: guest memory,
        # register file, threads, kernel, devices) is being written; until it exists a title links
        # whatever RUNTIME it passes, or its SOURCES must define the runtime ABI themselves
        # (wl/xboxrt/recomp/README.md).
        message(WARNING "wl_add_xbox_title(${target}): WiiLibs::xboxrt does not exist yet and no "
            "RUNTIME was given; the link needs the runtime ABI from SOURCES")
    endif()
    target_link_libraries(${target} PRIVATE m)
    wl_gfx_wrap_sqrtf(${target})
    wl_gfx_wrap_sqrt(${target})
    # the title's vertex programs compiled ahead of time
    set(vsh_list "")
    if(A_TITLE_CONFIG)
        string(JSON vsh_list ERROR_VARIABLE no_vsh_list GET "${config_text}" vertex_programs)
        if(no_vsh_list)
            set(vsh_list "")
        elseif(NOT IS_ABSOLUTE "${vsh_list}")
            set(vsh_list "${config_dir}/${vsh_list}")
        endif()
    endif()
    if(vsh_list)
        if(NOT WL_HOST_C_COMPILER)
            find_program(WL_HOST_C_COMPILER NAMES cc gcc clang)
        endif()
        if(NOT WL_HOST_C_COMPILER)
            message(FATAL_ERROR "wl_add_xbox_title(${target}): no C compiler for the build machine "
                "(tools/xbox_vsh_codegen.c); set WL_HOST_C_COMPILER")
        endif()
        set(vsh_dir ${out}/vsh)
        set(vsh_tool ${vsh_dir}/xbox_vsh_codegen${CMAKE_HOST_EXECUTABLE_SUFFIX})
        set(vsh_c ${vsh_dir}/vsh_natives.c)
        add_custom_command(OUTPUT ${vsh_tool}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${vsh_dir}
            COMMAND ${WL_HOST_C_COMPILER} -O1 -std=c11 -ffp-contract=off -I${wl_root}
                ${wl_root}/tools/xbox_vsh_codegen.c ${wl_root}/wl/gfx/gfx_sqrt.c -lm -o ${vsh_tool}
            DEPENDS ${wl_root}/tools/xbox_vsh_codegen.c ${wl_root}/wl/xbox/vsh.c ${wl_root}/wl/xbox/vsh.h
                ${wl_root}/wl/xbox/vsh_native.h ${wl_root}/wl/gfx/gfx_sqrt.c ${wl_root}/wl/gfx/gfx_sqrt.h
            COMMENT "Building the vertex program compiler for the build machine"
            VERBATIM)
        # (run at every build, the list being the runtime's; the C is replaced only when it changes)
        add_custom_target(${target}_vsh
            COMMAND ${vsh_tool} --if-exists --accessor wl_xb_title_vsh_natives ${vsh_list} ${vsh_c}.new
                wl_xb_vsh_natives
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${vsh_c}.new ${vsh_c}
            BYPRODUCTS ${vsh_c}
            DEPENDS ${vsh_tool}
            COMMENT "Compiling the vertex programs of ${vsh_list}"
            VERBATIM)
        target_sources(${target} PRIVATE ${vsh_c})
        set_source_files_properties(${vsh_c} PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-O2;-ffp-contract=off")
        add_dependencies(${target} ${target}_vsh)
    endif()
    if(WL_PLATFORM STREQUAL "wii")
        ogc_create_dol(${target})
    endif()
endfunction()
