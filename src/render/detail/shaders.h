/// @file
/// @brief Every HLSL source string the model renderer compiles at startup.
///
/// Kept together so a shader can be found and diffed without hunting through
/// pipeline code. These are castlemist's *own* reconstruction shaders; the
/// game's real material shaders are DXBC blobs pulled out of the .dat and are
/// never compiled here.
///
/// @warning Not a public header -- internal to `src/render/`.

#pragma once

namespace castlemist::render::shaders {

// Same HLSL as gw2viewer.cpp (albedo * lambert + hemispheric ambient, optional
// 3DCX normal mapping, effect-material emissive path).
inline constexpr const char* kModel = R"(
cbuffer CB : register(b0) {
    row_major float4x4 uMVP;
    row_major float4x4 uModel;
    float3 uLightDir;  float uHasTex;
    float4 uTint;
    float  uHasNormal; float uIsEffect; float uHasSkin; float uMatKind; // matKind: 0 normal, 1 terrain, 2 water
    float  uExposure; float uLightMode; float uCutout; float _cbpad; // exposure(0=>1); lightMode 1=camera headlight
};
// Bone matrix palette as an unbounded StructuredBuffer (4 float4 rows per bone),
// so rigs of ANY size work (no fixed cap). Rows are row-major SkinMat rows, so
// float4x4(r0,r1,r2,r3) rebuilds the row-major matrix and mul(rowvec, M) is exact.
StructuredBuffer<float4> gBoneRows : register(t2);
float4x4 boneMat(uint i){ uint b=i*4u; return float4x4(gBoneRows[b], gBoneRows[b+1u], gBoneRows[b+2u], gBoneRows[b+3u]); }
Texture2D    gDiffuse : register(t0);
Texture2D    gNormal  : register(t1);
SamplerState gSamp    : register(s0);

struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float3 bit:BITANGENT; float2 uv:TEXCOORD0;
               uint4 bidx:BLENDINDICES; float4 bwt:BLENDWEIGHT; };
struct VSOut { float4 pos:SV_POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float3 bit:BITANGENT; float2 uv:TEXCOORD0;
               float up:TEXCOORD1; float height:TEXCOORD2; };

VSOut VSMain(VSIn i){
    VSOut o;
    float3 p = i.pos, nn = i.nrm, tt = i.tan, bb = i.bit;
    if (uHasSkin > 0.5){
        // Linear-blend skinning: blend the per-bone skin matrices by weight, then
        // transform position + tangent frame into the animated (model) space.
        float4x4 sm = i.bwt.x*boneMat(i.bidx.x) + i.bwt.y*boneMat(i.bidx.y)
                    + i.bwt.z*boneMat(i.bidx.z) + i.bwt.w*boneMat(i.bidx.w);
        p  = mul(float4(i.pos,1.0), sm).xyz;
        nn = mul(float4(i.nrm,0.0), sm).xyz;
        tt = mul(float4(i.tan,0.0), sm).xyz;
        bb = mul(float4(i.bit,0.0), sm).xyz;
    }
    o.pos = mul(float4(p,1.0), uMVP);
    o.nrm = normalize(mul(float4(nn,0.0), uModel).xyz);
    o.tan = normalize(mul(float4(tt,0.0), uModel).xyz);
    o.bit = normalize(mul(float4(bb,0.0), uModel).xyz);
    o.uv  = i.uv;
    o.up  = i.nrm.z;   // geometric up-ness (Z-up terrain), before rotation -- for slope color
    o.height = p.z;    // world height (Z), for depth-tinting grass
    return o;
}

// Directional lighting calibrated to GW2's real per-map environment rig (env
// chunk, PackMapEnvDataLightV75: {byte3 color; float intensity; float3 dir}).
// A representative day preset has ONE dominant warm sun -- colour (255,233,192),
// intensity ~1.3 -- plus several dim COOL sky fills (intensity ~0.1-0.3, e.g.
// (124,172,248)). The old stand-in instead summed three near-equal white lights
// (0.85+0.45+0.30) on top of a bright ambient, for a peak radiance of ~2.1: any
// albedo above ~0.48 then clipped flat to white after saturate(). Here we mirror
// the game's shape -- a bright key + weak coloured fills + a modest hemisphere
// ambient -- for a sane energy budget, and let tonemapGamma() roll off highlights
// instead of hard-clipping (GW2 renders HDR, then tonemaps in post).
static const float3 kL0 = float3(-0.372, 0.651, 0.661); // key/sun (upper front-left)
static const float3 kL1 = float3( 0.780, 0.300,-0.548); // sky fill (right, cool)
static const float3 kL2 = float3( 0.000,-0.640,-0.768); // rim     (low back)
static const float3 kSun = float3(1.00, 0.91, 0.75);    // warm sun colour (env 255,233,192)
static const float3 kSky = float3(0.62, 0.74, 1.00);    // cool sky-fill colour (env ~124,172,248)
float wrapd(float3 N, float3 L){ float d = dot(N, L) * 0.5 + 0.5; return d * d; }
float3 envLight(float3 N){
    float ex = uExposure > 0.001 ? uExposure : 1.0;
    // Hemisphere ambient: cool sky above, slightly warmer ground bounce below.
    // The floor is deliberately non-trivial so faces turned away from the sun
    // (e.g. steep shadow-side cliffs) keep material detail instead of crushing to
    // black -- GW2's real ambient/sky term does the same.
    float3 amb = lerp(float3(0.21,0.22,0.25), float3(0.34,0.35,0.34), N.y * 0.5 + 0.5);
    if (uLightMode > 0.5){
        // Camera-following headlight. The key light comes from -uLightDir, a
        // camera-relative direction the CPU tilts via the "light angle" slider, so
        // the surface facing the viewer stays lit no matter how the model is
        // orbited (fixes "kamera diubah jadi gelap"). A weak opposite rim keeps
        // some shape; tilting the angle away from the camera darkens the front.
        float3 Lk = normalize(-uLightDir);
        float key = saturate(dot(N, Lk)) * 0.88 + wrapd(N, Lk) * 0.12;
        float rim = wrapd(N, normalize(float3(-Lk.x, Lk.y, -Lk.z))) * 0.10;
        return amb + (kSun * key + kSky * rim) * ex;
    }
    // Key sun: mostly Lambert with a touch of wrap so shadowed faces don't go black.
    float key  = saturate(dot(N, kL0)) * 0.80 + wrapd(N, kL0) * 0.20;
    float fill = wrapd(N, kL1) * 0.22;   // dim cool fill
    float rim  = wrapd(N, kL2) * 0.12;   // dim warm back-rim
    float3 diff = kSun * (key + rim * 0.5) + kSky * fill;
    return amb + diff * ex;               // ambient stays fixed; ex scales only direct light
}
// HDR-ish tonemap + gamma. The exponential rolloff asymptotes to 1 so bright
// surfaces read as bright rather than snapping to a flat, detail-less white
// (which is what made lit models look "putih"). Replaces a hard saturate().
float3 tonemapGamma(float3 c){
    c = 1.0 - exp(-max(c, 0.0) * 1.05);
    return pow(saturate(c), 1.0/2.2);
}

float4 PSMain(VSOut i):SV_TARGET{
    float3 N = normalize(i.nrm);
    float ndl = saturate(dot(N, -uLightDir));
    float3 amb = lerp(float3(0.18,0.20,0.24), float3(0.42,0.42,0.40), N.y*0.5+0.5);

    // Terrain: grass on flat ground, rock on steep slopes (slope from geometric
    // up-ness), with a little height variation. No real splat textures yet.
    if (uMatKind > 0.5 && uMatKind < 1.5){
        float slope = saturate(1.0 - i.up);                 // 0 flat .. 1 vertical
        float3 grass = float3(0.24, 0.42, 0.17);
        float3 grassHi = float3(0.34, 0.52, 0.24);
        float3 rock  = float3(0.34, 0.30, 0.26);
        float3 g = lerp(grass, grassHi, saturate(i.height*0.002 + 0.5));
        float3 albedo = lerp(g, rock, smoothstep(0.28, 0.62, slope));
        float3 col = albedo * envLight(N);
        return float4(tonemapGamma(col), 1.0);
    }
    // Water: translucent blue sheet (pools where terrain sits below it).
    if (uMatKind > 1.5 && uMatKind < 2.5){
        float3 water = float3(0.04, 0.22, 0.38);
        float fres = pow(1.0 - saturate(N.y), 4.0);          // subtle rim brighten
        float3 col = water * (0.7 + ndl*0.4) + float3(0.05,0.10,0.14)*fres;
        return float4(pow(saturate(col), 1.0/2.2), 0.78);
    }
    // Collision: translucent orange overlay (physics geometry), lit for shape.
    if (uMatKind > 2.5){
        float3 c = float3(1.00, 0.55, 0.08) * (0.45 + ndl*0.55);
        return float4(pow(saturate(c), 1.0/2.2), 0.55);
    }

    float4 tex = uHasTex > 0.5 ? gDiffuse.Sample(gSamp, i.uv) : float4(0.75,0.75,0.78,1.0);
    // GW2 ordinary (non-blended) materials are CUTOUT, never alpha-blended. Verified
    // against a real frame capture (sample1.rdc): 50 of 87 material/prepass pixel
    // shaders run exactly
    //     add_sat a,a,a ; add a,-0.5 ; lt a,0 ; discard_nz     ==  clip(saturate(2a) - 0.5)
    // on the DIFFUSE alpha -- i.e. discard where alpha < 0.25 -- and then write a
    // CONSTANT alpha (32 shaders `mov o0.w, cb0[..]`, 12 a literal; only 8 compute a
    // real opacity, and those are the genuinely blended ones). So a diffuse map's
    // alpha channel is a coverage MASK, not opacity: using it to blend is what made
    // foliage/hair/grates/chain look wrong. Cutout here, opaque alpha on output.
    // uCutout gates it to textures whose alpha really is a mask (see hasCutout), so a
    // material that just leaves alpha at 0 is not erased.
    if (uHasTex > 0.5 && uIsEffect < 0.5 && uCutout > 0.5) clip(saturate(tex.a * 2.0) - 0.5);
    if (uIsEffect > 0.5){
        // Split each texel between a shaded-surface part and a self-illuminated
        // (emissive) part. Genuinely bright pixels -- runes, energy trails, fire,
        // gem cores -- read as glow; the dark body of a texture (metal, cloth, a
        // plain diffuse map that only *looks* effect-y to the classifier) is lit
        // like an ordinary surface instead of collapsing to a flat, unlit blob.
        // That flat-blob output was the "aneh dan jelek" look on weapon/armor/mount
        // skins. envLight(N) here uses the geometric normal (no normal map in the
        // effect pass), which is enough to give the surface real form.
        float lum  = dot(tex.rgb, float3(0.299,0.587,0.114));
        float glow = smoothstep(0.30, 0.80, lum);        // 0 = surface .. 1 = emissive
        float3 base = tex.rgb * uTint.rgb;
        float3 lit  = base * envLight(N);                // shaded as a lit surface
        float3 col  = lerp(lit, base, glow) + base * glow * 0.5;  // emissive punch on the glow
        float  a    = saturate(max(lum, tex.a)) * uTint.a;       // alpha keying unchanged
        return float4(tonemapGamma(col), a);
    }
    if (uHasNormal > 0.5){
        float2 nxy = gNormal.Sample(gSamp, i.uv).xy * 2.0 - 1.0;
        float  nz  = sqrt(saturate(1.0 - dot(nxy,nxy)));
        float3x3 TBN = float3x3(normalize(i.tan), normalize(i.bit), N);
        N = normalize(mul(float3(nxy, nz), TBN));
        ndl = saturate(dot(N, -uLightDir));
    }
    float3 albedo = tex.rgb * uTint.rgb;
    float3 col = albedo * envLight(N);
    return float4(tonemapGamma(col), 1.0);
}
)";

// Position-only unlit shader for the skeleton overlay (bone lines + joint
// crosses). Reuses register(b0): only the leading uMVP of CB is read.
inline constexpr const char* kLine = R"(
cbuffer CB : register(b0) { row_major float4x4 uMVP; };
float4 VSLine(float3 pos : POSITION) : SV_POSITION { return mul(float4(pos, 1.0), uMVP); }
float4 PSLine() : SV_TARGET { return float4(0.20, 1.0, 0.35, 1.0); }
)";

// Per-vertex-colored line shader for the Blender-style gizmo, ground grid and
// world-axis triad. Reuses register(b0)'s leading uMVP (CB). Alpha lets faint
// grid lines and highlighted handles share one draw.
inline constexpr const char* kGizmo = R"(
cbuffer CB : register(b0) { row_major float4x4 uMVP; };
struct VIn  { float3 pos:POSITION; float4 col:COLOR0; };
struct VOut { float4 pos:SV_POSITION; float4 col:COLOR0; };
VOut VSGiz(VIn i){ VOut o; o.pos = mul(float4(i.pos,1.0), uMVP); o.col = i.col; return o; }
float4 PSGiz(VOut i):SV_TARGET{ return i.col; }
)";

// One vertex of a particle billboard quad (own small format). lu,lv = the quad
// corner sign in [-1,1] (local coord for the soft-edge falloff in PSPart).
struct PartVtx { float x, y, z, u, v, lu, lv, r, g, b, a; };

inline constexpr const char* kParticle = R"(
cbuffer CB : register(b0) { row_major float4x4 uMVP; };
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
// luv = quad-local coordinate in [-1,1] on each axis (the billboard corner sign),
// carried separately from the atlas uv so the soft-edge falloff is correct even
// when uv is a flipbook sub-rect.
struct VSIn  { float3 pos:POSITION; float2 uv:TEXCOORD0; float2 luv:TEXCOORD1; float4 col:COLOR0; };
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float2 luv:TEXCOORD1; float4 col:COLOR0; };
VSOut VSPart(VSIn i){ VSOut o; o.pos = mul(float4(i.pos,1), uMVP); o.uv=i.uv; o.luv=i.luv; o.col=i.col; return o; }
float4 PSPart(VSOut i):SV_TARGET{
    float4 t = tex0.Sample(samp0, i.uv);
    // Round off the billboard: fade the outer rim of the quad so sprites read as
    // soft round puffs instead of hard bright squares (the "efek partikel jelek"
    // look). Only the outer ~28% is touched, so authored sprite detail is kept.
    float r    = length(i.luv);
    float edge = 1.0 - smoothstep(0.72, 1.0, r);
    // premultiply-ish additive glow: modulate by particle color, weight by alpha
    float3 rgb = t.rgb * i.col.rgb;
    float  a   = t.a * i.col.a * edge;
    return float4(rgb * a, a);
}
)";

// Fullscreen blit of the game-shader output to the backbuffer.
//
// The game's equivalent final pass is literally three instructions -- sample,
// `mul r0, r0, cb0[0].x` with cb0[0].x = 0.999985, move out -- i.e. a plain copy
// with NO tonemap operator, because nothing upstream ever left LDR (see
// kGameLightDecode in state.h). This used to apply exposure 1.9, an ambient lift
// and a soft-knee filmic curve; that was compensation for a light-buffer decode
// that was 5x too dark, and with the decode fixed it only distorts the image.
//
// gP.x/y/z survive as *viewer* knobs (GW2_POSTEXP / POSTAMB / POSTGAMMA) and are
// neutral (1,0,1) by default, so the default output matches the game.
inline constexpr const char* kPost = R"(
Texture2D gScene : register(t0);
SamplerState gSamp : register(s0);
cbuffer Post : register(b0) { float4 gP; }; // x=exposure y=ambient z=invGamma
struct VO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VO VSPost(uint id:SV_VertexID) {
    VO o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.pos = float4(t * float2(2,-2) + float2(-1,1), 0, 1); return o;
}
float4 PSPost(VO i):SV_Target {
    float3 c = gScene.Sample(gSamp, i.uv).rgb;
    c = c * gP.x + gP.y;                        // exposure + ambient lift (neutral by default)
    // No tonemap operator, matching the game: its final pass is a plain copy. The
    // soft-knee rolloff that used to live here existed to hide the over-bright
    // result of a 1.9x exposure that itself only existed because the renderer was
    // drawing with each material's normal-prepass shader instead of its lit one.
    // Both are gone; with gP = (1,0,1) this is now literally a copy.
    c = pow(saturate(c), gP.z);                // gamma (invGamma in gP.z)
    return float4(c, 1);
}
)";

// (1) World-normal G-buffer. Reuses CB (register b0): only uMVP + uModel are read.
// Input is GVertex (POSITION+NORMAL used) -- works with the already-skinned buffer,
// so the stored normals match exactly what the material pass will draw at each pixel.
// The cutout MUST be applied here too. In the real frame the normal prepass runs the
// SAME `clip(saturate(2*diffuse.a) - 0.5)` test before writing the normal (e.g. the
// captured prepass PS samples `t0.wxyz` -> alpha only, discards, then writes
// normal*0.5+0.5). Without it, cut-away texels (leaf gaps, hair card edges, grate
// holes) still deposit a normal, so the light buffer -- and every material that
// samples it at t14 -- gets lit geometry where the game has a hole.
inline constexpr const char* kNormalGBuffer = R"(
cbuffer CB : register(b0) {
    row_major float4x4 uMVP; row_major float4x4 uModel;
    float3 uLightDir; float uHasTex;
    float4 _cbTint;
    float4 _cbFlags;
    float  _cbExposure; float _cbLightMode; float uCutout; float _cbPad;
};
Texture2D    gDiffuse : register(t0);
SamplerState gSamp    : register(s0);
struct VIn  { float3 pos:POSITION; float3 nrm:NORMAL; float2 uv:TEXCOORD0; };
struct VOut { float4 pos:SV_POSITION; float3 wn:TEXCOORD0; float2 uv:TEXCOORD1; };
VOut VSNrm(VIn i){
    VOut o;
    o.pos = mul(float4(i.pos, 1.0), uMVP);
    o.wn  = normalize(mul(float4(i.nrm, 0.0), uModel).xyz);  // world-space normal
    o.uv  = i.uv;
    return o;
}
float4 PSNrm(VOut i):SV_TARGET{
    if (uHasTex > 0.5 && uCutout > 0.5) clip(saturate(gDiffuse.Sample(gSamp, i.uv).a * 2.0) - 0.5);
    return float4(normalize(i.wn) * 0.5 + 0.5, 1.0);
}
)";

// (2) Fullscreen light-accumulation pass. Reads the world-normal buffer and writes
// the game's EXACT SH diffuse-lighting model -- ported verbatim from the real
// Forward-SH-lit material pixel shader extracted from the client executable
// (tools/shaders/exe_shaders #1428). The game computes, per channel:
//     SH ambient = dot(float4(N,1), sh{Red,Green,Blue})     (directional, not flat)
//     sun diffuse = saturate(dot(float4(N,1), shSun)) * shSunColor.rgb
//     colour      = SH ambient + shadow * sun diffuse        (shadow = 1 offline)
// driven by the SAME sh* rig the forward materials use (g_game_vals), so deferred
// materials (which sample this buffer as gSs14) and forward materials shade with
// one consistent, game-accurate lighting model. Background (N=0) collapses to the
// SH DC term (each channel's .w) = ambient only. .a carries a specular proxy.
inline constexpr const char* kLightAccum = R"(
Texture2D gNrm : register(t0);
SamplerState gSamp : register(s0);
// encParams.x = kGameLightEncode (0.25). The game's light-buffer pixel shader ends
// with `mul o0, light, cb0[0].x` for exactly this reason: the buffer is 8-bit UNORM,
// so light is stored pre-divided and every material multiplies it back by 4.0. Doing
// the same here means our buffer is interchangeable with the game's, quantisation
// included, and the materials' own x4 decode lands on the right value.
cbuffer L : register(b0) {
    float4 shRed; float4 shGreen; float4 shBlue; float4 shSun; float4 shSunColor;
    float4 encParams;
};
struct VO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
VO VSFS(uint id:SV_VertexID){
    VO o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.pos = float4(t * float2(2,-2) + float2(-1,1), 0, 1); return o;
}
float4 PSLight(VO i):SV_Target{
    float enc = encParams.x;
    float3 n = gNrm.Sample(gSamp, i.uv).rgb * 2.0 - 1.0;
    float len = length(n);
    // no geometry: SH DC term (ambient only), encoded like everything else
    if (len < 0.1) return float4(shRed.w, shGreen.w, shBlue.w, 0.0) * enc;
    n /= len;
    float4 N4 = float4(n, 1.0);
    float3 amb = float3(dot(N4, shRed), dot(N4, shGreen), dot(N4, shBlue)); // directional SH ambient
    float ndl = saturate(dot(N4, shSun));            // shSun.w = 0 => dot(n, shSun.xyz)
    float3 diff = amb + shSunColor.rgb * ndl;
    float spec = pow(ndl, 8.0) * shSunColor.w;       // .a spec proxy (no view vec offline)
    // Encode into the 0..1/enc range the 8-bit buffer carries, exactly as the game's
    // light pass does. Values above 1/enc (4.0) clip -- the game's own buffer peaks at
    // ~1.28 decoded, so there is ample headroom.
    return float4(diff, spec) * enc;
}
)";

} // namespace castlemist::render::shaders
