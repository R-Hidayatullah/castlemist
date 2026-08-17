/// @file
/// @brief Bind pose, animated pose, the bone palette and the CPU skinning fallback.

#include "detail/state.h"

#include "castlemist/native/granny_pose.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace castlemist::render {

// The pose composition and skin-matrix maths live in native/granny_pose.hpp so
// this renderer and the bgfx "Game 1:1" surface share one implementation.
using BoneXform = castlemist::granny::PoseXform;

// ModelJoint -> the shared PoseBone view. Rebuilt per call rather than cached:
// it is six pointers per bone against a full hierarchy walk, and caching it
// would need invalidating every time g_joints changed.
static std::vector<castlemist::granny::PoseBone> pose_bones() {
    std::vector<castlemist::granny::PoseBone> pb(g_joints.size());
    for (size_t i = 0; i < g_joints.size(); ++i) {
        const ModelJoint& j = g_joints[i];
        pb[i] = {j.name.c_str(), j.parent, j.localPos, j.localQuat, j.localScale, j.invWorld};
    }
    return pb;
}

// Poses the skeleton at the current clip + time into per-bone world transforms.
// clip -1 = bind pose: reuse the InverseWorld-derived positions with identity
// linear part (the skin palette then resolves to identity). Otherwise the shared
// Granny composition walks the hierarchy, sampling each bone's track by name and
// falling back to its bind local transform when the clip does not drive it.
void compute_world(std::vector<BoneXform>& out) {
    const auto& J = g_joints;
    const bool posed = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
    if (!posed) {
        out.assign(J.size(), BoneXform{});
        for (size_t i = 0; i < J.size(); ++i) std::memcpy(out[i].pos, J[i].pos, sizeof out[i].pos);
        return;
    }
    castlemist::granny::composePose(pose_bones(), &g_clips[g_clip_index], g_anim_time, out);
}

// Uploads the bone matrix palette (register b1). Each entry = InverseWorld(bind,
// model->bone) * AnimatedWorld(bone->model), so a bind-pose bone resolves to
// identity and the vertex is left where it started. Row-major to match the HLSL.
void update_bone_palette() {
    if (!g_bonePalette || g_bone_cap == 0) return;
    std::vector<BoneXform> W;
    compute_world(W);
    std::vector<castlemist::granny::PoseBone> pb = pose_bones();
    int n = std::min<int>(static_cast<int>(g_joints.size()), static_cast<int>(g_bone_cap));
    bool posed = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
    std::vector<float> pal(static_cast<size_t>(g_bone_cap) * 16, 0.0f);
    for (int b = 0; b < static_cast<int>(g_bone_cap); ++b) {
        float* m = pal.data() + b * 16;
        if (!posed || b >= n) { // identity
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            continue;
        }
        // Row-major, row-vector: the HLSL below does mul(rowvec, M).
        castlemist::granny::skinMatrix(pb[b], W[b], m);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_bonePalette.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, pal.data(), pal.size() * sizeof(float));
        g_ctx->Unmap(g_bonePalette.Get(), 0);
    }
}

// (Re)creates the bone palette StructuredBuffer sized to the current rig. 4 rows
// (float4) per bone; DYNAMIC so update_bone_palette refills it each frame.
void ensure_bone_palette(size_t bones) {
    if (bones == 0) { g_bonePalette.Reset(); g_bonePaletteSRV.Reset(); g_bone_cap = 0; return; }
    if (g_bone_cap >= bones && g_bonePalette) return; // existing buffer is big enough
    g_bonePaletteSRV.Reset();
    g_bonePalette.Reset();
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16; // one float4 row
    bd.ByteWidth = static_cast<UINT>(bones * 4 * 16); // 4 rows * 16 bytes per bone
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_bonePalette))) { g_bone_cap = 0; return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = static_cast<UINT>(bones * 4);
    if (FAILED(g_dev->CreateShaderResourceView(g_bonePalette.Get(), &sv, &g_bonePaletteSRV))) {
        g_bonePalette.Reset(); g_bone_cap = 0; return;
    }
    g_bone_cap = static_cast<UINT>(bones);
}

// Fills the dynamic line buffer from the current pose (bone segments + joint
// crosses). Cheap enough to run every frame during playback.
void rebuild_skel_lines() {
    if (!g_has_skeleton || !g_skelVb || g_joints.empty()) return;
    std::vector<BoneXform> W;
    compute_world(W);
    const float cross = (g_radius > 1e-3f ? g_radius : 1.0f) * 0.012f;
    std::vector<LineVtx> verts;
    verts.reserve(W.size() * 8);
    for (size_t i = 0; i < W.size(); ++i) {
        const float* p = W[i].pos;
        int par = g_joints[i].parent;
        if (par >= 0 && par < static_cast<int>(W.size())) {
            const float* pp = W[par].pos;
            verts.push_back({pp[0], pp[1], pp[2]});
            verts.push_back({p[0], p[1], p[2]});
        }
        verts.push_back({p[0] - cross, p[1], p[2]}); verts.push_back({p[0] + cross, p[1], p[2]});
        verts.push_back({p[0], p[1] - cross, p[2]}); verts.push_back({p[0], p[1] + cross, p[2]});
        verts.push_back({p[0], p[1], p[2] - cross}); verts.push_back({p[0], p[1], p[2] + cross});
    }
    g_skel_vert_count = std::min<UINT>(static_cast<UINT>(verts.size()), g_skel_vert_cap);
    if (!g_skel_vert_count) return;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_skelVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, verts.data(), g_skel_vert_count * sizeof(LineVtx));
        g_ctx->Unmap(g_skelVb.Get(), 0);
    }
}

// Stores the skeleton + decoded clips and allocates the dynamic line buffer.
void build_skeleton(const ModelPreview& model) {
    g_skelVb.Reset();
    g_skel_vert_count = 0;
    g_skel_vert_cap = 0;
    g_has_skeleton = false;
    g_joints = model.joints;
    g_clips = model.animClips;
    g_clip_index = -1;
    g_anim_time = 0.0f;
    g_anim_playing = false;
    if (g_dev) ensure_bone_palette(g_joints.size()); // dynamic, any rig size
    if (!g_dev || g_joints.empty()) return;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = static_cast<UINT>(g_joints.size() * 8 * sizeof(LineVtx));
    if (SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_skelVb))) {
        g_skel_vert_cap = static_cast<UINT>(g_joints.size() * 8);
        g_has_skeleton = true;
        rebuild_skel_lines();
    }
}

// Static lighting/fog/texture-transform uniform environment for the game
// shaders (ported from gw2gsviewer.cpp main()). Camera uniforms are refilled
// per frame in render_game(); these constants stay put. A daylight SH rig +
// full-visibility fxclr so forward-lit and stipple-discard shaders both draw.
// (Re)derives the sun + SH-ambient GameShader lighting uniforms from the active
// environment rig (g_env_rig -- the map's real env chunk, the UI pick, or the
// baked GW2-daylight default). Split out of setup_game_uniforms so it can be
// re-run whenever the rig changes without touching the static (fog/texture)
// uniforms. The game's light PS reads each shChannel as dot(float4(N,1), ch), so
// .w is the flat (DC) ambient term and .y adds a vertical sky gradient. GW2_LIGHT
// scales it. Mapping is calibrated so a real ~1.0-1.3 intensity sun lands near
// ~0.8 (moody, no white clip) rather than blowing out to a "lamp".

int animation_count() { return static_cast<int>(g_clips.size()); }
const char* animation_name(int i) {
    return (i >= 0 && i < static_cast<int>(g_clips.size())) ? g_clips[i].name.c_str() : "";
}
float animation_duration(int i) {
    return (i >= 0 && i < static_cast<int>(g_clips.size())) ? g_clips[i].duration : 0.0f;
}
// True if a curve is keyframed AND its control values actually vary across keys.
// Note: a static "zeropose" can still encode a few multi-knot spline curves whose
// control values are all identical (no real motion), so knots.size()>1 alone is not
// enough -- we must check that the samples actually change.
static bool curve_has_motion(const castlemist::granny::Curve& c) {
    if (c.knots.size() <= 1 || c.dim <= 0) return false;
    int nk = static_cast<int>(c.controls.size()) / c.dim;
    if (nk <= 1) return false;
    for (int d = 0; d < c.dim; ++d) {
        float mn = c.controls[d], mx = mn;
        for (int k = 1; k < nk; ++k) {
            float v = c.controls[(size_t)k * c.dim + d];
            if (v < mn) mn = v; if (v > mx) mx = v;
        }
        if (mx - mn > 1e-4f) return true;
    }
    return false;
}
// A clip has real motion if any transform curve is keyframed with varying values.
// Mount/prop models embed only a static "zeropose"; character/boss/weapon models
// embed many real keyframed clips that actually move the rig.
bool animation_has_motion(int i) {
    if (i < 0 || i >= static_cast<int>(g_clips.size())) return false;
    for (const castlemist::granny::Track& tr : g_clips[i].tracks)
        if (curve_has_motion(tr.ori) || curve_has_motion(tr.pos) || curve_has_motion(tr.sca))
            return true;
    return false;
}
int first_motion_animation() {
    // Prefer a substantial clip: the static "zeropose" can carry a couple of
    // barely-varying curves over its ~1-frame (0.033s) duration, so require both
    // real motion and a non-trivial length to land on an actual animation.
    for (int i = 0; i < static_cast<int>(g_clips.size()); ++i)
        if (g_clips[i].duration > 0.15f && animation_has_motion(i)) return i;
    // Fallback: any clip with motion regardless of length.
    for (int i = 0; i < static_cast<int>(g_clips.size()); ++i)
        if (animation_has_motion(i)) return i;
    return -1;
}
int current_animation() { return g_clip_index; }
void set_animation(int clip_index) {
    if (clip_index < -1 || clip_index >= static_cast<int>(g_clips.size()))
        clip_index = -1;
    g_clip_index = clip_index;
    g_anim_time = 0.0f;
    QueryPerformanceCounter(&g_qpc_last);
    rebuild_skel_lines();
}
void set_anim_time(float seconds) {
    g_anim_time = seconds;
    rebuild_skel_lines();
}
float anim_time() { return g_anim_time; }
void set_playing(bool playing) {
    g_anim_playing = playing;
    if (playing) QueryPerformanceCounter(&g_qpc_last);
}
bool is_playing() { return g_anim_playing; }


bool cpu_skin_into(ID3D11Buffer* dst) {
    if (!dst || g_cpu_verts.empty() || g_joints.empty()) return false;
    std::vector<BoneXform> W;
    compute_world(W);
    std::vector<castlemist::granny::PoseBone> pb = pose_bones();
    const int nb = static_cast<int>(g_joints.size());
    std::vector<std::array<float, 16>> S(nb); // per-bone SkinMat (row-major, row-vector)
    for (int b = 0; b < nb; ++b) castlemist::granny::skinMatrix(pb[b], W[b], S[b].data());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(dst, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    GVertex* out = static_cast<GVertex*>(ms.pData);
    for (size_t i = 0; i < g_cpu_verts.size(); ++i) {
        const GVertex& s = g_cpu_verts[i];
        GVertex o = s;
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, t[3] = {0, 0, 0}, bt[3] = {0, 0, 0}, wsum = 0;
        for (int c = 0; c < 4; ++c) {
            float w = s.bwt[c];
            if (w <= 0.0f) continue;
            int bi = static_cast<int>(s.bidx[c]);
            if (bi < 0 || bi >= nb) continue;
            const float* M = S[bi].data();
            p[0] += w*(s.px*M[0]+s.py*M[4]+s.pz*M[8]+M[12]);
            p[1] += w*(s.px*M[1]+s.py*M[5]+s.pz*M[9]+M[13]);
            p[2] += w*(s.px*M[2]+s.py*M[6]+s.pz*M[10]+M[14]);
            n[0] += w*(s.nx*M[0]+s.ny*M[4]+s.nz*M[8]);
            n[1] += w*(s.nx*M[1]+s.ny*M[5]+s.nz*M[9]);
            n[2] += w*(s.nx*M[2]+s.ny*M[6]+s.nz*M[10]);
            t[0] += w*(s.tx*M[0]+s.ty*M[4]+s.tz*M[8]);
            t[1] += w*(s.tx*M[1]+s.ty*M[5]+s.tz*M[9]);
            t[2] += w*(s.tx*M[2]+s.ty*M[6]+s.tz*M[10]);
            bt[0] += w*(s.bx*M[0]+s.by*M[4]+s.bz*M[8]);
            bt[1] += w*(s.bx*M[1]+s.by*M[5]+s.bz*M[9]);
            bt[2] += w*(s.bx*M[2]+s.by*M[6]+s.bz*M[10]);
            wsum += w;
        }
        if (wsum > 1e-6f) {
            o.px = p[0]; o.py = p[1]; o.pz = p[2];
            o.nx = n[0]; o.ny = n[1]; o.nz = n[2];
            o.tx = t[0]; o.ty = t[1]; o.tz = t[2];
            o.bx = bt[0]; o.by = bt[1]; o.bz = bt[2];
        }
        out[i] = o;
    }
    g_ctx->Unmap(dst, 0);
    return true;
}

// Fullscreen relight pass: samples the offscreen game-shader render (g_sceneSRV)
// and writes exposure+ambient+gamma-brightened color to the backbuffer. Bound
// with no depth view so the fullscreen triangle always draws.

} // namespace castlemist::render
