/// @file
/// @brief GrFvf -> bgfx::VertexLayout, ported 1:1 from the client.
///
/// Sources, all in `Gw2-64.exe` (imagebase 0x140000000), re-anchored 2026-08-16:
///
///   GrFvf_BuildVertexLayout   0x140B9C310  BgfxBuffer.cpp:406
///   GrFvf_CreateVertexLayout  0x140B9D110  BgfxBuffer.cpp:754  (carries DDI_STRIDE inline)
///   GrFvf_TexCoordCountF32    0x140BA8CE0
///   GrFvf_TexCoordCountF16    0x140BA8D00
///   s_kTexCoordRemap          0x141C94420  = {10,11,12,13,14,15,16,17}
///
/// Resolve these by their Perforce-path xrefs, never by remembered address:
/// the addresses move every client patch, silently.
///
/// @par Why this matters more than a vertex-format helper usually does
///
/// `GrFvf_CreateVertexLayout` asserts, at BgfxBuffer.cpp:755, that
///
///     DDI_STRIDE(fvf) == vertexLayout->CalcStride()
///
/// -- the on-disk stride equals the GPU stride. That assert is live in the
/// shipping client, so for every fvf the game actually loads, **the vertex
/// buffer inside the MODL is already in GPU layout** and can be handed to
/// bgfx byte-for-byte. No repacking, no per-semantic offset table, no
/// guessing an element format from a reflection mask.
///
/// (Three fvf bits would break that equality if they ever appeared: 0x01000000
/// and 0x02000000 add 48 and 4 bytes of on-disk data the layout builder emits
/// nothing for, and 0x10000000 is 6 bytes on disk against a 4-byte Normal slot
/// on the GPU. The assert is what tells us they do not occur in shipped
/// content. ::grFvfStrideMatches checks for them rather than assuming.)

#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>

namespace gw2bgfx {

/// @brief GrFvf bitmask. Names are the client's own, from the assert strings
///        and from `GrFvf_ToString` (0x140BA8E90, GrFvf.cpp:1659), which prints
///        one mnemonic letter per bit: `p w i n c t b f`.
enum GrFvf : uint32_t {
    GR_FVF_POSITION            = 0x00000001, ///< 'p' 3 x float32
    GR_FVF_WEIGHTS             = 0x00000002, ///< 'w' 4 x uint8, normalized
    GR_FVF_GROUP               = 0x00000004, ///< 'i' 4 x uint8, RAW (bone indices)
    GR_FVF_NORMAL              = 0x00000008, ///< 'n' 3 x float32
    GR_FVF_COLOR               = 0x00000010, ///< 'c' 4 x uint8 normalized, BGRA
    GR_FVF_TANGENT             = 0x00000020, ///< 't' 3 x float32
    GR_FVF_BITANGENT           = 0x00000040, ///< 'b' 3 x float32
    GR_FVF_TANGENT_FRAME       = 0x00000080, ///< 'f' N+T+B packed, 3 x (4 x uint8 norm)

    GR_FVF_TEXCOORD_BITS       = 0x0000FF00, ///< TEXCOORD0..7, 2 x float32 each
    GR_FVF_TEXCOORD_F16_BITS   = 0x00FF0000, ///< the same channels as 2 x half

    GR_FVF_UNK_24              = 0x01000000, ///< +48 bytes on disk, no GPU element
    GR_FVF_UNK_25              = 0x02000000, ///< +4 bytes on disk, no GPU element
    GR_FVF_NORMAL_COMPRESSED   = 0x04000000, ///< 4 x uint8 normalized
    GR_FVF_POSITION4           = 0x08000000, ///< 4 x float32
    GR_FVF_POSITION_COMPRESSED = 0x10000000, ///< 6 bytes on disk, emitted as a Normal slot
    GR_FVF_TANGENT_FRAME_ALT   = 0x20000000, ///< 'F' second tangent-frame encoding
};

/// @brief `highestSetBit(fvf & 0xFF00) - 7` -- GrFvf_TexCoordCountF32, 0x140BA8CE0.
///
/// A count, not a popcount: texcoord bits are always allocated contiguously
/// upward from bit 8, and the client relies on that.
inline uint32_t grFvfTexCoordCountF32(uint32_t fvf) {
    uint32_t bits = fvf & GR_FVF_TEXCOORD_BITS;
    if (!bits) return 0;
    uint32_t hi = 31u - (uint32_t)__builtin_clz(bits);
    return hi - 7u;
}

/// @brief `highestSetBit(fvf & 0xFF0000) - 15` -- GrFvf_TexCoordCountF16, 0x140BA8D00.
inline uint32_t grFvfTexCoordCountF16(uint32_t fvf) {
    uint32_t bits = fvf & GR_FVF_TEXCOORD_F16_BITS;
    if (!bits) return 0;
    uint32_t hi = 31u - (uint32_t)__builtin_clz(bits);
    return hi - 15u;
}

/// @brief `DDI_STRIDE(fvf)` -- the on-disk vertex stride.
///
/// Transcribed from the expression the compiler inlined into
/// `GrFvf_CreateVertexLayout` at 0x140B9D110, with the bit-twiddling unfolded
/// back into the field tests it came from. The client computes, in one
/// expression:
///
/// @verbatim
///   (a2 & 4)                          GROUP               ->  4
///   ((a2 | 0x20000140) >> 2)  & 4     COLOR               ->  4
///   ((a2 | 0x20000140) >> 24) & 4     NORMAL_COMPRESSED   ->  4
///   ((a2 | 0x20000140) >> 23) & 0x10  POSITION4           -> 16
///   2 * (a2 & 2)                      WEIGHTS             ->  4
///   ...plus the explicit 12s and the texcoord terms
/// @endverbatim
///
/// The `| 0x20000140` is an optimiser artefact -- it sets bits 2, 6, 8 and 29,
/// none of which survives the shift-and-mask that follows, so each term is
/// just a branchless "is this bit set" test on bits 4, 26 and 27.
inline uint32_t grFvfDdiStride(uint32_t fvf) {
    uint32_t s = 0;
    if (fvf & GR_FVF_POSITION)            s += 12;
    if (fvf & GR_FVF_POSITION4)           s += 16;
    if (fvf & GR_FVF_WEIGHTS)             s += 4;
    if (fvf & GR_FVF_GROUP)               s += 4;
    if (fvf & GR_FVF_NORMAL)              s += 12;
    if (fvf & GR_FVF_NORMAL_COMPRESSED)   s += 4;
    if (fvf & GR_FVF_POSITION_COMPRESSED) s += 6;
    if (fvf & GR_FVF_COLOR)               s += 4;
    if (fvf & GR_FVF_TANGENT)             s += 12;
    if (fvf & GR_FVF_BITANGENT)           s += 12;
    if (fvf & GR_FVF_TANGENT_FRAME)       s += 12;
    if (fvf & GR_FVF_TANGENT_FRAME_ALT)   s += 12;
    if (fvf & GR_FVF_UNK_24)              s += 48;
    if (fvf & GR_FVF_UNK_25)              s += 4;
    s += 8 * grFvfTexCoordCountF32(fvf);
    s += 4 * grFvfTexCoordCountF16(fvf);
    return s;
}

/// @brief True when the on-disk buffer is already in GPU layout, so it can be
///        uploaded verbatim.
///
/// This is the client's own BgfxBuffer.cpp:755 assert, evaluated instead of
/// asserted. False means the fvf carries one of the three bits whose on-disk
/// size differs from its GPU element (0x01000000, 0x02000000, 0x10000000) and
/// the vertices would need converting first -- the job of
/// `GrFvf_ConvertVertices` (0x140BA7E60) and its ~50 per-format converters.
inline bool grFvfStrideMatches(uint32_t fvf, const bgfx::VertexLayout& layout) {
    return grFvfDdiStride(fvf) == layout.getStride();
}

/// @brief `GrFvf -> bgfx::VertexLayout`, a straight transcription of
///        `GrFvf_BuildVertexLayout` (0x140B9C310, BgfxBuffer.cpp:406).
///
/// **The order of the `add` calls below is the byte order in the vertex.**
/// This function is the authority on that -- not any table written by hand,
/// and not the shader's input signature. Keep the sequence identical to the
/// client's, including the two apparent oddities:
///
///  - `POSITION_COMPRESSED` (0x10000000) is emitted as a *Normal* slot, four
///    normalized uint8. It reads like a bug and is not: the field is
///    decompressed before upload, so the on-disk 6 bytes and the GPU 4 bytes
///    genuinely describe different things.
///  - `TANGENT_FRAME` expands to *three* adds, not one, and is mutually
///    exclusive with NORMAL|TANGENT|BITANGENT (asserted at :406).
///
/// @note `begin(RendererType::Noop)` is deliberate and matches the client's
///       `BgfxVertexLayout_Begin(&out->layout, BGFX_RENDERER_NOOP)`. It reads
///       like a stub and is not: `add` sizes each element through
///       `s_attribTypeSize[m_hash]`, and `initAttribTypeSizeTable` rewrites
///       **slot 0 -- the Noop slot -- to the active backend's table** when the
///       renderer comes up (vertexlayout.cpp:50). So passing Noop is bgfx's
///       idiom for "size these for whichever renderer is running", and it is
///       the only value that stays correct if the backend changes.
///
///       It is not cosmetic: the D3D1x and GL tables disagree. A 3-component
///       Int16 or Half is 8 bytes on D3D1x (padded to 4) and 6 on GL. No fvf
///       below reaches that cell -- texcoords are 2-component -- but hardcoding
///       a backend here is how a layout silently mis-strides later.
inline bgfx::VertexLayout grFvfBuildVertexLayout(uint32_t fvf) {
    // s_kTexCoordRemap @ 0x141C94420. IDA auto-typed it as the string "\n"
    // because the first entry is 10; it is a dword array.
    static const bgfx::Attrib::Enum kTexCoordRemap[8] = {
        bgfx::Attrib::TexCoord0, bgfx::Attrib::TexCoord1,
        bgfx::Attrib::TexCoord2, bgfx::Attrib::TexCoord3,
        bgfx::Attrib::TexCoord4, bgfx::Attrib::TexCoord5,
        bgfx::Attrib::TexCoord6, bgfx::Attrib::TexCoord7,
    };

    bgfx::VertexLayout layout;
    layout.begin(bgfx::RendererType::Noop);

    if (fvf & GR_FVF_POSITION)
        layout.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float);
    if (fvf & GR_FVF_POSITION4)
        layout.add(bgfx::Attrib::Position,  4, bgfx::AttribType::Float);
    if (fvf & GR_FVF_WEIGHTS)
        layout.add(bgfx::Attrib::Weight,    4, bgfx::AttribType::Uint8, true);
    if (fvf & GR_FVF_GROUP)
        layout.add(bgfx::Attrib::Indices,   4, bgfx::AttribType::Uint8, false);
    if (fvf & GR_FVF_NORMAL)
        layout.add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float);
    if (fvf & GR_FVF_NORMAL_COMPRESSED)
        layout.add(bgfx::Attrib::Normal,    4, bgfx::AttribType::Uint8, true);
    if (fvf & GR_FVF_POSITION_COMPRESSED)
        layout.add(bgfx::Attrib::Normal,    4, bgfx::AttribType::Uint8, true);
    if (fvf & GR_FVF_COLOR)
        layout.add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true);
    if (fvf & GR_FVF_TANGENT)
        layout.add(bgfx::Attrib::Tangent,   3, bgfx::AttribType::Float);
    if (fvf & GR_FVF_BITANGENT)
        layout.add(bgfx::Attrib::Bitangent, 3, bgfx::AttribType::Float);
    if (fvf & GR_FVF_TANGENT_FRAME) {
        layout.add(bgfx::Attrib::Normal,    4, bgfx::AttribType::Uint8, true);
        layout.add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Uint8, true);
        layout.add(bgfx::Attrib::Bitangent, 4, bgfx::AttribType::Uint8, true);
    }

    uint32_t nF32 = grFvfTexCoordCountF32(fvf);
    for (uint32_t i = 0; i < nF32 && i < 8; ++i)
        layout.add(kTexCoordRemap[i], 2, bgfx::AttribType::Float);

    uint32_t nF16 = grFvfTexCoordCountF16(fvf);
    for (uint32_t i = 0; i < nF16 && i < 8; ++i)
        layout.add(kTexCoordRemap[i], 2, bgfx::AttribType::Half);

    layout.end();
    return layout;
}

} // namespace gw2bgfx
