cmake_minimum_required(VERSION 3.22)
project(museld LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Building for the CPU doing the build is right for local use, but a binary that
# is distributed has to target a baseline the audience actually has: set this to
# e.g. x86-64-v3 (AVX2, Haswell/Zen and later), which still enables the AVX filter
# paths, or x86-64-v2 for a lower floor without them.
set(MUSELD_ARCH "native" CACHE STRING "CPU to target, as passed to -march=")

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -march=${MUSELD_ARCH}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math -march=${MUSELD_ARCH}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math -march=${MUSELD_ARCH}")

# AddressSanitizer is not supported by MinGW-GCC on Windows
if(NOT MINGW)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
endif()

# On MinGW this makes off_t 64-bit and remaps lseek to its 64-bit variant, so
# seeking works in captures larger than 2 GB. No-op on Linux/macOS.
add_compile_definitions(_FILE_OFFSET_BITS=64)

option(BUILD_AC3RF_DECODE "Build the ac3rf-efm-decode CLI executable"                           ON)
option(BUILD_MUSE         "Build the MUSE/NTSC decoder (needs Vulkan/GLFW)"                     ON)
option(BUILD_PYTHON       "Build the nanobind Python bindings"                                  ON)
option(USE_LIBAV          "Use FFmpeg for video file output (--write), if a new enough one is found" ON)

add_subdirectory(src)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
if(BUILD_PYTHON)
    add_subdirectory(python)
endif()
