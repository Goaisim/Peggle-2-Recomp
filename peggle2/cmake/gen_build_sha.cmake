# Build-time Git SHA header generator (only updates timestamp on SHA change).
find_package(Git QUIET)
set(BUILD_SHA "")
if(GIT_FOUND AND DEFINED SRC_DIR)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE BUILD_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT BUILD_SHA AND DEFINED FALLBACK_SHA AND NOT "${FALLBACK_SHA}" STREQUAL "")
    set(BUILD_SHA "${FALLBACK_SHA}")
endif()
if(NOT BUILD_SHA)
    set(BUILD_SHA "unknown")
endif()

get_filename_component(OUT_DIR "${OUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUT_DIR}")
set(TMP_FILE "${OUT_FILE}.tmp")
file(WRITE "${TMP_FILE}" "#pragma once\n#define PEGGLE2_BUILD_SHA \"${BUILD_SHA}\"\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TMP_FILE}" "${OUT_FILE}"
)
file(REMOVE "${TMP_FILE}")
