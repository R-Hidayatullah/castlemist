/// @file
/// @brief Extraction of the game's own bgfx (DXBC) material shaders.

#include "internal.h"

#include <cstring>
#include <map>
#include <set>
#include <span>

#include "castlemist/native/cmp_decompress_method0.hpp"

#include "castlemist/core/packfile.h"

namespace castlemist::extract {

// ---- game shaders (real bgfx DXBC) --------------------------------------

// Decompress one MFT entry addressed by physical index (== baseId). Mirrors the
// `decomp` helper in gw2gsviewer.cpp; used to pull the AMAT package that sits at
// (texture baseId - 1) for a material.
std::vector<uint8_t> decompress_by_index(Gw2Dat& dat, uint32_t idx) {
    if (idx >= dat.mft_data_list.size()) return {};
    const MftData& e = dat.mft_data_list[idx];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0) return s;
    if (s.size() < 8) return {};
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
    return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
}

// Parse a bgfx shader blob (v11) -> raw DXBC + uniform table. Byte-for-byte the
// same layout gw2gsviewer.cpp::parse_bgfx reads: 'V'/'F'/'C' + ver, hashes,
// uniform count, per-uniform {nameLen,name,type,_,reg,regCnt,(texInfo)(texFmt)},
// codeSize, DXBC, attrCount, attrs, constBufSize.
void parse_bgfx(const std::vector<uint8_t>& d, std::vector<uint8_t>& dxbc,
                std::vector<GameShaderUniform>& uniforms, uint32_t& constBuf) {
    dxbc.clear(); uniforms.clear(); constBuf = 0;
    if (d.size() < 8 || (d[0] != 'V' && d[0] != 'F' && d[0] != 'C')) return;
    size_t p = 3; int ver = d[p++]; p += 4; if (ver >= 6) p += 4;
    if (p + 2 > d.size()) return;
    uint16_t cnt = d[p] | (d[p + 1] << 8); p += 2;
    for (int i = 0; i < cnt; i++) {
        if (p >= d.size()) return;
        int nl = d[p++];
        if (p + nl > d.size()) return;
        std::string nm((const char*)&d[p], nl); p += nl;
        if (p + 6 > d.size()) return;
        int type = d[p++]; p++;
        int reg = d[p] | (d[p + 1] << 8); p += 2;
        int rc = d[p] | (d[p + 1] << 8); p += 2;
        if (ver >= 8) p += 2; if (ver >= 10) p += 2;
        int stage = (type & 0x20) ? 2 : (type & 0x10) ? 1 : 0;
        uniforms.push_back({nm, type & 0x0F, stage, reg, rc});
    }
    if (p + 4 > d.size()) return;
    uint32_t cs = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24); p += 4;
    if (p + cs > d.size()) return;
    dxbc.assign(d.begin() + p, d.begin() + p + cs); p += cs;
    if (p >= d.size()) return;
    int ac = d[p++]; p += ac * 2;
    if (p + 2 <= d.size()) constBuf = d[p] | (d[p + 1] << 8);
}

// GW2's fixed engine-global env-cubemap sampler slot (reflection/irradiance);
// bgfx strips the DXBC RDEF chunk so we can't detect it via reflection.
constexpr int kEnvCubeSlot = 13;
// AMAT sampler `textureIndex` values >= this are engine-GLOBAL texture ROLES
// (not indices into the material's own texture list): observed 33=env cube (slot
// 13), 34=light buffer (slot 14), 35=slot 12, 37=shadow map (slot 15). Below this
// the value is a direct index into the material's textures.
constexpr uint32_t kGlobalRoleBase = 33;

// Traces a material's AMAT shader package (at baseId-1 of its filename texture),
// picks a matched VS+PS via extractAmat, parses both bgfx blobs and resolves the
// PS sampler bindings to either a decoded material texture (via `get_tex`, which
// dedups into ModelPreview::textures) or a global stand-in. All CPU-side; the
// renderer creates the GPU objects. Returns a GameMaterial (ok=false on failure).
// Engine-global shader uniforms: filled by the engine/renderer (camera, lighting,
// fog, transforms), NOT per-material MatConstants. Everything else in a shader's
// uniform table is a material constant that pairs with the AMAT constants[] tokens.
static const std::set<std::string> kEngineGlobalUniforms = {
    "CameraPosition", "FogColorNearMinusFar", "FogColorFar", "FogColorHeight", "FogDepthCue", "FogParam0",
    "ViewProjection", "World", "WorldView", "WorldViewProjection", "WorldToShadowA", "WorldToShadowB",
    "WorldToShadowC", "TexTransform0A", "TexTransform0B", "TexTransform1A", "TexTransform1B", "TexTransform2A",
    "TexTransform2B", "TexTransform3A", "TexTransform3B", "LightBuffer", "StencilId", "ScreenDims", "fxclr",
    "shSunColor", "shSunData", "shSun", "shRed", "shGreen", "shBlue", "TexelOffset", "StippleDensity", "Time",
    "SunColor", "SunDirection", "AmbientColor", "AlphaRef", "fadedif",
    // Read by the real forward-lit material shaders that the AMAT picker now
    // selects (see gw2model.hpp::classifyShader). They are engine-filled, so they
    // must be listed here or a MODL MatConstant whose token happens to decode to
    // one of these names would be written over them: WorldToShadowD scales the
    // shadow fade, and LightCount/LightPointAndSpotData drive the per-object local
    // point/spot light loop. Deliberately left at zero -- LightCount 0 means "no
    // local lights", so a model is lit purely by the global environment rig (the SH
    // ambient + sun), which is what the map's env chunk supplies.
    "WorldToShadowD", "LightCount", "LightPointAndSpotData", "View", "DepthPeelPush",
    "SubmodelId", "BacklightColor", "BacklightDirection", "BacklightShadowInfluence",
    "WorldToBacklightShadowA", "WorldToBacklightShadowB"};

// GW2 "Token" decode (Arena/Services/Token/Token.cpp; client sub_140E3B360):
// a base-23 packed lowercase string. Verified against the client and the dat --
// a MODL ModelConstantData.name (token32) decodes straight to the shader's bgfx
// uniform name (0xBEF8FD7F -> "gloover", 0x304F2815 -> "blint", 0x54731015 ->
// "glofade", ...). Rule: v = (token - 0x30000000) mod 2^32; peel base-23 digits.
static std::string decode_token(uint32_t token) {
    static const char* kAlpha = "abcdefghiklmnopvrstuwxy"; // 23 chars, no j/q/z
    uint32_t v = token - 0x30000000u;                      // unsigned wrap, matches engine
    std::string out;
    while (v) { out.push_back(kAlpha[v % 23]); v /= 23; }
    return out;
}

GameMaterial extract_game_material(Gw2Dat& dat, const nlohmann::json& tpl, const castlemist::model::Material& m,
                                   const std::function<int(uint32_t)>& get_tex, bool effectHint) {
    GameMaterial out;
    out.index = m.index;
    uint32_t fnBase = get_by_base_id(dat, m.materialFile);
    if (!fnBase) return out;
    std::vector<uint8_t> amat;
    try { amat = decompress_by_index(dat, fnBase - 1); } catch (const std::exception&) { return out; }
    if (amat.empty()) return out;
    castlemist::model::AmatSet set;
    try { set = castlemist::model::Extractor(amat, tpl).extractAmat(); } catch (const std::exception&) { return out; }

    // Prefer the AMAT's OPAQUE colour effect; the blended one is used only when the
    // AMAT defines no opaque colour effect at all (extractAmat's fallback at the end
    // of the tier selection). That mirrors the engine closely enough: it resolves the
    // effect by the surface's 64-bit token with a default of token 183330 = the pass's
    // FIRST effect (Gw2-64 sub_140BFAD20), and in every AMAT measured the first effect
    // is the plain opaque one -- while a material like AMAT 57224 (glass), whose every
    // colour effect carries blend bits, has no opaque variant to fall back to and is
    // therefore correctly drawn blended.
    //
    // What this must NOT do is consult the MODL: `sortLayer` does not exist on
    // ModelMaterialDataV65 (the field is `sortOrder`), so the old
    // `set.hasTrans && m.sortLayer > 0` gate was always false and stripped the blend
    // bits off EVERY material. And the engine never looks at it either -- the draw
    // path ORs `effectData->renderState` in unconditionally
    // (sub_140BFAD20: `out+464 |= *(_QWORD *)(effect + 8)`), sortOrder only orders
    // the draws.
    int vsIdx = set.vsIndex;
    int psIdx = set.psIndex;
    if (std::getenv("GW2_MATDBG")) {
        std::fprintf(stderr,
                     "[mat %u] matId=%u flags=0x%08x sortOrder=%u ps=%d rs=0x%llx passFlags=0x%x "
                     "transPs=%d transRs=0x%llx nConst=%zu\n",
                     m.index, m.materialId, m.materialFlags, m.sortOrder, set.psIndex,
                     (unsigned long long)set.renderState, set.passFlags, set.transPsIndex,
                     (unsigned long long)set.transRenderState, m.constants.size());
        // Decode each MatConstant token32 via the engine's base-23 Token::Decode.
        for (const auto& c : m.constants) {
            std::string nm = decode_token(c.name);
            std::fprintf(stderr, "    const token=0x%08x name='%s' val=[%.3f %.3f %.3f %.3f]\n",
                         c.name, nm.c_str(), c.value[0], c.value[1], c.value[2], c.value[3]);
        }
    }
    if (vsIdx < 0 || psIdx < 0 || vsIdx >= (int)set.shaders.size() || psIdx >= (int)set.shaders.size()) return out;

    parse_bgfx(set.shaders[vsIdx].dxbc, out.vsDXBC, out.vsUniforms, out.vsConstBuf);
    parse_bgfx(set.shaders[psIdx].dxbc, out.psDXBC, out.psUniforms, out.psConstBuf);
    if (out.vsDXBC.empty() || out.psDXBC.empty()) return out;

    // Bind MODL MatConstants -> shader cbuffer offsets BY NAME. Each MatConstant's
    // name (token32) is a base-23 GW2 Token that decodes to the bgfx uniform name;
    // match it to the shader's live uniform of that name and write the float4 at
    // that uniform's byte offset. This is exact -- no positional pairing and no
    // dependency on the AMAT constants[] tokens -- and it survives the DXBC
    // compiler stripping unused uniforms (a MatConstant with no live uniform is
    // simply skipped). Verified against the client Token::Decode (sub_140E3B360)
    // and the dat: MODL 143917 gloover/gloptrb/glofade/envcp/blint (and the sci-fi
    // material's slscale/holoclr/opacity/...) all resolve to real shader uniforms.
    auto lc = [](std::string s) { for (char& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
    auto bind_consts = [&](const std::vector<GameShaderUniform>& unis,
                           std::vector<GameConstOverride>& outc) {
        for (const auto& c : m.constants) {
            std::string nm = decode_token(c.name);
            if (nm.empty()) continue;
            for (const auto& u : unis) {
                if (u.type == 0 || kEngineGlobalUniforms.count(u.name)) continue; // engine-filled
                if (lc(u.name) != nm) continue;
                GameConstOverride ov; ov.byteOff = u.byteOff;
                for (int k = 0; k < 4; ++k) ov.value[k] = c.value[k];
                outc.push_back(ov);
                if (std::getenv("GW2_CONSTDBG"))
                    std::fprintf(stderr, "[mat %u] const '%s'@%d = [%.3f %.3f %.3f %.3f]\n",
                                 m.index, u.name.c_str(), u.byteOff, c.value[0], c.value[1], c.value[2], c.value[3]);
                break;
            }
        }
    };
    bind_consts(out.vsUniforms, out.vsConsts);
    bind_consts(out.psUniforms, out.psConsts);

    for (const auto& b : set.shaders[psIdx].samplers) {
        if (b.textureSlot >= 16) continue;
        GameSamplerCPU s;
        s.slot = static_cast<int>(b.textureSlot);
        if (b.textureIndex >= kGlobalRoleBase) {
            // Engine-global role we don't have offline. Match the known roles so
            // each gets a sane stand-in (env cube grey / shadow map WHITE = fully
            // lit / other globals mid-grey) instead of a flat-white blob.
            if (b.textureIndex == 33 || b.textureSlot == kEnvCubeSlot) s.global = 2;      // env cubemap
            else if (b.textureIndex == 37 || b.textureSlot == 15) s.global = 3;           // shadow map -> white
            else s.global = 1;                                                            // light buffer / other -> grey
        } else if (b.textureIndex < m.textures.size()) {
            uint32_t fid = m.textures[b.textureIndex].fileId;
            s.gameTex = fid ? get_tex(fid) : -1;
            if (s.gameTex < 0) s.global = 1; // texture failed to decode -> grey, not white
        } else {
            // A material-texture index that's out of range (material has fewer
            // textures than the shader samples): grey stand-in, never white.
            s.global = 1;
        }
        out.samplers.push_back(s);
    }
    // The effect's own bgfx word IS the blend state -- no second-guessing, no gate.
    //
    // This used to clear the blend field whenever the MODL didn't ask for the sorted
    // bucket, because blending an effect whose pixel shader ends with
    // `mov o0.w, cb0[..]` (a constant StencilId, not opacity) makes solid geometry go
    // see-through. The reasoning was right, the mechanism was wrong: the engine keeps
    // those two apart with the WRITE MASK, not by dropping the blend bits.
    // sub_140AAFDB0 builds the mask from shaderPassFlags and never touches
    // `effectData->renderState`, which sub_140BFAD20 ORs into the draw state
    // unconditionally.
    //
    // Concretely, on model 771205: material 1's AMAT (57224) has renderState
    // 0x4242000 = ONE/INV_SRC_COLOR on *every* colour effect, passFlags 0x4041, and a
    // pixel shader that outputs premultiplied alpha
    // (`o0.w = fresnel*albedo.a*mask*diffade`, `o0.rgb = o0.w * albedo.rgb`). Its
    // albedo has 90% of its alpha below 0.25, so drawing it opaque paints a
    // premultiplied-by-nearly-zero, i.e. BLACK, patch. Restoring the blend bits is
    // what makes it glass again.
    out.renderState = set.renderState;
    out.shaderPassFlags = set.passFlags;
    out.prepassCutout = set.prepassCutout;
    out.ok = true;
    return out;
}

// Extracts geometry + materials (via gw2model, template-driven) and decodes
// every referenced texture into a self-contained, ready-to-upload ModelPreview.
// Returns null if the packfile isn't a renderable model.
//
// Takes the archive as an already-parsed Gw2Dat& (header + MFT tables only --
// the file handle inside it is already closed by load_dat_file, so this is
// pure read-only lookups: safe to call concurrently from many worker threads
// against the *same* Gw2Dat, same as read_entry_bytes/decode_texture_by_fileid
// already assume). Callers that loop over many unique models (map props, zone
// models) MUST load the Gw2Dat once and pass it in here -- re-parsing the
// whole MFT table (header + up to ~1M entries, each sorted twice) per model
// was previously the actual cost of "loading peta 3d lambat": for a map with
// 600 unique prop models, that meant re-doing a multi-hundred-thousand-entry
// parse + two full sorts 600 times over, on top of running that redundant
// work across several threads at once (see the string-path overload below,
// kept only for the single-model preview path which has no shared Gw2Dat).

} // namespace castlemist::extract
