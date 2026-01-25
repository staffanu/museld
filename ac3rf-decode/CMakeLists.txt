cmake_minimum_required(VERSION 3.22)
project(ac3rf-decode)

set(CMAKE_CXX_STANDARD 23)

find_package(PkgConfig REQUIRED)

find_package(Eigen3 REQUIRED)  # Used by firpm

pkg_check_modules(FLAC REQUIRED flac++)

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

add_library(firpm STATIC
        external/firpm/src/pm.cpp
        external/firpm/src/band.cpp
        external/firpm/src/barycentric.cpp
        external/firpm/src/cheby.cpp
)

target_include_directories(firpm
        PUBLIC
        external/firpm/include
        Eigen3::Eigen
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(firpm
        PUBLIC
        Eigen3::Eigen
)

add_executable(ac3rf-decode
        main.cpp
        Version.h
        Logger.h Logger.cpp
        input/InputReader.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
        filter/FirFilterStage.cpp filter/FirFilterStage.h
        filter/ComplexFirFilterStage.cpp filter/ComplexFirFilterStage.h
        filter/FirPM.h
        filter/FFT.h
        filter/Window.h
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

target_include_directories(ac3rf-decode
        PRIVATE ${FLAC_INCLUDE_DIRS}
)

target_link_options(ac3rf-decode PRIVATE "-L/usr/local/lib")
target_link_libraries(ac3rf-decode
        PRIVATE ${GNURADIO_FILTER_LINK_LIBRARIES}
        PRIVATE ${FLAC_LINK_LIBRARIES}
        PRIVATE firpm
)
