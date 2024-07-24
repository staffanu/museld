find_package(Catch2 3 REQUIRED)

include_directories("..")
add_executable(Catch_tests_run
        ReedSolomonTest.cpp
        BchDecoderTest.cpp
        ../util/Logger.cpp
        ../efm/ReedSolomon.h
        ../efm/GFValue.h
        ../efm/ByteWithErasureFlag.h
        ../muse/BchDecoder.cpp ../muse/BchDecoder.h
)

target_link_libraries(Catch_tests_run
        PRIVATE Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(Catch_tests_run)
