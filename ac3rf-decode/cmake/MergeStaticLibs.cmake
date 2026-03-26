# MergeStaticLibs.cmake
# Merges LIB2 into OUTPUT (which already contains LIB1's content).
# Called at build time via: cmake -DOUTPUT=... -DLIB1=... -DLIB2=... -DAR=... -P MergeStaticLibs.cmake
#
# Uses an ar MRI script, which is supported by GNU ar and LLVM ar (used by
# the Nix clang toolchain on both Linux and macOS).  Apple's BSD ar does not
# support MRI, but that ar is never CMAKE_AR in a Nix build.

set(mri "${OUTPUT}.mri")
file(WRITE "${mri}" "CREATE ${OUTPUT}\nADDLIB ${LIB1}\nADDLIB ${LIB2}\nSAVE\nEND\n")
execute_process(
    COMMAND sh -c "${AR} -M < ${mri}"
    RESULT_VARIABLE result
    ERROR_VARIABLE err
)
file(REMOVE "${mri}")

if(result)
    message(FATAL_ERROR "Failed to merge static libraries: ${err}")
endif()
