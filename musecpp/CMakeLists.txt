cmake_minimum_required(VERSION 3.22)
project(musecpp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 3.3 REQUIRED)
find_package(Vulkan REQUIRED)

#SET(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig/")
SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake-modules" ${CMAKE_MODULE_PATH})
find_package(PORTAUDIO REQUIRED)
include_directories("${PORTAUDIO_INCLUDE_DIRS}")

find_package(LIBAV)
if (HAVE_LIBAV)
    add_compile_definitions(HAVE_LIBAV)
    include_directories("${LIBAV_INCLUDE_DIR}")
else()
    message("FFMpeg not found or wrong version; not building support for writing video files.")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ffast-math -fsanitize=address")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ffast-math -UNDEBUG")

include_directories(.)

add_executable(musecpp
        main.cpp
        AudioPlayback.cpp AudioPlayback.h
        InputReader.h
        AudioDefs.h
        TextRenderer.cpp TextRenderer.h
        RfDemodulator.h
        InputBlockBase.h
        muse/FrameBuffer.cpp muse/FrameBuffer.h
        muse/FieldBufferView.cpp muse/FieldBufferView.h
        muse/AudioDecoder.cpp muse/AudioDecoder.h
        muse/BchDecoder.cpp muse/BchDecoder.h
        muse/AudioChannelDecoder.cpp muse/AudioChannelDecoder.h
        muse/ControlSignalDecoder.cpp muse/ControlSignalDecoder.h
        muse/Shaders.cpp muse/Shaders.h
        muse/MuseDecoder.cpp muse/MuseDecoder.h
        muse/ResamplingInputReader.cpp muse/ResamplingInputReader.h
        muse/PhaseCorrect16MHzInputReader.cpp muse/PhaseCorrect16MHzInputReader.h
        muse/MuseRfDemodulator.cpp muse/MuseRfDemodulator.h
        muse/MuseInputBlock.h
        muse/MuseConstants.h
        util/PercentileFilter.cpp util/PercentileFilter.h
        util/Logger.cpp util/Logger.h
        util/ConstExprHelpers.h
        musevk/VulkanManager.cpp musevk/VulkanManager.h
        musevk/CommandBuffer.cpp musevk/CommandBuffer.h
        musevk/ComputeShader.cpp musevk/ComputeShader.h
        musevk/VulkanMemoryObject.cpp musevk/VulkanMemoryObject.h
        musevk/VulkanImage.cpp musevk/VulkanImage.h
        musevk/VulkanBuffer.cpp musevk/VulkanBuffer.h
        musevk/TimestampStatistics.cpp musevk/TimestampStatistics.h
        musevk/HalfFloatUtil.h
        musevk/MemoryAllocator.cpp musevk/MemoryAllocator.h
        musevk/TimestampQueryPool.cpp musevk/TimestampQueryPool.h
        musevk/VulkanUtil.cpp musevk/VulkanUtil.h
        musevk/Size.h
        musevk/CommandPool.h
        musevk/Queue.h
        efm/ReedSolomon.h
        efm/GFValue.h
        efm/EfmPll.cpp efm/EfmPll.h
        efm/EfmDecoder.cpp efm/EfmDecoder.h
        efm/ByteWithErasureFlag.h
        util/FmtAddons.h
)
if (HAVE_LIBAV)
    target_sources(musecpp PRIVATE
        VideoFileWriter.cpp VideoFileWriter.h
    )
endif()

target_link_libraries(musecpp
        PRIVATE glfw
        PRIVATE Vulkan::Vulkan
        PRIVATE ${PORTAUDIO_LIBRARIES}
        PRIVATE ${LIBAV_LIBRARIES}
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