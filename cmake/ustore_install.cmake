# The install rules, the export sets, the CMake and pkg-config package files, and the preview SDK archive.
#
# The root names what installs before including this file, as each project ships its own set of libraries:
#
# - `ustore_install_sdk_targets_`: the targets of the `sdk` component, exported as `ustoreTargets`;
# - `ustore_install_programs_`: the executables the `sdk` component ships beside its libraries, outside the export set.
#
# One export set holds `ustore::abi`, `ustore::cpp` and both libraries, because `ustore::cpp` links `ustore::abi` and
# `ustore::static`. An empty list installs nothing, and a component no list covers installs from the root after this
# file.

if (USTORE_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(ustore_config_install_dir_ "${CMAKE_INSTALL_LIBDIR}/cmake/ustore")
    if (ustore_install_sdk_targets_)
        install(
            TARGETS ${ustore_install_sdk_targets_}
            EXPORT ustoreTargets
            COMPONENT sdk
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
        install(
            EXPORT ustoreTargets
            NAMESPACE ustore::
            DESTINATION ${ustore_config_install_dir_}
            COMPONENT sdk
        )
    endif ()
    if (ustore_install_programs_)
        install(
            TARGETS ${ustore_install_programs_}
            COMPONENT sdk
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif ()

    # A major version is the only break the C interface admits, so a consumer asking for one accepts its later minors.
    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/ustoreConfig.cmake.in" "${PROJECT_BINARY_DIR}/ustoreConfig.cmake"
        INSTALL_DESTINATION ${ustore_config_install_dir_}
    )
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/ustoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(
        FILES "${PROJECT_BINARY_DIR}/ustoreConfig.cmake" "${PROJECT_BINARY_DIR}/ustoreConfigVersion.cmake"
        DESTINATION ${ustore_config_install_dir_}
        COMPONENT sdk
    )
    install(
        DIRECTORY "${PROJECT_SOURCE_DIR}/c/ustore"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT sdk
    )
    if (IS_DIRECTORY "${PROJECT_SOURCE_DIR}/cpp/ustore")
        install(
            DIRECTORY "${PROJECT_SOURCE_DIR}/cpp/ustore"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            COMPONENT sdk
        )
    endif ()
    install(
        FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ustore
        COMPONENT sdk
    )

    # A `.pc` naming the shared library, for the consumers CMake never reaches, wherever the `sdk` installs one.
    # Spelling the prefix against `${pcfiledir}` keeps it correct after `cmake --install --prefix` moves the tree.
    # `file(RELATIVE_PATH)` ends a path to an ancestor with a slash, which the `.` closes.
    # `GNUInstallDirs` returns a relative path unless a distribution forced an absolute one, and only the former may
    # follow a prefix.
    if (ustore_shared IN_LIST ustore_install_sdk_targets_)
        file(RELATIVE_PATH ustore_pc_to_prefix_ "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
        set(ustore_pc_prefix_ "\${pcfiledir}/${ustore_pc_to_prefix_}.")
        if (IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
            set(ustore_pc_libdir_ "${CMAKE_INSTALL_LIBDIR}")
        else ()
            set(ustore_pc_libdir_ "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
        endif ()
        if (IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
            set(ustore_pc_includedir_ "${CMAKE_INSTALL_INCLUDEDIR}")
        else ()
            set(ustore_pc_includedir_ "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
        endif ()
        configure_file("${PROJECT_SOURCE_DIR}/cmake/ustore.pc.in" "${PROJECT_BINARY_DIR}/ustore.pc" @ONLY)
        install(
            FILES "${PROJECT_BINARY_DIR}/ustore.pc"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
            COMPONENT sdk
        )
    endif ()
endif ()

# The preview SDK: `cpack --config <build>/CPackConfig.cmake` archives the `sdk` component alone, stripped, under one
# top-level directory, as `ustore-<version>-<platform>-sdk.tar.gz`, or a `.zip` on Windows.
# A parent project packages itself.
if (USTORE_INSTALL AND PROJECT_IS_TOP_LEVEL)
    set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
    if (WIN32)
        set(CPACK_GENERATOR ZIP)
    else ()
        set(CPACK_GENERATOR TGZ)
    endif ()
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_COMPONENTS_ALL sdk)
    set(CPACK_STRIP_FILES ON)
    include(CPack)
endif ()
