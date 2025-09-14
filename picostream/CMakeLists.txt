cmake_minimum_required(VERSION 3.22)
project(picostream)

set(PICOSCOPE_INCLUDE_DIR /opt/picoscope/include)
set(PICOSCOPE_LIBRARY_DIR /opt/picoscope/lib)
set(PICOSCOPE_LIBRARIES libps5000a.so)

include_directories("${PICOSCOPE_INCLUDE_DIR}")
link_directories(${PICOSCOPE_LIBRARY_DIR})

add_executable(picostream
        picostream.c
)

target_link_libraries(picostream
        PRIVATE ${PICOSCOPE_LIBRARIES}
)

add_executable(picostream-digital
        picostream-digital.c
)

target_link_libraries(picostream-digital
        PRIVATE ${PICOSCOPE_LIBRARIES}
)
