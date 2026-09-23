cmake_minimum_required(VERSION 3.22)
project(ldstack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_definitions(_FILE_OFFSET_BITS=64)

add_executable(ldstack
        src/ldstack.cpp
        src/LdsFile.cpp
        src/FmDemod.cpp
        src/Scanner.cpp
        src/Vbi.cpp
        src/Stack.cpp
        src/CircEncoder.cpp
        src/EfmPredict.cpp
        src/EfmBits.cpp
        src/EfmScore.cpp
)
target_compile_options(ldstack PRIVATE -Wall -Wextra)
target_include_directories(ldstack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../player/src)

install(TARGETS ldstack RUNTIME DESTINATION bin)
