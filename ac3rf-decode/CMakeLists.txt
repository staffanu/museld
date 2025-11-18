cmake_minimum_required(VERSION 3.22)
project(ac3rf-decode)

set(CMAKE_CXX_STANDARD 23)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GNURADIO_FILTER REQUIRED gnuradio-filter)
include_directories("${GNURADIO_FILTER_INCLUDE_DIRS}")

pkg_check_modules(FLAC REQUIRED flac++)
include_directories("${FLAC_INCLUDE_DIRS}")

# -mavx is hopefully implied by -march=native
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math -fsanitize=address -march=native")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math -march=native")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mmacosx-version-min=15.0")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -mmacosx-version-min=15.0")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -mmacosx-version-min=15.0")
endif()

add_executable(ac3rf-decode
        main.cpp
        Logger.h Logger.cpp
        input/InputReader.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
        filter/FirFilterStage.cpp filter/FirFilterStage.h
        filter/ComplexFirFilterStage.cpp filter/ComplexFirFilterStage.h
        filter/FFT.h
        rs/ReedSolomon.h
        rs/GFValue.h
        rs/ByteWithErasureFlag.h
        ac3/Ac3RfDemodulator.h ac3/Ac3RfDemodulator.cpp
        ac3/Ac3DPLL.cpp ac3/Ac3DPLL.h
        ac3/Ac3InputFraming.cpp ac3/Ac3InputFraming.h
        ac3/Ac3BlockHandler.cpp ac3/Ac3BlockHandler.h
        efm/EfmDemodulator.cpp efm/EfmDemodulator.h
        efm/EfmDecoder.cpp efm/EfmDecoder.h
        efm/EfmPll.cpp efm/EfmPll.h
        efm/TwoChannelSample.h
        efm/TimingRecovery.cpp efm/TimingRecovery.h
        efm/FractionalResampler.cpp efm/FractionalResampler.h
        efm/AdaptiveFilter.h
        efm/AdaptiveFilterImpl.cpp efm/AdaptiveFilterImpl.h
)

target_link_options(ac3rf-decode PRIVATE "-L/usr/local/lib")
target_link_libraries(ac3rf-decode
        PRIVATE ${GNURADIO_FILTER_LINK_LIBRARIES}
        PRIVATE ${FLAC_LINK_LIBRARIES}
)
