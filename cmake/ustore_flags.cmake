# The warning flags and sanitizers UStore's own targets take through `ustore_warnings` and `ustore_sanitizers`.

# Warnings apply to UStore's own targets only, so fetched dependencies build with their own flags.
add_library(ustore_warnings INTERFACE)
if (MSVC)
    target_compile_options(ustore_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
    if (USTORE_WERROR)
        target_compile_options(ustore_warnings INTERFACE /WX)
    endif ()
else ()
    # The C ABI takes named arguments as partially designated structs, leaving the rest zeroed. CUDA sources take the
    # compactor's own flags instead, since nvcc's host pass reads line markers that pedantic mode rejects.
    target_compile_options(
        ustore_warnings
        INTERFACE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic;-Wno-unknown-pragmas;-Wno-missing-field-initializers>"
            "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CXX_COMPILER_ID:Clang,AppleClang>>:-Wdocumentation>"
    )
    if (USTORE_WERROR)
        target_compile_options(ustore_warnings INTERFACE -Werror)
    endif ()
endif ()

# Sanitizers instrument UStore's own targets and the sibling headers they compile, while RocksDB, LevelDB, liburing,
# fmt, yyjson and the sibling archives keep their release flags. -O1 keeps the instrumented suites tolerable outside
# Debug.
add_library(ustore_sanitizers INTERFACE)
if (USTORE_BUILD_SANITIZERS STREQUAL "address")
    if (MSVC)
        set(ustore_sanitizer_flags_ /fsanitize=address)
    else ()
        set(ustore_sanitizer_flags_ -fsanitize=address,undefined)
    endif ()
elseif (USTORE_BUILD_SANITIZERS STREQUAL "thread")
    if (MSVC)
        message(FATAL_ERROR "MSVC has no ThreadSanitizer, so USTORE_BUILD_SANITIZERS=thread needs Clang or GCC")
    endif ()
    set(ustore_sanitizer_flags_ -fsanitize=thread)
elseif (NOT USTORE_BUILD_SANITIZERS STREQUAL "off")
    message(FATAL_ERROR "USTORE_BUILD_SANITIZERS is off, address or thread, not '${USTORE_BUILD_SANITIZERS}'")
endif ()
if (ustore_sanitizer_flags_ AND NOT MSVC)
    target_compile_options(
        ustore_sanitizers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${ustore_sanitizer_flags_};-fno-omit-frame-pointer>"
                                    "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<CONFIG:Debug>>>:-O1>"
    )
    target_link_options(ustore_sanitizers INTERFACE "$<$<LINK_LANGUAGE:C,CXX>:${ustore_sanitizer_flags_}>")
elseif (ustore_sanitizer_flags_)
    target_compile_options(ustore_sanitizers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${ustore_sanitizer_flags_}>")
endif ()
