cmake_minimum_required(VERSION 3.22)
project(museld LANGUAGES CXX)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math -fsanitize=address -march=native")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math -march=native")

option(BUILD_AC3RF_DECODE "Build the ac3rf-efm-decode CLI executable"                           ON)
option(BUILD_MUSE         "Build the MUSE/NTSC decoder (needs Vulkan/GLFW/PortAudio/GNU Radio)" ON)
option(BUILD_PYTHON       "Build the nanobind Python bindings"                                  ON)

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
if(BUILD_PYTHON)
    add_subdirectory(python)
endif()
