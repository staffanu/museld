find_package(Catch2 3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FLAC REQUIRED flac++)

add_executable(museld-tests
    ReedSolomonTest.cpp
    BchDecoderTest.cpp
    FilterSimdParityTest.cpp
    InputProbeTest.cpp
    Eia608DecoderTest.cpp
    NtscCadenceTrackerTest.cpp
    PrefetchingInputReaderTest.cpp
    SrtParserTest.cpp
    ../src/subtitles/Eia608Decoder.cpp
    ../src/bch/BchDecoder.cpp
    ../src/InputProbe.cpp
    ../src/input/InputReaderFactory.cpp
    ../src/input/LdsInputReader.cpp
    ../src/input/LdfInputReader.cpp
    ../src/input/PrefetchingInputReader.cpp
    ../src/ntsc/NtscCadenceTracker.cpp
    ../src/subtitles/SrtParser.cpp
)

target_include_directories(museld-tests PRIVATE ../src ${FLAC_INCLUDE_DIRS})

target_link_options(museld-tests PRIVATE "-L/usr/local/lib")
target_link_libraries(museld-tests
    PRIVATE Catch2::Catch2WithMain
    PRIVATE ac3rf
    PRIVATE ${FLAC_LINK_LIBRARIES}
)

include(CTest)
include(Catch)
catch_discover_tests(museld-tests)
