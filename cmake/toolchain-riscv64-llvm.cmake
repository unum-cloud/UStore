# Clang cross-compilation toolchain for 64-bit RISC-V, driving `clang++-23` over the distribution's cross libstdc++.
#
# Usage: cmake --preset linux_riscv64, then ctest --preset linux_riscv64, which runs the static binaries under
# qemu-riscv64.
#
# StringZilla's RVV kernels need Clang 21 or newer; with an older compiler they probe as 0 and the `alibaba_c930` class
# drops out. Every unit compiles at the `rv64gc` floor, and each kernel picks its own extensions per function.
#
# `-D RISCV_QEMU_CPU=...`, or the environment variable, picks the emulated CPU: `max`, the default, runs the
# `alibaba_c930` class, while `rv64` runs the serial floor alone.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(ustore_triple_ riscv64-linux-gnu)
set(ustore_sysroot_ /usr/${ustore_triple_})
# The newest cross GCC installed holds the libstdc++ the static binaries link.
file(
    GLOB ustore_gcc_library_directories_
    LIST_DIRECTORIES TRUE
    /usr/lib/gcc-cross/${ustore_triple_}/*
)
list(
    SORT ustore_gcc_library_directories_
    COMPARE NATURAL
    ORDER DESCENDING
)
list(GET ustore_gcc_library_directories_ 0 ustore_gcc_library_directory_)

set(CMAKE_C_COMPILER clang-23)
set(CMAKE_CXX_COMPILER clang++-23)

# No `--sysroot`: the distribution cross packages install into the build root, and their `libc.so` script names
# `/usr/riscv64-linux-gnu/lib` absolutely, which the linker would prefix with the sysroot a second time.
set(ustore_target_flags_ "--target=${ustore_triple_} --gcc-toolchain=/usr -march=rv64gc")

set(CMAKE_C_FLAGS_INIT "${ustore_target_flags_}")
set(CMAKE_CXX_FLAGS_INIT "${ustore_target_flags_} -stdlib=libstdc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -stdlib=libstdc++ -L${ustore_gcc_library_directory_}")

if (DEFINED ENV{RISCV_QEMU_CPU})
    set(ustore_qemu_cpu_default_ "$ENV{RISCV_QEMU_CPU}")
else ()
    set(ustore_qemu_cpu_default_ "max")
endif ()
set(RISCV_QEMU_CPU
    "${ustore_qemu_cpu_default_}"
    CACHE STRING "CPU model for `qemu-riscv64 -cpu`"
)
# Nested try-compile projects reread this file and inherit the environment, not the cache.
set(ENV{RISCV_QEMU_CPU} "${RISCV_QEMU_CPU}")
set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-riscv64;-cpu;${RISCV_QEMU_CPU};-L;${ustore_sysroot_}")

set(CMAKE_FIND_ROOT_PATH ${ustore_sysroot_})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
