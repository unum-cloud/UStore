# gRPC: https://grpc.io/blog/cmake-improvements/
# https://github.com/grpc/grpc/blob/master/test/distrib/cpp/run_distrib_test_cmake_fetchcontent.sh
include(FetchContent)
FetchContent_Declare(
    gRPC
    GIT_REPOSITORY https://github.com/grpc/grpc
    GIT_TAG v1.47.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(gRPC)