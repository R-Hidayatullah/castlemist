/// @file
/// @brief Implementation of the draw-state ports. See bgfx_draw.h for the
///        source addresses each function was transcribed from.

#include "bgfx_draw.h"

namespace gw2bgfx {

GrDrawState grComputeDepthState(uint32_t passIndex,
                                uint32_t passFlags,
                                const GrSurfaceState& surf,
                                bool drawContextMode) {
    GrDrawState out;

    const uint32_t mat = surf.materialFlags;

    // `v10` in the decompilation: the DEPTH_TEST field, as a raw nibble<<4.
    uint64_t test;
    bool write;

    if (!drawContextMode) {
        // drawCtx.mode == 0: everything passes, nothing writes.
        test = BGFX_STATE_DEPTH_TEST_ALWAYS;    // 0x80
        write = false;
    } else {
        // Pass 0 lays the depth down; later passes match it exactly. The
        // 0x10 / 0x2000 escapes let an effect or a surface opt back into
        // LEQUAL on a later pass.
        if (passIndex == 0 || (passFlags & 0x10) || (mat & 0x2000))
            test = BGFX_STATE_DEPTH_TEST_LEQUAL;  // 0x20
        else
            test = BGFX_STATE_DEPTH_TEST_EQUAL;   // 0x30

        // `v12` -- the depth-write predicate, read off the surface's own word.
        write = !(surf.surfaceFlags & 0x4000)
             || ((surf.surfaceFlags & 0x2000) && !(mat & 0x3800000));
    }

    if ((passFlags & 0x40) || (mat & 0x2000)) write = false;
    if ((passFlags & 0x20) || (mat & 0x1000)) test = 0;
    if (mat & 0x100) test = BGFX_STATE_DEPTH_TEST_EQUAL;    // 0x30
    if (mat & 0x200) test = BGFX_STATE_DEPTH_TEST_GREATER;  // 0x50

    out.state = test | (write ? BGFX_STATE_WRITE_Z : 0);

    // --- Depth bias -------------------------------------------------------
    // Three material tokens are the decal family and pull toward the camera;
    // everything else either pushes (the 0x8000 / 0x80 flags) or takes the
    // context's per-frame bias.
    if (surf.materialToken == AmatTokenChain::kRemapTo2         // 0x6584D816C9EE
     || surf.materialToken == AmatTokenChain::kRemapFrom2       // 0x2C26C0B64F711EC
     || surf.materialToken == 0x48A635447ull) {
        out.depthBias = 65.0f;
    } else if ((passFlags & 0x8000) || (mat & 0x80)) {
        out.depthBias = -65.0f;
    }
    // The client has a third branch here: when neither of the above fires it
    // reads a per-frame float from the draw context and, only in that case,
    // ORs bit 59 into the state. Both +-65 branches set the bias WITHOUT
    // setting bit 59, so bit 59 is not a general "bias active" flag -- it
    // distinguishes the context-supplied bias from the two fixed ones. There
    // is no draw context offline, so that branch has no analogue here.
    return out;
}

uint64_t grWriteMask(uint32_t passFlags, uint32_t materialFlags) {
    const bool noRGB   = (passFlags & 0x0008u) || (materialFlags & 0x800u);
    const bool noAlpha = (passFlags & 0x4000u) || (materialFlags & 0x400u);

    if (passFlags & 0x0004u) return 0;

    if (!noRGB)   return noAlpha ? BGFX_STATE_WRITE_RGB
                                 : (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    if (!noAlpha) return BGFX_STATE_WRITE_A;
    return 0;
}

uint64_t grCullState(uint32_t passFlags, uint32_t shaderFlags, bool mirrored) {
    uint64_t cull;
    if (shaderFlags & kShaderCullCcw)       cull = BGFX_STATE_CULL_CCW;
    else if (shaderFlags & kShaderCullCw)   cull = BGFX_STATE_CULL_CW;
    else if (shaderFlags & kShaderCullNone) cull = 0;
    else if (passFlags & 1)                 cull = BGFX_STATE_CULL_CCW;
    else if (passFlags & 2)                 cull = BGFX_STATE_CULL_CW;
    else                                    cull = 0;

    if (mirrored) {
        if (cull == BGFX_STATE_CULL_CCW)     cull = BGFX_STATE_CULL_CW;
        else if (cull == BGFX_STATE_CULL_CW) cull = BGFX_STATE_CULL_CCW;
    }
    return cull;
}

GrDrawState grComposeDrawState(const AmatEffect& effect,
                               uint32_t passIndex,
                               const GrSurfaceState& surf,
                               uint32_t shaderFlags,
                               bool mirrored,
                               bool drawContextMode) {
    const uint32_t passFlags = effect.shaderPassFlags;

    GrDrawState out = grComputeDepthState(passIndex, passFlags, surf, drawContextMode);

    // Two-sided surfaces skip the cull bits entirely -- the client guards the
    // OR with `!(meshFlags & 0x4000)` rather than forcing CULL_NONE, which is
    // the same thing given cull is a two-bit field that defaults to off.
    if (!(surf.materialFlags & 0x4000u))
        out.state |= grCullState(passFlags, shaderFlags, mirrored);

    // The effect's own word: blend function and ALPHA_REF, ORed unconditionally.
    out.state |= effect.renderState;

    // passFlags 0x2000 forces a reverse-subtract blend equation on BOTH
    // channels. In the binary this is the literal 0x120000000, which is exactly
    // BGFX_STATE_BLEND_EQUATION(REVSUB) -- the macro ORs the value in at both
    // shift 28 and shift 31.
    if (passFlags & 0x2000u)
        out.state |= BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_REVSUB);

    if (surf.instancing && (passFlags & 0x40000u))
        out.state |= 0x0002000000000000ull;

    out.state |= grWriteMask(passFlags, surf.materialFlags);
    return out;
}

} // namespace gw2bgfx
