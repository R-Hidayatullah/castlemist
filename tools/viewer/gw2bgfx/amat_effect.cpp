/// @file
/// @brief Implementation of the AMAT selection ports. See amat_effect.h for
///        the source addresses each function was transcribed from.

#include "amat_effect.h"

#include "gr_token.h"

namespace gw2bgfx {

int amatQualityOf(uint32_t qualityToken) {
    // BgfxShader_BuildPasses decodes the token and switches on the FIRST
    // CHARACTER of the decoded name -- not on the whole string. `ultra`,
    // `high`, `medium`, `low`.
    std::string name = tokenDecode32(qualityToken);
    if (name.empty()) return kQualityInvalid;
    switch (name[0]) {
        case 'u': return kQualityUltra;
        case 'h': return kQualityHigh;
        case 'm': return kQualityMedium;
        case 'l': return kQualityLow;
        default:  return kQualityInvalid;   // client logs "Invalid shader technique name"
    }
}

int amatSelectTechnique(const AmatPackage& pkg, int maxQuality, int* outQuality) {
    if (outQuality) *outQuality = kQualityInvalid;
    if (pkg.techniques.empty()) return -1;

    // Transcribed from BgfxShader_BuildPasses (0x140BFFB20). The control flow
    // matters more than it looks:
    //
    //   - the loop breaks the moment m_techniqueIndex stops being 0xFF, i.e. on
    //     the FIRST technique whose quality is affordable, in file order;
    //   - an unrecognised name sets the index unconditionally (quality 0), so
    //     it always terminates the walk;
    //   - if the walk ends without a hit, the LAST technique examined is used
    //     with its own quality, rather than the best affordable one.
    //
    // Scanning for "highest quality <= cap" instead would agree with this on
    // most files and disagree on any file that orders its techniques
    // low-to-high.
    int chosen = -1;
    int chosenQuality = kQualityInvalid;
    int lastIndex = -1;
    int lastQuality = kQualityInvalid;

    for (size_t i = 0; i < pkg.techniques.size(); ++i) {
        int q = amatQualityOf(pkg.techniques[i].quality);
        lastIndex = (int)i;
        lastQuality = q;
        if (q == kQualityInvalid || maxQuality >= q) {
            chosen = (int)i;
            chosenQuality = q;
            break;
        }
    }

    if (chosen < 0) {
        chosen = lastIndex;
        chosenQuality = lastQuality;
    }
    if (outQuality) *outQuality = chosenQuality;
    return chosen;
}

uint32_t vsVariantFromMeshFlags(uint32_t meshFlags, uint32_t surfaceFlags, bool instanced) {
    // BgfxDraw_MeshDrawLoop (0x140AB2CA0), transcribed. Bits 0x2|0x4 are blend
    // weights + blend indices, so this first branch is the GPU-skinned feed --
    // see the note on GrVsVariant before changing it.
    if ((meshFlags & 4) && (meshFlags & 2) && !(surfaceFlags & 2))
        return kVsSkinned;
    if (!(meshFlags & 0x80))
        return instanced ? kVsPlainInstanced : kVsPlain;
    return instanced ? kVsFlag80Instanced : kVsFlag80;
}

namespace {

/// @brief `sub_140BF7550` -- linear search of a pass's effects by token64.
const AmatEffect* findEffectByToken(const AmatPass& pass, uint64_t token) {
    for (const auto& e : pass.effects)
        if (e.token == token) return &e;
    return nullptr;
}

/// @brief `BgfxShader_FindVsVariant` (0x140BFE990).
///
/// Searches for the requested variant id; failing that, retries once.
///
/// `BgfxShader_FindVsVariant` (0x140BFE990) does this on the raw ids:
/// `v7 = 2; if (a1 != 4) v7 = 0;` -- so a request for 4 retries 2, and
/// EVERYTHING else, the skinned feed included, retries 0. The client asserts
/// ("vertexShader", BgfxShader.cpp:1037) if even that is absent.
///
/// Note what this means for skinning: an effect that offers no variant 1 falls
/// back to the plain feed and simply is not skinned. That is the client's own
/// behaviour, so a mesh drawing in bind pose here can be correct rather than a
/// bug -- check the effect's variant list before assuming otherwise.
const AmatVertexShaderVariant* findVsVariant(const AmatEffect& effect, uint32_t variant) {
    for (const auto& v : effect.vertexShaderVariants)
        if (v.variant == variant) return &v;

    uint32_t fallback = (variant == kVsFlag80Instanced) ? kVsFlag80 : kVsPlain;
    if (fallback != variant) {
        for (const auto& v : effect.vertexShaderVariants)
            if (v.variant == fallback) return &v;
    }
    return nullptr;
}

} // namespace

AmatSelection amatSelectEffect(const AmatPackage& pkg,
                               int techniqueIndex,
                               uint32_t passIndex,
                               uint64_t materialToken,
                               uint32_t variant) {
    AmatSelection sel;
    if (techniqueIndex < 0 || techniqueIndex >= (int)pkg.techniques.size()) return sel;
    const AmatTechnique& tech = pkg.techniques[techniqueIndex];
    if (passIndex >= tech.passes.size()) return sel;   // assert "pass < techniqueData.passCount"
    const AmatPass& pass = tech.passes[passIndex];

    // --- The lookup chain, verbatim from BgfxShader_SelectEffect ------------
    const AmatEffect* effect = findEffectByToken(pass, materialToken);
    bool byMaterialToken = effect != nullptr;

    if (!effect) {
        // Exactly three tokens get a remap, and each gets exactly one attempt.
        uint64_t remap = 0;
        if      (materialToken == AmatTokenChain::kRemapFrom0) remap = AmatTokenChain::kRemapTo0;
        else if (materialToken == AmatTokenChain::kRemapFrom1) remap = AmatTokenChain::kRemapTo1;
        else if (materialToken == AmatTokenChain::kRemapFrom2) remap = AmatTokenChain::kRemapTo2;
        if (remap) effect = findEffectByToken(pass, remap);
    }

    if (!effect) {
        effect = findEffectByToken(pass, AmatTokenChain::kDefault);
        // The default is accepted on pass 0 only. On any later pass the client
        // bails out and draws nothing rather than substituting -- which is how
        // a material legitimately opts out of a frame pass. Silently drawing
        // the default here would add geometry the game never renders.
        if (effect && passIndex != 0) return sel;
    }

    if (!effect) return sel;
    if (effect->vertexShaderVariants.empty()) return sel;  // asserted at :1138

    const AmatVertexShaderVariant* vs = findVsVariant(*effect, variant);
    if (!vs) return sel;                                   // asserted at :1037

    if (vs->vertexShaderIndex >= pkg.shaders.size()) return sel;   // asserted at :1172
    if (effect->pixelShaderIndex >= pkg.shaders.size()) return sel; // asserted at :1179

    sel.ok = true;
    sel.effect = effect;
    sel.vertexShaderIndex = vs->vertexShaderIndex;
    sel.pixelShaderIndex = effect->pixelShaderIndex;
    sel.variant = vs->variant;
    sel.matchedMaterialToken = byMaterialToken;
    return sel;
}

} // namespace gw2bgfx
