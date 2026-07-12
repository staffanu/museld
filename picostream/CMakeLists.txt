cmake_minimum_required(VERSION 3.22)
project(picostream CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PICOSCOPE_INCLUDE_DIR /opt/picoscope/include)
set(PICOSCOPE_LIBRARY_DIR /opt/picoscope/lib)
set(PICOSCOPE_LIBRARIES libps5000a.so)

include_directories("${PICOSCOPE_INCLUDE_DIR}")
link_directories(${PICOSCOPE_LIBRARY_DIR})

add_executable(picostream
        picostream.cpp
)

target_link_libraries(picostream
        PRIVATE ${PICOSCOPE_LIBRARIES}
)
