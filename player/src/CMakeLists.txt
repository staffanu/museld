# ============================================================================
# ac3rf — reusable library (no CLI, no FLAC, no input readers)
# ============================================================================

add_library(ac3rf STATIC
    logging/Logger.h logging/StreamLogger.h logging/StreamLogger.cpp
    filter/FirFilterStage.cpp filter/FirFilterStage.h
    filter/ComplexFirFilterStage.cpp filter/ComplexFirFilterStage.h
    filter/HalfBand.h
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
    efm/AdaptiveFilter.cpp efm/AdaptiveFilter.h
    efm/concealment/SlowArErasureConcealer.cpp efm/concealment/SlowArErasureConcealer.h
    efm/concealment/ErasureConcealer.h efm/concealment/ErasureConcealer.cpp
    efm/concealment/LinearInterpolationErasureConcealer.cpp efm/concealment/LinearInterpolationErasureConcealer.h
    efm/concealment/RepeatingSampleErasureConcealer.cpp efm/concealment/RepeatingSampleErasureConcealer.h
    efm/concealment/ArErasureConcealer.cpp efm/concealment/ArErasureConcealer.h
    efm/PopDetector.cpp efm/PopDetector.h
    efm/EfmPcmProcessor.cpp efm/EfmPcmProcessor.h
)

target_compile_features(ac3rf PUBLIC cxx_std_20)

# StreamLogger and the input readers use std::thread/std::mutex; link the platform
# threading library PUBLIC-ly so it propagates to everything that links ac3rf.
target_link_libraries(ac3rf PUBLIC Threads::Threads)

# The library is installed and exported for external use, so it may be linked
# into a shared object, which requires position-independent code on Linux.
set_target_properties(ac3rf PROPERTIES POSITION_INDEPENDENT_CODE ON)

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

# ============================================================================
# Install — allows the library to be found using find_package(ac3rf)
# ============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ac3rf
    EXPORT ac3rfTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES logging/Logger.h logging/StreamLogger.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf/logging)
install(DIRECTORY ac3 efm filter rs
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ac3rf
    FILES_MATCHING PATTERN "*.h"
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
        ac3rf-efm-decode.cpp
        CliOptions.h
        Version.h
        input/InputReader.h
        input/InputReaderFactory.cpp input/InputReaderFactory.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
        input/PrefetchingInputReader.cpp input/PrefetchingInputReader.h
    )

    target_include_directories(ac3rf-efm-decode PRIVATE ${FLAC_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_options(ac3rf-efm-decode PRIVATE "-L/usr/local/lib")
    target_link_libraries(ac3rf-efm-decode
        PRIVATE ac3rf
        PRIVATE ${FLAC_LINK_LIBRARIES}
    )
endif()

# ============================================================================
# museld — real-time MUSE/NTSC laserdisc player (Vulkan + GLFW + miniaudio)
# ============================================================================

if(BUILD_MUSE)
    find_package(glfw3 3.3 REQUIRED)
    find_package(Vulkan REQUIRED)

    set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules" ${CMAKE_MODULE_PATH})

    find_package(PkgConfig REQUIRED)

    if(USE_LIBAV)
        find_package(LIBAV)
        if(HAVE_LIBAV)
            add_compile_definitions(HAVE_LIBAV)
        else()
            message("FFmpeg not found or wrong version; not building video file writer support.")
        endif()
    else()
        message("USE_LIBAV is off; not building video file writer support.")
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FLAC REQUIRED flac++)

    add_executable(museld
        museld.cpp
        CliOptions.h
        AudioDefs.h
        input/InputReader.h
        input/InputReaderFactory.cpp input/InputReaderFactory.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
        input/PrefetchingInputReader.cpp input/PrefetchingInputReader.h
        AudioPlayback.cpp AudioPlayback.h
        FrameReader.h FrameReader.impl.h
        InputBlockBase.h
        Decoder.h
        DiscInfo.h
        DropoutMode.h
        ResultImages.h
        RfDemodulator.h
        TextRenderer.cpp TextRenderer.h
        PlayerState.h
        OsdOverlay.cpp OsdOverlay.h
        subtitles/SrtParser.cpp subtitles/SrtParser.h
        subtitles/SubtitleFont.cpp subtitles/SubtitleFont.h
        subtitles/SubtitleOverlay.cpp subtitles/SubtitleOverlay.h
        FrameBlitter.cpp FrameBlitter.h
        FrameExporter.cpp FrameExporter.h
        InputController.cpp InputController.h

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
        muse/MuseAdaptiveEqualizer.cpp muse/MuseAdaptiveEqualizer.h
        muse/MuseEqReference.h
        muse/ResamplingFrameReader.cpp muse/ResamplingFrameReader.h
        muse/PhaseCorrect16MHzFrameReader.cpp muse/PhaseCorrect16MHzFrameReader.h
        muse/MuseRfDemodulator.cpp muse/MuseRfDemodulator.h
        muse/MuseInputBlock.h
        muse/MuseConstants.h
        muse/DiscCode.h muse/DiscCode.cpp
        ntsc/NtscRfDemodulator.cpp ntsc/NtscRfDemodulator.h
        ntsc/NtscFrameReader.cpp ntsc/NtscFrameReader.h
        ntsc/NtscInputBlock.h
        ntsc/NtscConstants.h
        ntsc/NtscCadenceTracker.cpp ntsc/NtscCadenceTracker.h
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
        target_sources(museld PRIVATE VideoFileWriter.cpp VideoFileWriter.h)
    endif()

    target_include_directories(museld PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${FLAC_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio
    )
    if(HAVE_LIBAV)
        target_include_directories(museld PRIVATE ${LIBAV_INCLUDE_DIR})
    endif()

    target_link_options(museld PRIVATE "-L/usr/local/lib")
    target_link_libraries(museld
        PRIVATE ac3rf
        PRIVATE glfw
        PRIVATE Vulkan::Vulkan
        PRIVATE ${CMAKE_DL_LIBS}
        PRIVATE ${FLAC_LINK_LIBRARIES}
    )
    if(HAVE_LIBAV)
        target_link_libraries(museld PRIVATE ${LIBAV_LIBRARIES})
    endif()

    # Compile GLSL compute shaders to SPIR-V
    set(GLSLC "glslc")
    file(GLOB_RECURSE GLSL_SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)
    foreach(GLSL ${GLSL_SOURCE_FILES})
        get_filename_component(FILE_NAME ${GLSL} NAME)
        set(SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/${FILE_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/"
            COMMAND ${GLSLC} -Werror -O ${GLSL}
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders/muse
                    -o ${SPIRV}
            DEPENDS ${GLSL}
        )
        list(APPEND SPIRV_BINARY_FILES ${SPIRV})
    endforeach()
    add_custom_target(Shaders DEPENDS ${SPIRV_BINARY_FILES})
    add_dependencies(museld Shaders)

    # Copy the bundled subtitle font next to the binary so museld can find it at
    # "${executable_dir}/fonts/NotoSansJP-Regular.ttf" by default.
    set(MUSELD_FONT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/noto/NotoSansJP-Regular.ttf)
    set(MUSELD_FONT_DST ${CMAKE_CURRENT_BINARY_DIR}/fonts/NotoSansJP-Regular.ttf)
    add_custom_command(
        OUTPUT ${MUSELD_FONT_DST}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MUSELD_FONT_SRC} ${MUSELD_FONT_DST}
        DEPENDS ${MUSELD_FONT_SRC}
    )
    add_custom_target(SubtitleFontAsset DEPENDS ${MUSELD_FONT_DST})
    add_dependencies(museld SubtitleFontAsset)
endif()

# ============================================================================
# Subtitle OCR (USE_OCR) — PP-OCR text detection/recognition on ONNX Runtime.
# subocr-test is the standalone harness; the museld integration builds on the
# same OcrEngine.
# ============================================================================

if(USE_OCR)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime)
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR "USE_OCR is on but ONNX Runtime was not found")
    endif()

    add_library(museld-ocr STATIC
        ocr/OcrEngine.cpp ocr/OcrEngine.h
        ocr/OcrWorker.cpp ocr/OcrWorker.h)
    target_compile_features(museld-ocr PUBLIC cxx_std_20)
    target_include_directories(museld-ocr
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(museld-ocr PUBLIC ${ONNXRUNTIME_LIBRARY} Threads::Threads)

    if(BUILD_MUSE)
        target_sources(museld PRIVATE ocr/OcrBandCapture.cpp ocr/OcrBandCapture.h)
        target_compile_definitions(museld PRIVATE HAVE_OCR)
        target_link_libraries(museld PRIVATE museld-ocr)
    endif()

    add_executable(subocr-test ocr/subocr-test.cpp)
    target_include_directories(subocr-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb)
    target_link_libraries(subocr-test PRIVATE museld-ocr ac3rf)
endif()
