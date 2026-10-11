# SPDX-License-Identifier: MIT
# Big-endian host tests: cross-compile for 32-bit big-endian PowerPC Linux and run
# the tests under qemu-user. Same byte order and word size as the Wii's Broadway CPU.
# Packages (Ubuntu): gcc-powerpc-linux-gnu qemu-user. Binaries are linked statically, so no
# sysroot (QEMU_LD_PREFIX) is needed at run time.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ppc)
set(CMAKE_C_COMPILER powerpc-linux-gnu-gcc)
set(CMAKE_AR powerpc-linux-gnu-ar CACHE FILEPATH "")
set(CMAKE_RANLIB powerpc-linux-gnu-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP powerpc-linux-gnu-strip CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH /usr/powerpc-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-ppc)
