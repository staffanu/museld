cmake_minimum_required(VERSION 3.25)
project(musecpp)

set(CMAKE_CXX_STANDARD 17)

#list(APPEND CMAKE_MODULE_PATH, "/home/staffanu/work/ldaudio/musecpp")
#message("CMAKE_MODULE_PATH: " ${CMAKE_MODULE_PATH})
#find_package(LIBVLC REQUIRED)

#include(${CMAKE_SOURCE_DIR}/FindLIBVLC.cmake)
#list(APPEND CMAKE_LIBRARY_PATH ${LIBVLC_LIBRARY})
#message("CMAKE_LIBRARY_PATH: " ${CMAKE_LIBRARY_PATH})

find_package(OpenCV REQUIRED)
INCLUDE_DIRECTORIES( ${OpenCV_INCLUDE_DIRS} )
#LINK_DIRECTORIES( ${OpenCV_LIB_DIRS} )

add_executable(musecpp
        main.cpp
        FrameBuffer.cpp FrameBuffer.h
        FieldBufferView.cpp FieldBufferView.h
        VideoDecoder.cpp VideoDecoder.h
        AudioDecoder.cpp AudioDecoder.h
        FilterUtil.cpp FilterUtil.h
        MuseTypes.h ControlSignalDecoder.cpp ControlSignalDecoder.h)
target_link_libraries(musecpp PRIVATE ${OpenCV_LIBS}) # libvlc.so)
