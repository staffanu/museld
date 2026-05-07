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

# Homebrew LLVM's libc++ headers reference std::__1::__hash_memory, which is
# not exported by the macOS 15 SDK's libc++. Link against Homebrew's libc++
# when the Homebrew LLVM compiler is in use so the Python module resolves it.
if(APPLE AND CMAKE_CXX_COMPILER MATCHES "/(opt/llvm|Cellar/llvm)/")
    get_filename_component(_brew_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_brew_llvm_root "${_brew_llvm_bin}" DIRECTORY)
    set(_brew_libcxx_dir "${_brew_llvm_root}/lib/c++")
    if(EXISTS "${_brew_libcxx_dir}/libc++.dylib")
        target_link_options(ac3rf_python PRIVATE
            "-L${_brew_libcxx_dir}"
            "-Wl,-rpath,${_brew_libcxx_dir}")
    endif()
endif()

install(TARGETS ac3rf_python
    LIBRARY DESTINATION ${Python_SITEARCH}
)
