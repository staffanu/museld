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
        MuseBuffer.h
        MuseSubBuffer.cpp MuseSubBuffer.h
        FrameBuffer.cpp FrameBuffer.h
        FieldBufferView.cpp FieldBufferView.h
        AudioDecoder.cpp AudioDecoder.h
        ControlSignalDecoder.cpp ControlSignalDecoder.h
        Shaders.cpp Shaders.h
        MuseDecoder.cpp MuseDecoder.h
        musevk/VulkanManager.cpp musevk/VulkanManager.h
        musevk/CommandQueue.cpp musevk/CommandQueue.h
        musevk/ComputeShader.cpp musevk/ComputeShader.h
        musevk/VulkanMemoryObject.cpp musevk/VulkanMemoryObject.h
        musevk/VulkanImage.cpp musevk/VulkanImage.h
        musevk/VulkanBuffer.cpp musevk/VulkanBuffer.h
        musevk/TimestampStatistics.cpp musevk/TimestampStatistics.h
        )
target_link_libraries(musecpp
#        PRIVATE ${OpenCV_LIBS}
        PRIVATE glfw
        PRIVATE Vulkan::Vulkan
        )
