# The CPU UStore's own units are tuned for, or a WebAssembly module's one SIMD capability.

# USTORE_TARGET_ARCH tunes UStore's own units, which all link `ustore_warnings`, for one CPU: `native` for this host, or
# a CPU name like `graniterapids`, also when cross-compiling. A WebAssembly module takes its one SIMD capability
# instead: serial, v128 or v128relaxed. Fetched dependencies keep their own flags.
if (USTORE_TARGET_ARCH STREQUAL "native" AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "USTORE_TARGET_ARCH=native needs the host as the target; name a CPU instead")
elseif (EMSCRIPTEN OR CMAKE_SYSTEM_PROCESSOR MATCHES "wasm")
    if (USTORE_TARGET_ARCH STREQUAL "v128relaxed")
        target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-msimd128;-mrelaxed-simd>")
    elseif (USTORE_TARGET_ARCH STREQUAL "v128")
        target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-msimd128>")
    elseif (USTORE_TARGET_ARCH AND NOT USTORE_TARGET_ARCH STREQUAL "serial")
        message(
            FATAL_ERROR "USTORE_TARGET_ARCH on WebAssembly: serial, v128 or v128relaxed, not `${USTORE_TARGET_ARCH}`"
        )
    endif ()
elseif (
    USTORE_TARGET_ARCH
    AND MSVC
    AND NOT USTORE_TARGET_ARCH STREQUAL "native"
)
    message(FATAL_ERROR "MSVC takes USTORE_TARGET_ARCH=native only; pass an /arch: value such as AVX2 or AVX512 "
                        "through CMAKE_CXX_FLAGS instead of `${USTORE_TARGET_ARCH}`"
    )
elseif (USTORE_TARGET_ARCH AND MSVC)
    # MSVC has no `/arch:native`, and no UStore unit needs instruction-set flags, so the toolchain default stays.
elseif (USTORE_TARGET_ARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64|arm64|ARM64")
    # AArch64 names CPUs through `-mcpu`, like `neoverse-v2`, as `-march` takes only architecture versions.
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${USTORE_TARGET_ARCH}>")
elseif (USTORE_TARGET_ARCH)
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-march=${USTORE_TARGET_ARCH}>")
endif ()
