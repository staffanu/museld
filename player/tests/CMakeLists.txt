find_package(Catch2 3 REQUIRED)

add_executable(museld-tests
    ReedSolomonTest.cpp
    BchDecoderTest.cpp
    ../src/bch/BchDecoder.cpp
    ../src/logging/CategoryLogger.cpp
)

target_link_libraries(museld-tests
    PRIVATE Catch2::Catch2WithMain
    PRIVATE ac3rf
)

include(CTest)
include(Catch)
catch_discover_tests(museld-tests)
