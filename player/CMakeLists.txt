cmake_minimum_required(VERSION 3.22)
project(museld LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The threading library is linked explicitly. On glibc/Linux libpthread symbols
# are pulled in transitively, but FreeBSD (and a strict linker generally) needs
# an explicit -lpthread, so link Threads::Threads into the targets that use it.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Building for the CPU doing the build is right for local use, but a binary that
# is distributed has to target a baseline the audience actually has: set this to
# e.g. x86-64-v3 (AVX2, Haswell/Zen and later), which still enables the AVX filter
# paths, or x86-64-v2 for a lower floor without them. Set it to nothing at all to
# leave the compiler's own default alone, which is what the arm64 macOS packages
# do — every Mac that runs them is an M1 or later anyway.
set(MUSELD_ARCH "native" CACHE STRING "CPU to target, as passed to -march=; empty to leave the default")
if(MUSELD_ARCH)
    set(MUSELD_ARCH_FLAG "-march=${MUSELD_ARCH}")
else()
    set(MUSELD_ARCH_FLAG "")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math ${MUSELD_ARCH_FLAG}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math ${MUSELD_ARCH_FLAG}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math ${MUSELD_ARCH_FLAG}")

# AddressSanitizer is not supported by MinGW-GCC on Windows
if(NOT MINGW)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
endif()

# On MinGW this makes off_t 64-bit and remaps lseek to its 64-bit variant, so
# seeking works in captures larger than 2 GB. No-op on Linux/macOS.
add_compile_definitions(_FILE_OFFSET_BITS=64)

option(BUILD_AC3RF_DECODE "Build the ac3rf-efm-decode CLI executable"                           ON)
option(BUILD_MUSE         "Build the MUSE/NTSC decoder (needs Vulkan/GLFW)"                     ON)
option(USE_LIBAV          "Use FFmpeg for video file output (--write), if a new enough one is found" ON)

add_subdirectory(src)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
