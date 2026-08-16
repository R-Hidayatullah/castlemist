# =============================================================================
# Bgfx.cmake -- builds the bgfx that Guild Wars 2 itself vendors.
#
# GW2's renderer is not "bgfx-like": it is upstream bgfx, vendored verbatim,
# with an ArenaNet wrapper (BgfxDdi/Draw/Texture/Shader/Buffer.cpp) on top. The
# Perforce paths in the client's .rdata say so outright:
#
#     Engine\Gr\Bgfx\External\bgfx\src\renderer_d3d11.cpp
#     Engine\Gr\Bgfx\External\bx\src\file.cpp
#     Engine\Gr\Bgfx\External\bimg\src\image.cpp
#
# So the AMAT shader blobs in Gw2.dat are literally bgfx shader blobs, the
# effect renderState words are literally bgfx 64-bit state words, and the
# GrFvf-derived vertex layouts are literally bgfx::VertexLayout. Linking the
# SAME bgfx the client links turns every one of those from "a format we
# translate" into "a value we pass through".
#
# ---------------------------------------------------------------------------
# The version is load-bearing, so it is pinned and checked here.
# ---------------------------------------------------------------------------
#
# Pulled from the client's own debug-stats banner call:
#
#     mov [rsp+...], 2247h            ; BGFX_REV_NUMBER  = 8775
#     mov [rsp+...], 80h              ; BGFX_API_VERSION = 128
#     lea rax, "... (commit: a476c5b9a42...)"
#
# A bgfx-master checkout is NOT a drop-in substitute. Two differences are
# ABI-visible and both silently corrupt vertex data rather than failing loudly:
#
#     | symbol             | this pin              | master (rev 9149) |
#     | Attrib::Count      | 18 (TexCoord0..7)     | 26 (TexCoord0..15)|
#     | AttribType::Count  | 5 {U8,U10,I16,Hf,Flt} | 9                 |
#
# bgfx::VertexLayout is sized off both counts, so a mismatched header shifts
# every offset past m_stride.
#
# The three fingerprints verified below do not depend on trusting the SHA
# string, and are what to re-check after any client patch. Note that
# src/version.h at this commit reads 8762/f37ffe97 -- it is auto-generated and
# was last regenerated 13 commits earlier -- so it is deliberately NOT one of
# the checks. Likewise BGFX_API_VERSION reads 127 here against the client's
# 128: ArenaNet's checkout is a handful of commits later than the SHA baked
# into their version.h. Neither number touches anything used below.
# =============================================================================

set(BGFX_DIR "${PROJECT_SOURCE_DIR}/external/bgfx")
set(BX_DIR   "${PROJECT_SOURCE_DIR}/external/bx")
set(BIMG_DIR "${PROJECT_SOURCE_DIR}/external/bimg")

foreach(_d BGFX BX BIMG)
    if (NOT EXISTS "${${_d}_DIR}/src")
        message(FATAL_ERROR
            "external/${_d} is missing. Fetch the pinned trio:\n"
            "  bgfx a476c5b9a42d3779af59a0099d4d222fa8898d36\n"
            "  bx   e7ede513dc8b90386960587e348c73b241f7735d\n"
            "  bimg 2afa64c14c1e3dd5d28412ee03bee0dfe7242f03\n"
            "See docs/research/gw2-bgfx-vendored-version.md.")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Fingerprint 1 + 2: the two enum counts VertexLayout is sized off.
#
# Checked by reading the header rather than compiling a probe, so a wrong
# checkout fails at configure time with a message that says what to do.
# -----------------------------------------------------------------------------
file(READ "${BGFX_DIR}/include/bgfx/bgfx.h" _bgfx_h)

if (NOT _bgfx_h MATCHES "TexCoord7,[^\n]*\n[ \t]*\n[ \t]*Count")
    message(FATAL_ERROR
        "external/bgfx: Attrib::Count is not 18 (TexCoord7 is not the last "
        "texcoord). This is not the bgfx Guild Wars 2 vendors -- see "
        "docs/research/gw2-bgfx-vendored-version.md.")
endif()

if (NOT _bgfx_h MATCHES "Uint8,[^\n]*\n[ \t]*Uint10,[^\n]*\n[ \t]*Int16,[^\n]*\n[ \t]*Half,[^\n]*\n[ \t]*Float,[^\n]*\n[ \t]*\n[ \t]*Count")
    message(FATAL_ERROR
        "external/bgfx: AttribType::Count is not 5 {Uint8,Uint10,Int16,Half,"
        "Float}. Wrong bgfx revision -- see "
        "docs/research/gw2-bgfx-vendored-version.md.")
endif()

# -----------------------------------------------------------------------------
# Fingerprint 3: the shader blob version byte.
#
# The client's embedded blobs carry 'VSH\x0b' / 'FSH\x0b'. Every AMAT blob we
# hand to bgfx::createShader must match what this tree's createShader accepts,
# so a bump here means the whole shader path silently stops loading.
# -----------------------------------------------------------------------------
file(READ "${BGFX_DIR}/tools/shaderc/shaderc.cpp" _shaderc_cpp LIMIT 2048)
if (NOT _shaderc_cpp MATCHES "define BGFX_SHADER_BIN_VERSION 11")
    message(FATAL_ERROR
        "external/bgfx: BGFX_SHADER_BIN_VERSION is not 11, so this tree will "
        "reject the client's VSH\\x0b / FSH\\x0b blobs.")
endif()

message(STATUS "bgfx: pinned tree verified (Attrib::Count=18, "
               "AttribType::Count=5, shader bin version 11)")

# -----------------------------------------------------------------------------
# bx
#
# amalgamated.cpp pulls in every translation unit except crtnone.cpp (the
# no-CRT build, which we do not want).
# -----------------------------------------------------------------------------
add_library(bx STATIC "${BX_DIR}/src/amalgamated.cpp")
target_include_directories(bx PUBLIC
    "${BX_DIR}/include"
    "${BX_DIR}/3rdparty")

# bx/bx.h includes <alloca.h>, <dirent.h> and the SAL annotation headers
# unconditionally. MinGW ships none of them, so bx carries its own shims and
# expects the toolchain's compat directory on the include path -- this is what
# bx's own GENie build does per-toolchain, and it is PUBLIC because bx.h is
# included transitively by every bgfx and bimg header we touch.
if (MINGW)
    target_include_directories(bx PUBLIC "${BX_DIR}/include/compat/mingw")
elseif (MSVC)
    target_include_directories(bx PUBLIC "${BX_DIR}/include/compat/msvc")
endif()
target_compile_definitions(bx PUBLIC
    __STDC_LIMIT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_CONSTANT_MACROS
    $<$<CONFIG:Debug>:BX_CONFIG_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:BX_CONFIG_DEBUG=0>)
target_compile_features(bx PUBLIC cxx_std_17)

# -----------------------------------------------------------------------------
# bimg
#
# Only the core is built. image_decode.cpp / image_encode.cpp pull in the
# astc, tinyexr and libsquish trees under bimg/3rdparty and exist to load PNG,
# EXR and friends -- formats that never appear in Gw2.dat. Every texture we
# feed bgfx is already-decoded RGBA8 from castlemist's ATEX/BCn path, which
# needs nothing beyond image.cpp's format tables.
# -----------------------------------------------------------------------------
# image.cpp's imageDecodeToRgba8 calls into the ASTC codec unconditionally --
# it is one arm of a format switch, so the reference exists even though nothing
# here decodes ASTC and the linker cannot drop it. bimg's own build compiles
# the vendored astc-encoder for exactly this reason, so do the same rather than
# stub out vendored source.
file(GLOB _astc_src "${BIMG_DIR}/3rdparty/astc-encoder/source/*.cpp")

add_library(bimg STATIC
    "${BIMG_DIR}/src/image.cpp"
    "${BIMG_DIR}/src/image_gnf.cpp"
    ${_astc_src})
target_include_directories(bimg PUBLIC "${BIMG_DIR}/include")
# image.cpp includes <astcenc.h> for the ASTC block descriptors even though we
# never encode ASTC; only the header is needed, not the encoder library.
target_include_directories(bimg PRIVATE
    "${BIMG_DIR}/3rdparty"
    "${BIMG_DIR}/3rdparty/astc-encoder/include")
target_link_libraries(bimg PUBLIC bx)

# -----------------------------------------------------------------------------
# bgfx
#
# Direct3D 11 only -- the same single backend the client ships (its .rdata
# carries renderer_d3d11.cpp and renderer_noop.cpp and nothing else). The other
# renderers still compile, but their config macros reduce them to empty
# translation units, which is how bgfx is meant to be trimmed.
#
# BGFX_CONFIG_MULTITHREADED=0 keeps every bgfx call on the calling thread, so a
# frame can be single-stepped in a debugger and an assert fires on the line
# that caused it rather than one frame later on the render thread.
# -----------------------------------------------------------------------------
add_library(bgfx STATIC "${BGFX_DIR}/src/amalgamated.cpp")
target_include_directories(bgfx
    PUBLIC  "${BGFX_DIR}/include"
    PRIVATE "${BGFX_DIR}/3rdparty"
            "${BGFX_DIR}/3rdparty/khronos"
            "${BGFX_DIR}/3rdparty/directx-headers/include/directx")
target_compile_definitions(bgfx PUBLIC
    BGFX_CONFIG_RENDERER_DIRECT3D11=1
    BGFX_CONFIG_RENDERER_DIRECT3D12=0
    BGFX_CONFIG_RENDERER_OPENGL=0
    BGFX_CONFIG_RENDERER_OPENGLES=0
    BGFX_CONFIG_RENDERER_VULKAN=0
    BGFX_CONFIG_RENDERER_AGC=0
    BGFX_CONFIG_RENDERER_GNM=0
    BGFX_CONFIG_RENDERER_NVN=0
    BGFX_CONFIG_RENDERER_METAL=0
    BGFX_CONFIG_RENDERER_WEBGPU=0
    BGFX_CONFIG_MULTITHREADED=0)
target_link_libraries(bgfx PUBLIC bx bimg)
if (WIN32)
    target_link_libraries(bgfx PUBLIC gdi32 psapi uuid ole32 oleaut32)
endif()

foreach(_t bx bimg bgfx)
    set_target_properties(${_t} PROPERTIES FOLDER "castlemist/external")
    # Third-party trees are not ours to keep warning-clean, and bgfx in
    # particular trips -Wattributes and -Wunused-* wholesale under MinGW.
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${_t} PRIVATE -w)
    endif()
endforeach()

add_library(ext::bgfx ALIAS bgfx)
