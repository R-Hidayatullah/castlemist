# =============================================================================
# Resources.cmake -- compile src/app/castlemist.rc.in into a linkable object.
#
# Sets CASTLEMIST_RES_OBJ to the object file, or to nothing when no resource
# compiler is available (the executable then builds without an icon, version
# block or manifest rather than failing).
#
# Why a custom command instead of listing the .rc as a source: MinGW's windres
# assembles its preprocessor invocation as an unquoted shell string. Adding the
# .rc to the target makes CMake pass every include directory the target has,
# and the first path containing a space aborts the preprocessor. Driving
# windres directly keeps the command line short -- and, crucially, lets us run
# it *from* the directory holding the .rc so the input is a bare filename. Even
# with no -I at all, an input path containing a space fails the same way,
# because windres hands that to the preprocessor unquoted as well.
# =============================================================================

find_program(CASTLEMIST_RC windres NAMES windres llvm-windres
             HINTS "${CMAKE_C_COMPILER}/.." PATHS ENV PATH)

set(CASTLEMIST_RES_OBJ "")

if (NOT CASTLEMIST_RC)
    message(STATUS "castlemist: windres not found -- exe will have no icon")
    return()
endif()

set(_app "${PROJECT_SOURCE_DIR}/src/app")

# Baked into the .rc so windres needs no -D. RC string literals accept a quoted
# path, so a directory with a space in it is fine here even though it is not on
# the command line.
set(CM_VER_MAJOR  "${PROJECT_VERSION_MAJOR}")
set(CM_VER_MINOR  "${PROJECT_VERSION_MINOR}")
set(CM_VER_PATCH  "${PROJECT_VERSION_PATCH}")
set(CM_VER_STRING "${PROJECT_VERSION}")
# Backslashes are RC escapes, so hand it forward slashes.
set(CM_ICON_PATH     "${_app}/castlemist.ico")
set(CM_MANIFEST_PATH "${_app}/castlemist.manifest")

set(_rc  "${PROJECT_BINARY_DIR}/castlemist.rc")
set(_obj "${PROJECT_BINARY_DIR}/castlemist_res.obj")

configure_file("${_app}/castlemist.rc.in" "${_rc}" @ONLY)

add_custom_command(
    OUTPUT  "${_obj}"
    # Bare input filename, resolved by WORKING_DIRECTORY. The output path may
    # contain spaces; only the preprocessor's input argument is fragile.
    COMMAND "${CASTLEMIST_RC}" -O coff -i castlemist.rc -o "${_obj}"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
    DEPENDS "${_rc}"
            "${_app}/castlemist.ico"
            "${_app}/castlemist.manifest"
    COMMENT "Compiling resources (icon, version, manifest)"
    VERBATIM)

add_custom_target(castlemist_resources DEPENDS "${_obj}")

set(CASTLEMIST_RES_OBJ "${_obj}")
# The object is produced by a custom command, not compiled from source; without
# this CMake tries to work out a language for it and fails.
set_source_files_properties("${_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

message(STATUS "castlemist: resources    = ${CASTLEMIST_RC}")
