find_package(Catch2 2.13 REQUIRED)

include_directories("..")
add_executable(Catch_tests_run
        ReedSolomonTest.cpp
        ../util/Logger.cpp
        ../efm/ReedSolomon.h
        ../efm/GFValue.h
        ../efm/ByteWithErasureFlag.h)

target_link_libraries(Catch_tests_run
        PRIVATE Catch2::Catch2WithMain
        PRIVATE fmt::fmt
)

include(Catch)
catch_discover_tests(Catch_tests_run)
