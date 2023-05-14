cmake_minimum_required(VERSION 3.25)
project(musecpp)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})
#LINK_DIRECTORIES( ${OpenCV_LIB_DIRS} )

set(KOMPUTE_OPT_LOG_LEVEL "Warn")
add_subdirectory(../../kompute/ ${CMAKE_CURRENT_BINARY_DIR}/kompute_build)
include_directories(${kompute_SOURCE_DIR}/src/include)
include_directories(${kompute_SOURCE_DIR}/build/src/shaders/glsl)
#list(APPEND CMAKE_PREFIX_PATH "${kompute_SOURCE_DIR}/cmake")

add_executable(musecpp
        main.cpp
        FrameBuffer.cpp FrameBuffer.h
        FieldBufferView.cpp FieldBufferView.h
        VideoDecoder.cpp VideoDecoder.h
        AudioDecoder.cpp AudioDecoder.h
        #FilterUtil.cpp FilterUtil.h
        ControlSignalDecoder.cpp ControlSignalDecoder.h
        Shaders.cpp Shaders.h
        MuseBuffer.h
        MuseSubBuffer.cpp MuseSubBuffer.h
        )
target_link_libraries(musecpp
        PRIVATE ${OpenCV_LIBS}
        PRIVATE kompute
#        PRIVATE ${kompute_LIBS}
        )
