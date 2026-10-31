# The install rules, the export sets, the CMake package files, and the preview SDK archive.

# Both libraries install with a CMake package, `find_package(ustore)` naming them `ustore::shared` and `ustore::static`,
# in three components. The `sdk` takes the shared library, which links every engine inside, with the C headers and the
# C++ wrappers; `static` takes the archive and the engine archives it links, `ustore::rocksdb`, `ustore::leveldb` and
# `ustore::uring`; `core` takes the header-only templates. Each library component exports its own set, so the package
# loads whichever the prefix holds.
if (USTORE_INSTALL)
    include(CMakePackageConfigHelpers)
    set(ustore_config_install_dir_ "${CMAKE_INSTALL_LIBDIR}/cmake/ustore")
    # The archive holds the device merger, whose consumers also link the static CUDA runtime it calls where it has one.
    set(ustore_static_archives_ ustore_static ${ustore_engine_archives_})
    set(ustore_static_dependencies_)
    if (ustore_device_compaction_)
        set(ustore_static_dependencies_ "find_dependency(CUDAToolkit)")
    endif ()
    install(
        TARGETS ustore_header ustore_shared
        EXPORT ustoreTargets
        COMPONENT sdk
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(
        TARGETS ${ustore_static_archives_}
        EXPORT ustoreStaticTargets
        COMPONENT static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(
        EXPORT ustoreTargets
        NAMESPACE ustore::
        DESTINATION ${ustore_config_install_dir_}
        COMPONENT sdk
    )
    install(
        EXPORT ustoreStaticTargets
        NAMESPACE ustore::
        DESTINATION ${ustore_config_install_dir_}
        COMPONENT static
    )
    file(
        CONFIGURE
        OUTPUT
        "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake"
        CONTENT
        [[include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/ustoreTargets.cmake")
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/ustoreStaticTargets.cmake")
    @ustore_static_dependencies_@
    include("${CMAKE_CURRENT_LIST_DIR}/ustoreStaticTargets.cmake")
endif ()
]]
        @ONLY
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake"
        DESTINATION ${ustore_config_install_dir_}
        COMPONENT sdk
    )
    install(
        DIRECTORY c/ustore cpp/ustore
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT sdk
    )
    install(
        DIRECTORY include/ustore
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT core
    )
    install(
        FILES LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ustore
        COMPONENT sdk
    )

    # A `.pc` for the consumers CMake never reaches. Spelling the prefix against `${pcfiledir}` keeps it correct after
    # `cmake --install --prefix` moves the tree. `GNUInstallDirs` returns a relative path unless a distro forced an
    # absolute one, and only the former may follow a prefix.
    cmake_path(
        RELATIVE_PATH CMAKE_INSTALL_PREFIX BASE_DIRECTORY "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" OUTPUT_VARIABLE
        ustore_pc_relative_prefix_
    )
    set(ustore_pc_prefix_ "\${pcfiledir}/${ustore_pc_relative_prefix_}")
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
    configure_file(cmake/ustore.pc.in "${CMAKE_CURRENT_BINARY_DIR}/ustore.pc" @ONLY)
    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/ustore.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
        COMPONENT sdk
    )
endif ()

# The preview SDK: `cpack --config build_release/CPackConfig.cmake` archives the `sdk` component alone, the stripped
# shared library with the C headers, the C++ wrappers, the CMake package, the `.pc` and the license, as
# `ustore-<version>-<platform>-sdk.tar.gz`. A parent project packages itself.
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
