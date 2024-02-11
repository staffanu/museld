find_package(Catch2 2.13 REQUIRED)

include_directories("..")
add_executable(Catch_tests_run ReedSolomonTest.cpp ../Logger.cpp)

target_link_libraries(Catch_tests_run
        PRIVATE Catch2::Catch2WithMain
        PRIVATE fmt::fmt
        efm_lib)

include(Catch)
catch_discover_tests(Catch_tests_run)
