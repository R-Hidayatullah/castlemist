cmake_minimum_required(VERSION 3.21)

project(castlemist
    VERSION     0.1.0
    DESCRIPTION "Guild Wars 2 .dat explorer (Win32 + Direct3D 11)"
    LANGUAGES   C CXX
)

# =============================================================================
# castlemist is built as a stack of small libraries, one per layer, so that
# each layer compiles, tests and documents on its own:
#
#   app     WinMain, the process entry point                     (executable)
#    +-- ui       Win32 shell: window procs, widgets, layout, content browser
#         +-- render   Direct3D 11 image/model/scene renderers
#         |    +-- sim      the reverse-engineered GW2 cloth solver
#         +-- extract  MFT entry -> previewable payload (the format dispatcher)
#         |    +-- media    audio (dr_mp3/stb_vorbis) and Bink video playback
#         |    +-- format   single-format decoders (dds, strs, chat links, ...)
#         +-- db       SQLite reader for the index tools/gw2index builds
#              +-- core     byte readers and other dependency-free helpers
#                   +-- native  GW2 dat/MFT, method-0 codec, ATEX, MODL, granny
#
# Dependencies point downward only. Nothing below `ui` may include a ui header,
# and nothing below `render` may touch Direct3D.
# =============================================================================

if (NOT WIN32)
    message(FATAL_ERROR "castlemist is a Win32 / Direct3D 11 application.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Everything runnable lands in one directory: the exe, the layer DLLs and the
# toolchain runtime next to each other, so the build tree is also the run tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------

option(CASTLEMIST_SHARED_LAYERS
       "Build each layer as its own DLL (fast incremental relinks)" ON)
option(CASTLEMIST_BUILD_TESTS "Build the unit tests"            ON)
option(CASTLEMIST_BUILD_TOOLS "Build the CLI tools under tools/" ON)
option(CASTLEMIST_BUILD_DOCS  "Add the 'docs' target (needs Doxygen)" OFF)
option(CASTLEMIST_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

add_compile_definitions(UNICODE _UNICODE)

if (CASTLEMIST_WARNINGS_AS_ERRORS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Werror)
endif()

include(Layers)
include(Externals)

message(STATUS "castlemist: build type   = ${CMAKE_BUILD_TYPE}")
message(STATUS "castlemist: layer kind   = ${CASTLEMIST_LAYER_KIND}")

# -----------------------------------------------------------------------------
# Layers (bottom-up). Each one arrives with its own tests; see docs/architecture.md.
# -----------------------------------------------------------------------------

castlemist_add_layer(native DEPS ext::json)

# -----------------------------------------------------------------------------
# Tests and documentation
# -----------------------------------------------------------------------------

if (CASTLEMIST_BUILD_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

if (CASTLEMIST_BUILD_TOOLS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

if (CASTLEMIST_BUILD_DOCS)
    include(Docs)
endif()
