cmake_minimum_required(VERSION 3.21)

project(castlemist
    VERSION     0.3.0
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

# The bgfx Guild Wars 2 itself vendors, pinned and fingerprint-checked.
#
# Two consumers now: the 1:1 tools under tools/viewer/gw2bgfx, and the app's
# "Game 1:1" surface (src/render/gw2bgfx_view.cpp), which runs the same path on
# its own bgfx device inside a child window. The application's original
# Direct3D 11 renderer is untouched and remains the default -- the bgfx surface
# is a second, independent view for comparing against it.
#
# No longer gated on CASTLEMIST_BUILD_TOOLS, since the app itself wants it. A
# clone without external/bgfx still configures: gw2bgfx_view.cpp compiles to a
# stub whose available() returns false, and the UI hides the mode.
if (EXISTS "${PROJECT_SOURCE_DIR}/external/bgfx/src")
    include(Bgfx)
    set(CASTLEMIST_HAVE_BGFX ON)

    # The backend-agnostic half of the 1:1 renderer: AMAT effect selection and
    # the bgfx draw-state composition. Declared here rather than in
    # tools/CMakeLists.txt so the render layer can link it too.
    add_library(gw2bgfx_core STATIC
        tools/viewer/gw2bgfx/amat_effect.cpp
        tools/viewer/gw2bgfx/bgfx_draw.cpp)
    target_include_directories(gw2bgfx_core PUBLIC tools/viewer/gw2bgfx)
    target_link_libraries(gw2bgfx_core PUBLIC ext::bgfx)
    set_target_properties(gw2bgfx_core PROPERTIES FOLDER "castlemist/tools")
endif()

message(STATUS "castlemist: build type   = ${CMAKE_BUILD_TYPE}")
message(STATUS "castlemist: layer kind   = ${CASTLEMIST_LAYER_KIND}")

# -----------------------------------------------------------------------------
# Layers (bottom-up). Each one arrives with its own tests; see docs/architecture.md.
# -----------------------------------------------------------------------------

castlemist_add_layer(native DEPS ext::json)

castlemist_add_layer(core   DEPS castlemist::native)

castlemist_add_layer(format DEPS castlemist::core castlemist::native
                                 ext::jpegturbo ext::webp ext::stb
                                 windowscodecs ole32 oleaut32 uuid)

# db also builds the index (index_builder), which reads the archive directly and
# resolves chunk struct variants from the JSON template.
castlemist_add_layer(db     DEPS castlemist::core castlemist::native ext::sqlite ext::json)

castlemist_add_layer(media  DEPS castlemist::core castlemist::native
                                 ext::drlibs ext::stb ext::bink2
                                 winmm version)

# extract now also consults the gw2index DB (castlemist::db::lookup) to route
# entry detection off its pre-computed type/container/chunks before falling
# back to magic/chunk sniffing -- see entry_extractor.cpp's DB-first routing.
castlemist_add_layer(extract DEPS castlemist::format castlemist::media
                                  castlemist::native castlemist::db)

# sim sits beside render rather than under it, but it does need extract's mesh
# types: the solver is fed a ModelMeshCPU straight out of model extraction.
castlemist_add_layer(sim    DEPS castlemist::extract ext::glm)

castlemist_add_layer(render DEPS castlemist::extract castlemist::sim
                                 d3d11 dxgi d3dcompiler dxguid)

# The "Game 1:1" surface. PRIVATE: nothing above render needs bgfx headers --
# gw2bgfx_view.h is plain C++ and the UI only ever calls available().
if (CASTLEMIST_HAVE_BGFX)
    target_link_libraries(castlemist_render PRIVATE gw2bgfx_core)
    target_compile_definitions(castlemist_render PRIVATE CASTLEMIST_HAVE_BGFX=1)

    # bgfx.cpp defines two __declspec(dllexport) globals -- NvOptimusEnablement
    # and AmdPowerXpressRequestHighPerformance, the driver hints that ask a
    # laptop for its discrete GPU. On MinGW an explicit dllexport anywhere in a
    # DLL switches OFF ld's "export everything" default, and WINDOWS_EXPORT_ALL_
    # SYMBOLS relies on exactly that default: linking bgfx in dropped the render
    # layer's export table from hundreds of symbols to those two, and every
    # castlemist::render call from the ui layer failed to link.
    #
    # Passing the flag explicitly forces the behaviour back on rather than
    # leaving it as a default ld withdraws. The vendored bgfx tree is pinned and
    # fingerprint-checked, so it is not ours to patch.
    if (MINGW AND CASTLEMIST_SHARED_LAYERS)
        target_link_options(castlemist_render PRIVATE -Wl,--export-all-symbols)
    endif()
endif()

# uxtheme: SetWindowTheme, for the dark scroll bars and list headers.
# dwmapi is *not* linked -- DwmSetWindowAttribute is resolved with GetProcAddress
# so the exe still starts on a Windows without it.
castlemist_add_layer(ui     DEPS castlemist::render castlemist::db castlemist::extract
                                 comctl32 comdlg32 shell32 user32 gdi32 advapi32 uxtheme)


# -----------------------------------------------------------------------------
# Resources: the icon, the version block and the manifest
#
# Compiled with an explicit custom command rather than by adding the .rc to the
# target, because MinGW's windres assembles its preprocessor invocation as an
# unquoted shell string. CMake would hand it every include directory of the
# target, and the first one containing a space -- which, on Windows, is most of
# them -- kills the preprocessor with a "not recognized as an internal or
# external command" that names half a path. Here windres gets no -I and no -D
# at all: the version and the asset paths are substituted into the .rc at
# configure time instead.
# -----------------------------------------------------------------------------

include(Resources)

file(GLOB_RECURSE APP_SRCS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/app/*.cpp")
add_executable(castlemist WIN32 ${APP_SRCS} ${CASTLEMIST_RES_OBJ})
target_link_libraries(castlemist PRIVATE castlemist::ui)
target_include_directories(castlemist PRIVATE "${PROJECT_SOURCE_DIR}/src/app")
castlemist_link_runtime(castlemist)

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
