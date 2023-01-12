# Installation

The most straight forward ways to start playing with UKV are:

* For Python: `pip install ukv`
* For Conan: `conan install ukv`
* For Docker image: `docker run --rm --name test_ukv -p 38709:38709 unum/ukv`

## Building From Source

We use CMake as the default build tool.
Most dependencies will be fetched with the integrated CMake scripts.

```sh
    cmake \
        -DUKV_BUILD_ENGINE_UMEM=1 \
        -DUKV_BUILD_ENGINE_LEVELDB=1 \
        -DUKV_BUILD_ENGINE_ROCKSDB=1 \
        -DUKV_BUILD_TESTS=1 \
        -DUKV_BUILD_BENCHMARKS=1 \
        -DUKV_BUILD_API_FLIGHT_CLIENT=0 \
        -DUKV_BUILD_API_FLIGHT_SERVER=0 \
        -B ./build_release && \
    make -j16 -C ./build_release
```

This will only produce 1 library for each embedded backend.
Building Flight API and REST API may require [additional steps](#partial--customized-builds).

### Adding UKV to Your CMake Project

Assuming CMake is the least straightforward build tool in history, there is a pretty short example of you can link UKV to your CMake project.

```cmake
cmake_minimum_required(VERSION 3.11)
project(ukv_example)

include(FetchContent)

FetchContent_Declare(
    ukv
    GIT_REPOSITORY https://github.com/unum-cloud/ukv.git
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ukv)
set(ukv_INCLUDE_DIR ${ukv_SOURCE_DIR}/include)
include_directories(${ukv_INCLUDE_DIR})

add_executable(ukv_example_test main.cpp)
target_link_libraries(ukv_example_test ukv::ukv_embedded_umem)
```

### Partial & Customized Builds

By default, our builds prioritize compatibility over performance.
Still, you can link against your favorite allocator or build every one of the provided bindings.
The CMake options include:

| Option                      | Default | Manual Installation Requirements                                                             |
| :-------------------------- | :-----: | :------------------------------------------------------------------------------------------- |
| UKV_BUILD_TESTS             |    1    |                                                                                              |
| UKV_BUILD_BENCHMARKS        |    0    | May require Arrow.                                                                           |
| UKV_BUILD_API_FLIGHT_CLIENT |    0    | Apache Arrow: Flight.                                                                        |
| UKV_BUILD_API_FLIGHT_SERVER |    0    | Apache Arrow: Flight.                                                                        |
| UKV_BUILD_API_REST_SERVER   |    0    | Boost: Beast and ASIO.                                                                       |
| UKV_BUILD_SDK_PYTHON        |    0    | Python: Interpreter, Development libraries. Apache Arrow: Dataset, Flight, Python libraries. |
| UKV_USE_JEMALLOC            |    0    | JeMalloc or AutoConf to be visible.                                                          |

Following scripts are provided to help:

* `./cmake/arrow.sh`: To pre-install needed Arrow components.
* `./cmake/boost.sh`: To pre-install needed Boost components.
* `./python/pack.sh`: build Python package.
* `./golang/pack.sh`: Build GoLang package.
* `./java/pack.sh`: Build Java package.

To build the Apache Arrow Flight RPC Docker image locally, for both x86 and Arm:

```sh
docker buildx build --platform=linux/amd64,linux/arm64 --file docker/Dockerfile . --progress=plain -c 32 --tag unum/ukv:nightly
```

To fetch the most recent Python bindings:

```sh
pip install --force-reinstall ukv
```

To build the Conan package locally, without installing it:

```sh
conan create . ukv/testing --build=missing
```

On MacOS:

```sh
brew install cmake apache-arrow openssl zlib protobuf
cmake \
    -D CMAKE_C_COMPILER=gcc \
    -D CMAKE_CXX_COMPILER=g++ \
    -D UKV_BUILD_API_FLIGHT_CLIENT=1 \
    .. && make -j 4
```

## Cloud Deployments

UKV is coming to the clouds across the globe!
You can still manually `docker run` us, but for more enterprise-y deployments you may want:

* horizontal auto-scaling,
* higher performance,
* rolling updates,
* support.

For that, try us on your favorite marketplace:

* Amazon Web Services.
* Microsoft Azure.
* Google Cloud Platform.
* Alibaba Cloud.

## Open Shift Integration

Coming Soon.

---

* To see a usage examples, check the [C][c-example] API and the [C++ API](cpp-example) tests.
* To read the documentation, [check unum.cloud/ukv](https://unum.cloud/ukv).
* To contribute to the development, [check the `src/`](https://github.com/unum-cloud/ukv/blob/main/src).

[c-example]: https://github.com/unum-cloud/ukv/blob/main/tests/compilation.cpp
[cpp-example]: https://github.com/unum-cloud/ukv/blob/main/tests/compilation.cpp
