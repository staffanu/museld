cmake_minimum_required(VERSION 4.0)
project(ac3rf-decode)

set(CMAKE_CXX_STANDARD 23)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GNURADIO REQUIRED gnuradio-filter)
include_directories("${GNURADIO_INCLUDE_DIRS}")

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -mmacosx-version-min=15.0")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math -fsanitize=address -mmacosx-version-min=15.0")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math -mmacosx-version-min=15.0")

add_executable(ac3rf-decode
        main.cpp
        Logger.h Logger.cpp
        Ac3RfDemodulator.h Ac3RfDemodulator.cpp
        InputReader.h
        rs/ReedSolomon.h
        rs/GFValue.h
        rs/ByteWithErasureFlag.h
        Ac3DPLL.cpp Ac3DPLL.h
        Ac3InputFraming.cpp
        Ac3InputFraming.h
        Ac3BlockHandler.cpp
        Ac3BlockHandler.h
)

target_link_options(ac3rf-decode PRIVATE "-L/usr/local/lib")
target_link_libraries(ac3rf-decode
        PRIVATE ${GNURADIO_LINK_LIBRARIES}
)
