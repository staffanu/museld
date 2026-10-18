# ============================================================================
# ac3rf — reusable library (no CLI, no FLAC, no input readers)
# ============================================================================

add_library(ac3rf STATIC
    logging/Logger.h logging/StreamLogger.h logging/StreamLogger.cpp
    filter/FirFilterStage.cpp filter/FirFilterStage.h
    filter/ComplexFirFilterStage.cpp filter/ComplexFirFilterStage.h
    filter/HalfBand.h
    filter/KaiserLowPass.h
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
    analog/AnalogAudioDemodulator.cpp analog/AnalogAudioDemodulator.h
    analog/CxExpander.cpp analog/CxExpander.h
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
install(DIRECTORY ac3 analog efm filter rs
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
# libFLAC++ for the .ldf input: the system package where there is one, built
# from source where there is none (tvOS).  Sets MUSELD_FLAC_INCLUDE_DIRS and
# MUSELD_FLAC_LIBRARIES.
# ============================================================================

macro(museld_find_flac)
    if(NOT DEFINED MUSELD_FLAC_LIBRARIES)
        if(MUSELD_TVOS)
            message(STATUS "Building libogg and libFLAC from source into the app")
            include(FetchContent)
            # .ldf files are FLAC in an Ogg container (ld-compress), so libogg
            # first; FLAC links the Ogg::ogg target when it already exists
            set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
            set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
            set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
            set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(Ogg
                URL https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.xz
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
            FetchContent_MakeAvailable(Ogg)
            set(BUILD_CXXLIBS ON CACHE BOOL "" FORCE)
            set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
            set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
            set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
            set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
            set(WITH_OGG ON CACHE BOOL "" FORCE)
            set(WITH_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
            set(WITH_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
            set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
            set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
            set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
            set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(FLAC
                URL https://github.com/xiph/flac/releases/download/1.5.0/flac-1.5.0.tar.xz
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
            FetchContent_MakeAvailable(FLAC)
            set(MUSELD_FLAC_INCLUDE_DIRS "")
            set(MUSELD_FLAC_LIBRARIES FLAC::FLAC++)
        else()
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(FLAC REQUIRED flac++)
            set(MUSELD_FLAC_INCLUDE_DIRS ${FLAC_INCLUDE_DIRS})
            set(MUSELD_FLAC_LIBRARIES ${FLAC_LINK_LIBRARIES})
        endif()
    endif()
endmacro()

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

    museld_find_flac()

    add_executable(ac3rf-efm-decode
        ac3rf-efm-decode.cpp
        CliOptions.h
        Version.h
        input/InputReader.h
        input/ByteSource.cpp input/ByteSource.h
        input/HttpByteSource.cpp input/HttpByteSource.h input/HttpClient.h
        input/EthadcByteSource.cpp input/EthadcByteSource.h
        input/InputReaderFactory.cpp input/InputReaderFactory.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
        input/PrefetchingInputReader.cpp input/PrefetchingInputReader.h
    )

    target_include_directories(ac3rf-efm-decode PRIVATE ${MUSELD_FLAC_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/httplib
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
    target_link_options(ac3rf-efm-decode PRIVATE "-L/usr/local/lib")
    target_link_libraries(ac3rf-efm-decode
        PRIVATE ac3rf
        PRIVATE ${MUSELD_FLAC_LIBRARIES}
    )
    if(WIN32)
        # cpp-httplib (the http:// input) uses Winsock
        target_link_libraries(ac3rf-efm-decode PRIVATE ws2_32)
    endif()
endif()

# ============================================================================
# museld — real-time MUSE/NTSC laserdisc player (Vulkan + SDL3 + miniaudio)
# ============================================================================

if(BUILD_MUSE)
    # SDL3 for the window, the Vulkan surface and the keyboard (GLFW has no
    # tvOS, which is where museld is headed).  Distributions older than 2025
    # have no package for it, so fall back to building it into museld from a
    # release tarball; that needs the X11/Wayland development headers.
    if(NOT MUSELD_TVOS)
        find_package(SDL3 CONFIG QUIET)
    endif()
    if(SDL3_FOUND)
        message(STATUS "Using the installed SDL3 ${SDL3_VERSION}")
    else()
        message(STATUS "No SDL3 package found; building SDL3 from source into museld")
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    if(MUSELD_TVOS)
        # No Vulkan loader on tvOS: the headers from Khronos, and MoltenVK
        # linked statically from its release download (or an xcframework of
        # your own, e.g. the LunarG SDK's, through MUSELD_MOLTENVK_XCFRAMEWORK).
        # vulkan.hpp's static dispatcher then binds directly to MoltenVK.
        include(FetchContent)
        set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(VulkanHeaders
            URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.350.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(VulkanHeaders)

        set(MUSELD_MOLTENVK_XCFRAMEWORK "" CACHE PATH
            "A static MoltenVK.xcframework with tvOS slices; empty downloads the MoltenVK release")
        if(MUSELD_MOLTENVK_XCFRAMEWORK)
            set(MOLTENVK_XCFRAMEWORK "${MUSELD_MOLTENVK_XCFRAMEWORK}")
        else()
            FetchContent_Declare(MoltenVK
                URL https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-all.tar
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
            FetchContent_MakeAvailable(MoltenVK)
            set(MOLTENVK_XCFRAMEWORK "${moltenvk_SOURCE_DIR}/MoltenVK/static/MoltenVK.xcframework")
        endif()
        if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            set(MOLTENVK_SLICE tvos-arm64_x86_64-simulator)
        else()
            set(MOLTENVK_SLICE tvos-arm64_arm64e)
        endif()
        set(MOLTENVK_LIBRARY "${MOLTENVK_XCFRAMEWORK}/${MOLTENVK_SLICE}/libMoltenVK.a")
        if(NOT EXISTS "${MOLTENVK_LIBRARY}")
            message(FATAL_ERROR "MoltenVK for tvOS not found: ${MOLTENVK_LIBRARY}")
        endif()
        message(STATUS "Linking MoltenVK statically: ${MOLTENVK_LIBRARY}")
    else()
        find_package(Vulkan REQUIRED)
    endif()

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

    museld_find_flac()

    add_executable(museld
        museld.cpp
        CliOptions.h
        AudioDefs.h
        input/InputReader.h
        input/ByteSource.cpp input/ByteSource.h
        input/HttpByteSource.cpp input/HttpByteSource.h input/HttpClient.h
        input/EthadcByteSource.cpp input/EthadcByteSource.h
        input/InputReaderFactory.cpp input/InputReaderFactory.h
        input/LdsInputReader.cpp input/LdsInputReader.h
        input/LdfInputReader.cpp input/LdfInputReader.h
        input/PrefetchingInputReader.cpp input/PrefetchingInputReader.h
        input/PreparedInput.cpp input/PreparedInput.h
        input/RebatchingInputReader.h
        InputProbe.cpp InputProbe.h
        AudioPlayback.cpp AudioPlayback.h
        CompressedAudioDecoder.cpp CompressedAudioDecoder.h
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
        subtitles/Eia608Decoder.cpp subtitles/Eia608Decoder.h
        subtitles/SrtParser.cpp subtitles/SrtParser.h
        subtitles/SubtitleFont.cpp subtitles/SubtitleFont.h
        subtitles/SubtitleOverlay.cpp subtitles/SubtitleOverlay.h
        FrameBlitter.cpp FrameBlitter.h
        DisplayGeometry.cpp DisplayGeometry.h
        FrameExporter.cpp FrameExporter.h
        InputController.cpp InputController.h
        ChapterSearch.cpp ChapterSearch.h

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
        sdtv/SdtvRfDemodulator.cpp sdtv/SdtvRfDemodulator.h
        sdtv/SdtvFrameReader.cpp sdtv/SdtvFrameReader.h
        sdtv/SdtvInputBlock.h
        sdtv/VideoStandard.cpp sdtv/VideoStandard.h
        sdtv/NtscCadenceTracker.cpp sdtv/NtscCadenceTracker.h
        sdtv/PalCadenceTracker.cpp sdtv/PalCadenceTracker.h sdtv/CadenceAction.h
        sdtv/SdtvDecoder.cpp sdtv/SdtvDecoder.h
        sdtv/SdtvShaders.cpp sdtv/SdtvShaders.h
        sdtv/SdtvFrame.cpp sdtv/SdtvFrame.h
        sdtv/SdtvFieldView.cpp sdtv/SdtvFieldView.h
        sdtv/VbiData.cpp sdtv/VbiData.h
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
        ${MUSELD_FLAC_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/httplib
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    )
    if(WIN32)
        # cpp-httplib (the http:// input) uses Winsock
        target_link_libraries(museld PRIVATE ws2_32)
    endif()
    if(HAVE_LIBAV)
        target_include_directories(museld PRIVATE ${LIBAV_INCLUDE_DIR})
    endif()

    target_link_libraries(museld
        PRIVATE ac3rf
        PRIVATE SDL3::SDL3
        PRIVATE ${MUSELD_FLAC_LIBRARIES}
    )
    if(MUSELD_TVOS)
        target_compile_definitions(museld PRIVATE MUSELD_VULKAN_NO_LOADER)
        target_link_libraries(museld
            PRIVATE Vulkan::Headers
            PRIVATE "${MOLTENVK_LIBRARY}"
            # MoltenVK's
            PRIVATE "-framework Metal" "-framework QuartzCore" "-framework IOSurface"
                    "-framework CoreGraphics" "-framework Foundation" "-framework UIKit"
            # miniaudio's (no runtime linking on Apple's mobile platforms)
            PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox"
                    "-framework AVFoundation"
        )
    else()
        target_link_options(museld PRIVATE "-L/usr/local/lib")
        target_link_libraries(museld
            PRIVATE Vulkan::Vulkan
            PRIVATE ${CMAKE_DL_LIBS}
        )
    endif()
    if(HAVE_LIBAV)
        target_link_libraries(museld PRIVATE ${LIBAV_LIBRARIES})
    endif()
    if(APPLE)
        # The window surface is made from the window's Metal layer
        # (VulkanManager::createSurface), on macOS as on tvOS
        target_compile_definitions(museld PRIVATE VK_USE_PLATFORM_METAL_EXT)
    endif()

    # Compile GLSL compute shaders to SPIR-V
    set(GLSLC "glslc")
    file(GLOB_RECURSE GLSL_SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)
    # the headers the shaders include: glslc writes no depfile here, so every
    # SPIR-V build depends on all of them
    file(GLOB_RECURSE GLSL_HEADER_FILES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.h)
    # add_spirv(<glsl source> <output name> [<glslc define>]): one SPIR-V build
    function(add_spirv GLSL SPIRV_NAME)
        set(SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SPIRV_NAME}")
        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/"
            COMMAND ${GLSLC} -Werror -O ${ARGN} ${GLSL}
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders/muse
                    -o ${SPIRV}
            DEPENDS ${GLSL} ${GLSL_HEADER_FILES}
        )
        set(SPIRV_BINARY_FILES ${SPIRV_BINARY_FILES} ${SPIRV} PARENT_SCOPE)
    endfunction()
    # One SPIR-V per shader, except the SD shaders (shaders/sdtv/sdtv_*.comp),
    # which are built once per video standard: ntsc_*.comp.spv with the NTSC
    # frame geometry and pal_*.comp.spv with -DSDTV_PAL (see shaders/muse/muse.h);
    # SdtvShaders picks the set by VideoStandard::shader_prefix
    foreach(GLSL ${GLSL_SOURCE_FILES})
        get_filename_component(FILE_NAME ${GLSL} NAME)
        if(FILE_NAME MATCHES "^sdtv_(.*)$")
            add_spirv(${GLSL} "ntsc_${CMAKE_MATCH_1}.spv")
            add_spirv(${GLSL} "pal_${CMAKE_MATCH_1}.spv" -DSDTV_PAL)
        else()
            add_spirv(${GLSL} "${FILE_NAME}.spv")
        endif()
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

    if(MUSELD_TVOS)
        # miniaudio's AVAudioSession code is Objective-C
        set_source_files_properties(AudioPlayback.cpp PROPERTIES LANGUAGE OBJCXX)

        # The app bundle.  Signing: a free Apple ID's team suffices to run on
        # your own Apple TV (CMakeUserPresets.json is the place for the team
        # ID; the Xcode scheme's launch arguments are museld's command line).
        set(MUSELD_BUNDLE_IDENTIFIER "se.ulfberg.museld" CACHE STRING "Bundle identifier of the tvOS app")
        set(MUSELD_DEVELOPMENT_TEAM "" CACHE STRING "Apple developer team ID that signs the tvOS app")
        set(MUSELD_TVOS_LAUNCH_ARGUMENTS "" CACHE STRING
            "museld's command line when launched from the Xcode scheme, as a ;-separated list")
        set_target_properties(museld PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/tvOS-Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${MUSELD_BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE_BUNDLE_NAME museld
            MACOSX_BUNDLE_BUNDLE_VERSION 1
            MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MUSELD_BUNDLE_IDENTIFIER}"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY 3
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MUSELD_DEVELOPMENT_TEAM}"
            "XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED[sdk=appletvsimulator*]" NO
            XCODE_GENERATE_SCHEME TRUE
            XCODE_SCHEME_ARGUMENTS "${MUSELD_TVOS_LAUNCH_ARGUMENTS}"
        )

        # The shaders and the font ride in the bundle.  A tvOS bundle is flat:
        # "Resources" is the bundle root, next to the executable, so the
        # lookup relative to the executable's directory is the desktop's.
        target_sources(museld PRIVATE ${SPIRV_BINARY_FILES} ${MUSELD_FONT_DST})
        set_source_files_properties(${SPIRV_BINARY_FILES} PROPERTIES
            GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources/shaders)
        set_source_files_properties(${MUSELD_FONT_DST} PROPERTIES
            GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources/fonts)
    endif()
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
        ocr/OcrWorker.cpp ocr/OcrWorker.h
        ocr/TranslationWorker.cpp ocr/TranslationWorker.h)
    target_compile_features(museld-ocr PUBLIC cxx_std_20)
    target_include_directories(museld-ocr
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${ONNXRUNTIME_INCLUDE_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/httplib
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
    target_link_libraries(museld-ocr PUBLIC ${ONNXRUNTIME_LIBRARY} Threads::Threads)
    if(WIN32)
        # cpp-httplib (TranslationWorker) uses Winsock
        target_link_libraries(museld-ocr PUBLIC ws2_32)
    endif()

    if(BUILD_MUSE)
        target_sources(museld PRIVATE ocr/OcrBandCapture.cpp ocr/OcrBandCapture.h)
        target_compile_definitions(museld PRIVATE HAVE_OCR)
        target_link_libraries(museld PRIVATE museld-ocr)
    endif()

    add_executable(subocr-test ocr/subocr-test.cpp)
    target_include_directories(subocr-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb)
    target_link_libraries(subocr-test PRIVATE museld-ocr ac3rf)
endif()
