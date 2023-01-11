cmake_minimum_required(VERSION 3.16)

file(READ "VERSION" UKV_VERSION)
project(UKV
  LANGUAGES C CXX
  VERSION ${UKV_VERSION}
  HOMEPAGE_URL "https://github.com/unum-cloud/ukv"
  DESCRIPTION "Universal Binary DBMS Interface to manage Docs • Graphs • Blobs • Vectors × in Local • Remote × RocksDB • LevelDB • UMem • UDisk × from C • C++ • Python • Java • GoLang"
)

option(UKV_BUILD_ENGINE_UMEM "Building REST API server for all backends" ON)
option(UKV_BUILD_ENGINE_LEVELDB "Building REST API server for all backends")
option(UKV_BUILD_ENGINE_ROCKSDB "Building REST API server for all backends")

option(UKV_BUILD_TESTS "Building C/C++ native tests" ON)
option(UKV_BUILD_BENCHMARKS "Building C/C++ native benchmarks")

option(UKV_BUILD_SDK_PYTHON "Building Python bidings for all SDKs")
option(UKV_BUILD_API_FLIGHT "Building Apache Arrow Flight RPC server and client for all backends")
option(UKV_BUILD_API_FLIGHT_CLIENT "Building Apache Arrow Flight RPC client for all backends")
option(UKV_BUILD_API_FLIGHT_SERVER "Building Apache Arrow Flight RPC server for all backends")
option(UKV_BUILD_API_REST_SERVER "Building REST API server for all backends")

option(UKV_USE_JEMALLOC "Faster allocator, that requires autoconf to be installed")
option(UKV_USE_ONEAPI "Faster concurrency primitives from Intel")
option(UKV_USE_UUID "Replaces default 64-bit keys with 128-bit UUID compatible integers")

set(UKV_ENGINE_UDISK_PATH "" CACHE STRING "Pass a path to UDisk binary to produce a full range of bindings")

if(UKV_BUILD_API_FLIGHT)
  set(UKV_BUILD_API_FLIGHT_SERVER ON)
  set(UKV_BUILD_API_FLIGHT_CLIENT ON)
endif()

# Some settings are mutually dependant
include(CMakeDependentOption)
cmake_dependent_option(UKV_REBUILD_JEMALLOC "Enable to attempt to source and build JeMalloc locally" ON "UKV_USE_JEMALLOC" OFF)
cmake_dependent_option(UKV_REBUILD_ONEAPI "Enable to attempt to source and build Intel OneAPI locally" ON "UKV_USE_ONEAPI" OFF)
cmake_dependent_option(UKV_REBUILD_BOOST "Enable to attempt to source and build Boost locally" ON "UKV_BUILD_API_REST_SERVER" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")
set(CMAKE_CACHEFILE_DIR "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to static libs")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to shared libs")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/bin")

message("CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message("CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
message("CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fstrict-vtable-pointers")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wunknown-attributes")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ferror-limit=1")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic")

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-builtin-memcmp -fPIC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wfatal-errors")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ferror-limit=1")

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Za")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /w2 /DEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MP /INCREMENTAL:NO")
endif()

# Optimizations for common x86 CPUs.
# https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html
if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse -msse4.2 -mavx2") # -march=native
endif()

# Optimizations for ARM CPUs.
if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfloat-abi=softfp -mfpu=neon -march=armv8-a+simd")
endif()

# Risky optimizations, that require tuning.
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -momit-leaf-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -funsafe-math-optimizations")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -funroll-loops")

# Other release build specs.
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -DNDEBUG -O3")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG -O3")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -fvisibility=hidden")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fvisibility=hidden")

# Debugging flags.
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DUKV_DEBUG -g")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DUKV_DEBUG -g")

find_package(Threads REQUIRED)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)

if(NOT ${UKV_BUILD_SDK_PYTHON})
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fsanitize-address-use-after-scope")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fsanitize-address-use-after-scope")
  set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=address -fsanitize-address-use-after-scope")
endif()

# Install 3rd Party Packages
include(FetchContent)
include_directories(include/)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(FETCHCONTENT_QUIET OFF)
else()
  set(FETCHCONTENT_QUIET ON)
endif()

# Engines:
if(${UKV_BUILD_ENGINE_UMEM})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ucset.cmake")
endif()

if(${UKV_BUILD_ENGINE_ROCKSDB})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rocksdb.cmake")
endif()

if(${UKV_BUILD_ENGINE_LEVELDB})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/leveldb.cmake")
endif()

# Modalities:
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fmt.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/json.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/bson.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/yyjson.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/simdjson.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pcre2.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mpack.cmake")

if(${UKV_USE_JEMALLOC})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/jemalloc.cmake")
endif()

if(${UKV_USE_ONEAPI})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/oneapi.cmake")
endif()

# Distributions:
# > UKV_BUILD_ENGINE_UMEM: Uses Arrow Parquet format to save binary collections on disk.
# > UKV_BUILD_API_FLIGHT: Uses Arrow Flight RPC as a client-server communication protocol.
# > UKV_BUILD_SDK_PYTHON: Uses Arrow for zero-copy data exchage with the Python runtime.
if(${UKV_BUILD_SDK_PYTHON} OR ${UKV_BUILD_API_FLIGHT_CLIENT} OR ${UKV_BUILD_API_FLIGHT_SERVER} OR ${UKV_BUILD_ENGINE_UMEM})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/arrow.cmake")
endif()

if(${UKV_BUILD_API_FLIGHT_SERVER})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/clipp.cmake")
endif()

if(${UKV_BUILD_API_REST_SERVER})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/boost.cmake")
endif()

if(${UKV_BUILD_TESTS})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/gtest.cmake")
endif()

if(${UKV_BUILD_BENCHMARKS})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/gtest.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/gbench.cmake")
endif()

set(UKV_ENGINE_NAMES)
set(UKV_CLIENT_LIBS)
set(UKV_TEST_NAMES "test_api" "test_compilation" "test_units" "stress_atomicity" "stress_linearizability")

# Define the Engine libraries we will need to build
if(${UKV_BUILD_ENGINE_UMEM})
  add_library(ukv_embedded_umem src/engine_umem.cpp src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ukv_embedded_umem pthread yyjson simdjson bson pcre2 arrow::parquet arrow::arrow arrow::bundled ${JEMALLOC_LIBRARIES} ${TBB_LIBRARIES})
  target_compile_definitions(ukv_embedded_umem INTERFACE UKV_VERSION="${UKV_VERSION}")
  target_compile_definitions(ukv_embedded_umem INTERFACE UKV_ENGINE_IS_UMEM=1)

  list(APPEND UKV_ENGINE_NAMES "umem")
  list(APPEND UKV_CLIENT_LIBS "ukv_embedded_umem")
endif()

if(${UKV_BUILD_ENGINE_ROCKSDB})
  add_library(ukv_embedded_rocksdb src/engine_rocksdb.cpp src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ukv_embedded_rocksdb rocksdb pthread yyjson simdjson bson pcre2 ${JEMALLOC_LIBRARIES})
  target_compile_definitions(ukv_embedded_rocksdb INTERFACE UKV_VERSION="${UKV_VERSION}")
  target_compile_definitions(ukv_embedded_rocksdb INTERFACE UKV_ENGINE_IS_ROCKSDB=1)

  list(APPEND UKV_ENGINE_NAMES "rocksdb")
  list(APPEND UKV_CLIENT_LIBS "ukv_embedded_rocksdb")
endif()

if(${UKV_BUILD_ENGINE_LEVELDB})
  add_library(ukv_embedded_leveldb src/engine_leveldb.cpp src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ukv_embedded_leveldb leveldb pthread yyjson simdjson bson pcre2 ${JEMALLOC_LIBRARIES})
  set_source_files_properties(src/engine_leveldb.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
  target_compile_definitions(ukv_embedded_leveldb INTERFACE UKV_VERSION="${UKV_VERSION}")
  target_compile_definitions(ukv_embedded_leveldb INTERFACE UKV_ENGINE_IS_LEVELDB=1)

  list(APPEND UKV_ENGINE_NAMES "leveldb")
  list(APPEND UKV_CLIENT_LIBS "ukv_embedded_leveldb")
endif()

if(EXISTS ${UKV_ENGINE_UDISK_PATH})
  add_library(udisk STATIC IMPORTED)
  target_link_libraries(udisk INTERFACE dl pthread explain uring numa)
  set_property(TARGET udisk PROPERTY IMPORTED_LOCATION ${UKV_ENGINE_UDISK_PATH})
  set_property(TARGET udisk PROPERTY LINK_LIBRARIES "")

  add_library(ukv_embedded_udisk src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ukv_embedded_udisk udisk pthread yyjson simdjson bson pcre2 nlohmann_json::nlohmann_json ${JEMALLOC_LIBRARIES})
  target_compile_definitions(ukv_embedded_udisk INTERFACE UKV_VERSION="${UKV_VERSION}")
  target_compile_definitions(ukv_embedded_udisk INTERFACE UKV_ENGINE_IS_UDISK=1)

  list(APPEND UKV_ENGINE_NAMES "udisk")
  list(APPEND UKV_CLIENT_LIBS "ukv_embedded_udisk")
endif()

# The rest of CMake will apply to all client libraries
# Generate Apache Arrow Flight RPC backends
set(UKV_CLIENT_NAMES ${UKV_ENGINE_NAMES})

if(${UKV_BUILD_API_FLIGHT_CLIENT})
  add_library(ukv_flight_client src/flight_client.cpp src/modality_docs.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ukv_flight_client pthread yyjson simdjson bson pcre2 fmt::fmt arrow::flight arrow::bundled arrow::dataset arrow::arrow ssl crypto ${JEMALLOC_LIBRARIES})
  target_compile_definitions(ukv_flight_client INTERFACE UKV_FLIGHT_CLIENT=TRUE)
  list(APPEND UKV_CLIENT_NAMES "flight_client")
  list(APPEND UKV_CLIENT_LIBS "ukv_flight_client")
endif()

if(${UKV_BUILD_API_FLIGHT_SERVER})
  foreach(engine_name IN ITEMS ${UKV_ENGINE_NAMES})
    string(CONCAT embedded_lib_name "ukv_embedded_" ${engine_name})
    get_target_property(embedded_dependencies ${embedded_lib_name} LINK_LIBRARIES)

    string(CONCAT server_exe_name "ukv_flight_server_" ${engine_name})
    add_executable(${server_exe_name} src/flight_server.cpp)
    target_link_libraries(${server_exe_name} pthread yyjson simdjson bson arrow::flight arrow::bundled arrow::dataset arrow::arrow ssl crypto ${embedded_lib_name} ${embedded_dependencies})
    target_compile_definitions(${server_exe_name} INTERFACE UKV_ENGINE_NAME=${engine_name})

    if(${engine_name} STREQUAL "umem")
      target_compile_definitions(${server_exe_name} INTERFACE UKV_ENGINE_IS_UMEM=1)
    elseif(${engine_name} STREQUAL "rocksdb")
      target_compile_definitions(${server_exe_name} INTERFACE UKV_ENGINE_IS_ROCKSDB=1)
    elseif(${engine_name} STREQUAL "leveldb")
      target_compile_definitions(${server_exe_name} INTERFACE UKV_ENGINE_IS_LEVELDB=1)
    elseif(${engine_name} STREQUAL "udisk")
      target_compile_definitions(${server_exe_name} INTERFACE UKV_ENGINE_IS_UDISK=1)
    endif()
  endforeach()
endif()

# Enable warning and sanitization for our primary targets
foreach(client_lib IN ITEMS ${UKV_CLIENT_LIBS})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_target_properties(${client_lib} PROPERTIES CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-builtin-memcmp -fPIC")
    set_target_properties(${client_lib} PROPERTIES CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wfatal-errors")
    set_target_properties(${client_lib} PROPERTIES CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    set_target_properties(${client_lib} PROPERTIES CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-pragmas") # Allow "pragma region" source sections
    set_target_properties(${client_lib} PROPERTIES CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=deprecated-copy -Wno-deprecated-copy -Wno-error=pessimizing-move -Wno-pessimizing-move")
  endif()
endforeach()

# Generate tests
# We build them with the original sources (instead of linking with already compiled libraries)
# to maximize the number of exposed symbols and make debugging easier.
if(${UKV_BUILD_TESTS})
  foreach(client_lib IN ITEMS ${UKV_CLIENT_LIBS})
    get_target_property(client_dependencies ${client_lib} LINK_LIBRARIES)

    foreach(test_name IN ITEMS ${UKV_TEST_NAMES})
      string(CONCAT test_exe ${test_name} "_" ${client_lib})
      add_executable(${test_exe} tests/${test_name}.cpp)
      target_compile_definitions(${test_exe} PUBLIC UKV_TEST_PATH="tmp/${client_lib}/")
      target_link_libraries(${test_exe} gtest fmt::fmt ${client_lib} ${client_dependencies})
      add_test(NAME "${test_exe}" COMMAND "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${test_exe}")
    endforeach()
  endforeach()
endif()

# Generate benchmarks: Bitcoin Core & Twitter
if(${UKV_BUILD_BENCHMARKS})
  foreach(client_lib IN ITEMS ${UKV_CLIENT_LIBS})
    get_target_property(client_dependencies ${client_lib} LINK_LIBRARIES)

    string(CONCAT bench_name "bench_twitter_" ${client_lib})
    add_executable(${bench_name} benchmarks/twitter.cpp)
    target_link_libraries(${bench_name} benchmark ${client_lib} ${client_dependencies})

    string(CONCAT bench_name "bench_tabular_graph_" ${client_lib})
    add_executable(${bench_name} benchmarks/tabular_graph.cpp)
    target_link_libraries(${bench_name} benchmark ${client_lib} ${client_dependencies})
  endforeach()
endif()

# Build Python bindings linking to precompiled client SDKs
if(${UKV_BUILD_SDK_PYTHON})
  # find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.module)
  set(PYARROW_DIR "" CACHE PATH "Path to pyarrow install dir")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pybind11.cmake")
  file(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/ukv)

  foreach(client_lib_name IN ITEMS ${UKV_CLIENT_NAMES})
    string(CONCAT client_lib "ukv_embedded_" ${client_lib_name})

    if(${client_lib_name} STREQUAL "flight_client")
      string(CONCAT client_lib "ukv_" ${client_lib_name})
    endif()

    string(CONCAT wrap_lib "py_" ${client_lib_name})
    get_target_property(client_dependencies ${client_lib} LINK_LIBRARIES)

    pybind11_add_module(${wrap_lib} MODULE python/pybind.cpp python/database.cpp python/networkx.cpp python/documents.cpp python/pandas.cpp)
    target_include_directories(${wrap_lib} PUBLIC python/ ${PYARROW_DIR}/include)
    target_link_directories(${wrap_lib} PUBLIC ${PYARROW_DIR})
    target_link_libraries(${wrap_lib} PRIVATE pthread arrow::dataset arrow_python arrow::flight arrow::bundled arrow::arrow fmt::fmt ${client_lib} ${client_dependencies})
    target_compile_definitions(${wrap_lib} PRIVATE UKV_VERSION="${UKV_VERSION}" UKV_PYTHON_MODULE_NAME=${client_lib_name})
    set_target_properties(${wrap_lib} PROPERTIES OUTPUT_NAME ${client_lib_name})
  endforeach()
endif()
