find_package(Eigen3 REQUIRED)

# ============================================================================
# firpm — Parks-McClellan FIR filter design (vendored, Eigen3-based)
# BSD 3 Licence
# ============================================================================

add_library(firpm STATIC
    ${CMAKE_SOURCE_DIR}/external/firpm/src/pm.cpp
    ${CMAKE_SOURCE_DIR}/external/firpm/src/band.cpp
    ${CMAKE_SOURCE_DIR}/external/firpm/src/barycentric.cpp
    ${CMAKE_SOURCE_DIR}/external/firpm/src/cheby.cpp
)

target_include_directories(firpm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/firpm/include>
    $<INSTALL_INTERFACE:include/ac3rf>
)
target_link_libraries(firpm PUBLIC Eigen3::Eigen)

# ============================================================================
# ac3rf — reusable library (no CLI, no FLAC, no input readers)
# ============================================================================

add_library(ac3rf STATIC
    logging/Logger.h logging/StreamLogger.h logging/StreamLogger.cpp
    filter/FirFilterStage.cpp filter/FirFilterStage.h
    filter/ComplexFirFilterStage.cpp filter/ComplexFirFilterStage.h
    filter/FirPM.h
    filter/FFT.h
    filter/Window.h
    filter/RaisedCosine.h
    filter/IirFilter.h
    rs/ReedSolomon.h
    rs/GFValue.h
    rs/ByteWithErasureFlag.h
    ac3/Ac3Decoder.h ac3/Ac3Decoder.cpp
    ac3/Ac3RfDemodulator.h ac3/Ac3RfDemodulator.cpp
    ac3/Ac3DPLL.cpp ac3/Ac3DPLL.h
    ac3/Ac3InputFraming.cpp ac3/Ac3InputFraming.h
    ac3/Ac3BlockHandler.cpp ac3/Ac3BlockHandler.h
    efm/EfmDemodulator.cpp efm/EfmDemodulator.h
    efm/EfmDecoder.cpp efm/EfmDecoder.h
    efm/TwoChannelSample.h
    efm/TimingRecovery.cpp efm/TimingRecovery.h
    efm/FractionalResampler.cpp efm/FractionalResampler.h
    efm/AdaptiveFilter.h
    efm/AdaptiveFilterImpl.cpp efm/AdaptiveFilterImpl.h
    efm/concealment/SlowArErasureConcealer.cpp efm/concealment/SlowArErasureConcealer.h
    efm/concealment/ErasureConcealer.h efm/concealment/ErasureConcealer.cpp
    efm/concealment/LinearInterpolationErasureConcealer.cpp efm/concealment/LinearInterpolationErasureConcealer.h
    efm/concealment/RepeatingSampleErasureConcealer.cpp efm/concealment/RepeatingSampleErasureConcealer.h
    efm/concealment/ArErasureConcealer.cpp efm/concealment/ArErasureConcealer.h
    efm/PopDetector.cpp efm/PopDetector.h
)

target_compile_features(ac3rf PUBLIC cxx_std_23)

# std::format is experimental in Clang/libc++ < 17
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    target_compile_options(ac3rf PUBLIC -fexperimental-library)
    target_link_options(ac3rf PUBLIC -fexperimental-library)
endif()

target_include_directories(ac3rf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/ac3rf>
)

target_link_libraries(ac3rf PUBLIC firpm)

# ============================================================================
# Install — allows the library to be found using find_package(ac3rf)
# ============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ac3rf firpm
    EXPORT ac3rfTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES logging/Logger.h logging/StreamLogger.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf/logging)
install(DIRECTORY ac3 efm filter rs
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/external/firpm/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf
)

install(EXPORT ac3rfTargets
    FILE ac3rfTargets.cmake
    NAMESPACE ac3rf::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/ac3rfConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfigVersion.cmake
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ac3rfConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ac3rf
)

# ============================================================================
# ac3rf-efm-decode — CLI executable (adds input readers on top of the library)
# ============================================================================

if(BUILD_AC3RF_DECODE)
    find_package(Git REQUIRED)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_VERSION
        RESULT_VARIABLE ERROR_CODE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(GIT_VERSION STREQUAL "")
        set(GIT_VERSION GitVersionUnknown)
        message(WARNING "Failed to determine version from Git tags: ${ERROR_CODE}. Using default version ${GIT_VERSION}.")
    endif()
    configure_file(Version.h.in Version.h @ONLY)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FLAC REQUIRED flac++)

    add_executable(ac3rf-efm-decode
        main-ac3rf-efm-decode.cpp
        Version.h
        input/InputReader.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
    )

    target_include_directories(ac3rf-efm-decode PRIVATE ${FLAC_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_options(ac3rf-efm-decode PRIVATE "-L/usr/local/lib")
    target_link_libraries(ac3rf-efm-decode
        PRIVATE ac3rf
        PRIVATE ${FLAC_LINK_LIBRARIES}
    )
endif()

# ============================================================================
# musecpp — real-time MUSE/NTSC laserdisc player (Vulkan + GLFW + PortAudio)
# ============================================================================

if(BUILD_MUSE)
    find_package(glfw3 3.3 REQUIRED)
    find_package(Vulkan REQUIRED)

    set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules" ${CMAKE_MODULE_PATH})
    find_package(PORTAUDIO REQUIRED)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GNURADIO REQUIRED gnuradio-filter)

    find_package(LIBAV)
    if(HAVE_LIBAV)
        add_compile_definitions(HAVE_LIBAV)
    else()
        message("FFmpeg not found or wrong version; not building video file writer support.")
    endif()

    add_executable(musecpp
        main-musecpp.cpp
        AudioDefs.h
        AudioPlayback.cpp AudioPlayback.h
        InputReader.h InputReader.impl.h
        InputBlockBase.h
        Decoder.h
        DiscInfo.h
        DropoutMode.h
        ResultImages.h
        RfDemodulator.h
        TextRenderer.cpp TextRenderer.h
        logging/CategoryLogger.cpp logging/CategoryLogger.h
        bch/BchDecoder.cpp bch/BchDecoder.h
        util/PercentileFilter.cpp util/PercentileFilter.h
        util/LinearRegression.cpp util/LinearRegression.h
        util/ConstExprHelpers.h
        util/FmtAddons.h
        muse/FrameBuffer.cpp muse/FrameBuffer.h
        muse/FieldBufferView.cpp muse/FieldBufferView.h
        muse/AudioDecoder.cpp muse/AudioDecoder.h
        muse/AudioChannelDecoder.cpp muse/AudioChannelDecoder.h
        muse/ControlSignalDecoder.cpp muse/ControlSignalDecoder.h
        muse/Shaders.cpp muse/Shaders.h
        muse/MuseDecoder.cpp muse/MuseDecoder.h
        muse/ResamplingInputReader.cpp muse/ResamplingInputReader.h
        muse/PhaseCorrect16MHzInputReader.cpp muse/PhaseCorrect16MHzInputReader.h
        muse/MuseRfDemodulator.cpp muse/MuseRfDemodulator.h
        muse/MuseInputBlock.h
        muse/MuseConstants.h
        muse/DiscCode.h muse/DiscCode.cpp
        ntsc/NtscRfDemodulator.cpp ntsc/NtscRfDemodulator.h
        ntsc/NtscInputReader.cpp ntsc/NtscInputReader.h
        ntsc/NtscInputBlock.h
        ntsc/NtscConstants.h
        ntsc/NtscDecoder.cpp ntsc/NtscDecoder.h
        ntsc/NtscShaders.cpp ntsc/NtscShaders.h
        ntsc/NtscFrame.cpp ntsc/NtscFrame.h
        ntsc/NtscFieldView.cpp ntsc/NtscFieldView.h
        ntsc/VbiData.cpp ntsc/VbiData.h
        musevk/VulkanManager.cpp musevk/VulkanManager.h
        musevk/CommandBuffer.cpp musevk/CommandBuffer.h
        musevk/CommandPool.h
        musevk/ComputeShader.cpp musevk/ComputeShader.h
        musevk/VulkanMemoryObject.cpp musevk/VulkanMemoryObject.h
        musevk/VulkanImage.cpp musevk/VulkanImage.h
        musevk/VulkanBuffer.cpp musevk/VulkanBuffer.h
        musevk/TimestampStatistics.cpp musevk/TimestampStatistics.h
        musevk/TimestampQueryPool.cpp musevk/TimestampQueryPool.h
        musevk/MemoryAllocator.cpp musevk/MemoryAllocator.h
        musevk/VulkanUtil.cpp musevk/VulkanUtil.h
        musevk/HalfFloatUtil.h
        musevk/Size.h
        musevk/Queue.h
    )

    if(HAVE_LIBAV)
        target_sources(musecpp PRIVATE VideoFileWriter.cpp VideoFileWriter.h)
    endif()

    target_include_directories(musecpp PRIVATE
        ${PORTAUDIO_INCLUDE_DIRS}
        ${GNURADIO_INCLUDE_DIRS}
        ${Vulkan_INCLUDE_DIRS}
    )
    if(HAVE_LIBAV)
        target_include_directories(musecpp PRIVATE ${LIBAV_INCLUDE_DIR})
    endif()

    target_link_options(musecpp PRIVATE "-L/usr/local/lib")
    target_link_libraries(musecpp
        PRIVATE ac3rf
        PRIVATE glfw
        PRIVATE Vulkan::Vulkan
        PRIVATE ${PORTAUDIO_LIBRARIES}
        PRIVATE ${GNURADIO_LINK_LIBRARIES}
    )
    if(HAVE_LIBAV)
        target_link_libraries(musecpp PRIVATE ${LIBAV_LIBRARIES})
    endif()

    # Compile GLSL compute shaders to SPIR-V
    set(GLSLC "glslc")
    file(GLOB GLSL_SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)
    foreach(GLSL ${GLSL_SOURCE_FILES})
        get_filename_component(FILE_NAME ${GLSL} NAME)
        set(SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/${FILE_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/"
            COMMAND ${GLSLC} -Werror -O ${GLSL} -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders -o ${SPIRV}
            DEPENDS ${GLSL}
        )
        list(APPEND SPIRV_BINARY_FILES ${SPIRV})
    endforeach()
    add_custom_target(Shaders DEPENDS ${SPIRV_BINARY_FILES})
    add_dependencies(musecpp Shaders)
endif()
