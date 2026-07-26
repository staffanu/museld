cmake_minimum_required(VERSION 3.22)
project(ldconv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Deliberately no -ffast-math here, unlike the player: this tool only moves
# integer code values around and every conversion has to be exact.

# On MinGW this makes off_t 64-bit, so captures larger than 2 GB still seek.
add_compile_definitions(_FILE_OFFSET_BITS=64)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FLAC REQUIRED flac++)

add_executable(ldconv
        src/ldconv.cpp
        src/FileIo.cpp
        src/Format.cpp
        src/SampleSink.cpp
        src/SampleSource.cpp
        src/Transform.cpp
)

target_include_directories(ldconv PRIVATE ${FLAC_INCLUDE_DIRS})
target_link_directories(ldconv PRIVATE ${FLAC_LIBRARY_DIRS})
target_link_libraries(ldconv PRIVATE ${FLAC_LINK_LIBRARIES})

find_package(Threads REQUIRED)
target_link_libraries(ldconv PRIVATE Threads::Threads)

install(TARGETS ldconv RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    enable_testing()
    # Invoked through bash by name: Windows cannot execute a script by its
    # shebang, so handing the path straight to ctest fails there.
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        add_test(NAME roundtrip
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/roundtrip.sh $<TARGET_FILE:ldconv>)
    else()
        message(WARNING "bash not found; the round-trip test will not be run")
    endif()
endif()
