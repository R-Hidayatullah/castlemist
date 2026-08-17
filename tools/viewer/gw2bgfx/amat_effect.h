/// @file
/// @brief AMAT technique/pass/effect selection, ported 1:1 from the client.
///
/// Sources in `Gw2-64.exe`, re-anchored 2026-08-16 (the addresses in the older
/// notes have rotted -- resolve by the `BgfxShader.cpp` path anchor at
/// 0x141D12310, never by remembered address):
///
///   BgfxShader_BuildPasses    0x140BFFB20  BgfxShader.cpp:860   technique = quality
///   BgfxShader_SelectEffect   0x140BFDC40  BgfxShader.cpp:1069-1200
///   BgfxShader_FindVsVariant  0x140BFE990  BgfxShader.cpp:1037
///
/// @par Why this replaces a heuristic
///
/// An AMAT holds every shader the engine binds for that material across the
/// whole frame -- the deferred normal prepass, depth-only, depth-peel, and the
/// real colour pass. Picking by "which pixel shader has the most samplers", or
/// by scanning for the SH uniform block, gets the right answer often and the
/// wrong answer silently. The engine does not guess: it looks the effect up by
/// a **64-bit token**, with a hardcoded three-link remap chain and a default.
/// That is what is implemented here.
///
/// `shaderPassFlags` is emphatically **not** an opaque/transparent or
/// colour/prepass label -- see ::AmatEffect::shaderPassFlags.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gw2bgfx {

// ---------------------------------------------------------------------------
// The parsed AMAT tree, one struct per packfile type.
//
// Field names are the packfile schema's own (dumps/packfile/gw2_packfile.json),
// and every offset was cross-checked against the runtime layout the decompiler
// shows -- AmatEffectV1 is 8+8+4+4+12 = 36 bytes, exactly the stride
// BgfxShader_SelectEffect indexes with.
// ---------------------------------------------------------------------------

/// @brief `AmatVertexShaderVariantV1` -- one vertex feed of an effect.
struct AmatVertexShaderVariant {
    /// @brief The variant id the engine matches against, derived from mesh flags.
    ///
    /// Not a Token: these are the small integers 0..4 enumerated in
    /// ::vsVariantFromMeshFlags.
    uint32_t variant = 0;
    uint32_t vertexShaderIndex = 0; ///< Index into AmatPackage::shaders.
};

/// @brief `AmatEffectV1` -- one (vertex shader set + pixel shader + state) tuple.
struct AmatEffect {
    /// @brief The selection key. This, not any content heuristic, is how the
    ///        engine finds the effect it wants.
    uint64_t token = 0;
    /// @brief The bgfx 64-bit state word, ORed into the draw state unconditionally.
    ///
    /// **Partial, not a full state.** GW2 leaves the write-mask, depth and cull
    /// fields clear here because the engine composes those separately at submit
    /// and ORs this on top (see bgfx_draw.h). Decoding it as a complete bgfx
    /// state zeroes the colour write mask and the draw comes out invisible.
    /// Only the blend and ALPHA_REF fields are meaningful.
    uint64_t renderState = 0;
    /// @brief Render-target / depth / cull bits, read off the engine's draw path.
    ///
    /// | bit | meaning |
    /// |---|---|
    /// | 0x0001 | cull CCW |
    /// | 0x0002 | cull CW |
    /// | 0x0004 | colour write mask = 0 (depth-only / prepass) |
    /// | 0x0008 | no RGB write |
    /// | 0x0010 | force DEPTH_TEST_LEQUAL even on a later pass |
    /// | 0x0020 | disable depth test |
    /// | 0x0040 | disable depth write (translucent) |
    /// | 0x2000 | blend equation REVSUB, both channels |
    /// | 0x4000 | no ALPHA write |
    /// | 0x8000 | depth bias -65 (decal push) |
    /// | 0x40000 | with instancing, ORs 0x2000000000000 |
    ///
    /// Bits 0 and 1 are new here: older notes read 0x1 as "opaque". It is the
    /// cull mode, set in the pass builder at 0x140BFFD20.
    uint32_t shaderPassFlags = 0;
    uint32_t pixelShaderIndex = 0;  ///< Index into AmatPackage::shaders.
    std::vector<AmatVertexShaderVariant> vertexShaderVariants;
};

/// @brief `AmatPassV1`.
struct AmatPass {
    std::vector<AmatEffect> effects;
};

/// @brief `AmatTechniqueV1` -- a **quality level**, not a frame pass.
struct AmatTechnique {
    /// @brief Token32 decoding to `ultra` / `high` / `medium` / `low`.
    ///
    /// Only the first character is looked at (`BgfxShader_BuildPasses`
    /// switches on `name[0]`), which is why an unrecognised name is merely
    /// logged and treated as quality 0 rather than rejected.
    uint32_t quality = 0;
    std::vector<AmatPass> passes;
};

/// @brief `AmatSamplerConstantV1` -- one texture binding of a shader.
struct AmatSamplerConstant {
    uint64_t token = 0;
    uint32_t stateIndex = 0;    ///< Index into AmatPackage::samplerStates.
    /// @brief Which of the MODL material's textures to bind.
    ///
    /// `0xFFFFFFFF` means this sampler consumes **no bgfx uniform** -- the
    /// rule the engine's own count assert depends on (BgfxShader.cpp:336).
    uint32_t textureIndex = 0;
    uint32_t textureSlot = 0;   ///< The t#/s# register to bind it to.
};

/// @brief `AmatShaderBinaryV1`.
struct AmatShaderBinary {
    /// @brief The bgfx shader blob -- `VSH\x0b` / `FSH\x0b` plus DXBC.
    ///
    /// This is handed to `bgfx::createShader` verbatim. It is not a container
    /// we unpack: it is the exact byte sequence bgfx's own `ShaderD3D11::create`
    /// expects, which is the whole reason for linking the client's bgfx.
    std::vector<uint8_t> data;
    /// @brief Ordered token32 list, one per **non-sampler** uniform of the shader.
    std::vector<uint32_t> constants;
    /// @brief Ordered list, one per **sampler** uniform of the shader.
    std::vector<AmatSamplerConstant> samplers;
};

/// @brief `AmatShaderV1`.
struct AmatShader {
    bool isPixelShader = false;
    AmatShaderBinary dx11Shader;
    /// @brief The Metal blob. Present in the file, never used on this path.
    ///
    /// The client picks between the two on a context flag that is zero on the
    /// Direct3D 11 backend. Older notes called this field `pbrShader` and read
    /// the choice as a shading-model switch; the packfile schema names it
    /// `osxShader` and it is a platform switch.
    AmatShaderBinary osxShader;
};

/// @brief `AmatMaterialV1` -- the whole BGFX chunk of an AMAT file.
struct AmatPackage {
    std::vector<AmatShader> shaders;
    /// @brief `AmatSamplerStateV1::state` -- a bgfx sampler-flags dword.
    std::vector<uint32_t> samplerStates;
    std::vector<AmatTechnique> techniques;
    std::string error;

    bool ok() const { return error.empty() && !shaders.empty(); }
};

// ---------------------------------------------------------------------------
// Quality / technique selection -- BgfxShader_BuildPasses, 0x140BFFB20.
// ---------------------------------------------------------------------------

/// @brief Shader quality tiers, in the client's own numbering.
enum GrShaderQuality : int {
    kQualityInvalid = 0, ///< A technique whose name did not decode to one of the four.
    kQualityLow     = 1,
    kQualityMedium  = 2,
    kQualityHigh    = 3,
    kQualityUltra   = 4,
    kQualityCount   = 5, ///< `GR_SHADER_QUALITIES`, asserted against at :860.
};

/// @brief Rank a technique's quality token.
///
/// The client decodes the token to a string and switches on its first
/// character only.
int amatQualityOf(uint32_t qualityToken);

/// @brief Pick the technique index to draw with, exactly as
///        `BgfxShader_BuildPasses` does.
///
/// The loop is subtle and worth preserving verbatim: it walks techniques in
/// **file order** and stops at the first one whose quality is `<= maxQuality`,
/// rather than scanning for the best match. If none qualifies, it falls back to
/// the last technique examined. So the answer depends on the order techniques
/// appear in the file, not only on their quality values.
///
/// @param maxQuality The engine's configured cap (::GrShaderQuality).
/// @param outQuality Receives the chosen tier, if non-null.
/// @return Technique index, or -1 when the package has none.
int amatSelectTechnique(const AmatPackage& pkg, int maxQuality, int* outQuality = nullptr);

// ---------------------------------------------------------------------------
// Effect selection -- BgfxShader_SelectEffect, 0x140BFDC40.
// ---------------------------------------------------------------------------

/// @brief The vertex-shader variant ids, as the draw loop computes them.
///
/// From `BgfxDraw_MeshDrawLoop` (0x140AB2CA0), on the mesh word at +40:
///
/// @verbatim
///   if ((meshFlags & 4) && (meshFlags & 2) && !(surfaceFlags & 2))  -> 1
///   else if (!(meshFlags & 0x80))   -> instanced ? 3 : 0
///   else                            -> instanced ? 4 : 2
/// @endverbatim
///
/// @par Variant 1 is the GPU-SKINNED one, and 0x80 is not the skinned bit
///
/// An earlier reading of this called `0x80` "the skinned bit" and named variant
/// 2 `kVsSkinned`, leaving variant 1 as an unexplained special case. That was
/// backwards, and it is checkable: walk every effect in a rigged model's AMAT
/// and ask which vertex shaders declare the engine's bone palette `grbones`.
/// It is **always variant 1, and only variant 1** -- verified across fileIds
/// 1634661 (302-bone boss), 562804 and 904350, every material, with
/// `gw2bgfx_probe --skinned`.
///
/// The engine's own condition agrees: bits `0x2` and `0x4` are `GR_FVF_WEIGHTS`
/// and `GR_FVF_GROUP`, i.e. blend weights and blend indices. Variant 1 is
/// selected exactly when a mesh carries BOTH -- which is precisely, and only,
/// when GPU skinning is possible. `surfaceFlags & 2` is the opt-out.
///
/// So asking for variant 2 on a skinned mesh binds a shader that never reads the
/// palette, and the model draws in bind pose no matter what is uploaded.
///
/// What variants 2/4 actually are is NOT settled here. `0x80` is
/// `GR_FVF_TANGENT_FRAME` if this word is the geoset's GrFvf, which would make
/// them the packed-tangent-frame feeds -- consistent, but not confirmed, so the
/// names below stay descriptive of the evidence rather than of the guess.
enum GrVsVariant : uint32_t {
    kVsPlain            = 0, ///< No weights+indices, no 0x80.
    kVsSkinned          = 1, ///< Weights + indices: the feed that reads `grbones`.
    kVsFlag80           = 2, ///< 0x80 set (tangent frame, unconfirmed), not instanced.
    kVsPlainInstanced   = 3,
    kVsFlag80Instanced  = 4,
};

/// @copydoc GrVsVariant
uint32_t vsVariantFromMeshFlags(uint32_t meshFlags, uint32_t surfaceFlags, bool instanced);

/// @brief The outcome of selecting an effect for one draw.
struct AmatSelection {
    bool ok = false;
    const AmatEffect* effect = nullptr;
    uint32_t vertexShaderIndex = 0;
    uint32_t pixelShaderIndex = 0;
    uint32_t variant = 0;          ///< The variant actually used (may differ from the request).
    /// @brief True when the effect was found by the material's own token.
    ///
    /// False means the remap chain or the default token 183330 answered
    /// instead. Worth surfacing: it is the difference between "the client would
    /// draw this" and "the client would draw its fallback".
    bool matchedMaterialToken = false;
};

/// @brief Effect-lookup fallback chain, hardcoded in the client.
///
/// `BgfxShader_SelectEffect` tries the material's own token, then -- for three
/// specific tokens only -- one remapped token each, then the default.
struct AmatTokenChain {
    static constexpr uint64_t kRemapFrom0 = 0x1481311EC;
    static constexpr uint64_t kRemapTo0   = 0x51C59061370654;
    static constexpr uint64_t kRemapFrom1 = 0x1779028991EC;
    static constexpr uint64_t kRemapTo1   = 0x5DE40A268A40A4;
    static constexpr uint64_t kRemapFrom2 = 0x2C26C0B64F711EC;
    static constexpr uint64_t kRemapTo2   = 0x6584D816C9EE;
    /// @brief The token every material falls back to. Decimal in the binary.
    static constexpr uint64_t kDefault    = 183330;
};

/// @brief Select the effect for one draw -- the port of `BgfxShader_SelectEffect`.
///
/// @param pkg            The parsed AMAT.
/// @param techniqueIndex From ::amatSelectTechnique.
/// @param passIndex      Frame pass index. Pass 0 is the depth/prepass slot and
///                       is the only pass allowed to fall back to the default
///                       token when the material's token is absent.
/// @param materialToken  The **render-mode** token64 the calling pass wants --
///                       NOT the MODL material's `token` field. The client reads
///                       it from the runtime McMaterial, and one AMAT offers the
///                       same fixed menu of these regardless of material.
///                       `0x914C6A8A883B1EE` is opaque, `0x51C59061370654` is
///                       faded (the paper-doll writes exactly that pair). Passing
///                       the MODL token instead falls through to the default on
///                       every material and silently picks a different shader.
/// @param variant        From ::vsVariantFromMeshFlags.
AmatSelection amatSelectEffect(const AmatPackage& pkg,
                               int techniqueIndex,
                               uint32_t passIndex,
                               uint64_t materialToken,
                               uint32_t variant);

} // namespace gw2bgfx
