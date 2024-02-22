cmake_minimum_required(VERSION 3.25)
project(musecpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fmt REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(Vulkan REQUIRED)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake-modules" ${CMAKE_MODULE_PATH})
find_package(PORTAUDIO REQUIRED)
include_directories("${PORTAUDIO_INCLUDE_DIRS}")

#add_compile_options(-O0 -pg -g)
#SET(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Og -pg -g")
#SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -pg -g")

# -mavx is hopefully implied by -march=native
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math -march=native") # -fsanitize=address")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math -march=native")

add_executable(musecpp
        main.cpp
        FrameBuffer.cpp FrameBuffer.h
        FieldBufferView.cpp FieldBufferView.h
        AudioDecoder.cpp AudioDecoder.h
        BchDecoder.cpp BchDecoder.h
        AudioChannelDecoder.cpp AudioChannelDecoder.h
        ControlSignalDecoder.cpp ControlSignalDecoder.h
        Shaders.cpp Shaders.h
        MuseDecoder.cpp MuseDecoder.h
        AudioPlayback.cpp AudioPlayback.h
        InputReader.cpp InputReader.h
        ResamplingInputReader.cpp ResamplingInputReader.h
        InputPll.cpp InputPll.h
        PhaseCorrect16MHzInputReader.cpp PhaseCorrect16MHzInputReader.h
        RfDemodulator.cpp RfDemodulator.h
        util/PercentileFilter.cpp util/PercentileFilter.h
        AudioMode.h
        util/Logger.cpp util/Logger.h
        musevk/VulkanManager.cpp musevk/VulkanManager.h
        musevk/CommandBuffer.cpp musevk/CommandBuffer.h
        musevk/ComputeShader.cpp musevk/ComputeShader.h
        musevk/VulkanMemoryObject.h
        musevk/VulkanImage.cpp musevk/VulkanImage.h
        musevk/VulkanBuffer.cpp musevk/VulkanBuffer.h
        musevk/TimestampStatistics.cpp musevk/TimestampStatistics.h
        musevk/HalfFloatUtil.h
        musevk/MemoryAllocator.cpp musevk/MemoryAllocator.h
        musevk/TimestampQueryPool.cpp musevk/TimestampQueryPool.h
        efm/ReedSolomon.h
        efm/GFValue.h
        efm/EfmPll.cpp efm/EfmPll.h
        efm/EfmDecoder.cpp efm/EfmDecoder.h
        efm/ByteWithErasureFlag.h
        musevk/Size.h
)
target_link_libraries(musecpp
        PRIVATE glfw
        PRIVATE Vulkan::Vulkan
        PRIVATE ${PORTAUDIO_LIBRARIES}
        PRIVATE fmt::fmt
)

set(GLSLC "glslc")
file(GLOB GLSL_SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)

foreach(GLSL ${GLSL_SOURCE_FILES})
    get_filename_component(FILE_NAME ${GLSL} NAME)
    set(SPIRV "${PROJECT_BINARY_DIR}/shaders/${FILE_NAME}.spv")
    add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/shaders/"
            COMMAND ${GLSLC} -Werror -O ${GLSL} -o ${SPIRV}
            DEPENDS ${GLSL})
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(GLSL)

add_custom_target(Shaders DEPENDS ${SPIRV_BINARY_FILES})
add_dependencies(musecpp Shaders)

add_subdirectory(Catch_tests)