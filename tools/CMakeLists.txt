# =============================================================================
# tools/ -- standalone utilities that share castlemist's format layers
#
# Everything here is a command-line program: it dumps, converts or probes, and
# writes into dumps/. None of it is needed to run castlemist itself, so the
# whole directory can be switched off with -DCASTLEMIST_BUILD_TOOLS=OFF.
#
# The tools that are not built here -- the python scripts under shaders/, strs/
# and structs/, the in-process probes under hook/, the offline host and server
# research under host/ and emu/ -- have their own instructions in tools/README.md.
# =============================================================================

# -----------------------------------------------------------------------------
# castlemist_add_tool(<name> SOURCES <files...> [DEPS <targets...>] [WIN32])
# -----------------------------------------------------------------------------
function(castlemist_add_tool name)
    cmake_parse_arguments(ARG "WIN32" "" "SOURCES;DEPS" ${ARGN})

    if (ARG_WIN32)
        add_executable(${name} WIN32 ${ARG_SOURCES})
    else()
        add_executable(${name} ${ARG_SOURCES})
    endif()
    target_link_libraries(${name} PRIVATE ${ARG_DEPS})
    castlemist_link_runtime(${name})
    set_target_properties(${name} PROPERTIES FOLDER "castlemist/tools")
endfunction()

# -----------------------------------------------------------------------------
# gw2index -- builds dumps/index/gw2_index.db from a Gw2.dat
#
# Multithreaded and resumable: a full index is a few hundred thousand entries
# and every one has to be decompressed far enough to learn its real type, so a
# cold run takes a while and must survive being interrupted.
# -----------------------------------------------------------------------------
castlemist_add_tool(gw2index
    SOURCES gw2index/gw2index.cpp
    DEPS    castlemist::db)

# -----------------------------------------------------------------------------
# gw2dat_cli -- extract, decompress, decode and re-compress single entries
#
# The scriptable half of castlemist, and what the MCP servers under mcp/ shell
# out to.
# -----------------------------------------------------------------------------
castlemist_add_tool(gw2dat_cli
    SOURCES gw2dat_cli/main.cpp
    DEPS    castlemist::native ext::json ext::stb)

# -----------------------------------------------------------------------------
# The two prototype viewers the renderer was developed against. They predate
# src/render and are kept because they are small enough to bisect a rendering
# question in, which the full application is not.
#
#   gw2viewer     a model and texture viewer with hand-written HLSL
#   gw2gsviewer   the same, but driving the game's own DXBC shaders
# -----------------------------------------------------------------------------
castlemist_add_tool(gw2viewer   WIN32
    SOURCES viewer/gw2viewer.cpp
    DEPS    castlemist::native ext::json ext::stb d3d11 dxgi d3dcompiler dxguid)

castlemist_add_tool(gw2gsviewer WIN32
    SOURCES viewer/gw2gsviewer.cpp
    DEPS    castlemist::native ext::json ext::stb d3d11 dxgi d3dcompiler dxguid)

# -----------------------------------------------------------------------------
# gw2bgfx -- the 1:1 renderer.
#
# The two viewers above translate GW2's render data into hand-written Direct3D
# 11 calls. This one does not translate: it links the same bgfx the client
# links (external/bgfx, pinned -- see cmake/Bgfx.cmake) and hands bgfx the
# client's own bytes. AMAT shader blobs go to bgfx::createShader unmodified,
# effect renderState words go into bgfx::setState, GrFvf-derived layouts are
# bgfx::VertexLayout, and vertex buffers upload without repacking because the
# client's own BgfxBuffer.cpp:755 assert guarantees the on-disk stride already
# equals the GPU stride.
#
# Everything under gw2bgfx/ is a transcription of decompiled client code with
# the source address and Perforce line in the comment. selftest checks those
# transcriptions against values read out of the binary and out of real archive
# content -- run it after touching any of them.
# -----------------------------------------------------------------------------
if (CASTLEMIST_HAVE_BGFX)
    # gw2bgfx_core itself is declared in the top-level CMakeLists now: the app's
    # "Game 1:1" surface (src/render/gw2bgfx_view.cpp) links it too, so it has
    # to exist whether or not tools are being built.

    castlemist_add_tool(gw2bgfx_selftest
        SOURCES viewer/gw2bgfx/selftest.cpp
        DEPS    gw2bgfx_core)

    # gw2bgfx_probe -- runs the faithful path over a real model and reports what
    # the client would do, without rendering. Checks the two things that have
    # definite right answers: the vertex-stride identity and whether each
    # material's effect was found by its own token or by the fallback.
    castlemist_add_tool(gw2bgfx_probe
        SOURCES viewer/gw2bgfx/probe.cpp
        DEPS    gw2bgfx_core castlemist::native ext::json)

    # gw2bgfx_viewer -- the renderer itself.
    castlemist_add_tool(gw2bgfx_viewer
        SOURCES viewer/gw2bgfx/viewer.cpp
        DEPS    gw2bgfx_core castlemist::native ext::json)

    if (CASTLEMIST_BUILD_TESTS)
        add_test(NAME gw2bgfx_selftest COMMAND gw2bgfx_selftest)
    endif()
endif()
