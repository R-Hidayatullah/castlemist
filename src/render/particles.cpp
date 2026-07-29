/// @file
/// @brief The baked particle/effect system: capture, simulation and billboard rendering.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace castlemist::render {

void set_show_effects(bool on) { g_show_effects = on; if (!on) g_fxParticles.clear(); }
bool show_effects() { return g_show_effects; }
bool has_effects() { return effects_present(); }
void effect_stats(int& clouds, int& emitters, int& lights, int& live) {
    clouds = (int)g_fxClouds.size(); emitters = (int)g_fxEmitters.size();
    lights = (int)g_fxLights.size(); live = (int)g_fxParticles.size();
}
void set_show_skeleton(bool show) { g_show_skeleton = show; }
bool has_skeleton() { return g_has_skeleton; }


// ---- particle system: capture, simulate, render ----------------------------
void clear_effects() {
    g_fxClouds.clear(); g_fxEmitters.clear(); g_fxLights.clear();
    g_fxEmitterCloud.clear(); g_fxCloudOrigin.clear();
    g_fxTris.clear(); g_fxTriArea = 0; g_fxAccum.clear(); g_fxParticles.clear();
    g_fx_qpc_init = false;
}

void capture_effects(const ModelPreview& model) {
    clear_effects();
    if (!model.hasEffects()) return;
    g_fxClouds = model.clouds;
    g_fxEmitters = model.emitters;
    g_fxLights = model.effectLights;
    g_fxEmitterCloud.assign(g_fxEmitters.size(), -1);
    g_fxCloudOrigin.assign(g_fxClouds.size(), Vec3{0, 0, 0});
    for (size_t ci = 0; ci < g_fxClouds.size(); ++ci) {
        const auto& c = g_fxClouds[ci];
        if (c.boneJoint >= 0 && c.boneJoint < (int)model.joints.size())
            g_fxCloudOrigin[ci] = {model.joints[c.boneJoint].pos[0], model.joints[c.boneJoint].pos[1], model.joints[c.boneJoint].pos[2]};
        for (uint32_t ei : c.emitterIndices)
            if (ei < g_fxEmitterCloud.size()) g_fxEmitterCloud[ei] = (int)ci;
    }
    g_fxAccum.assign(g_fxEmitters.size(), 0.0f);
    // Surface-emission triangles (LOD0), with a cumulative-area table for uniform
    // sampling. Capped so huge meshes don't blow memory.
    float area = 0; size_t cap = 40000;
    for (const auto& m : model.meshes) {
        for (size_t i = 0; i + 2 < m.indices.size() && g_fxTris.size() < cap; i += 3) {
            const auto& va = m.vertices[m.indices[i]];
            const auto& vb = m.vertices[m.indices[i+1]];
            const auto& vc = m.vertices[m.indices[i+2]];
            Vec3 A{va.px, va.py, va.pz}, B{vb.px, vb.py, vb.pz}, C{vc.px, vc.py, vc.pz};
            float ar = 0.5f * std::sqrt(dot(cross(sub(B, A), sub(C, A)), cross(sub(B, A), sub(C, A))));
            area += ar;
            g_fxTris.push_back({A, B, C, area});
        }
    }
    g_fxTriArea = area;
    g_fxRng = 0x1234567u;
    g_fxTime = 0;
}

bool effects_present() { return !g_fxClouds.empty() || !g_fxLights.empty(); }

// GW2 refraction/heat-haze emitters store a flat surface normal (~0,0,1) as their
// "color" and perturb the framebuffer instead of adding light -- invisible in-game.
// Our additive pass would draw them as huge blue/purple streaks, so skip them.
bool fxIsDistortion(const ParticleEmitterCPU& e) {
    float r = e.colorBegin[0][0], g = e.colorBegin[0][1], b = e.colorBegin[0][2];
    float er = e.colorEnd[0][0], eg = e.colorEnd[0][1];
    bool beginNormal = (b > 0.8f && r < 0.2f && g < 0.2f);       // flat normal (0,0,1)
    bool endNormal   = (er > 0.8f && eg < 0.2f);                 // -> tilted normal (1,0,*)
    return beginNormal && endNormal;
}

// Pick a uniform-random point on the model surface (for spawnShape 4/5).
Vec3 fxSurfacePoint() {
    if (g_fxTris.empty() || g_fxTriArea <= 0) return {0, 0, 0};
    float t = fxRand01() * g_fxTriArea;
    // binary search the cumulative-area table
    int lo = 0, hi = (int)g_fxTris.size() - 1;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_fxTris[mid].cumArea < t) lo = mid + 1; else hi = mid; }
    const FxTri& tr = g_fxTris[lo];
    float u = fxRand01(), v = fxRand01();
    if (u + v > 1) { u = 1 - u; v = 1 - v; }
    return { tr.a.x + u*(tr.b.x-tr.a.x) + v*(tr.c.x-tr.a.x),
             tr.a.y + u*(tr.b.y-tr.a.y) + v*(tr.c.y-tr.a.y),
             tr.a.z + u*(tr.b.z-tr.a.z) + v*(tr.c.z-tr.a.z) };
}

// Transform a direction/point by the emitter's ModelMatrix43 (3 rows of float4).
Vec3 fxXformDir(const float m[12], Vec3 d) {
    return { d.x*m[0] + d.y*m[4] + d.z*m[8],
             d.x*m[1] + d.y*m[5] + d.z*m[9],
             d.x*m[2] + d.y*m[6] + d.z*m[10] };
}

void fxSpawnOne(int emIdx) {
    const ParticleEmitterCPU& e = g_fxEmitters[emIdx];
    int cloud = g_fxEmitterCloud[emIdx];
    FxParticle p;
    p.emitter = emIdx;
    p.material = (cloud >= 0) ? (int)g_fxClouds[cloud].materialIndex : -1;
    Vec3 origin = (cloud >= 0 && cloud < (int)g_fxCloudOrigin.size()) ? g_fxCloudOrigin[cloud] : Vec3{0,0,0};

    // Position from spawnShape (mirrors sub_140ACA4B0). {base,span} radius pairs.
    Vec3 pos{0, 0, 0};
    float r = fxSample(e.spawnRadius);
    switch (e.spawnShape) {
        case 0: { float a = fxRand01()*6.2831853f; pos = {std::cos(a)*r, std::sin(a)*r, -fxRand01()*r}; } break; // cylinder
        case 1: { float a = fxRand01()*6.2831853f; pos = {std::cos(a)*r, std::sin(a)*r, 0}; } break;             // disc
        case 2: pos = {fxRandSym()*r, 0, 0}; break;                                                              // line
        case 3: pos = {fxRandSym()*r, fxRandSym()*r, fxRandSym()*r}; break;                                      // box
        case 4: case 5: pos = fxSurfacePoint(); break;                                                           // mesh surface
        default: pos = {0, 0, 0}; break;                                                                        // point
    }
    // emitter transform translation (row 3 = m[3],m[7],m[11] are the .w of each row)
    if (e.hasTransform) { pos.x += e.transform[3]; pos.y += e.transform[7]; pos.z += e.transform[11]; }
    p.pos = {origin.x + pos.x, origin.y + pos.y, origin.z + pos.z};

    // Velocity dir (normalized) * speed, then rotate by the emitter transform.
    Vec3 dir = norm({fxSample(e.velocity[0]), fxSample(e.velocity[1]), fxSample(e.velocity[2])});
    float speed = fxSample(e.velocity[3]);
    if (e.hasTransform) dir = fxXformDir(e.transform, dir);
    p.vel = {dir.x * speed, dir.y * speed, dir.z * speed};
    // Cloud-level constant drift (the sand's base movement) + an initial wind kick
    // along the environment wind direction (data-driven magnitude spawnWindSpeed).
    if (cloud >= 0) { p.vel.x += g_fxClouds[cloud].velocity[0]; p.vel.y += g_fxClouds[cloud].velocity[1]; p.vel.z += g_fxClouds[cloud].velocity[2]; }
    if (e.windInfluence > 0 || e.spawnWindSpeed[0] != 0 || e.spawnWindSpeed[1] != 0) {
        float ws = fxSample(e.spawnWindSpeed);
        p.vel.x += g_fxWindDir.x * ws; p.vel.y += g_fxWindDir.y * ws; p.vel.z += g_fxWindDir.z * ws;
    }
    // Acceleration dir * magnitude + the cloud's constant accel (gravity).
    Vec3 adir = norm({fxSample(e.acceleration[0]), fxSample(e.acceleration[1]), fxSample(e.acceleration[2])});
    float amag = fxSample(e.acceleration[3]);
    if (e.hasTransform) adir = fxXformDir(e.transform, adir);
    p.acc = {adir.x * amag, adir.y * amag, adir.z * amag};
    if (cloud >= 0) { p.acc.x += g_fxClouds[cloud].acceleration[0]; p.acc.y += g_fxClouds[cloud].acceleration[1]; p.acc.z += g_fxClouds[cloud].acceleration[2]; }

    p.life = std::max(0.05f, fxSample(e.lifetime));
    p.age = 0;
    for (int k = 0; k < 4; ++k) { p.c0[k] = e.colorBegin[0][k] + fxRand01()*e.colorBegin[1][k]; p.c1[k] = e.colorEnd[0][k] + fxRand01()*e.colorEnd[1][k]; }
    p.sx = fxSample(e.scaleInitial[0]); p.sy = fxSample(e.scaleInitial[1]);
    if (p.sx <= 0) p.sx = 0.05f; if (p.sy <= 0) p.sy = p.sx;
    p.rot = fxSample(e.rotationInitial); p.rotVel = fxSample(e.rotationChange);
    p.frameSeed = fxRand01();
    g_fxParticles.push_back(p);
}

void update_particles(float dt) {
    if (!g_show_effects || dt <= 0 || g_fxClouds.empty()) { if (!g_fxParticles.empty() && !g_show_effects) g_fxParticles.clear(); return; }
    dt = std::min(dt, 0.05f); // clamp big frame gaps (paused/minimized)
    g_fxTime += dt;
    const size_t kMaxParticles = 6000;
    // spawn per emitter that a cloud references
    for (int ei = 0; ei < (int)g_fxEmitters.size(); ++ei) {
        if (g_fxEmitterCloud[ei] < 0) continue;
        const ParticleEmitterCPU& e = g_fxEmitters[ei];
        if (e.spawnPeriod <= 1e-5f) continue;
        if (fxIsDistortion(e)) continue; // heat-haze/refraction field -> not renderable additively
        g_fxAccum[ei] += dt;
        int guard = 0;
        while (g_fxAccum[ei] >= e.spawnPeriod && g_fxParticles.size() < kMaxParticles && guard++ < 64) {
            g_fxAccum[ei] -= e.spawnPeriod;
            if (fxRand01() > e.spawnProbability) continue;
            int n = (int)std::lround(fxSample(e.spawnGroupSize));
            for (int k = 0; k < n && g_fxParticles.size() < kMaxParticles; ++k) fxSpawnOne(ei);
        }
    }
    // integrate + age out
    for (size_t i = 0; i < g_fxParticles.size();) {
        FxParticle& p = g_fxParticles[i];
        p.age += dt;
        if (p.age >= p.life) { p = g_fxParticles.back(); g_fxParticles.pop_back(); continue; }
        const ParticleEmitterCPU& e = g_fxEmitters[p.emitter];
        p.vel.x += p.acc.x * dt; p.vel.y += p.acc.y * dt; p.vel.z += p.acc.z * dt;
        // Gusting wind push (turbulent, per-particle phase) for wind-influenced
        // particles -> the "blown by the wind" sway of sand/dust.
        if (e.windInfluence > 0) {
            float gust = g_fxWindGust * g_radius * (0.55f + 0.45f * std::sin(g_fxTime * 1.9f + p.pos.x * 2.0f + p.pos.y));
            float infl = std::clamp(e.windInfluence / 255.0f, 0.15f, 1.0f);
            p.vel.x += g_fxWindDir.x * gust * infl * dt;
            p.vel.y += g_fxWindDir.y * gust * infl * dt;
            p.vel.z += g_fxWindDir.z * gust * infl * dt;
        }
        float drag = std::clamp(1.0f - e.drag * dt, 0.0f, 1.0f);
        p.vel.x *= drag; p.vel.y *= drag; p.vel.z *= drag;
        p.pos.x += p.vel.x * dt; p.pos.y += p.vel.y * dt; p.pos.z += p.vel.z * dt;
        p.rot += p.rotVel * dt;
        ++i;
    }
}

// Evaluate a lifetime curve (keys (t,value)) at t; empty -> default fade.
float fxEvalCurve(const std::vector<std::pair<float,float>>& keys, float t, float dflt) {
    if (keys.empty()) return dflt;
    if (t <= keys.front().first) return keys.front().second;
    if (t >= keys.back().first) return keys.back().second;
    for (size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].first) {
            float t0 = keys[i-1].first, t1 = keys[i].first;
            float f = (t1 > t0) ? (t - t0) / (t1 - t0) : 0;
            return keys[i-1].second + f * (keys[i].second - keys[i-1].second);
        }
    return keys.back().second;
}

void render_particles(const Mat4& mvp) {
    if (!g_show_effects || !g_partVs || !g_partIl) return;
    if (g_fxParticles.empty() && g_fxLights.empty()) return;

    // Camera basis in model space (billboards face the camera): world right/up
    // are +X/+Y (fixed lookAt); the trackball model rotation g_rot maps model->
    // world for directions, so model-space basis = columns 0/1 of g_rot.
    Vec3 right{g_rot.m[0], g_rot.m[4], g_rot.m[8]};
    Vec3 up{g_rot.m[1], g_rot.m[5], g_rot.m[9]};

    struct Quad { const FxParticle* p; float u0,v0,u1,v1; float rgba[4]; int material; };
    std::vector<PartVtx> vtx;
    vtx.reserve((g_fxParticles.size() + g_fxLights.size()) * 6);

    auto emitQuad = [&](Vec3 c, float sx, float sy, float rot, const float col[4], int material, float u0, float v0, float u1, float v1) {
        float cs = std::cos(rot), sn = std::sin(rot);
        // rotated billboard axes
        Vec3 ax{ (right.x*cs + up.x*sn), (right.y*cs + up.y*sn), (right.z*cs + up.z*sn) };
        Vec3 ay{ (-right.x*sn + up.x*cs), (-right.y*sn + up.y*cs), (-right.z*sn + up.z*cs) };
        auto corner = [&](float dx, float dy, float u, float v) -> PartVtx {
            return { c.x + ax.x*dx*sx + ay.x*dy*sy, c.y + ax.y*dx*sx + ay.y*dy*sy, c.z + ax.z*dx*sx + ay.z*dy*sy,
                     u, v, dx, dy, col[0], col[1], col[2], col[3] };
        };
        PartVtx a = corner(-1,-1,u0,v1), b = corner(1,-1,u1,v1), d = corner(1,1,u1,v0), e = corner(-1,1,u0,v0);
        vtx.push_back(a); vtx.push_back(b); vtx.push_back(d);
        vtx.push_back(a); vtx.push_back(d); vtx.push_back(e);
    };

    // Group particles by material so we can bind each cloud material's texture in
    // one draw. We build all verts, tracking per-material spans.
    struct Span { int material; UINT start, count; };
    std::vector<Span> spans;
    // sort particle indices by material (stable enough: simple bucket by material)
    std::vector<int> order(g_fxParticles.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [](int a, int b){ return g_fxParticles[a].material < g_fxParticles[b].material; });

    int curMat = -2; UINT spanStart = 0;
    for (int oi = 0; oi < (int)order.size(); ++oi) {
        const FxParticle& p = g_fxParticles[order[oi]];
        if (p.material != curMat) {
            if (curMat != -2) spans.push_back({curMat, spanStart, (UINT)vtx.size() - spanStart});
            curMat = p.material; spanStart = (UINT)vtx.size();
        }
        const ParticleEmitterCPU& e = g_fxEmitters[p.emitter];
        float t = p.age / p.life;
        // Opacity: use the emitter curve if present, else a bell (fade in ~15%,
        // fade out the rest) so additively-stacked particles don't pop to full.
        float env = (t < 0.15f) ? (t / 0.15f) : (1.0f - t) / 0.85f;
        float op = e.opacityCurve.empty() ? env : fxEvalCurve(e.opacityCurve, t, 1.0f);
        float sc = fxEvalCurve(e.scaleCurve, t, 1.0f);
        // color lerp begin->end across life
        float col[4];
        for (int k = 0; k < 4; ++k) col[k] = p.c0[k] + (p.c1[k] - p.c0[k]) * t;
        col[3] *= std::clamp(op, 0.0f, 1.0f) * g_fxIntensity;
        // UV: start from the emitter's texCoordRect -- its assigned sub-rectangle of
        // the (usually shared, atlas) particle texture. Sampling the full [0,1] here
        // instead was THE bug that made effects look like fireworks: every billboard
        // showed the ENTIRE atlas (all the different flame/spark/glow sprites at once)
        // rather than the one sprite the emitter picks. Real data (Balthazar 556048)
        // has e.g. (0.5,0.5,1,1) or (0.45,0,0.5,0.25). Degenerate rects -> full.
        float ru0 = e.texCoordRect[0], rv0 = e.texCoordRect[1], ru1 = e.texCoordRect[2], rv1 = e.texCoordRect[3];
        if (ru1 <= ru0 || rv1 <= rv0) { ru0 = 0; rv0 = 0; ru1 = 1; rv1 = 1; }
        float u0 = ru0, v0 = rv0, u1 = ru1, v1 = rv1;
        // Flipbook: subdivide the emitter's rect into columns x rows cells and step
        // the animation frame within THAT rect (not the whole texture).
        if (e.flipbook.present && e.flipbook.count > 0 && e.flipbook.columns > 0 && e.flipbook.rows > 0) {
            int total = e.flipbook.count;
            int frame;
            if (e.flipbook.fps > 0.01f) frame = e.flipbook.start + (int)(p.age * e.flipbook.fps + p.frameSeed * total);
            else frame = e.flipbook.start + (int)(t * total);
            frame %= total; if (frame < 0) frame += total;
            int col_ = frame % e.flipbook.columns, row_ = frame / e.flipbook.columns;
            float fw = (ru1 - ru0) / e.flipbook.columns, fh = (rv1 - rv0) / e.flipbook.rows;
            u0 = ru0 + col_ * fw; u1 = u0 + fw; v0 = rv0 + row_ * fh; v1 = v0 + fh;
        }
        emitQuad(p.pos, p.sx * sc, p.sy * sc, p.rot, col, p.material, u0, v0, u1, v1);
    }
    if (curMat != -2) spans.push_back({curMat, spanStart, (UINT)vtx.size() - spanStart});

    // Effect lights: a soft additive glow disc at the bone position.
    if (!g_fxLights.empty()) {
        UINT ls = (UINT)vtx.size();
        for (const auto& L : g_fxLights) {
            float rad = std::max(0.02f, L.farDistance * 0.5f);
            float inten = std::clamp(L.intensity, 0.05f, 4.0f);
            float col[4] = {L.color[0]*inten, L.color[1]*inten, L.color[2]*inten, 1.0f};
            emitQuad({L.pos[0], L.pos[1], L.pos[2]}, rad, rad, 0, col, -1, 0, 0, 1, 1);
        }
        spans.push_back({-1, ls, (UINT)vtx.size() - ls});
    }
    if (vtx.empty()) return;

    // (re)alloc the dynamic VB
    if (g_partVbCap < vtx.size()) {
        g_partVb.Reset();
        D3D11_BUFFER_DESC vbd{};
        vbd.Usage = D3D11_USAGE_DYNAMIC; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vbd.ByteWidth = (UINT)(vtx.size() * sizeof(PartVtx) * 3 / 2 + 256);
        if (FAILED(g_dev->CreateBuffer(&vbd, nullptr, &g_partVb))) return;
        g_partVbCap = vbd.ByteWidth / sizeof(PartVtx);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_partVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    std::memcpy(ms.pData, vtx.data(), vtx.size() * sizeof(PartVtx));
    g_ctx->Unmap(g_partVb.Get(), 0);
    // upload mvp
    if (SUCCEEDED(g_ctx->Map(g_partCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, mvp.m, 64); g_ctx->Unmap(g_partCb.Get(), 0);
    }

    // pipeline state: additive, depth-test on / write off, particle shaders
    const float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendAdd.Get(), bf, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dssNoWrite.Get(), 0);
    g_ctx->RSSetState(g_rsSolid.Get());
    UINT stride = sizeof(PartVtx), off = 0;
    ID3D11Buffer* vbs[] = {g_partVb.Get()};
    g_ctx->IASetInputLayout(g_partIl.Get());
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_partVs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_partPs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_partCb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samps[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, samps);

    for (const auto& s : spans) {
        if (s.count == 0) continue;
        ID3D11ShaderResourceView* srv = g_softDisc.Get();
        if (s.material >= 0 && s.material < (int)g_mats.size() && g_mats[s.material].srv)
            srv = g_mats[s.material].srv.Get();
        ID3D11ShaderResourceView* srvs[] = {srv};
        g_ctx->PSSetShaderResources(0, 1, srvs);
        g_ctx->Draw(s.count, s.start);
    }
    // restore sane defaults for any following passes
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
}

// ==========================================================================
// Blender-style interactive transform gizmo
// ==========================================================================

} // namespace castlemist::render
