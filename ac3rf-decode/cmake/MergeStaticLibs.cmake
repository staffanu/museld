# MergeStaticLibs.cmake
# Merges LIB2 into OUTPUT (which already contains LIB1's content).
# Called at build time via: cmake -DOUTPUT=... -DLIB1=... -DLIB2=... -DAR=... -P MergeStaticLibs.cmake

if(APPLE)
    # macOS: libtool is always available and handles archive merging cleanly
    execute_process(
        COMMAND libtool -static -o "${OUTPUT}" "${LIB1}" "${LIB2}"
        RESULT_VARIABLE result
        ERROR_VARIABLE err
    )
else()
    # Linux: use ar MRI script (supported by GNU ar and LLVM ar)
    set(mri "${OUTPUT}.mri")
    file(WRITE "${mri}" "CREATE ${OUTPUT}\nADDLIB ${LIB1}\nADDLIB ${LIB2}\nSAVE\nEND\n")
    execute_process(
        COMMAND sh -c "${AR} -M < ${mri}"
        RESULT_VARIABLE result
        ERROR_VARIABLE err
    )
    file(REMOVE "${mri}")
endif()

if(result)
    message(FATAL_ERROR "Failed to merge static libraries: ${err}")
endif()
