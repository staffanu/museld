cmake_minimum_required(VERSION 3.22)
project(ac3rf-decode VERSION 0.1.0)

find_package(PkgConfig REQUIRED)
find_package(Eigen3 REQUIRED)  # Used by firpm

find_package(Git REQUIRED)
execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_VERSION
        RESULT_VARIABLE ERROR_CODE
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(GIT_VERSION STREQUAL "")
    set(GIT_VERSION GitVersionUnknown)
    message(WARNING "Failed to determine version from Git tags: ${ERROR_CODE}. Using default version ${GIT_VERSION}.")
endif()
configure_file(Version.h.in Version.h @ONLY)

# -mavx is hopefully implied by -march=native
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math -fsanitize=address -march=native")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math -march=native")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mmacosx-version-min=15.0")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -mmacosx-version-min=15.0")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -mmacosx-version-min=15.0")
endif()

# ============================================================================
# firpm — Parks-McClellan FIR filter design (vendored, Eigen3-based)
# ============================================================================

add_library(firpm STATIC
        external/firpm/src/pm.cpp
        external/firpm/src/band.cpp
        external/firpm/src/barycentric.cpp
        external/firpm/src/cheby.cpp
)

target_include_directories(firpm
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/firpm/include>
)
target_link_libraries(firpm PUBLIC Eigen3::Eigen)

# ============================================================================
# ac3rf — reusable library (no CLI, no FLAC, no input readers)
# ============================================================================

add_library(ac3rf STATIC
        Logger.h Logger.cpp
        filter/FirFilterStage.cpp filter/FirFilterStage.h
        filter/ComplexFirFilterStage.cpp filter/ComplexFirFilterStage.h
        filter/FirPM.h
        filter/FFT.h
        filter/Window.h
        filter/RaisedCosine.h
        filter/IirFilter.h
        rs/ReedSolomon.h
        rs/GFValue.h
        rs/ByteWithErasureFlag.h
        ac3/Ac3RfDemodulator.h ac3/Ac3RfDemodulator.cpp
        ac3/Ac3DPLL.cpp ac3/Ac3DPLL.h
        ac3/Ac3InputFraming.cpp ac3/Ac3InputFraming.h
        ac3/Ac3BlockHandler.cpp ac3/Ac3BlockHandler.h
        efm/EfmDemodulator.cpp efm/EfmDemodulator.h
        efm/EfmDecoder.cpp efm/EfmDecoder.h
        efm/TwoChannelSample.h
        efm/TimingRecovery.cpp efm/TimingRecovery.h
        efm/FractionalResampler.cpp efm/FractionalResampler.h
        efm/AdaptiveFilter.h
        efm/AdaptiveFilterImpl.cpp efm/AdaptiveFilterImpl.h
        efm/concealment/SlowArErasureConcealer.cpp efm/concealment/SlowArErasureConcealer.h
        efm/concealment/ErasureConcealer.h efm/concealment/ErasureConcealer.cpp
        efm/concealment/LinearInterpolationErasureConcealer.cpp efm/concealment/LinearInterpolationErasureConcealer.h
        efm/concealment/RepeatingSampleErasureConcealer.cpp efm/concealment/RepeatingSampleErasureConcealer.h
        efm/concealment/ArErasureConcealer.cpp efm/concealment/ArErasureConcealer.h
        efm/PopDetector.cpp efm/PopDetector.h
)

target_compile_features(ac3rf PUBLIC cxx_std_20)

# std::format is experimental in Clang/libc++ < 17; enable it explicitly
target_compile_options(ac3rf PUBLIC
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-fexperimental-library>
)

target_include_directories(ac3rf
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include/ac3rf>
)

# firpm is not linked normally — its objects are merged into libac3rf.a at POST_BUILD.
# Use add_dependencies for build ordering and a direct include path for compilation.
add_dependencies(ac3rf firpm)
target_include_directories(ac3rf PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/firpm/include>
)
target_link_libraries(ac3rf PRIVATE Eigen3::Eigen)

# Merge firpm's objects into libac3rf.a so consumers only need one library
add_custom_command(TARGET ac3rf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=$<TARGET_FILE:ac3rf>
        -DLIB1=$<TARGET_FILE:ac3rf>
        -DLIB2=$<TARGET_FILE:firpm>
        -DAR=${CMAKE_AR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MergeStaticLibs.cmake
    COMMENT "Merging firpm into libac3rf.a"
)

# ============================================================================
# Install — allows decode-orc (and others) to use find_package(ac3rf)
# ============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ac3rf
        EXPORT ac3rfTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Install public headers, preserving subdirectory structure
install(FILES Logger.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf)
install(DIRECTORY ac3 efm filter rs
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf
        FILES_MATCHING PATTERN "*.h"
)

install(EXPORT ac3rfTargets
        FILE ac3rfTargets.cmake
        NAMESPACE ac3rf::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

configure_package_config_file(
        cmake/ac3rfConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

# ============================================================================
# ac3rf-decode — CLI executable (adds FLAC input readers on top of the library)
# ============================================================================

option(BUILD_EXECUTABLE "Build the ac3rf-decode CLI executable" ON)
if(BUILD_EXECUTABLE)
    pkg_check_modules(FLAC REQUIRED flac++)

    add_executable(ac3rf-decode
            main.cpp
            Version.h
            input/InputReader.h
            input/LdsInputReader.cpp input/LdsInputReader.h
            input/LdfInputReader.cpp input/LdfInputReader.h
    )

    target_include_directories(ac3rf-decode PRIVATE ${FLAC_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_options(ac3rf-decode PRIVATE "-L/usr/local/lib")
    target_link_libraries(ac3rf-decode
            PRIVATE ac3rf
            PRIVATE ${FLAC_LINK_LIBRARIES}
    )
endif()
