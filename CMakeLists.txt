cmake_minimum_required(VERSION 3.16)

file(READ "VERSION" USTORE_VERSION)
string(STRIP ${USTORE_VERSION} USTORE_VERSION)
project(UStore
  LANGUAGES C CXX
  VERSION ${USTORE_VERSION}
  HOMEPAGE_URL "https://github.com/unum-cloud/ustore"
  DESCRIPTION "Universal Binary DBMS Interface to manage Docs • Graphs • Blobs • Vectors × in Local • Remote × RocksDB • LevelDB • UCSet • UDisk × from C • C++ • Python • Java • GoLang"
)

option(USTORE_BUILD_ENGINE_UCSET "Building REST API server for all backends" ON)
option(USTORE_BUILD_ENGINE_LEVELDB "Building REST API server for all backends")
option(USTORE_BUILD_ENGINE_ROCKSDB "Building REST API server for all backends")

option(USTORE_BUILD_TESTS "Building C/C++ native tests" ON)
option(USTORE_BUILD_SANITIZE "Use memory sanitizers for debug builds" ON)
option(USTORE_BUILD_BENCHMARKS "Building C/C++ native benchmarks")

option(USTORE_BUILD_CLI "Building CLI for UStore")
option(USTORE_BUILD_TOOLS "Building tools for UStore")
option(USTORE_BUILD_BUNDLES "Building bundles libraries for GoLang and Java SDKs")
option(USTORE_BUILD_SDK_PYTHON "Building Python bidings for all SDKs")
option(USTORE_BUILD_API_FLIGHT "Building Apache Arrow Flight RPC server and client for all backends")
option(USTORE_BUILD_API_FLIGHT_CLIENT "Building Apache Arrow Flight RPC client for all backends")
option(USTORE_BUILD_API_FLIGHT_SERVER "Building Apache Arrow Flight RPC server for all backends")
option(USTORE_BUILD_API_REST_SERVER "Building REST API server for all backends")

option(USTORE_USE_PYARROW "Ready-made arrow libraries from the installed pip package pyarrow")
option(USTORE_USE_JEMALLOC "Faster allocator, that requires autoconf to be installed")
option(USTORE_USE_ONEAPI "Faster concurrency primitives from Intel")
option(USTORE_USE_UUID "Replaces default 64-bit keys with 128-bit UUID compatible integers")

set(USTORE_ENGINE_UDISK_PATH "" CACHE STRING "Pass a path to UDisk binary to produce a full range of bindings")

if(${USTORE_BUILD_CLI})
  set(USTORE_BUILD_API_FLIGHT_CLIENT ON)
endif()

if(USTORE_BUILD_SDK_PYTHON)
  set(USTORE_USE_PYARROW ON)
endif()

if(USTORE_BUILD_API_FLIGHT)
  set(USTORE_BUILD_API_FLIGHT_SERVER ON)
  set(USTORE_BUILD_API_FLIGHT_CLIENT ON)
elseif(${USTORE_BUILD_API_FLIGHT_CLIENT} OR ${USTORE_BUILD_API_FLIGHT_SERVER})
  set(USTORE_BUILD_API_FLIGHT ON)
endif()

# Include conan dependencies
if(${USE_CONAN})
  include(${CMAKE_SOURCE_DIR}/conanbuildinfo.cmake)
  #include_directories(${CONAN_INCLUDE_DIRS})
  conan_basic_setup(NO_OUTPUT_DIRS)
endif()

# Some settings are mutually dependant
include(CMakeDependentOption)
cmake_dependent_option(USTORE_REBUILD_JEMALLOC "Enable to attempt to source and build JeMalloc locally" ON "USTORE_USE_JEMALLOC" OFF)
cmake_dependent_option(USTORE_REBUILD_ONEAPI "Enable to attempt to source and build Intel OneAPI locally" ON "USTORE_USE_ONEAPI" OFF)
cmake_dependent_option(USTORE_REBUILD_BOOST "Enable to attempt to source and build Boost locally" ON "USTORE_BUILD_API_REST_SERVER" OFF)

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
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wc++20-designator")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wgnu-zero-variadic-macro-arguments")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wformat-pedantic")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wextra-semi")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wunused-result")

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-builtin-memcmp -fPIC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wfatal-errors")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic -pedantic-errors")

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
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DUSTORE_DEBUG -g")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DUSTORE_DEBUG -g")

find_package(Threads REQUIRED)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)

if(${USTORE_BUILD_SANITIZE} AND NOT ${USTORE_BUILD_SDK_PYTHON})
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fsanitize-address-use-after-scope")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fsanitize-address-use-after-scope")
  set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=address -fsanitize-address-use-after-scope")
endif()

# Install 3rd Party Packages
include(FetchContent)
include_directories(include/)
include_directories(src/tools)
include_directories(src/helpers)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(FETCHCONTENT_QUIET OFF)
else()
  set(FETCHCONTENT_QUIET ON)
endif()

# Engines:
if(${USTORE_BUILD_ENGINE_UCSET})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ucset.cmake")
endif()

if(${USE_CONAN})
  # Engines:
  if(${USTORE_BUILD_ENGINE_LEVELDB})
    if(${USTORE_BUILD_SDK_PYTHON} OR ${USTORE_BUILD_BUNDLES})
      include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/leveldb.cmake")
      set(LIB_LEVELDB leveldb)
    else()
      include_directories(${CONAN_INCLUDE_DIRS_LEVELDB})
      set(LIB_LEVELDB ${CONAN_LIBS_LEVELDB})
    endif()
  endif()

  if(${USTORE_BUILD_ENGINE_ROCKSDB})
    if(${USTORE_BUILD_SDK_PYTHON} OR ${USTORE_BUILD_BUNDLES})
      include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rocksdb.cmake")
      set(LIB_ROCKSDB rocksdb)
    else()
      include_directories(${CONAN_INCLUDE_DIRS_ROCKSDB})
      set(LIB_ROCKSDB ${CONAN_LIBS_ROCKSDB})
    endif()
  endif()

  # Modalities:  
  # include_directories(${CONAN_INCLUDE_DIRS_FMT})
  include_directories(${CONAN_INCLUDE_DIRS_JSON})
  include_directories(${CONAN_INCLUDE_DIRS_BSON})
  include_directories(${CONAN_INCLUDE_DIRS_YYJSON})
  include_directories(${CONAN_INCLUDE_DIRS_SIMDJSON})
  include_directories(${CONAN_INCLUDE_DIRS_PCRE2})

  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fmt.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mpack.cmake")

  if(${USTORE_USE_JEMALLOC})
    include_directories(CONAN_INCLUDE_DIRS_JEMALLOC)
    set(JEMALLOC_LIBRARIES ${CONAN_LIBS_JEMALLOC})
  endif()

  if(${USTORE_USE_ONEAPI})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/oneapi.cmake")
  endif()

else()
  # Engines:
  if(${USTORE_BUILD_ENGINE_LEVELDB})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/leveldb.cmake")
    set(LIB_LEVELDB leveldb)
  endif()

  if(${USTORE_BUILD_ENGINE_ROCKSDB})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rocksdb.cmake")
    set(LIB_ROCKSDB rocksdb)
  endif()

  # Modalities:
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fmt.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/json.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/bson.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/yyjson.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/simdjson.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pcre2.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mpack.cmake")

  if(${USTORE_USE_JEMALLOC})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/jemalloc.cmake")
  endif()

  if(${USTORE_USE_ONEAPI})
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/oneapi.cmake")
  endif()
endif()

# Distributions:
# > USTORE_BUILD_ENGINE_UCSET: Uses Arrow Parquet format to save binary collections on disk.
# > USTORE_BUILD_API_FLIGHT: Uses Arrow Flight RPC as a client-server communication protocol.
# > USTORE_BUILD_SDK_PYTHON: Uses Arrow for zero-copy data exchage with the Python runtime.

if(${USTORE_BUILD_API_FLIGHT_CLIENT} OR ${USTORE_BUILD_API_FLIGHT_SERVER})
  if(${USE_CONAN})
    include_directories(${CONAN_INCLUDE_DIRS_OPENSSL})
  else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/openssl.cmake")
  endif()
endif()

if(${USTORE_BUILD_SDK_PYTHON} OR ${USTORE_BUILD_API_FLIGHT_CLIENT} OR ${USTORE_BUILD_API_FLIGHT_SERVER} OR ${USTORE_BUILD_ENGINE_UCSET} OR ${USTORE_BUILD_TESTS})
  if(NOT ${USTORE_USE_PYARROW})
    if(${USE_CONAN})
      include_directories(${CONAN_INCLUDE_DIRS_ARROW})
    else()
      include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/arrow.cmake")
    endif()
  else()
    include_directories(ustore_embedded_ucset PRIVATE ${PYARROW_DIR}/include)

    set(LIB_ARROW ${PYARROW_DIR}/libarrow.so.1000)
    set(LIB_ARROW_PARQUET ${PYARROW_DIR}/libparquet.so.1000)
    set(LIB_ARROW_DATASET ${PYARROW_DIR}/libarrow_dataset.so.1000)
    set(LIB_ARROW_FLIGHT ${PYARROW_DIR}/libarrow_flight.so.1000)
    set(LIB_ARROW_PYTHON ${PYARROW_DIR}/libarrow_python.so)
    set(LIB_ARROW_PYTHON_FLIGHT ${PYARROW_DIR}/libarrow_python_flight.so)
    set(LIB_ARROW_BUNDLED)
  endif()
endif()

if(${USTORE_BUILD_CLI})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/readline.cmake")
endif()

if(${USTORE_BUILD_API_FLIGHT_SERVER} OR ${USTORE_BUILD_CLI})
  if(${USE_CONAN})
    include_directories(${CONAN_INCLUDE_DIRS_CLIPP})
  else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/clipp.cmake")
  endif()
endif()

if(${USTORE_BUILD_API_REST_SERVER})
  if(${USE_CONAN})
    set(BOOST_ROOT ${CONAN_BOOST_ROOT})
    set(BOOST_INCLUDE_DIR ${CONAN_INCLUDE_DIRS_BOOST})
    set(BOOST_LIB_DIR ${CONAN_LIB_DIRS_BOOST})
  else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/boost.cmake")
  endif()
endif()

if(${USTORE_BUILD_TESTS})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/gtest.cmake")
endif()

if(${USTORE_BUILD_BENCHMARKS})
  if(${USE_CONAN})
    include_directories(${CONAN_INCLUDE_DIRS_BENCHMARK})
    include_directories(${CONAN_INCLUDE_DIRS_ARGPARSE})
  else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/gbench.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/argparse.cmake")
  endif()
endif()

if(${USE_CONAN})
  if (NOT ${USTORE_USE_PYARROW})
    add_library(arrow::arrow STATIC IMPORTED)
    set_property(TARGET arrow::arrow PROPERTY IMPORTED_LOCATION ${CONAN_LIB_DIRS_ARROW}/libarrow.a)
    set(LIB_ARROW arrow::arrow)

    add_library(arrow::flight STATIC IMPORTED)
    set_property(TARGET arrow::flight PROPERTY IMPORTED_LOCATION ${CONAN_LIB_DIRS_ARROW}/libarrow_flight.a)
    set(LIB_ARROW_FLIGHT arrow::flight)

    add_library(arrow::dataset STATIC IMPORTED)
    set_property(TARGET arrow::dataset PROPERTY IMPORTED_LOCATION ${CONAN_LIB_DIRS_ARROW}/libarrow_dataset.a)
    set(LIB_ARROW_DATASET arrow::dataset)

    add_library(arrow::parquet STATIC IMPORTED)
    set_property(TARGET arrow::parquet PROPERTY IMPORTED_LOCATION ${CONAN_LIB_DIRS_ARROW}/libparquet.a)
    set(LIB_ARROW_PARQUET arrow::parquet)

    add_library(thrift STATIC IMPORTED)
    set_property(TARGET thrift PROPERTY IMPORTED_LOCATION ${CONAN_LIB_DIRS_THRIFT}/libthrift.a)
    set(LIB_THRIFT thrift)

    add_library(re2 STATIC IMPORTED)
    set_property(TARGET re2 PROPERTY IMPORTED_LOCATION ${CONAN_LIB_DIRS_RE2}/libre2.a)
    set(LIB_RE2 re2)

    # add_library(arrow::bundled STATIC IMPORTED)
    # set_property(TARGET arrow::bundled ${CONAN_LIBS_RE2} ${CONAN_LIBS_UTF8PROC} ${CONAN_LIBS_THRIFT} ${CONAN_LIBS_ABSEIL} ${CONAN_LIBS_SNAPPY} ${CONAN_LIBS_PROTOBUF} ${CONAN_LIBS_ZLIB} ${CONAN_LIBS_C-ARES} ${CONAN_LIBS_GRPC})
    set(LIB_ARROW_BUNDLED ${LIB_RE2} ${CONAN_LIBS_UTF8PROC} ${CONAN_LIBS_THRIFT} ${LIB_THRIFT} ${CONAN_LIBS_ABSEIL} ${CONAN_LIBS_SNAPPY} ${CONAN_LIBS_PROTOBUF} ${CONAN_LIBS_ZLIB} ${CONAN_LIBS_C-ARES} ${CONAN_LIBS_GRPC})
    # set(LIB_ARROW_BUNDLED arrow::bundled)
  endif()


  set(LIB_SSL ssl)
  set(LIB_CRYPTO crypto)
  
  set(LIB_FMT fmt::fmt)
  set(LIB_BSON bson-static-1.0) 
  set(LIB_PCRE2 pcre2-8)
else()
  if(NOT ${USTORE_USE_PYARROW})
    set(LIB_ARROW arrow::arrow) 
    set(LIB_ARROW_FLIGHT arrow::flight)
    set(LIB_ARROW_DATASET arrow::dataset)
    set(LIB_ARROW_PARQUET arrow::parquet)
    set(LIB_ARROW_BUNDLED arrow::bundled)
  endif()
  
  set(LIB_SSL openssl::ssl)
  set(LIB_CRYPTO openssl::crypto)
  
  set(LIB_FMT fmt::fmt)
  set(LIB_BSON bson)
  set(LIB_PCRE2 pcre2)
endif()

set(USTORE_ENGINE_NAMES)
set(USTORE_CLIENT_LIBS)
set(USTORE_TEST_NAMES "test_tools" "test_compilation" "test_units" "stress_atomicity" "stress_linearizability")

# Define the Engine libraries we will need to build
if(${USTORE_BUILD_ENGINE_UCSET})
  add_library(ustore_embedded_ucset src/engine_ucset.cpp src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ustore_embedded_ucset pthread yyjson simdjson ${LIB_BSON} ${LIB_PCRE2} ${LIB_ARROW_PARQUET} ${LIB_ARROW} ${LIB_ARROW_BUNDLED} ${JEMALLOC_LIBRARIES} ${TBB_LIBRARIES})
  target_compile_definitions(ustore_embedded_ucset INTERFACE USTORE_VERSION="${USTORE_VERSION}")
  target_compile_definitions(ustore_embedded_ucset INTERFACE USTORE_ENGINE_IS_UCSET=1)

  list(APPEND USTORE_ENGINE_NAMES "ucset")
  list(APPEND USTORE_CLIENT_LIBS "ustore_embedded_ucset")
endif()

if(${USTORE_BUILD_ENGINE_ROCKSDB})
  add_library(ustore_embedded_rocksdb src/engine_rocksdb.cpp src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ustore_embedded_rocksdb ${LIB_ROCKSDB} pthread yyjson simdjson ${LIB_BSON} ${LIB_PCRE2} ${LIB_ARROW_BUNDLED} ${JEMALLOC_LIBRARIES})
  target_compile_definitions(ustore_embedded_rocksdb INTERFACE USTORE_VERSION="${USTORE_VERSION}")
  target_compile_definitions(ustore_embedded_rocksdb INTERFACE USTORE_ENGINE_IS_ROCKSDB=1)

  list(APPEND USTORE_ENGINE_NAMES "rocksdb")
  list(APPEND USTORE_CLIENT_LIBS "ustore_embedded_rocksdb")
endif()

if(${USTORE_BUILD_ENGINE_LEVELDB})
  add_library(ustore_embedded_leveldb src/engine_leveldb.cpp src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ustore_embedded_leveldb ${LIB_LEVELDB} pthread yyjson simdjson ${LIB_BSON} ${LIB_PCRE2} ${JEMALLOC_LIBRARIES})
  set_source_files_properties(src/engine_leveldb.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
  target_compile_definitions(ustore_embedded_leveldb INTERFACE USTORE_VERSION="${USTORE_VERSION}")
  target_compile_definitions(ustore_embedded_leveldb INTERFACE USTORE_ENGINE_IS_LEVELDB=1)

  list(APPEND USTORE_ENGINE_NAMES "leveldb")
  list(APPEND USTORE_CLIENT_LIBS "ustore_embedded_leveldb")
endif()

if(EXISTS ${USTORE_ENGINE_UDISK_PATH})
  add_library(udisk STATIC IMPORTED)
  target_link_libraries(udisk INTERFACE dl pthread explain uring numa)
  set_property(TARGET udisk PROPERTY IMPORTED_LOCATION ${USTORE_ENGINE_UDISK_PATH})
  set_property(TARGET udisk PROPERTY LINK_LIBRARIES "")
  add_library(ustore_embedded_udisk src/modality_docs.cpp src/modality_paths.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ustore_embedded_udisk udisk pthread yyjson simdjson ${LIB_BSON} ${LIB_PCRE2} nlohmann_json::nlohmann_json ${JEMALLOC_LIBRARIES})
  target_compile_definitions(ustore_embedded_udisk INTERFACE USTORE_VERSION="${USTORE_VERSION}")
  target_compile_definitions(ustore_embedded_udisk INTERFACE USTORE_ENGINE_IS_UDISK=1)

  list(APPEND USTORE_ENGINE_NAMES "udisk")
  list(APPEND USTORE_CLIENT_LIBS "ustore_embedded_udisk")
endif()

# The rest of CMake will apply to all client libraries
# Generate Apache Arrow Flight RPC backends
set(USTORE_CLIENT_NAMES ${USTORE_ENGINE_NAMES})

if(${USTORE_BUILD_API_FLIGHT_CLIENT})
  add_library(ustore_flight_client src/flight_client.cpp src/modality_docs.cpp src/modality_graph.cpp src/modality_vectors.cpp)
  target_link_libraries(ustore_flight_client pthread yyjson simdjson ${LIB_BSON} ${LIB_PCRE2} ${LIB_FMT} ${LIB_ARROW_FLIGHT} ${LIB_ARROW_BUNDLED} ${LIB_ARROW_DATASET} ${LIB_ARROW} ${LIB_SSL} ${LIB_CRYPTO} ${JEMALLOC_LIBRARIES})
  target_compile_definitions(ustore_flight_client INTERFACE USTORE_FLIGHT_CLIENT=TRUE)
  list(APPEND USTORE_CLIENT_NAMES "flight_client")
  list(APPEND USTORE_CLIENT_LIBS "ustore_flight_client")
endif()

if(${USTORE_BUILD_API_FLIGHT_SERVER})
  foreach(engine_name IN ITEMS ${USTORE_ENGINE_NAMES})
    string(CONCAT embedded_lib_name "ustore_embedded_" ${engine_name})
    get_target_property(embedded_dependencies ${embedded_lib_name} LINK_LIBRARIES)
    string(CONCAT server_exe_name "ustore_flight_server_" ${engine_name})
    add_executable(${server_exe_name} src/flight_server.cpp)
    target_link_libraries(${server_exe_name} pthread yyjson simdjson ${LIB_BSON} ${LIB_ARROW_FLIGHT} ${LIB_ARROW_BUNDLED} ${LIB_ARROW_DATASET} ${LIB_ARROW} ${LIB_SSL} ${LIB_CRYPTO} ${embedded_lib_name} ${embedded_dependencies})
    target_compile_definitions(${server_exe_name} INTERFACE USTORE_ENGINE_NAME=${engine_name})

    if(${engine_name} STREQUAL "ucset")
      target_compile_definitions(${server_exe_name} INTERFACE USTORE_ENGINE_IS_UCSET=1)
    elseif(${engine_name} STREQUAL "rocksdb")
      target_compile_definitions(${server_exe_name} INTERFACE USTORE_ENGINE_IS_ROCKSDB=1)
    elseif(${engine_name} STREQUAL "leveldb")
      target_compile_definitions(${server_exe_name} INTERFACE USTORE_ENGINE_IS_LEVELDB=1)
    elseif(${engine_name} STREQUAL "udisk")
      target_compile_definitions(${server_exe_name} INTERFACE USTORE_ENGINE_IS_UDISK=1)
    endif()
  endforeach()
endif()

if(${USTORE_BUILD_CLI})
  get_target_property(flight_client_dependencies ustore_flight_client LINK_LIBRARIES)
  add_executable(ustore src/tools/ustore_cli.cpp src/tools/dataset.cpp)
  target_link_libraries(ustore ${flight_client_dependencies} simdjson ${LIB_FMT} ${LIB_ARROW_FLIGHT} ${LIB_ARROW_PARQUET} ${LIB_ARROW} ${LIB_ARROW_BUNDLED} ustore_flight_client readline::readline readline::history ncurses)
endif()

# Enable warning and sanitization for our primary targets
foreach(client_lib IN ITEMS ${USTORE_CLIENT_LIBS})
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
if(${USTORE_BUILD_TESTS})
  foreach(client_lib IN ITEMS ${USTORE_CLIENT_LIBS})
    get_target_property(client_dependencies ${client_lib} LINK_LIBRARIES)

    foreach(test_name IN ITEMS ${USTORE_TEST_NAMES})
      string(CONCAT test_exe ${test_name} "_" ${client_lib})
      if(${test_name} MATCHES test_tools)
        add_executable(${test_exe} tests/${test_name}.cpp src/tools/dataset.cpp)
        if(${USTORE_BUILD_CLI})
          target_compile_definitions(${test_exe} PRIVATE USTORE_CLI=True)
        endif()
      else()
        add_executable(${test_exe} tests/${test_name}.cpp)
      endif()
      target_compile_definitions(${test_exe} PUBLIC USTORE_TEST_PATH="tmp/${client_lib}")
      target_link_libraries(${test_exe} gtest simdjson ${LIB_FMT} ${LIB_ARROW_FLIGHT} ${LIB_ARROW_PARQUET} ${LIB_ARROW} ${LIB_ARROW_BUNDLED} ${client_lib} ${client_dependencies})
      add_test(NAME "${test_exe}" COMMAND "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${test_exe}")
    endforeach()
  endforeach()
endif()

# Generate benchmarks: Bitcoin Core & Twitter
if(${USTORE_BUILD_BENCHMARKS})
  foreach(client_lib IN ITEMS ${USTORE_CLIENT_LIBS})
    get_target_property(client_dependencies ${client_lib} LINK_LIBRARIES)
    
    string(CONCAT bench_name "bench_twitter_" ${client_lib})
    add_executable(${bench_name} benchmarks/twitter.cpp)
    target_link_libraries(${bench_name} benchmark argparse ${LIB_FMT} ${client_lib} ${client_dependencies})
    
    string(CONCAT bench_name "bench_tabular_graph_" ${client_lib})
    add_executable(${bench_name} benchmarks/tabular_graph.cpp src/tools/dataset.cpp)
    target_link_libraries(${bench_name} benchmark argparse ${LIB_FMT} ${LIB_ARROW_FLIGHT} ${LIB_ARROW_PARQUET} ${LIB_ARROW} ${LIB_ARROW_BUNDLED} ${client_lib} ${client_dependencies})
  endforeach()
endif()

# Build Python bindings linking to precompiled client SDKs
if(${USTORE_BUILD_SDK_PYTHON})
  # find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.module)
  set(PYARROW_DIR "" CACHE PATH "Path to pyarrow install dir")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pybind11.cmake")
  file(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/ustore)
  
  foreach(client_lib_name IN ITEMS ${USTORE_CLIENT_NAMES})
    string(CONCAT client_lib "ustore_embedded_" ${client_lib_name})

    if(${client_lib_name} STREQUAL "flight_client")
      string(CONCAT client_lib "ustore_" ${client_lib_name})
    endif()

    string(CONCAT wrap_lib "py_" ${client_lib_name})
    get_target_property(client_dependencies ${client_lib} LINK_LIBRARIES)

    pybind11_add_module(${wrap_lib} MODULE python/pybind.cpp python/database.cpp python/networkx.cpp python/documents.cpp python/pandas.cpp)
    target_include_directories(${wrap_lib} PUBLIC python/ ${PYARROW_DIR}/include)
    target_link_directories(${wrap_lib} PUBLIC ${PYARROW_DIR})
    target_link_libraries(${wrap_lib} PRIVATE pthread ${LIB_FMT} ${LIB_ARROW_FLIGHT} ${LIB_ARROW_PARQUET} ${LIB_ARROW_PYTHON_FLIGHT} ${LIB_ARROW} ${LIB_ARROW_PYTHON} ${client_lib} ${client_dependencies})
    target_compile_definitions(${wrap_lib} PRIVATE USTORE_VERSION="${USTORE_VERSION}" USTORE_PYTHON_MODULE_NAME=${client_lib_name})
    set_target_properties(${wrap_lib} PROPERTIES OUTPUT_NAME ${client_lib_name})
  endforeach()
endif()

if(${USTORE_BUILD_BUNDLES})
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildUtils.cmake")

  foreach(client_lib_name IN ITEMS ${USTORE_CLIENT_NAMES})
    string(CONCAT client_lib "ustore_embedded_" ${client_lib_name})

    if(${client_lib_name} STREQUAL "flight_client")
      string(CONCAT client "ustore_" ${client_lib_name})
      string(CONCAT bundle_lib "ustore_" ${client_lib_name} "_bundle")
      bundle_static_library(${client} ${bundle_lib})
      continue()
    endif()

    string(CONCAT bundle_lib "ustore_" ${client_lib_name} "_bundle")
    bundle_static_library(${client_lib} ${bundle_lib})
  endforeach()
endif()
