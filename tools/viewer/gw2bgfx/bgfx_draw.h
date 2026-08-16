/// @file
/// @brief Composing the bgfx 64-bit draw state exactly as the client does.
///
/// Sources in `Gw2-64.exe`, re-anchored 2026-08-16 via the `BgfxDraw.cpp` path
/// anchor at 0x141C327A0:
///
///   BgfxDraw_MeshDrawLoop      0x140AB2CA0  BgfxDraw.cpp:2469-2505, 543
///   BgfxDraw_ComputeDepthState 0x140AB6D10
///   BgfxShader_BuildPassCull   0x140BFFD20  (cull, folded into the runtime effect)
///
/// @par The one thing to understand before reading further
///
/// An AMAT effect's `renderState` is **not** a complete bgfx state word. GW2
/// stores only the blend and ALPHA_REF fields there and leaves write-mask,
/// depth and cull clear, because the engine builds those three separately at
/// submit time and ORs the effect's word on top:
///
/// @verbatim
///   state = depthState            // BgfxDraw_ComputeDepthState
///         | instancingBit
///         | cull | effect.renderState
///         | blendEquationOverride
///         | writeMask
/// @endverbatim
///
/// Treat `renderState` as the whole state and the colour write mask comes out
/// zero -- the draw runs, costs fill rate, and paints nothing. That is a real
/// and very confusing failure mode.

#pragma once

#include <bgfx/bgfx.h>
#include <bgfx/defines.h>

#include <cstdint>

#include "amat_effect.h"

namespace gw2bgfx {

/// @brief Per-surface inputs the state composition reads.
///
/// Named for what the client reads rather than for a guess at intent: these
/// are two distinct flag words that the draw loop and the depth-state function
/// consult at different bit positions, and conflating them silently swaps
/// depth-write for depth-test.
struct GrSurfaceState {
    /// @brief `surface->material` flags -- `*(mesh->material + 28)`.
    ///
    /// | bit | effect |
    /// |---|---|
    /// | 0x0080 | depth bias -65 |
    /// | 0x0100 | force DEPTH_TEST_EQUAL |
    /// | 0x0200 | force DEPTH_TEST_GREATER |
    /// | 0x0400 | no ALPHA write |
    /// | 0x0800 | no RGB write |
    /// | 0x1000 | disable depth test |
    /// | 0x2000 | force LEQUAL **and** disable depth write |
    /// | 0x4000 | two-sided: skip the effect's cull bits entirely |
    uint32_t materialFlags = 0;
    /// @brief The mesh's own flag word -- `*(mesh + 40)` / `*(mesh + 44)`.
    ///        Feeds ::vsVariantFromMeshFlags only.
    uint32_t meshFlags = 0;
    /// @brief `*(surface + 12)`, consulted only for the depth-write decision.
    uint32_t surfaceFlags = 0;
    /// @brief The material's token64, which selects a fixed +65 depth bias for
    ///        the three decal-family tokens.
    uint64_t materialToken = 0;
    /// @brief `*(surface + 68)` -- non-zero enables the instancing state bit
    ///        when the effect also carries `shaderPassFlags & 0x40000`.
    bool instancing = false;
};

/// @brief Shader-level cull override, from `BgfxShader::m_flags`.
///
/// Checked *before* the effect's own cull bits and wins outright.
enum GrShaderCullFlags : uint32_t {
    kShaderCullCcw  = 0x04,
    kShaderCullCw   = 0x08,
    kShaderCullNone = 0x10,
};

/// @brief The composed state plus the pieces bgfx cannot carry.
struct GrDrawState {
    /// @brief The bgfx 64-bit state word, ready for `bgfx::setState`.
    uint64_t state = 0;
    /// @brief Depth bias in the client's units (+65 decal pull, -65 decal push).
    ///
    /// **Carried out of band -- stock bgfx has nowhere to put this.** The
    /// client's depth-state function returns the bias through a separate float
    /// out-parameter, not through the state word, so its caller must be
    /// programming a rasterizer state with it. Upstream bgfx's state word has
    /// no depth-bias field at all.
    ///
    /// Bit 59 (`0x0800000000000000`) is related but is *not* a "bias is
    /// active" flag: both fixed-bias branches set the float and leave bit 59
    /// clear, and only the third branch -- a per-frame bias read off the draw
    /// context -- sets it. Bit 59 is unused in upstream bgfx (56/57/58 are
    /// MSAA / LINEAA / CONSERVATIVE_RASTER), so it is an ArenaNet fork
    /// addition whose exact meaning is not yet established.
    ///
    /// Either way: linking stock bgfx means this value is computed and then
    /// dropped, so coplanar surfaces that depend on it -- decals, trim -- will
    /// z-fight until it is applied some other way.
    float depthBias = 0.0f;
};

/// @brief `BgfxDraw_ComputeDepthState` (0x140AB6D10) -- depth test, depth write
///        and depth bias.
///
/// @param passIndex   Frame pass index. Pass 0 tests LEQUAL and establishes the
///                    depth buffer; later passes test EQUAL against it, which is
///                    why drawing a later pass without having drawn pass 0 first
///                    produces an empty screen rather than a wrong-looking one.
/// @param passFlags   The effect's `shaderPassFlags`.
/// @param surf        Per-surface flags.
/// @param drawContextMode Mirrors the client's `drawCtx[+8] != 0` test. When
///                    false the whole depth decision collapses to
///                    DEPTH_TEST_ALWAYS with no depth write. What that context
///                    field actually selects has not been established, so it is
///                    named after where it is read rather than after a guess;
///                    the normal drawing path is `true`.
GrDrawState grComputeDepthState(uint32_t passIndex,
                                uint32_t passFlags,
                                const GrSurfaceState& surf,
                                bool drawContextMode = true);

/// @brief Render-target write mask, built as the draw loop builds it.
///
/// @verbatim
///   noRGB   = (passFlags & 0x0008) || (materialFlags & 0x800)
///   noAlpha = (passFlags & 0x4000) || (materialFlags & 0x400)
///   mask = (passFlags & 0x4) ? 0
///        : !noRGB            ? (noAlpha ? RGB : RGBA)
///        : !noAlpha          ? A
///        :                     0
/// @endverbatim
///
/// The old "blended draws get RGB, everything else RGBA" rule agreed with a
/// real frame by coincidence: every blended effect in that frame happened to
/// carry 0x4000. The cause is the flag, not the blending -- which also covers
/// the opaque effects that mask alpha and the blended ones that do not.
uint64_t grWriteMask(uint32_t passFlags, uint32_t materialFlags);

/// @brief Cull bits, from `BgfxShader_BuildPassCull` (0x140BFFD20).
///
/// Shader-level flags win over the effect's; `materialFlags & 0x4000`
/// (two-sided) suppresses culling entirely at the call site in
/// `BgfxShader_SelectEffect`.
///
/// @param mirrored Reproduces the client's mirrored-view swap: with a mirrored
///                 projection CULL_CW and CULL_CCW trade places.
uint64_t grCullState(uint32_t passFlags, uint32_t shaderFlags, bool mirrored = false);

/// @brief Compose the full draw state -- the port of the OR chain in
///        `BgfxDraw_MeshDrawLoop`.
///
/// @param effect      The selected effect; its `renderState` supplies blend and
///                    ALPHA_REF and nothing else.
/// @param shaderFlags `BgfxShader::m_flags`, for the cull override. Zero is the
///                    normal case (no override).
GrDrawState grComposeDrawState(const AmatEffect& effect,
                               uint32_t passIndex,
                               const GrSurfaceState& surf,
                               uint32_t shaderFlags = 0,
                               bool mirrored = false,
                               bool drawContextMode = true);

} // namespace gw2bgfx
