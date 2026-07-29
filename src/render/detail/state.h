/// @file
/// @brief The model renderer's shared Direct3D state and GPU-side structures.
///
/// The renderer is one logical object -- a device, a pipeline, one model or one
/// scene, and the passes that draw them -- split across several translation
/// units purely for readability. That shared state lives here as C++17 `inline`
/// variables: one definition across the whole layer, declared next to the
/// comments that explain it, with no separate definitions file to keep in sync.
///
/// @warning Not a public header -- internal to `src/render/`. Anything outside
///          the layer must go through `castlemist/render/model_renderer.h`.

#pragma once

#include <d3d11.h>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <wrl/client.h>

#include "castlemist/render/model_renderer.h"
#include "castlemist/sim/cloth_sim.h"

#include "detail/math.h"

namespace castlemist::render {

using Microsoft::WRL::ComPtr;

// ---- reconstruction constant buffer (register b0, shared by every pipeline) ----
struct CB {
    Mat4 mvp;
    Mat4 model;
    float lx, ly, lz, hasTex;
    float tintR, tintG, tintB, tintA;
    float hasNormal, isEffect, hasSkin, matKind;
    // cutout: 1 => run GW2's alpha test (discard where diffuse alpha < 0.25).
    float exposure = 0, lightMode = 0, cutout = 0, cbpad2 = 0;  // exposure; lightMode 1=headlight
};

struct LineVtx {
    float x, y, z;
};

struct ColVtx {
    float x, y, z;
    float r, g, b, a;
};

struct MaterialGPU {
    ComPtr<ID3D11ShaderResourceView> srv;              // diffuse (full res)
    ComPtr<ID3D11ShaderResourceView> srvNormal;        // normal map (full res)
    ComPtr<ID3D11ShaderResourceView> srvReduced;       // diffuse, half res ("reduced sibling")
    ComPtr<ID3D11ShaderResourceView> srvNormalReduced; // normal map, half res
    float tint[4] = {1, 1, 1, 1};
    bool isEffect = false;
    bool cutout = false; // diffuse alpha is a coverage mask -> run the game's alpha test
    int kind = 0; // 0 normal, 1 terrain, 2 water
    // Real per-material blend state decoded from the AMAT's bgfx render state
    // (via make_blend_state_from_bgfx), when the game-shader extraction
    // resolved one for this material. Null => caller falls back to the single
    // shared g_blendAlpha (SrcAlpha/InvSrcAlpha) for the effect pass.
    ComPtr<ID3D11BlendState> blend;
};
struct SubMesh {
    UINT indexStart = 0, indexCount = 0;   // ACTIVE range (the currently selected LOD)
    // Per-LOD index ranges into the shared index buffer: lodRanges[0] = LOD0 (full
    // detail), lodRanges[k] = LOD k. `lod` selects which one is active; changing it
    // just copies the range into indexStart/indexCount so every draw loop follows.
    std::vector<std::pair<UINT, UINT>> lodRanges;
    int lod = 0;                 // selected LOD index (clamped to lodRanges)
    bool texReduced = false;     // per-submesh: bind the half-res ("reduced") texture
    uint32_t matIndex = 0;
    bool hasSkin = false;
    float center[3] = {0, 0, 0}; // submesh centroid (model space), for game-shader transparent sort
    std::string label;           // "mesh k (matN, T tris)" for the UI submesh list
};

// ---- game-shader (real bgfx DXBC) GPU material ----------------------------
// Per material, built from ModelPreview::gameMaterials at set_model: the game's
// own VS+PS created straight from the DXBC blobs, an input layout from the VS
// reflection, DYNAMIC cbuffers refilled from a shared uniform map each frame,
// and one SRV per PS sampler slot. Mirrors gw2gsviewer.cpp::MatGPU.
struct GameMatGPU {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vcb, pcb;
    ComPtr<ID3D11ShaderResourceView> srv[16];
    std::vector<GameShaderUniform> vsU, psU;
    std::vector<GameConstOverride> vsConsts, psConsts; // per-material MatConstant values
    uint32_t vsCB = 0, psCB = 0;
    uint64_t renderState = 0;
    uint32_t passFlags = 0;         // effect's shaderPassFlags (write mask / depth bits)
    ComPtr<ID3D11BlendState> blend; // non-null => translucent (drawn in pass 2)
    // Blend state for the OPAQUE draw: blending disabled, but carrying this
    // effect's own render-target write mask (game_write_mask). The engine's own
    // opaque draws are not uniformly mask 15 -- a real frame's material pass shows
    // "One/Zero mask15" x156, "One/Zero mask7" x6 and "One/Zero mask8" x7 -- because
    // an effect whose shader writes no alpha (or only alpha) masks the channels it
    // does not own. Using one shared all-channel state instead let those shaders
    // stamp an undefined alpha over the target.
    ComPtr<ID3D11BlendState> opaqueBlend;
    bool depthWrite = true;         // false when shaderPassFlags & 0x40
    bool cutout = false; // albedo (ss0) alpha is a coverage mask -> alpha-test in the prepass
    // Bitmask of PS texture slots that are the deferred light-accumulation buffer
    // (gSs14, sampler global==1). When the light pre-pass is on, render_game binds
    // our reconstructed light buffer (g_lightSRV) at these slots instead of the
    // flat grey stand-in, giving the game's albedo*light term real directional shading.
    uint16_t lightSlots = 0;
    bool ok = false;
};

// Overall light intensity for the reconstruction (Full/Plain) shading. Multiplies
// the multi-directional envLight; adjustable from the UI "Light" slider. Seeded
// from GW2_LIGHT if set. 1.0 = default balanced look.
inline float g_light_intensity = 1.0f;

// Camera-following "headlight": when on, the reconstruction key light + the game
// SH sun point from a camera-relative direction so the viewer-facing side stays
// lit under any orbit. g_light_angle (UI slider, 0..1) tilts it from directly
// behind the camera (0 = brightest front) up and over the model (1 = grazing).
//
// OFF by default: a headlight is a viewer convenience, not what GW2 does. The game
// lights everything from ONE fixed world-space environment rig -- the map's env
// chunk sun plus its SH ambient -- so a surface's shading depends on where it faces
// in the world, not on where the camera happens to be. Following the camera flattens
// exactly the directional shading the real rig produces. Still available from the UI
// toggle for inspecting a dark side of a model.
inline bool  g_light_follow = false;
inline float g_light_angle  = 0.22f;

// "Toward-light" unit vector (world) for the headlight. The camera looks along
// +Z (eye on -Z), so angle 0 = (0,0,-1) straight at the front; increasing the
// angle lifts the light up and over so the front falls into shadow.
inline Vec3 headlight_toward() {
    float a = 0.14f + std::clamp(g_light_angle, 0.0f, 1.0f) * 1.70f; // ~8deg .. ~105deg
    return norm({0.0f, std::sin(a), -std::cos(a)});
}
// Reconstruction uLightDir (light *travel* direction = -toward): the headlight
// when following, else the fixed world key the scene/map paths have always used.
inline Vec3 recon_light_dir() {
    if (g_light_follow) { Vec3 t = headlight_toward(); return {-t.x, -t.y, -t.z}; }
    return norm({-0.4f, -0.8f, -0.4f});
}

inline HWND g_hwnd = nullptr;
inline ComPtr<ID3D11Device> g_dev;
inline ComPtr<ID3D11DeviceContext> g_ctx;
inline ComPtr<IDXGISwapChain> g_swap;
inline ComPtr<ID3D11RenderTargetView> g_rtv;
inline ComPtr<ID3D11DepthStencilView> g_dsv;
// Offscreen HDR scene target + post-process (GameShader relighting). The game's
// bgfx material shaders assume a real deferred light buffer we can't run offline,
// so their output is correct-but-dark; we render them into g_sceneTex then apply
// an exposure/ambient-lift/gamma pass to the backbuffer.
inline ComPtr<ID3D11Texture2D> g_sceneTex;
inline ComPtr<ID3D11RenderTargetView> g_sceneRTV;
inline ComPtr<ID3D11ShaderResourceView> g_sceneSRV;
inline ComPtr<ID3D11VertexShader> g_postVS;
inline ComPtr<ID3D11PixelShader> g_postPS;
inline ComPtr<ID3D11Buffer> g_postCB;
// GW2 does NOT use a float HDR pipeline. Verified across all four sample
// captures: the only float colour surface in a frame is the R32_FLOAT linear
// depth buffer; every colour target is 8-bit UNORM. Its dynamic range comes from
// RANGE COMPRESSION of the light buffer instead -- one shared engine constant
// vector, cb0[0] = (0.25, 4.0, 1/128, 128), is bound to both ends:
//
//   encode  light-buffer PS : light * cb0[0].x  (* 0.25)  -> 8-bit UNORM
//   decode  every material  : sample * cb0[0].y (* 4.0 )
//
// so a 0..4 lighting range rides inside 0..1. Measured on the real light buffer
// in sample1: median decodes to 0.98, max 1.28, nothing clipped -- i.e. a fully
// lit surface sits at ~1.0 and `albedo * light` is already display-referred.
// That is why the game's final composite is a plain `colour * 1.0` copy with NO
// tonemap operator at all.
// We deliberately do NOT reproduce the 0.25/4.0 storage encoding, and it is worth
// recording why, because copying it looks obviously right and is wrong here.
// `LightBuffer` exists in only 87 of the 2244 shaders in the exe blob pool: the
// other consumers of the light buffer get their scale from a constant we cannot
// supply per shader. Storing an encoded buffer therefore leaves every shader that
// lacks the uniform reading a value 4x too small -- measured: material output
// median collapsed from 0.77 (flat stand-in) to 0.078 with an encoded buffer.
//
// So the buffer here is DISPLAY-REFERRED: it stores light directly (~1.0 lit), and
// the decode uniform is 1.0. Buffer x decode lands on the same physical light value
// as the game's 0.245 x 4.0, so shaders that read the uniform and shaders that do
// not are both correct. What actually matters for matching the game is reproduced
// in full: display-referred lighting, no tonemap operator, and material output
// clamped on write.
inline constexpr float kLightBufferDecode = 1.0f;   // the "LightBuffer" uniform
inline constexpr float kLightBufferEncode = 1.0f;   // light-pass output scale

inline bool  g_post_ready = false;
// The final composite is now a PLAIN COPY, exactly like the game's: its own last
// pass is three instructions (sample, `mul r0, r0, cb0[0].x` with cb0[0].x =
// 0.999985, move out) with no tonemap operator at all -- measured at eid 6379 of
// sample1.
//
// These used to default to 1.9 exposure / 0.03 ambient lift / 1.18 gamma plus a
// soft-knee. That whole stack was compensation for drawing with the WRONG SHADER:
// the AMAT effect picker was selecting each material's deferred normal-prepass
// (see gw2model.hpp::classifyShader), which outputs an encoded normal and carries
// no lighting, so the image came out dark and flat and had to be dragged back up
// in post. With the real forward-lit material shaders selected, the lighting is
// computed correctly inside them and any post gain on top only blows out
// highlights. Neutral by default; the env knobs stay for eyeballing.
inline float g_post_exp = 1.0f;    // exposure multiply (env GW2_POSTEXP)
inline float g_post_amb = 0.0f;    // ambient shadow lift (env GW2_POSTAMB)
inline float g_post_gamma = 1.0f;  // >1 brightens midtones (env GW2_POSTGAMMA)
inline constexpr float kPostExpMax = 1.0f; // neutral = what the game does
inline bool g_post_exp_env = false;    // GW2_POSTEXP forced a fixed exposure
// ---- light pre-pass (Option 2): reconstruct the screen-space light-accumulation
// buffer GW2's DEFERRED materials sample (gSs14). A world-normal G-buffer feeds a
// fullscreen lighting pass (ambient + sun*N.L); its output is bound at each
// material's light-buffer slot so `albedo * light` gets real directional shading
// instead of a flat constant. Env GW2_LIGHTPREPASS=0 falls back to the flat stand-in.
inline ComPtr<ID3D11Texture2D> g_nrmTex;              // world-normal G-buffer (RGBA8, *0.5+0.5)
inline ComPtr<ID3D11RenderTargetView> g_nrmRTV;
inline ComPtr<ID3D11ShaderResourceView> g_nrmSRV;
inline ComPtr<ID3D11Texture2D> g_lightTex;           // screen-space light buffer (RGBA16F)
inline ComPtr<ID3D11RenderTargetView> g_lightRTV;
inline ComPtr<ID3D11ShaderResourceView> g_lightSRV;
inline ComPtr<ID3D11VertexShader> g_nrmVs;
inline ComPtr<ID3D11PixelShader> g_nrmPs;
inline ComPtr<ID3D11VertexShader> g_lightVs;
inline ComPtr<ID3D11PixelShader> g_lightPs;
inline ComPtr<ID3D11Buffer> g_lightCB;               // SH rig: shRed/shGreen/shBlue/shSun/shSunColor (5x float4)
inline bool g_lightprepass_on = true;
inline ComPtr<ID3D11VertexShader> g_vs;
inline ComPtr<ID3D11PixelShader> g_ps;
inline ComPtr<ID3D11InputLayout> g_il;
inline ComPtr<ID3D11Buffer> g_cb;
inline ComPtr<ID3D11SamplerState> g_samp;
inline ComPtr<ID3D11RasterizerState> g_rsSolid;
inline ComPtr<ID3D11RasterizerState> g_rsWire;
// Single-sided variants for the two-pass transparency draw (back faces first, then
// front): a translucent mesh drawn CULL_NONE composites its front and back facets
// in arbitrary buffer order, so you can't tell the near side from the far side --
// they smear into one flat blob. Drawing the far (back) facets, then the near
// (front) facets on top gives correct front-over-back layering per mesh.
inline ComPtr<ID3D11RasterizerState> g_rsCullFront; // shows BACK faces (far side)
inline ComPtr<ID3D11RasterizerState> g_rsCullBack;  // shows FRONT faces (near side)
inline ComPtr<ID3D11DepthStencilState> g_dss;
inline ComPtr<ID3D11DepthStencilState> g_dssNoWrite;
inline ComPtr<ID3D11BlendState> g_blendOpaque;
inline ComPtr<ID3D11BlendState> g_blendAlpha;

inline ComPtr<ID3D11Buffer> g_vb;
inline ComPtr<ID3D11Buffer> g_ib;
// GW2's material (game) vertex shaders take ALREADY-skinned vertices (they have
// no blend inputs / bone uniform -- the engine skins in a prior pass). So for the
// game-shader path we CPU-skin into this DYNAMIC buffer each posed frame. g_cpu_verts
// is the rest-pose source (kept only for skinned models).
inline std::vector<GVertex> g_cpu_verts;
inline ComPtr<ID3D11Buffer> g_vb_skinned;
inline LARGE_INTEGER g_qpc_start{}; // wall clock origin for the game shaders' Time uniform

// --- live cloth simulation (reconstruction path, single model) --------------
// The RE'd GW2 cloth solver run on the current mesh: the top band is pinned and
// the sheet drapes under gravity + wind. Uses a DYNAMIC vertex buffer refreshed
// each frame (like g_vb_skinned). g_cloth_rest/g_cloth_lod0 are the immutable
// source geometry (all submeshes concatenated, LOD0 triangles only).
inline castlemist::cloth::ClothSim    g_cloth;
inline bool                  g_cloth_enabled = false;
inline std::vector<GVertex>  g_cloth_rest;      // rest verts (concat of all submeshes)
inline std::vector<uint32_t> g_cloth_lod0;      // LOD0-only global index buffer
inline std::vector<GVertex>  g_cloth_work;      // per-frame deformed verts
inline ComPtr<ID3D11Buffer>  g_vb_cloth;        // DYNAMIC vb the sim writes into
inline float                 g_cloth_wind[3] = {0, 0, 0};
// File-driven cloth (authored ModelClothData pieces): the sim runs on each
// piece's own low-res proxy mesh; the proxy is drawn textured (per material)
// and the matching static render submeshes are hidden so we see the sim cape.
inline std::vector<ClothPieceCPU> g_cloth_pieces;
inline bool                  g_cloth_file_active = false;
inline ComPtr<ID3D11Buffer>  g_vb_clothproxy;   // DYNAMIC proxy vertex buffer
inline ComPtr<ID3D11Buffer>  g_ib_clothproxy;   // static proxy index buffer
inline std::vector<char>     g_cloth_hide_mat;  // materialIndex -> hide the static submesh
inline std::vector<uint32_t> g_render_verts_per_mat; // materialIndex -> total render-mesh verts
inline std::vector<MaterialGPU> g_mats;
inline std::vector<SubMesh> g_subs;
inline bool g_has_model = false;

// Game-shader (real DXBC) materials, indexed by material.index, + a shared
// uniform environment (camera refilled each frame, lighting rig static).
// Reuses g_samp (wrap linear), g_dss (depth write on) and g_dssNoWrite
// (transparent, depth read-only) from the reconstruction pipeline.
inline std::vector<GameMatGPU> g_game_mats;
inline std::map<std::string, std::vector<float>> g_game_vals;
inline bool g_game_any_ok = false;      // at least one material built with game shaders
inline bool g_game_uniforms_ready = false;
// Active environment light rig for the GameShader lighting uniforms. Holds the
// baked default (its in-class defaults = the verified GW2 daylight rig) until a
// real map's env chunk (or the UI) supplies one via set_environment_rig().
inline MapEnvRig g_env_rig;
inline bool g_env_rig_active = false;   // true when a real (map/UI) rig is in force

// Bone matrix palette as a DYNAMIC StructuredBuffer (VS register t2), sized to
// the model's bone count -- rigs of any size, no fixed cap.
inline ComPtr<ID3D11Buffer> g_bonePalette;
inline ComPtr<ID3D11ShaderResourceView> g_bonePaletteSRV;
inline UINT g_bone_cap = 0;      // capacity (bones) of the current palette buffer
inline bool g_skin_ok = false;   // model has an inline rig (any size)

// Skeleton overlay resources.
inline ComPtr<ID3D11VertexShader> g_lineVs;
inline ComPtr<ID3D11PixelShader> g_linePs;
inline ComPtr<ID3D11InputLayout> g_lineIl;
inline ComPtr<ID3D11DepthStencilState> g_dssNoDepth; // draw skeleton over the mesh
inline ComPtr<ID3D11Buffer> g_skelVb;
inline UINT g_skel_vert_count = 0;
inline UINT g_skel_vert_cap = 0;      // capacity of the dynamic line buffer
inline bool g_has_skeleton = false;
inline bool g_show_skeleton = false;

// --- interactive gizmo (colored line pipeline + object transform) ----------
inline ComPtr<ID3D11VertexShader> g_gizVs;
inline ComPtr<ID3D11PixelShader> g_gizPs;
inline ComPtr<ID3D11InputLayout> g_gizIl;
inline ComPtr<ID3D11Buffer> g_gizVb;      // dynamic, rebuilt each frame
inline UINT g_giz_vert_cap = 0;

inline GizmoMode g_gizmo_mode = GizmoMode::Translate;
inline bool g_show_grid = true;
inline int  g_gizmo_hover = -1;     // hovered axis for highlight (-1 none)
inline int  g_gizmo_axis = -1;      // axis being dragged (-1 idle)
inline bool g_gizmo_dragging = false;

// Editable object transform (pivot = model center). Euler degrees.
inline float g_obj_pos[3] = {0, 0, 0};
inline float g_obj_rot[3] = {0, 0, 0};   // degrees
inline float g_obj_scale[3] = {1, 1, 1};

// Drag anchors (captured on gizmo_begin).
inline POINT g_giz_down{};
inline float g_giz_start_pos[3] = {0, 0, 0};
inline float g_giz_start_rot[3] = {0, 0, 0};
inline float g_giz_start_scale[3] = {1, 1, 1};
inline float g_giz_start_angle = 0.0f;   // rotate mode: screen angle at grab
inline float g_giz_start_dist = 1.0f;    // scale mode: screen distance from pivot at grab

// Skeleton + decoded animation clips (owned copies, for runtime posing).
inline std::vector<ModelJoint> g_joints;
inline std::vector<castlemist::granny::Anim> g_clips;
inline int g_clip_index = -1;         // -1 = bind pose
inline float g_anim_time = 0.0f;
inline bool g_anim_playing = false;
inline LARGE_INTEGER g_qpc_freq{}, g_qpc_last{};

// --- map scene (multiple models placed at world transforms) ---
struct SceneModelGPU {
    ComPtr<ID3D11Buffer> vb, ib;
    std::vector<MaterialGPU> mats;
    std::vector<SubMesh> subs;
    bool ok = false;
    // Real game (bgfx DXBC) materials for the map "Shader" mode. Built lazily the
    // first time GameShader is selected on a map (see build_scene_game_materials);
    // empty until then. gameOk = at least one material compiled -> this model can
    // draw with game shaders; otherwise render_scene_game falls back to the
    // reconstruction materials for it.
    std::vector<GameMatGPU> gameMats;
    bool gameOk = false;
    bool gameTried = false;
    // Model-space bounds (for map instance picking + inset preview framing).
    float center[3] = {0, 0, 0};
    float radius = 1.0f;
};
struct SceneInstGPU {
    int model = 0;
    Mat4 world; // model->world (row-major, row-vector)
    int layer = 0;
};
inline std::vector<SceneModelGPU> g_scene_models;
inline std::vector<SceneInstGPU> g_scene_insts;
inline bool g_scene_mode = false;
inline Vec3 g_scene_center{0, 0, 0};
inline float g_scene_radius = 1.0f;
inline bool g_layer_visible[LAYER_COUNT] = {true, true, false, false}; // prop+terrain on; collision+zone off
// Map "focus" model: a prop the user picked from the map, shown in a small inset
// preview overlaid on the scene surface (-1 = none). The inset shares the scene
// orbit (g_rot), so dragging the map turns the picked model too.
inline int g_focus_model = -1;
inline bool g_show_focus = true;

inline int g_w = 1, g_h = 1;
inline Vec3 g_center{0, 0, 0};
inline float g_radius = 1.0f;
inline Mat4 g_rot = identity(); // accumulated free-trackball model rotation
inline float g_dist = 1.7f;     // camera distance in bounding-radius units (zoom)
inline RenderMode g_mode = RenderMode::Full;

// THE single-model camera. Every pass that shares the depth buffer must project
// with the same matrices or their depth values are not comparable, and the whole
// point of a shared depth buffer is that they are: the light pre-pass writes the
// normals the material pass reads at the same pixel, and the ground grid has to be
// occluded by the geometry drawn before it.
//
// This used to be open-coded three times with TWO different near/far ranges -- the
// material and pre-pass used a tight bracket around the model, the grid/gizmo used
// 0.02r..40r -- so grid depth and model depth meant different things. That was
// invisible only because the grid was drawn with the depth test switched off, which
// is exactly what made a solid model look see-through: the grid painted straight
// over it.
//
// The bracket is tight on purpose (a 1:2000 near:far ratio starves 24-bit depth and
// coplanar surfaces z-fight), but the far plane must still clear the ground grid,
// which reaches ~4.3r from the centre diagonally -- hence 6r rather than 3r.
inline void main_camera(Vec3& eye, Mat4& view, Mat4& proj) {
    float camDist = g_radius * g_dist;
    eye = Vec3{g_center.x, g_center.y, g_center.z - camDist};
    view = lookAt(eye, g_center, {0, 1, 0});
    float zn = std::max(camDist - g_radius * 1.5f, g_radius * 0.05f);
    float zf = camDist + g_radius * 6.0f;
    proj = perspective(0.9f, static_cast<float>(g_w) / std::max(1, g_h), zn, zf);
}

struct FxParticle {
    Vec3 pos, vel, acc;
    float age = 0, life = 1;
    float sx = 1, sy = 1;         // half-size per axis (model units)
    float rot = 0, rotVel = 0;    // in-plane billboard rotation
    float c0[4] = {1,1,1,1}, c1[4] = {1,1,1,1}; // begin/end rgba
    float frameSeed = 0;          // per-particle flipbook phase
    int   emitter = 0;            // index into g_fxEmitters
    int   material = -1;          // cloud material index (-1 = procedural glow)
};
// A triangle of the source mesh (model space), for surface-emission spawn shapes.
struct FxTri { Vec3 a, b, c; float cumArea; };

inline std::vector<ParticleCloudCPU>   g_fxClouds;
inline std::vector<ParticleEmitterCPU> g_fxEmitters;
inline std::vector<EffectLightCPU>     g_fxLights;
inline std::vector<int>                g_fxEmitterCloud;  // emitter idx -> owning cloud idx (-1 none)
inline std::vector<Vec3>               g_fxCloudOrigin;   // per-cloud spawn origin (bind-pose bone pos)
inline std::vector<FxTri>              g_fxTris;          // surface-spawn triangles
inline float                           g_fxTriArea = 0;
inline std::vector<float>              g_fxAccum;         // per-emitter spawn accumulator
inline std::vector<FxParticle>         g_fxParticles;
inline bool                            g_show_effects = true;
inline float                           g_fxIntensity = 0.55f; // additive moderation (GW2_FXINTENSITY)
inline Vec3                            g_fxWindDir{0.94f, 0.33f, 0.10f}; // model-space wind (GW2 Z-up); GW2_WINDDIR
inline float                           g_fxWindGust = 1.0f;   // gust strength scale (GW2_WIND)
inline float                           g_fxTime = 0;          // seconds since model load (drives gusts)
inline uint32_t                        g_fxRng = 0x1234567u;
inline LARGE_INTEGER                   g_fx_qpc_last{};
inline bool                            g_fx_qpc_init = false;

inline ComPtr<ID3D11VertexShader>      g_partVs;
inline ComPtr<ID3D11PixelShader>       g_partPs;
inline ComPtr<ID3D11InputLayout>       g_partIl;
inline ComPtr<ID3D11Buffer>            g_partVb;          // dynamic, rebuilt each frame
inline ComPtr<ID3D11Buffer>            g_partCb;          // mvp
inline UINT                            g_partVbCap = 0;   // capacity in vertices
inline ComPtr<ID3D11BlendState>        g_blendAdd;        // additive (glow)
inline ComPtr<ID3D11ShaderResourceView> g_softDisc;       // procedural soft radial sprite

ComPtr<ID3D11ShaderResourceView> make_srv(const std::vector<uint8_t>& px, int w, int h); // fwd

inline float fxRand01() { // xorshift, deterministic
    g_fxRng ^= g_fxRng << 13; g_fxRng ^= g_fxRng >> 17; g_fxRng ^= g_fxRng << 5;
    return (g_fxRng & 0xFFFFFF) / float(0x1000000);
}
inline float fxRandSym() { return fxRand01() * 2.0f - 1.0f; }
inline float fxSample(const float r[2]) { return r[0] + fxRand01() * r[1]; }  // {base,span}

struct PartVtx { float x, y, z, u, v, lu, lv, r, g, b, a; };

// ---------------------------------------------------------------------------
// Cross-file entry points. Each is defined in the .cpp named beside it.
// ---------------------------------------------------------------------------

// device.cpp
bool create_device(HWND hwnd);
void create_targets();
bool init_pipeline();
ComPtr<ID3D11ShaderResourceView> make_srv(const std::vector<uint8_t>& px, int w, int h);
ComPtr<ID3D11ShaderResourceView> make_srv_half(const std::vector<uint8_t>& px, int w, int h);
ComPtr<ID3D11ShaderResourceView> make_solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
ComPtr<ID3D11ShaderResourceView> make_solid_cube(uint8_t level);
LONGLONG now_qpc();

// game_material.cpp
UINT8 game_write_mask(uint32_t passFlags);
ComPtr<ID3D11BlendState> make_blend_state_from_bgfx(uint64_t state, uint32_t passFlags = 0x4000u);
ComPtr<ID3D11BlendState> make_opaque_blend_state(uint32_t passFlags);
UINT game_cb_size(const std::vector<GameShaderUniform>& u, uint32_t constBuf);
void game_fill_cb(std::vector<uint8_t>& buf, const std::vector<GameShaderUniform>& u,
                  uint32_t constBuf, const std::map<std::string, std::vector<float>>& vals,
                  const std::vector<GameConstOverride>& consts);
ComPtr<ID3D11Buffer> game_build_cb(const std::vector<GameShaderUniform>& u, uint32_t constBuf,
                                   const std::map<std::string, std::vector<float>>& vals,
                                   const std::vector<GameConstOverride>& consts);
void game_update_cb(ID3D11Buffer* b, const std::vector<GameShaderUniform>& u, uint32_t constBuf,
                    const std::map<std::string, std::vector<float>>& vals,
                    const std::vector<GameConstOverride>& consts);
UINT game_sem_off(const char* semantic, UINT index);
bool build_one_game_mat(const GameMaterial& c, const ModelPreview& model, GameMatGPU& g,
                        const std::vector<ComPtr<ID3D11ShaderResourceView>>& srvs);
void build_game_materials(const ModelPreview& model);
void setup_game_uniforms();
void apply_env_uniforms();

// geometry.cpp
bool upload_scene_model(const ModelPreview& model, SceneModelGPU& out);

// skeleton.cpp
void build_skeleton(const ModelPreview& model);
void rebuild_skel_lines();
void update_bone_palette();
bool cpu_skin_into(ID3D11Buffer* dst);

// lighting.cpp
void post_process_relight();
void render_light_prepass(ID3D11Buffer* vbUse);
void debug_dump_light_buffer();
void render_scene_light_prepass(const Mat4& sceneRot, const Mat4& VP);

// particles.cpp
void capture_effects(const ModelPreview& model);
void clear_effects();
void update_particles(float dt);
void render_particles(const Mat4& mvp);
bool effects_present();

// cloth_bridge.cpp
ID3D11Buffer* cloth_step_and_upload();
void render_cloth_proxy(const Mat4& model, const Mat4& mvp, const Vec3& L);

// gizmo.cpp
Mat4 object_matrix();
bool gizmo_identity();
Mat4 giz_view_transform();
Mat4 compute_giz_mvp();
void build_gizmo_verts(std::vector<ColVtx>& v, size_t* gridVerts = nullptr);
void draw_gizmo(const Mat4& gizMVP);
int  gizmo_hit(int mx, int my);

// scene.cpp
void render_scene();
int  scene_pick(int sx, int sy);
void render_focus_inset();
void render_scene_game();

// model_renderer.cpp
void render_game(ID3D11Buffer* vbUse);

} // namespace castlemist::render
