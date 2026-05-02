find_package(Eigen3 REQUIRED)

# ============================================================================
# firpm — Parks-McClellan FIR filter design (vendored, Eigen3-based)
# BSD 3 Licence
# ============================================================================

add_library(firpm STATIC
    ${CMAKE_SOURCE_DIR}/external/firpm/src/pm.cpp
    ${CMAKE_SOURCE_DIR}/external/firpm/src/band.cpp
    ${CMAKE_SOURCE_DIR}/external/firpm/src/barycentric.cpp
    ${CMAKE_SOURCE_DIR}/external/firpm/src/cheby.cpp
)

target_include_directories(firpm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/firpm/include>
    $<INSTALL_INTERFACE:include/ac3rf>
)
target_link_libraries(firpm PUBLIC Eigen3::Eigen)

# ============================================================================
# ac3rf — reusable library (no CLI, no FLAC, no input readers)
# ============================================================================

add_library(ac3rf STATIC
    logging/Logger.h logging/StreamLogger.h logging/StreamLogger.cpp
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
    ac3/Ac3Decoder.h ac3/Ac3Decoder.cpp
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

target_compile_features(ac3rf PUBLIC cxx_std_23)

# std::format is experimental in Clang/libc++ < 17
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    target_compile_options(ac3rf PUBLIC -fexperimental-library)
    target_link_options(ac3rf PUBLIC -fexperimental-library)
endif()

target_include_directories(ac3rf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/ac3rf>
)

target_link_libraries(ac3rf PUBLIC firpm)

# ============================================================================
# Install — allows the library to be found using find_package(ac3rf)
# ============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ac3rf firpm
    EXPORT ac3rfTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES logging/Logger.h logging/StreamLogger.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf/logging)
install(DIRECTORY ac3 efm filter rs
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/firpm/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf
)

install(EXPORT ac3rfTargets
    FILE ac3rfTargets.cmake
    NAMESPACE ac3rf::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/ac3rfConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfigVersion.cmake
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

# ============================================================================
# ac3rf-efm-decode — CLI executable (adds input readers on top of the library)
# ============================================================================

option(BUILD_EXECUTABLE "Build the ac3rf-efm-decode CLI executable" ON)
if(BUILD_EXECUTABLE)
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

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FLAC REQUIRED flac++)

    add_executable(ac3rf-efm-decode
        main-ac3rf-efm-decode.cpp
        Version.h
        input/InputReader.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
    )

    target_include_directories(ac3rf-efm-decode PRIVATE ${FLAC_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_options(ac3rf-efm-decode PRIVATE "-L/usr/local/lib")
    target_link_libraries(ac3rf-efm-decode
        PRIVATE ac3rf
        PRIVATE ${FLAC_LINK_LIBRARIES}
    )
endif()
