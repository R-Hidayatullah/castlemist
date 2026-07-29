/// @file
/// @brief Building D3D pipeline objects out of the game's own bgfx (DXBC) material shaders.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <map>
#include <string>

namespace castlemist::render {

static const D3D11_BLEND kBgfxBlendFactor[14][2] = {
    { D3D11_BLEND(0),               D3D11_BLEND(0)               }, // 0 ignored
    { D3D11_BLEND_ZERO,             D3D11_BLEND_ZERO             }, // 1 ZERO
    { D3D11_BLEND_ONE,              D3D11_BLEND_ONE              }, // 2 ONE
    { D3D11_BLEND_SRC_COLOR,        D3D11_BLEND_SRC_ALPHA        }, // 3 SRC_COLOR
    { D3D11_BLEND_INV_SRC_COLOR,    D3D11_BLEND_INV_SRC_ALPHA    }, // 4 INV_SRC_COLOR
    { D3D11_BLEND_SRC_ALPHA,        D3D11_BLEND_SRC_ALPHA        }, // 5 SRC_ALPHA
    { D3D11_BLEND_INV_SRC_ALPHA,    D3D11_BLEND_INV_SRC_ALPHA    }, // 6 INV_SRC_ALPHA
    { D3D11_BLEND_DEST_ALPHA,       D3D11_BLEND_DEST_ALPHA       }, // 7 DST_ALPHA
    { D3D11_BLEND_INV_DEST_ALPHA,   D3D11_BLEND_INV_DEST_ALPHA   }, // 8 INV_DST_ALPHA
    { D3D11_BLEND_DEST_COLOR,       D3D11_BLEND_DEST_ALPHA       }, // 9 DST_COLOR
    { D3D11_BLEND_INV_DEST_COLOR,   D3D11_BLEND_INV_DEST_ALPHA   }, // a INV_DST_COLOR
    { D3D11_BLEND_SRC_ALPHA_SAT,    D3D11_BLEND_ONE              }, // b SRC_ALPHA_SAT
    { D3D11_BLEND_BLEND_FACTOR,     D3D11_BLEND_BLEND_FACTOR     }, // c FACTOR
    { D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR }, // d INV_FACTOR
};
// bgfx blend equation table (s_blendEquation).
static const D3D11_BLEND_OP kBgfxBlendEquation[5] = {
    D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT,
    D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX,
};
D3D11_BLEND bgfx_blend(uint32_t v, int col = 0) {
    return (v < 14) ? kBgfxBlendFactor[v][col & 1] : D3D11_BLEND_ONE;
}

// Decodes a bgfx 64-bit render-state word's blend fields into a real D3D11
// blend state (BGFX_STATE_BLEND_SHIFT=12, EQUATION_SHIFT=28,
// ALPHA_TO_COVERAGE=bit35 -- same layout bgfx's D3D11 renderer uses). Returns
// null when the word has no blend bits set (opaque / unknown). Shared by the
// game-shader material path (build_game_materials) and the reconstruction
// material path (build below) so BOTH draw transparent materials with the
// material's own real blend function instead of a single hardcoded
// SrcAlpha/InvSrcAlpha guess.
// Render-target write mask, built exactly as the engine builds it from an effect's
// shaderPassFlags (Gw2-64 sub_140AAFDB0, BgfxDraw.cpp):
//
//     if (flags & 0x0004)  mask = 0                 // depth-only pass
//     else                 mask = (flags & 0x0008 ? 0 : RGB)
//                               | (flags & 0x4000 ? 0 : A)
//
// bgfx then hands `state & 0xF` straight to D3D11 as RenderTargetWriteMask
// (sub_140B744E0 masks the state with 0xFFFFFF00F and does
// `LOBYTE(desc.RenderTargetWriteMask) = state & 0xF`), and the bit order matches
// D3D11_COLOR_WRITE_ENABLE_* one-for-one.
//
// This replaces the old hardcoded "blended draws get mask 7, everything else 15".
// The observation behind that was right -- in the capture every blended draw does
// use mask 7, because the target's alpha is a submodel/stencil id the opaque pass
// owns -- but the cause is the 0x4000 flag on those effects, not the fact that
// they blend. Reading it off the flag also covers the opaque effects that mask
// alpha and the (rare) blended ones that do not.
UINT8 game_write_mask(uint32_t passFlags) {
    if (passFlags & 0x0004u) return 0;
    UINT8 m = 0;
    if (!(passFlags & 0x0008u))
        m |= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (!(passFlags & 0x4000u)) m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return m;
}

// Blending DISABLED, write mask from the effect's own shaderPassFlags. This is the
// opaque counterpart of make_blend_state_from_bgfx: the game's material pass does
// not use one shared all-channel opaque state, it masks per effect (a real frame
// shows One/Zero at mask 15, 7 and 8 side by side). Binding mask 15 for everything
// let a shader that writes no meaningful alpha -- most of them write a StencilId
// there -- stamp that value over the target's alpha channel.
ComPtr<ID3D11BlendState> make_opaque_blend_state(uint32_t passFlags) {
    ComPtr<ID3D11BlendState> out;
    if (!g_dev) return out;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = game_write_mask(passFlags);
    g_dev->CreateBlendState(&bd, &out);
    return out;
}

ComPtr<ID3D11BlendState> make_blend_state_from_bgfx(uint64_t st, uint32_t passFlags) {
    ComPtr<ID3D11BlendState> out;
    if (!g_dev) return out;
    uint32_t blend = (uint32_t)((st >> 12) & 0xffff);
    if (blend == 0) return out;
    uint32_t equ = (uint32_t)((st >> 28) & 0x3f);
    uint32_t srcRGB = blend & 0xf, dstRGB = (blend >> 4) & 0xf, srcA = (blend >> 8) & 0xf, dstA = (blend >> 12) & 0xf;
    uint32_t equRGB = equ & 0x7, equA = (equ >> 3) & 0x7;
    D3D11_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable = (st & 0x0000000800000000ull) != 0; // BGFX_STATE_BLEND_ALPHA_TO_COVERAGE
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = bgfx_blend(srcRGB, 0);
    bd.RenderTarget[0].DestBlend = bgfx_blend(dstRGB, 0);
    bd.RenderTarget[0].BlendOp = kBgfxBlendEquation[equRGB < 5 ? equRGB : 0];
    bd.RenderTarget[0].SrcBlendAlpha = bgfx_blend(srcA ? srcA : 2, 1);
    bd.RenderTarget[0].DestBlendAlpha = bgfx_blend(dstA ? dstA : 1, 1);
    bd.RenderTarget[0].BlendOpAlpha = kBgfxBlendEquation[equA < 5 ? equA : 0];
    bd.RenderTarget[0].RenderTargetWriteMask = game_write_mask(passFlags);
    g_dev->CreateBlendState(&bd, &out);
    return out;
}

// Size a cbuffer to the largest register the shader actually reads (bgfx's
// constBufSize can be smaller than the DXBC's CB0 declaration).
UINT game_cb_size(const std::vector<GameShaderUniform>& u, uint32_t constBuf) {
    UINT need = constBuf;
    for (const auto& x : u) {
        if (x.type == 0) continue;
        UINT end = (UINT)x.byteOff + (UINT)x.vec4s * 16;
        if (end > need) need = end;
    }
    UINT sz = (need + 15) & ~15u;
    return sz < 16 ? 16 : sz;
}
void game_fill_cb(std::vector<uint8_t>& buf, const std::vector<GameShaderUniform>& u,
                  const std::map<std::string, std::vector<float>>& vals,
                  const std::vector<GameConstOverride>& consts) {
    std::fill(buf.begin(), buf.end(), (uint8_t)0);
    for (const auto& x : u) {
        if (x.type == 0) continue;
        auto it = vals.find(x.name);
        if (it == vals.end()) continue;
        size_t n = std::min((size_t)x.vec4s * 16, it->second.size() * 4);
        if ((size_t)x.byteOff + n <= buf.size()) std::memcpy(buf.data() + x.byteOff, it->second.data(), n);
    }
    // Per-material MatConstants overwrite the shared fill at their exact offsets
    // (glow/spec/scroll/tint/etc.) -- these are what make effect materials look right.
    for (const auto& c : consts) {
        if ((size_t)c.byteOff + sizeof(c.value) <= buf.size())
            std::memcpy(buf.data() + c.byteOff, c.value, sizeof(c.value));
    }
}
ComPtr<ID3D11Buffer> game_build_cb(const std::vector<GameShaderUniform>& u, uint32_t constBuf,
                                   const std::map<std::string, std::vector<float>>& vals,
                                   const std::vector<GameConstOverride>& consts) {
    UINT sz = game_cb_size(u, constBuf);
    std::vector<uint8_t> buf(sz, 0);
    game_fill_cb(buf, u, vals, consts);
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; bd.ByteWidth = sz;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = buf.data();
    ComPtr<ID3D11Buffer> b;
    g_dev->CreateBuffer(&bd, &sd, &b);
    return b;
}
void game_update_cb(ID3D11Buffer* b, const std::vector<GameShaderUniform>& u, uint32_t constBuf,
                    const std::map<std::string, std::vector<float>>& vals,
                    const std::vector<GameConstOverride>& consts) {
    if (!b) return;
    UINT sz = game_cb_size(u, constBuf);
    std::vector<uint8_t> buf(sz, 0);
    game_fill_cb(buf, u, vals, consts);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, buf.data(), sz);
        g_ctx->Unmap(b, 0);
    }
}
// GVertex byte offset for a VS input semantic. GVertex now carries all 8 UV sets
// (UV0 at 48, UV1..7 at 56 in 8-byte steps) so multi-UV game materials sample the
// correct detail/decal channels (matches gw2gsviewer's castlemist::model::Vertex layout).
UINT game_sem_off(const char* s, UINT idx) {
    if (!std::strcmp(s, "POSITION")) return 0;
    if (!std::strcmp(s, "NORMAL")) return 12;
    if (!std::strcmp(s, "TANGENT")) return 24;
    if (!std::strcmp(s, "BINORMAL") || !std::strcmp(s, "BITANGENT")) return 36;
    if (!std::strcmp(s, "TEXCOORD")) return 48 + 8 * (idx > 7 ? 7 : idx);
    if (!std::strcmp(s, "BLENDINDICES")) return 112;
    if (!std::strcmp(s, "BLENDWEIGHT")) return 128;
    return 0;
}


void apply_env_uniforms() {
    auto put = [&](const char* n, std::initializer_list<float> f) { g_game_vals[n] = std::vector<float>(f); };
    float lit = 1.0f;
    if (const char* e = std::getenv("GW2_LIGHT")) { double v = std::atof(e); if (v > 0.01) lit = static_cast<float>(v); }
    const MapEnvRig& r = g_env_rig;

    float sd[3] = {r.sunDir[0], r.sunDir[1], r.sunDir[2]};
    // Single-model view with the headlight on: the game SH sun follows the camera
    // too (angle from the "light angle" slider), so a skin in Shader mode stays
    // lit from the front. Map scenes keep their real env sun.
    if (g_light_follow && !g_scene_mode) { Vec3 t = headlight_toward(); sd[0] = t.x; sd[1] = t.y; sd[2] = t.z; }
    { float l = std::sqrt(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]); if (l > 1e-6f) { sd[0]/=l; sd[1]/=l; sd[2]/=l; } }
    put("shSun", {sd[0], sd[1], sd[2], 0});
    put("shSunData", {sd[0], sd[1], sd[2], 0});
    // Calibrated against the REAL light buffer, not by eye. Decoding the game's own
    // buffer (sample1 RT 6826, x4) gives this distribution over the frame:
    //     p1 0.32   p10 0.49   p25 0.78   p50 0.98   p90 1.06   max 1.28
    // Two things to match: a fully lit surface sits near 1.0-1.28, and -- easy to
    // miss -- the buffer NEVER drops below ~0.32, so shadowed sides stay well off
    // black.
    //
    // These are the same numbers the forward materials want. GW2's deferred light
    // pass and its forward material shaders run the SAME model off the SAME rig:
    //     colour = dot(float4(N,1), sh{Red,Green,Blue})            // SH ambient
    //            + saturate(dot(float4(N,1), shSun)) * shSunColor  // sun
    // (deferred: sample1 ps 8641 cb0[50..54]; forward: AMAT 135429 ps 4 cb0[1..6]),
    // so one calibration serves both.
    //
    // Solved for the measured targets rather than dialled in: with
    // luminance(sunColor) = 0.92 and luminance(fillColor) = 0.66,
    //     ambient DC  0.32 / 0.66 = 0.49  -> shadow side lands on the measured p1
    //     sun         0.75 / 0.92 = 0.82  -> 0.32 + 0.75 = 1.07, the measured p90
    // The previous 0.62 sun / 0.12 ambient gave ~0.68 lit and ~0.08 shadow: about
    // 4x too dark in shadow. That was survivable only because a 1.9x post exposure
    // and 1.18 gamma sat on top of it; both are gone now (they were compensating for
    // the unlit normal-prepass shader being drawn), so the rig has to be right here.
    const float kSunLum = 0.919f;   // luminance of the default warm sun colour
    const float kFillLum = 0.656f;  // luminance of the default cool fill colour
    float si = std::clamp(r.sunIntensity, 0.0f, 3.0f);
    float sMag = (0.75f / kSunLum) * si * lit;
    put("shSunColor", {r.sunColor[0]*sMag, r.sunColor[1]*sMag, r.sunColor[2]*sMag, 1});
    // SH-ambient DC from the intensity-weighted fill/sky colour, plus a modest
    // up-facing sky gradient (.y). The floor is the measured p1 (~0.32 luminance),
    // so shadow sides keep material detail exactly as the game's buffer does.
    float fi = std::clamp(r.fillIntensity * (0.32f / kFillLum) / 0.3f, 0.30f, 0.90f) * lit;
    float aR = r.fillColor[0]*fi, aG = r.fillColor[1]*fi, aB = r.fillColor[2]*fi;
    const float gy = 0.30f;                              // share of ambient added to the .y (sky) term
    put("shRed",   {0, aR*gy, 0, aR});
    put("shGreen", {0, aG*gy, 0, aG});
    put("shBlue",  {0, aB*gy, 0, aB});
    put("SunColor", {0, 0, 0, 0});
    put("SunDirection", {-sd[0], -sd[1], -sd[2], 0}); // travel direction = -toward-light
}

void setup_game_uniforms() {
    if (g_game_uniforms_ready) return;
    auto put = [&](const char* n, std::initializer_list<float> f) { g_game_vals[n] = std::vector<float>(f); };
    // Overall brightness knob (env GW2_LIGHT, default 1.0).
    float lit = 1.0f;
    if (const char* e = std::getenv("GW2_LIGHT")) { double v = std::atof(e); if (v > 0.01) lit = static_cast<float>(v); }
    put("TexTransform0A", {1, 0, 0, 0}); put("TexTransform0B", {0, 1, 0, 0});
    put("TexTransform1A", {1, 0, 0, 0}); put("TexTransform1B", {0, 1, 0, 0});
    // `LightBuffer` is NOT a scalar -- it is the engine's shared range-compression
    // constant VECTOR, and each lane means something different. Measured identical in
    // every material cbuffer of the capture (sample1 eid 1650 cb0[0], and the light
    // pass's own cb0[0] at eid 1599):
    //
    //     .x = 0.25   light-buffer ENCODE  (the light pass ends `mul o0, light, .x`)
    //     .y = 4.0    light-buffer DECODE  (materials do `light * .y`)
    //     .z = 1/128  reciprocal of .w
    //     .w = 128    SPECULAR POWER scale (`pow(NdotH, gloss * .w)`)
    //
    // This used to broadcast 0.8 into all four lanes, which silently wrecked the two
    // lanes that are not the decode: every shader computing specular got an exponent
    // of gloss*0.8 instead of gloss*128, turning a tight highlight into a wash across
    // the whole surface.
    //
    // Only the DECODE is ours to change: our light buffer is stored display-referred
    // rather than pre-divided by 4 (see kLightBufferDecode in state.h), so the
    // matching decode is 1.0 and `buffer * decode` lands on the same physical light
    // as the game's 0.245 * 4.0. The other three lanes are the game's own values.
    put("LightBuffer", {kLightBufferEncode * 0.25f, kLightBufferDecode * lit, 1.0f / 128.0f, 128.0f});
    put("fxclr", {1, 1, 1, 1});                        // stipple-fade visibility (0 = discards all)
    put("StippleDensity", {0, 0, 0, 0}); put("StencilId", {0, 0, 0, 0});
    // Paired with ScreenDims in the deferred light-buffer fetch:
    //     uv = (screenPos.xy + TexelOffset.x) * ScreenDims.xy
    // Zero is correct on D3D11, where pixel centres already sit at the half-texel;
    // this is the D3D9-era half-pixel fixup. Set explicitly so the value is a
    // decision rather than an unwritten cbuffer slot that happens to be zero.
    put("TexelOffset", {0, 0, 0, 0});
    put("AmbientColor", {0, 0, 0, 0}); put("fadedif", {1, 1, 1, 1});
    // Alpha-test reference. Measured from a real frame: the material cbuffer holds
    // 0.25 here, and every cutout shader discards where diffuse alpha < 0.25 (they
    // spell it `saturate(2a) < 0.5`). Leaving this at 0 disabled the cutout in the
    // game shaders that read AlphaRef instead of hardcoding the constant, which drew
    // masked geometry (foliage, grates, hair) as solid quads.
    put("AlphaRef", {0.25f, 0.25f, 0.25f, 0.25f});
    put("FogColorNearMinusFar", {0, 0, 0, 0}); put("FogColorFar", {0, 0, 0, 0}); put("FogColorHeight", {0, 0, 0, 0});
    put("FogDepthCue", {0, 0, 0, 0}); put("FogParam0", {0, 0, 0, 0});
    put("WorldToShadowA", {0, 0, 0, 0}); put("WorldToShadowB", {0, 0, 0, 0}); put("WorldToShadowC", {0, 0, 0, 0});
    // Shadowing, explicitly disabled rather than left to chance. The lit material
    // shaders sample the shadow map at gSs15 (bound to solid WHITE by
    // build_one_game_mat) and then fade the result with WorldToShadowD:
    //     shadow = saturate(v7.w * WorldToShadowD.x + WorldToShadowD.y)
    // blended toward the sampled value. With a white map and D = 0 the whole chain
    // collapses to shadow = 1.0 (fully lit) -- traced through the real shader,
    // AMAT 135429 ps 4 instructions 8..30. That is the right offline answer: we
    // have no shadow-caster render, so nothing should be in shadow.
    put("WorldToShadowD", {0, 0, 0, 0});
    // No local lights. GW2's forward materials loop over LightCount point/spot
    // lights supplied per object by the engine; offline there are none, so the model
    // is lit by the global environment rig alone (SH ambient + sun), which is what
    // "lit like the game" means for a map's daylight.
    put("LightCount", {0, 0, 0, 0});
    put("LightPointAndSpotData", {0, 0, 0, 0});
    apply_env_uniforms();   // sun + SH ambient from the active environment rig
    g_game_uniforms_ready = true;
}

void set_environment_rig(const MapEnvRig& rig) {
    if (rig.present) { g_env_rig = rig; g_env_rig_active = true; }
    else { g_env_rig = MapEnvRig{}; g_env_rig_active = false; }
    if (g_game_uniforms_ready) apply_env_uniforms();   // live update if already set up
}
void clear_environment_rig() {
    g_env_rig = MapEnvRig{}; g_env_rig_active = false;
    if (g_game_uniforms_ready) apply_env_uniforms();
}
bool environment_rig_active() { return g_env_rig_active; }

// Builds ONE GameMatGPU from a GameMaterial (game DXBC VS+PS, input layout from
// the VS reflection, DYNAMIC cbuffers, one SRV per PS sampler slot). Shared by the
// single-model path (build_game_materials) and the map-scene path
// (build_scene_game_materials) so both extract identically. `albLumSum/albWSum`,
// when non-null, accumulate diffuse-albedo luminance for the single-model auto-
// exposure (the scene path passes null -- it uses the shared exposure). Returns

bool build_one_game_mat(const GameMaterial& c, const ModelPreview& model, GameMatGPU& g,
                        double* albLumSum, double* albWSum) {
    if (!c.ok || c.vsDXBC.empty() || c.psDXBC.empty()) return false;
    if (FAILED(g_dev->CreateVertexShader(c.vsDXBC.data(), c.vsDXBC.size(), nullptr, &g.vs))) return false;
    if (FAILED(g_dev->CreatePixelShader(c.psDXBC.data(), c.psDXBC.size(), nullptr, &g.ps))) return false;

    // Input layout from the VS input signature (bgfx keeps the ISGN chunk).
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(c.vsDXBC.data(), c.vsDXBC.size(), IID_ID3D11ShaderReflection,
                          reinterpret_cast<void**>(refl.GetAddressOf()))))
        return false;
    D3D11_SHADER_DESC sdesc{};
    refl->GetDesc(&sdesc);
    std::vector<std::string> sem(sdesc.InputParameters);
    std::vector<D3D11_INPUT_ELEMENT_DESC> il;
    for (UINT i = 0; i < sdesc.InputParameters; i++) {
        D3D11_SIGNATURE_PARAMETER_DESC pd{};
        refl->GetInputParameterDesc(i, &pd);
        sem[i] = pd.SemanticName ? pd.SemanticName : "";
    }
    for (UINT i = 0; i < sdesc.InputParameters; i++) {
        D3D11_SIGNATURE_PARAMETER_DESC pd{};
        refl->GetInputParameterDesc(i, &pd);
        D3D11_INPUT_ELEMENT_DESC e{};
        e.SemanticName = sem[i].c_str();
        e.SemanticIndex = pd.SemanticIndex;
        e.Format = (pd.Mask <= 3) ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT;
        e.AlignedByteOffset = game_sem_off(sem[i].c_str(), pd.SemanticIndex);
        e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        il.push_back(e);
    }
    if (FAILED(g_dev->CreateInputLayout(il.data(), static_cast<UINT>(il.size()), c.vsDXBC.data(), c.vsDXBC.size(),
                                        &g.layout)))
        return false;

    g.vsU = c.vsUniforms; g.psU = c.psUniforms; g.vsCB = c.vsConstBuf; g.psCB = c.psConstBuf;
    g.vsConsts = c.vsConsts; g.psConsts = c.psConsts;
    g.vcb = game_build_cb(g.vsU, g.vsCB, g_game_vals, g.vsConsts);
    g.pcb = game_build_cb(g.psU, g.psCB, g_game_vals, g.psConsts);

    for (const auto& s : c.samplers) {
        if (s.slot < 0 || s.slot >= 16) continue;
        if (s.global == 2) g.srv[s.slot] = make_solid_cube(64);
        else if (s.global == 3) g.srv[s.slot] = make_solid(255, 255, 255, 255); // shadow map: unshadowed
        // Light-buffer / other global engine textures: a dark-grey stand-in
        // (not white) so deferred albedo*lightBuffer terms don't blow out to a
        // flat "lamp". Tunable via GW2_GLOBALLIT (0..255, default 110).
        else if (s.global == 1) {
            int lv = 110;
            if (const char* e = std::getenv("GW2_GLOBALLIT")) { int v = std::atoi(e); if (v >= 0 && v <= 255) lv = v; }
            g.srv[s.slot] = make_solid(static_cast<uint8_t>(lv), static_cast<uint8_t>(lv), static_cast<uint8_t>(lv), 255);
            g.lightSlots |= static_cast<uint16_t>(1u << s.slot); // deferred light buffer (gSs14)
        }
        else if (s.gameTex >= 0 && s.gameTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[s.gameTex];
            g.srv[s.slot] = make_srv(t.rgba, t.width, t.height);
            // Whether the prepass alpha-tests ss0 is decided by the GAME'S OWN prepass
            // shader (c.prepassCutout), not by looking at the texture. The alpha
            // histogram cannot tell a coverage mask from a gloss map -- both are
            // mostly-low and near-binary -- and treating a gloss map as coverage makes
            // the prepass discard most of the surface, so the light buffer ends up
            // full of holes and the material renders with torn, see-through patches
            // (fileId 3772446: 81% of its albedo alpha sits below the threshold and is
            // fed to `pow` as a specular exponent, not tested).
            // t.hasCutout still gates the RECONSTRUCTION shader, which has no game
            // shader to consult.
            if (s.slot == 0) g.cutout = c.prepassCutout;
            // Sample the diffuse (slot 0) albedo for auto-exposure (subsampled,
            // alpha-weighted). Only slot 0 = the base colour drives brightness.
            if (albLumSum && albWSum && s.slot == 0 && t.width > 0 && t.height > 0 &&
                t.rgba.size() >= (size_t)t.width * t.height * 4) {
                size_t n = (size_t)t.width * t.height;
                size_t step = std::max<size_t>(1, n / 4096); // cap ~4k samples/tex
                for (size_t p = 0; p < n; p += step) {
                    const uint8_t* px = &t.rgba[p * 4];
                    double a = px[3] / 255.0;
                    double lum = (0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]) / 255.0;
                    *albLumSum += lum * a;
                    *albWSum += a;
                }
            }
        }
    }

    // Decode the bgfx 64-bit state word exactly as bgfx's D3D11 renderer does.
    // GW2's AMAT renderState is a PARTIAL (blend-focused) word -- WRITE/DEPTH/CULL
    // bits are left clear because the engine builds those separately at submit
    // (sub_140AB3E10 for depth/cull, shaderPassFlags for the write mask) and ORs the
    // effect's word on top. So: blend fields from the word, everything else from the
    // flags. BGFX_STATE_BLEND_SHIFT=12, EQUATION_SHIFT=28, ALPHA_TO_COVERAGE=bit35.
    uint64_t st = c.renderState;
    g.renderState = st;
    g.passFlags = c.shaderPassFlags;
    // shaderPassFlags 0x40 = "depth write off" -- the engine's actual translucency
    // marker (sub_140AB3E10 clears its WRITE_Z bit on it). AMAT 57224's glass
    // effects carry it; AMAT 511755's alpha-blended eff13 does not, so that one
    // still writes depth like the cutout material it is.
    g.depthWrite = (c.shaderPassFlags & 0x0040u) == 0;
    g.blend = make_blend_state_from_bgfx(st, c.shaderPassFlags);
    g.opaqueBlend = make_opaque_blend_state(c.shaderPassFlags);
    g.ok = true;
    return true;
}

// Builds a GameMatGPU per material from ModelPreview::gameMaterials: creates the
// game's VS+PS from the DXBC blobs, an input layout from the VS reflection, the
// DYNAMIC cbuffers, and one SRV per PS sampler slot. Mirrors gw2gsviewer's
// build_material (minus the debug prints). Reuses the model's g_vb/g_ib.
void build_game_materials(const ModelPreview& model) {
    g_game_mats.clear();
    g_game_any_ok = false;
    if (!g_dev || model.gameMaterials.empty()) return;
    setup_game_uniforms();

    uint32_t maxIdx = 0;
    for (const auto& m : model.materials) maxIdx = std::max(maxIdx, m.index);
    for (const auto& gm : model.gameMaterials) maxIdx = std::max(maxIdx, gm.index);
    g_game_mats.resize(maxIdx + 1);

    // Auto-exposure accumulator: mean luminance of the DIFFUSE (slot 0) textures,
    // alpha-weighted (transparent texels don't count). GW2 uses histogram auto-
    // exposure; we approximate it from albedo so a pale/white model (e.g. fileId
    // 1925381) doesn't blow out to a glaring white under the fixed exposure that's
    // tuned for dark models. See the exposure calc after the material loop.
    double albLumSum = 0.0, albWSum = 0.0;

    for (const auto& c : model.gameMaterials) {
        GameMatGPU g;
        if (!build_one_game_mat(c, model, g, &albLumSum, &albWSum)) continue;
        if (c.index < g_game_mats.size()) {
            g_game_mats[c.index] = std::move(g);
            g_game_any_ok = true;
        }
    }

    // Albedo-adaptive exposure, OFF by default now.
    //
    // This existed to stop a pale model glaring under the fixed 1.9x post exposure,
    // which itself existed because materials were being drawn with their deferred
    // normal-prepass shader (unlit, hence dark) instead of the real forward-lit one.
    // With the correct shader selected the material computes its own exposure-correct
    // result and second-guessing it from mean albedo just makes brightness depend on
    // which model is loaded -- something the game never does. Opt back in with
    // GW2_AUTOEXP=1 for eyeballing.
    bool autoOn = false;
    if (const char* e = std::getenv("GW2_AUTOEXP")) autoOn = std::atoi(e) != 0;
    if (autoOn && !g_post_exp_env && albWSum > 1.0) {
        float albLum = static_cast<float>(albLumSum / albWSum); // ~[0,1]
        const float knee = 0.30f;
        float exp = kPostExpMax;
        if (albLum > knee) exp = kPostExpMax * std::pow(knee / albLum, 1.7f);
        g_post_exp = std::clamp(exp, 0.6f, kPostExpMax);
        if (std::getenv("GW2_EXPDBG"))
            std::fprintf(stderr, "[autoexp] meanAlbedo=%.3f -> exposure=%.3f\n", albLum, g_post_exp);
    } else if (!g_post_exp_env) {
        g_post_exp = kPostExpMax;
    }
}


void build_scene_game_materials(const std::vector<ModelPreview>& models) {
    if (!g_dev) return;
    setup_game_uniforms();
    size_t n = std::min(models.size(), g_scene_models.size());
    for (size_t i = 0; i < n; ++i) {
        SceneModelGPU& sm = g_scene_models[i];
        sm.gameTried = true;
        if (sm.gameOk) continue;
        const ModelPreview& mp = models[i];
        if (mp.gameMaterials.empty()) continue;
        uint32_t maxIdx = 0;
        for (const auto& m : mp.materials) maxIdx = std::max(maxIdx, m.index);
        for (const auto& gm : mp.gameMaterials) maxIdx = std::max(maxIdx, gm.index);
        sm.gameMats.clear();
        sm.gameMats.resize(maxIdx + 1);
        for (const auto& c : mp.gameMaterials) {
            GameMatGPU g;
            if (!build_one_game_mat(c, mp, g, nullptr, nullptr)) continue;
            if (c.index < sm.gameMats.size()) { sm.gameMats[c.index] = std::move(g); sm.gameOk = true; }
        }
    }
}
// True once at least one scene model has usable game materials.

} // namespace castlemist::render
