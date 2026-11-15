# GCC cross-compilation toolchain for little-endian 64-bit Power, driving `powerpc64le-linux-gnu-g++-15`.
#
# Usage: cmake --preset linux_ppc64le, then ctest --preset linux_ppc64le, which runs the static binaries under
# qemu-ppc64le.
#
# GCC 15 is the floor: its `#pragma GCC target("power9-vector")` opens the POWER9 half of `altivec.h` inside
# StringZilla's and SmashTable's Power VSX kernels, so every unit compiles at the `power8` floor. Clang never opens it
# without `-mcpu=power9`, so it leaves Power VSX, and with it the `ibm_power10` class, off.
#
# `-D PPC_QEMU_CPU=...`, or the environment variable, picks the emulated CPU: `power10`, the default, runs the
# `ibm_power10` class, while `power8` runs the serial floor alone.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ppc64le)

set(ustore_triple_ powerpc64le-linux-gnu)
set(ustore_sysroot_ /usr/${ustore_triple_})

set(CMAKE_C_COMPILER ${ustore_triple_}-gcc-15)
set(CMAKE_CXX_COMPILER ${ustore_triple_}-g++-15)

set(CMAKE_C_FLAGS_INIT "-mcpu=power8")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=power8")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

if (DEFINED ENV{PPC_QEMU_CPU})
    set(ustore_qemu_cpu_default_ "$ENV{PPC_QEMU_CPU}")
else ()
    set(ustore_qemu_cpu_default_ "power10")
endif ()
set(PPC_QEMU_CPU
    "${ustore_qemu_cpu_default_}"
    CACHE STRING "CPU model for `qemu-ppc64le -cpu`"
)
# Nested try-compile projects reread this file and inherit the environment, not the cache.
set(ENV{PPC_QEMU_CPU} "${PPC_QEMU_CPU}")
set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-ppc64le;-cpu;${PPC_QEMU_CPU};-L;${ustore_sysroot_}")

set(CMAKE_FIND_ROOT_PATH ${ustore_sysroot_})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
