find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

include(FetchContent)
FetchContent_Declare(
    nanobind
    GIT_REPOSITORY https://github.com/wjakob/nanobind.git
    GIT_TAG        v2.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nanobind)

nanobind_add_module(ac3rf_python ac3rf.cpp)
target_link_libraries(ac3rf_python PRIVATE ac3rf)
# Suppress -Wdeprecated-literal-operator triggered by nanobind headers (third-party)
target_compile_options(nanobind-static PRIVATE -Wno-deprecated-literal-operator)
target_compile_options(ac3rf_python    PRIVATE -Wno-deprecated-literal-operator)
set_target_properties(ac3rf_python PROPERTIES OUTPUT_NAME ac3rf)

install(TARGETS ac3rf_python
    LIBRARY DESTINATION ${Python_SITEARCH}
)
