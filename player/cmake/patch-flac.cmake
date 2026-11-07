# Applies the patches in cmake/patches to the libFLAC source FetchContent
# extracted (run as its PATCH_COMMAND, in the source directory).  Idempotent:
# a marker file records what was applied, so a re-run leaves the tree alone.
#   cmake -DPATCH_DIR=<dir with the .patch files> -P patch-flac.cmake
if(EXISTS "${CMAKE_SOURCE_DIR}/.museld-patched")
    return()
endif()
file(GLOB PATCHES "${PATCH_DIR}/flac-*.patch")
list(SORT PATCHES)
foreach(PATCH ${PATCHES})
    message(STATUS "libFLAC: applying ${PATCH}")
    execute_process(COMMAND patch -p1 --forward -i "${PATCH}"
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "patch failed (${RESULT}): ${OUT} ${ERR}")
    endif()
endforeach()
file(WRITE "${CMAKE_SOURCE_DIR}/.museld-patched" "${PATCHES}\n")
