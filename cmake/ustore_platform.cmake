# The CPU UStore's own units are tuned for, or a WebAssembly module's one SIMD capability.

# USTORE_MARCH tunes UStore's own units, which all link `ustore_warnings`, for one CPU: an ISA level like `x86-64-v3`,
# `native` for this host, or a CPU name like `graniterapids`, also when cross-compiling. A WebAssembly module takes its
# one SIMD capability instead: serial, v128 or v128relaxed. Fetched dependencies keep their own flags.
if (USTORE_MARCH STREQUAL "native" AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "USTORE_MARCH=native needs the host as the target; name a CPU instead")
elseif (EMSCRIPTEN OR CMAKE_SYSTEM_PROCESSOR MATCHES "wasm")
    if (USTORE_MARCH STREQUAL "v128relaxed")
        target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-msimd128;-mrelaxed-simd>")
    elseif (USTORE_MARCH STREQUAL "v128")
        target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-msimd128>")
    elseif (USTORE_MARCH AND NOT USTORE_MARCH STREQUAL "serial")
        message(FATAL_ERROR "USTORE_MARCH on WebAssembly: serial, v128 or v128relaxed, not `${USTORE_MARCH}`")
    endif ()
elseif (
    USTORE_MARCH
    AND MSVC
    AND NOT USTORE_MARCH STREQUAL "native"
)
    message(FATAL_ERROR "MSVC takes USTORE_MARCH=native only; pass an /arch: value such as AVX2 or AVX512 "
                        "through CMAKE_CXX_FLAGS instead of `${USTORE_MARCH}`"
    )
elseif (USTORE_MARCH AND MSVC)
    # MSVC has no `/arch:native`, and no UStore unit needs instruction-set flags, so the toolchain default stays.
elseif (USTORE_MARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64|arm64|ARM64")
    # AArch64 names CPUs through `-mcpu`, like `neoverse-v2`, as `-march` takes only architecture versions.
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${USTORE_MARCH}>")
elseif (USTORE_MARCH)
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-march=${USTORE_MARCH}>")
endif ()
