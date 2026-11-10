# The CPU UStore's own units are tuned for, or a WebAssembly module's one SIMD capability.

# USTORE_MARCH sets the floor of UStore's own units, which all link `ustore_warnings`: empty keeps the architecture's
# floor, whatever a distribution compiler defaults to, an ISA level like `x86-64-v3` raises it, `native` tunes for this
# host, and a CPU name like `graniterapids` for that CPU, also when cross-compiling. A WebAssembly module takes its one
# SIMD capability instead: serial, v128 or v128relaxed. It lands after the siblings, so MSVC reads which capabilities
# StringZilla found this host runs.
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
    # MSVC has no `/arch:native`, so it takes the `/arch:` of the widest capability this host runs.
    foreach (capability_ IN ITEMS icelake skylake haswell neon)
        if (stringzilla_target_${capability_}_runs AND NOT ustore_arch_flags_)
            set(ustore_arch_flags_ ${stringzilla_target_${capability_}_flags})
        endif ()
    endforeach ()
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${ustore_arch_flags_}>")
elseif (USTORE_MARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64|arm64|ARM64")
    # AArch64 names CPUs through `-mcpu`, like `neoverse-v2`, as `-march` takes only architecture versions.
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-mcpu=${USTORE_MARCH}>")
elseif (USTORE_MARCH)
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-march=${USTORE_MARCH}>")
elseif (MSVC OR APPLE)
    # MSVC's defaults are the floor, and on Apple `-arch` pins it per slice of a universal build.
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64|arm64|ARM64")
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-march=armv8-a>")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64>")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    target_compile_options(ustore_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-march=rv64gc>")
endif ()
