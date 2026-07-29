# =============================================================================
# Docs.cmake -- the `docs` target.
#
#   cmake --build build --target docs      -> build/docs/html/index.html
#
# Doxygen is optional: if it is not installed the target is simply not created
# and configuration still succeeds.
# =============================================================================

find_package(Doxygen QUIET)

if (NOT DOXYGEN_FOUND)
    message(STATUS "castlemist: Doxygen not found -- 'docs' target disabled")
    return()
endif()

# Every path must be quoted: the project lives under a directory with a space
# in it, and Doxygen splits an unquoted INPUT on whitespace.
set(CASTLEMIST_DOXYGEN_INPUT
    "\"${PROJECT_SOURCE_DIR}/include\" \\\n    \"${PROJECT_SOURCE_DIR}/src\" \\\n    \"${PROJECT_SOURCE_DIR}/docs\" \\\n    \"${PROJECT_SOURCE_DIR}/README.md\"")
set(CASTLEMIST_DOXYGEN_OUTPUT "${PROJECT_BINARY_DIR}/docs")
set(CASTLEMIST_DOXYGEN_MAINPAGE "${PROJECT_SOURCE_DIR}/README.md")

configure_file("${PROJECT_SOURCE_DIR}/Doxyfile.in"
               "${PROJECT_BINARY_DIR}/Doxyfile" @ONLY)

add_custom_target(docs
    COMMAND ${DOXYGEN_EXECUTABLE} "${PROJECT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM)

message(STATUS "castlemist: docs target       = ${CASTLEMIST_DOXYGEN_OUTPUT}/html/index.html")
