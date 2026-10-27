# CCCL's CUB, Thrust and libcudacxx, whose include directories precede the copies the CUDA toolkit bundles
include(FetchContent)
FetchContent_Declare(
    cccl
    GIT_REPOSITORY https://github.com/NVIDIA/cccl.git
    GIT_TAG v3.4.2
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(cccl)
