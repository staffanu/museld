cmake_minimum_required(VERSION 3.25)
project(musecpp)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

find_package(PkgConfig)

pkg_check_modules(GTKMM gtkmm-3.0 REQUIRED)
link_directories( ${GTKMM_LIBRARY_DIRS} )
include_directories( ${GTKMM_INCLUDE_DIRS} )

find_package(glfw3 3.3 REQUIRED)

set(KOMPUTE_OPT_LOG_LEVEL "Warn")
set(KOMPUTE_OPT_DISABLE_VK_DEBUG_LAYERS ON)
add_subdirectory(../../kompute/ ${CMAKE_CURRENT_BINARY_DIR}/kompute_build)
include_directories(${kompute_SOURCE_DIR}/src/include)
include_directories(${kompute_SOURCE_DIR}/build/src/shaders/glsl)
#list(APPEND CMAKE_PREFIX_PATH "${kompute_SOURCE_DIR}/cmake")

add_executable(musecpp
        main.cpp
        FrameBuffer.cpp FrameBuffer.h
        FieldBufferView.cpp FieldBufferView.h
        AudioDecoder.cpp AudioDecoder.h
        #FilterUtil.cpp FilterUtil.h
        ControlSignalDecoder.cpp ControlSignalDecoder.h
        Shaders.cpp Shaders.h
        MuseBuffer.h
        MuseSubBuffer.cpp MuseSubBuffer.h
        MuseDecoder.cpp MuseDecoder.h
        MuseWindow.cpp MuseWindow.h
        MuseDrawingArea.cpp MuseDrawingArea.h
        )
target_link_libraries(musecpp
        PRIVATE ${OpenCV_LIBS}
        PRIVATE kompute
#        PRIVATE ${kompute_LIBS}
        PRIVATE ${GTKMM_LIBRARIES}
        )
