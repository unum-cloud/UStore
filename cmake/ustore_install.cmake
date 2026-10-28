# The install rules, the export set, and the CMake package files.

# Both libraries install with a CMake package, `find_package(ustore)` naming them `ustore::static` and `ustore::shared`;
# the engine archives install beside them as `ustore::rocksdb`, `ustore::leveldb` and `ustore::uring`, which
# `ustore::static` links
if (USTORE_INSTALL)
    include(CMakePackageConfigHelpers)
    set(ustore_config_install_dir_ "${CMAKE_INSTALL_LIBDIR}/cmake/ustore")
    install(
        TARGETS ustore_header ustore_static ustore_shared ${ustore_engine_archives_}
        EXPORT ustoreTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(
        EXPORT ustoreTargets
        NAMESPACE ustore::
        DESTINATION ${ustore_config_install_dir_}
    )
    file(
        WRITE "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake"
        [[include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/ustoreTargets.cmake")
]]
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfig.cmake"
                  "${CMAKE_CURRENT_BINARY_DIR}/ustoreConfigVersion.cmake" DESTINATION ${ustore_config_install_dir_}
    )
    install(DIRECTORY c/ustore cpp/ustore include/ustore DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()
