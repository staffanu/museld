cmake_minimum_required(VERSION 3.25)
project(musecpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#find_package(OpenCV REQUIRED)
#include_directories(${OpenCV_INCLUDE_DIRS})

find_package(glfw3 3.3 REQUIRED)
find_package(Vulkan REQUIRED)

add_executable(musecpp
        main.cpp
        FrameBuffer.cpp FrameBuffer.h
        FieldBufferView.cpp FieldBufferView.h
        AudioDecoder.cpp AudioDecoder.h
        ControlSignalDecoder.cpp ControlSignalDecoder.h
        Shaders.cpp Shaders.h
        MuseBuffer.h
        MuseSubBuffer.cpp MuseSubBuffer.h
        MuseDecoder.cpp MuseDecoder.h
        VulkanManager.cpp VulkanManager.h
        VulkanResources.cpp VulkanResources.h
        ComputeShader.cpp ComputeShader.h
        VulkanBuffer.cpp VulkanBuffer.h
        )
target_link_libraries(musecpp
#        PRIVATE ${OpenCV_LIBS}
        PRIVATE glfw
        PRIVATE Vulkan::Vulkan
        )
