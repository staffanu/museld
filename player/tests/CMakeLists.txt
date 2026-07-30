find_package(Catch2 3 REQUIRED)

add_executable(museld-tests
    ReedSolomonTest.cpp
    BchDecoderTest.cpp
    FilterSimdParityTest.cpp
    NtscCadenceTrackerTest.cpp
    PrefetchingInputReaderTest.cpp
    SrtParserTest.cpp
    ../src/bch/BchDecoder.cpp
    ../src/input/PrefetchingInputReader.cpp
    ../src/ntsc/NtscCadenceTracker.cpp
    ../src/subtitles/SrtParser.cpp
)

target_include_directories(museld-tests PRIVATE ../src)

target_link_libraries(museld-tests
    PRIVATE Catch2::Catch2WithMain
    PRIVATE ac3rf
)

include(CTest)
include(Catch)
catch_discover_tests(museld-tests)
