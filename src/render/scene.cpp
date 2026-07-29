/// @file
/// @brief Map-scene drawing: the reconstruction pass, the game-shader pass, picking and the inset.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace castlemist::render {

void render_scene() {
    bool wire = (g_mode == RenderMode::Wireframe);
    bool textured = (g_mode == RenderMode::Full);
    g_ctx->RSSetState(wire ? g_rsWire.Get() : g_rsSolid.Get());

    float camDist = g_scene_radius * g_dist;
    Vec3 eye{g_scene_center.x, g_scene_center.y, g_scene_center.z - camDist};
    Mat4 view = lookAt(eye, g_scene_center, {0, 1, 0});
    Mat4 proj = perspective(0.9f, static_cast<float>(g_w) / std::max(1, g_h),
                            g_scene_radius * 0.005f, g_scene_radius * 80.0f);
    Mat4 sceneRot = mul(translate({-g_scene_center.x, -g_scene_center.y, -g_scene_center.z}),
                        mul(g_rot, translate(g_scene_center)));
    Mat4 VP = mul(sceneRot, mul(view, proj));
    Vec3 L = norm({-0.4f, -0.8f, -0.4f});

    g_ctx->IASetInputLayout(g_il.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    // Scene props render unskinned (uHasSkin=0), so the bone palette (t2) is unused.
    ID3D11SamplerState* samps[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, samps);
    const float bf[4] = {0, 0, 0, 0};
    UINT stride = sizeof(GVertex), off = 0;

    // One draw item, gathered so the effect (transparent) pass can be sorted
    // back-to-front across every visible instance/submesh before drawing --
    // otherwise overlapping translucent textures (e.g. glass, water, glow
    // masks) composite in whatever order the scene happened to store them,
    // which is what made them look "berantakan" (out of order).
    struct EffectDraw {
        const SceneModelGPU* sm;
        Mat4 modelMat, mvp;
        const SubMesh* sub;
        const MaterialGPU* mat;
        float viewDepthSq;
    };

    int passes = textured ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        bool effectPass = textured && (pass == 1);
        g_ctx->OMSetDepthStencilState(effectPass ? g_dssNoWrite.Get() : g_dss.Get(), 0);

        std::vector<EffectDraw> effectDraws;
        if (!effectPass) g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);

        for (const auto& in : g_scene_insts) {
            if (!g_layer_visible[in.layer]) continue;
            const SceneModelGPU& sm = g_scene_models[in.model];
            if (!sm.ok) continue;
            Mat4 modelMat = mul(in.world, sceneRot);
            Mat4 mvp = mul(in.world, VP);
            for (const auto& s : sm.subs) {
                const MaterialGPU* mat = (s.matIndex < sm.mats.size()) ? &sm.mats[s.matIndex] : nullptr;
                bool isBlend = mat && (mat->kind == 2 || mat->kind == 3); // water + collision blend
                bool isEffect = mat && (mat->isEffect || isBlend);        // rendered in the effect pass
                if (textured && isEffect != effectPass) continue;
                if (effectPass) {
                    Vec3 wp = transformPoint({s.center[0], s.center[1], s.center[2]}, modelMat);
                    Vec3 d = sub(wp, eye);
                    effectDraws.push_back({&sm, modelMat, mvp, &s, mat, dot(d, d)});
                    continue;
                }
                ID3D11Buffer* vbs[] = {sm.vb.Get()};
                g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
                g_ctx->IASetIndexBuffer(sm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
                CB cb{}; cb.exposure = g_light_intensity;
                cb.model = modelMat;
                cb.mvp = mvp;
                cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
                cb.hasSkin = 0.0f;
                cb.matKind = mat ? static_cast<float>(mat->kind) : 0.0f; // terrain/water shading
                if (textured) {
                    cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
                    cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
                    cb.isEffect = (mat && mat->isEffect) ? 1.0f : 0.0f;
                    cb.cutout = (mat && mat->cutout) ? 1.0f : 0.0f;
                    cb.tintR = mat ? mat->tint[0] : 1.0f; cb.tintG = mat ? mat->tint[1] : 1.0f;
                    cb.tintB = mat ? mat->tint[2] : 1.0f; cb.tintA = mat ? mat->tint[3] : 1.0f;
                } else {
                    cb.hasTex = cb.hasNormal = cb.isEffect = cb.cutout = 0.0f;
                    cb.tintR = cb.tintG = cb.tintB = cb.tintA = 1.0f;
                }
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                    std::memcpy(ms.pData, &cb, sizeof cb);
                    g_ctx->Unmap(g_cb.Get(), 0);
                }
                ID3D11ShaderResourceView* srvs[2] = {textured && mat ? mat->srv.Get() : nullptr,
                                                     textured && mat ? mat->srvNormal.Get() : nullptr};
                g_ctx->PSSetShaderResources(0, 2, srvs);
                g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
            }
        }

        if (!effectPass) continue;

        // Farthest-from-camera first so nearer translucent surfaces composite
        // on top of what's behind them.
        std::sort(effectDraws.begin(), effectDraws.end(),
                  [](const EffectDraw& a, const EffectDraw& b) { return a.viewDepthSq > b.viewDepthSq; });

        for (const auto& d : effectDraws) {
            const MaterialGPU* mat = d.mat;
            g_ctx->OMSetBlendState(mat && mat->blend ? mat->blend.Get() : g_blendAlpha.Get(), bf, 0xffffffff);
            ID3D11Buffer* vbs[] = {d.sm->vb.Get()};
            g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
            g_ctx->IASetIndexBuffer(d.sm->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            CB cb{}; cb.exposure = g_light_intensity;
            cb.model = d.modelMat;
            cb.mvp = d.mvp;
            cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
            cb.hasSkin = 0.0f;
            cb.matKind = mat ? static_cast<float>(mat->kind) : 0.0f;
            cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
            cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
            cb.isEffect = (mat && mat->isEffect) ? 1.0f : 0.0f;
            cb.cutout = (mat && mat->cutout) ? 1.0f : 0.0f;
            cb.tintR = mat ? mat->tint[0] : 1.0f; cb.tintG = mat ? mat->tint[1] : 1.0f;
            cb.tintB = mat ? mat->tint[2] : 1.0f; cb.tintA = mat ? mat->tint[3] : 1.0f;
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof cb);
                g_ctx->Unmap(g_cb.Get(), 0);
            }
            ID3D11ShaderResourceView* srvs[2] = {mat ? mat->srv.Get() : nullptr, mat ? mat->srvNormal.Get() : nullptr};
            g_ctx->PSSetShaderResources(0, 2, srvs);
            // Back facets first, then front, so translucent props (glass/water) read
            // as front-over-back instead of a merged blob (see the single-model path).
            g_ctx->RSSetState(g_rsCullFront.Get());
            g_ctx->DrawIndexed(d.sub->indexCount, d.sub->indexStart, 0);
            g_ctx->RSSetState(g_rsCullBack.Get());
            g_ctx->DrawIndexed(d.sub->indexCount, d.sub->indexStart, 0);
        }
        g_ctx->RSSetState(g_rsSolid.Get());
    }
}


void render_scene_game() {
    float camDist = g_scene_radius * g_dist;
    Vec3 eye{g_scene_center.x, g_scene_center.y, g_scene_center.z - camDist};
    Mat4 view = lookAt(eye, g_scene_center, {0, 1, 0});
    Mat4 proj = perspective(0.9f, static_cast<float>(g_w) / std::max(1, g_h),
                            g_scene_radius * 0.005f, g_scene_radius * 80.0f);
    Mat4 sceneRot = mul(translate({-g_scene_center.x, -g_scene_center.y, -g_scene_center.z}),
                        mul(g_rot, translate(g_scene_center)));
    Mat4 VP = mul(view, proj);
    Vec3 L = norm({-0.4f, -0.8f, -0.4f});

    // View-level game uniforms (World / WVP / WorldView are set per instance).
    auto pM = [&](const char* n, const Mat4& m) {
        Mat4 t{};
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t.m[i * 4 + j] = m.m[j * 4 + i];
        g_game_vals[n] = std::vector<float>(t.m, t.m + 16);
    };
    pM("ViewProjection", VP);
    g_game_vals["CameraPosition"] = std::vector<float>{eye.x, eye.y, eye.z, 1};
    // (1/w, 1/h, w, h) -- reciprocals first; see the note in model_renderer.cpp.
    // Deferred materials multiply screen position by .xy to index the light buffer.
    g_game_vals["ScreenDims"] =
        std::vector<float>{1.0f / std::max(1, g_w), 1.0f / std::max(1, g_h), (float)g_w, (float)g_h};
    double tsec = g_qpc_freq.QuadPart ? double(now_qpc() - g_qpc_start.QuadPart) / g_qpc_freq.QuadPart : 0.0;
    float tf = static_cast<float>(tsec);
    g_game_vals["Time"] = std::vector<float>{tf, tf, tf, tf};

    if (g_lightprepass_on) render_scene_light_prepass(sceneRot, VP);

    bool useHDR = g_post_ready && g_sceneRTV && g_sceneSRV;
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    const float bf[4] = {0, 0, 0, 0};
    const float sclr[4] = {0.10f, 0.11f, 0.13f, 1.0f};
    ID3D11RenderTargetView* tgt = useHDR ? g_sceneRTV.Get() : g_rtv.Get();
    g_ctx->ClearRenderTargetView(tgt, sclr);
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* rt[] = {tgt};
    g_ctx->OMSetRenderTargets(1, rt, g_dsv.Get());
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rsSolid.Get());

    UINT stride = sizeof(GVertex), off = 0;
    // Fallback exposure compensation: the reconstruction shader is already lit, so
    // pre-divide by the relight exposure to keep fallback models from blowing out.
    float reconExp = g_light_intensity / (useHDR ? std::max(0.2f, g_post_exp) : 1.0f);

    // Push this instance's matrices into every game-material CB of the model.
    auto setInstance = [&](SceneModelGPU& sm, const Mat4& modelMat, const Mat4& mvp) {
        pM("World", modelMat);
        pM("WorldViewProjection", mvp);
        pM("WorldView", mul(modelMat, view));
        for (auto& g : sm.gameMats) {
            if (!g.ok) continue;
            game_update_cb(g.vcb.Get(), g.vsU, g.vsCB, g_game_vals, g.vsConsts);
            game_update_cb(g.pcb.Get(), g.psU, g.psCB, g_game_vals, g.psConsts);
        }
    };
    auto drawSubGame = [&](SceneModelGPU& sm, const SubMesh& s) {
        if (s.matIndex >= sm.gameMats.size()) return;
        GameMatGPU& g = sm.gameMats[s.matIndex];
        if (!g.ok) return;
        g_ctx->IASetInputLayout(g.layout.Get());
        g_ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g.ps.Get(), nullptr, 0);
        ID3D11Buffer* vcb[] = {g.vcb.Get()};
        ID3D11Buffer* pcb[] = {g.pcb.Get()};
        g_ctx->VSSetConstantBuffers(0, 1, vcb);
        g_ctx->PSSetConstantBuffers(0, 1, pcb);
        ID3D11ShaderResourceView* srvs[16];
        for (int i = 0; i < 16; i++) srvs[i] = g.srv[i].Get();
        if (g_lightprepass_on && g_lightSRV && g.lightSlots)
            for (int i = 0; i < 16; i++)
                if (g.lightSlots & (1u << i)) srvs[i] = g_lightSRV.Get();
        g_ctx->PSSetShaderResources(0, 16, srvs);
        ID3D11Buffer* vbs[] = {sm.vb.Get()};
        g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        g_ctx->IASetIndexBuffer(sm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
    };
    // Reconstruction fallback for a model with no game shaders (uses g_vs/g_ps/g_cb).
    auto drawSubRecon = [&](SceneModelGPU& sm, const SubMesh& s, const Mat4& modelMat, const Mat4& mvp) {
        const MaterialGPU* mat = (s.matIndex < sm.mats.size()) ? &sm.mats[s.matIndex] : nullptr;
        g_ctx->IASetInputLayout(g_il.Get());
        g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {g_cb.Get()};
        g_ctx->VSSetConstantBuffers(0, 1, cbs);
        g_ctx->PSSetConstantBuffers(0, 1, cbs);
        CB cb{}; cb.exposure = reconExp;
        cb.model = modelMat; cb.mvp = mvp;
        cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
        cb.hasSkin = 0.0f;
        cb.matKind = mat ? static_cast<float>(mat->kind) : 0.0f;
        cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
        cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
        cb.isEffect = (mat && mat->isEffect) ? 1.0f : 0.0f;
        cb.cutout = (mat && mat->cutout) ? 1.0f : 0.0f;
        cb.tintR = mat ? mat->tint[0] : 1.0f; cb.tintG = mat ? mat->tint[1] : 1.0f;
        cb.tintB = mat ? mat->tint[2] : 1.0f; cb.tintA = mat ? mat->tint[3] : 1.0f;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &cb, sizeof cb);
            g_ctx->Unmap(g_cb.Get(), 0);
        }
        ID3D11ShaderResourceView* srvs[2] = {mat ? mat->srv.Get() : nullptr, mat ? mat->srvNormal.Get() : nullptr};
        g_ctx->PSSetShaderResources(0, 2, srvs);
        ID3D11Buffer* vbs[] = {sm.vb.Get()};
        g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        g_ctx->IASetIndexBuffer(sm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
    };

    ID3D11SamplerState* samps16[16];
    for (int i = 0; i < 16; i++) samps16[i] = g_samp.Get();
    g_ctx->PSSetSamplers(0, 16, samps16);

    auto matIsBlend = [&](const SceneModelGPU& sm, const SubMesh& s) {
        if (sm.gameOk && s.matIndex < sm.gameMats.size() && sm.gameMats[s.matIndex].ok)
            return sm.gameMats[s.matIndex].blend != nullptr;
        return (s.matIndex < sm.mats.size()) ? (sm.mats[s.matIndex].blend != nullptr) : false;
    };

    // --- opaque pass ---
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    for (const auto& in : g_scene_insts) {
        if (!g_layer_visible[in.layer]) continue;
        SceneModelGPU& sm = g_scene_models[in.model];
        if (!sm.ok) continue;
        Mat4 modelMat = mul(in.world, sceneRot);
        Mat4 mvp = mul(in.world, VP);
        if (sm.gameOk) setInstance(sm, modelMat, mvp);
        for (const auto& s : sm.subs) {
            if (matIsBlend(sm, s)) continue;
            if (sm.gameOk && s.matIndex < sm.gameMats.size() && sm.gameMats[s.matIndex].ok)
                drawSubGame(sm, s);
            else
                drawSubRecon(sm, s, modelMat, mvp);
        }
    }

    // --- translucent pass: gather across all instances, sort back-to-front ---
    struct TItem { SceneModelGPU* sm; const SubMesh* sub; Mat4 modelMat, mvp; float depth; };
    std::vector<TItem> titems;
    for (const auto& in : g_scene_insts) {
        if (!g_layer_visible[in.layer]) continue;
        SceneModelGPU& sm = g_scene_models[in.model];
        if (!sm.ok) continue;
        Mat4 modelMat = mul(in.world, sceneRot);
        Mat4 mvp = mul(in.world, VP);
        for (const auto& s : sm.subs) {
            if (!matIsBlend(sm, s)) continue;
            Vec3 wp = transformPoint({s.center[0], s.center[1], s.center[2]}, modelMat);
            Vec3 d = sub(wp, eye);
            titems.push_back({&sm, &s, modelMat, mvp, dot(d, d)});
        }
    }
    if (!titems.empty()) {
        std::sort(titems.begin(), titems.end(),
                  [](const TItem& a, const TItem& b) { return a.depth > b.depth; });
        g_ctx->OMSetDepthStencilState(g_dssNoWrite.Get(), 0);
        for (const auto& t : titems) {
            SceneModelGPU& sm = *t.sm;
            bool useG = sm.gameOk && t.sub->matIndex < sm.gameMats.size() && sm.gameMats[t.sub->matIndex].ok;
            if (useG) {
                const GameMatGPU& gm = sm.gameMats[t.sub->matIndex];
                // shaderPassFlags 0x40 = depth write off; a blended cutout keeps it on.
                g_ctx->OMSetDepthStencilState(gm.depthWrite ? g_dss.Get() : g_dssNoWrite.Get(), 0);
                ID3D11BlendState* bl = gm.blend.Get();
                g_ctx->OMSetBlendState(bl ? bl : g_blendAlpha.Get(), bf, 0xffffffff);
                setInstance(sm, t.modelMat, t.mvp);
                drawSubGame(sm, *t.sub);
            } else {
                g_ctx->OMSetDepthStencilState(g_dssNoWrite.Get(), 0);
                const MaterialGPU* mat = (t.sub->matIndex < sm.mats.size()) ? &sm.mats[t.sub->matIndex] : nullptr;
                g_ctx->OMSetBlendState(mat && mat->blend ? mat->blend.Get() : g_blendAlpha.Get(), bf, 0xffffffff);
                drawSubRecon(sm, *t.sub, t.modelMat, t.mvp);
            }
        }
        g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    }

    if (useHDR) post_process_relight();
}

// Picks the map instance under a surface pixel (screen-space: nearest camera-facing
// instance whose projected bounding circle contains the cursor). Returns the scene
// MODEL index (into g_scene_models) or -1. Same camera as render_scene.
int scene_pick(int sx, int sy) {
    if (!g_scene_mode || g_scene_insts.empty()) return -1;
    float camDist = g_scene_radius * g_dist;
    Vec3 eye{g_scene_center.x, g_scene_center.y, g_scene_center.z - camDist};
    Mat4 view = lookAt(eye, g_scene_center, {0, 1, 0});
    Mat4 proj = perspective(0.9f, static_cast<float>(g_w) / std::max(1, g_h),
                            g_scene_radius * 0.005f, g_scene_radius * 80.0f);
    Mat4 sceneRot = mul(translate({-g_scene_center.x, -g_scene_center.y, -g_scene_center.z}),
                        mul(g_rot, translate(g_scene_center)));
    Mat4 viewProj = mul(view, proj);
    auto proj_pt = [&](const Mat4& M, Vec3 p, float& ox, float& oy, float& ow) -> bool {
        float x = p.x * M.m[0] + p.y * M.m[4] + p.z * M.m[8] + M.m[12];
        float y = p.x * M.m[1] + p.y * M.m[5] + p.z * M.m[9] + M.m[13];
        float w = p.x * M.m[3] + p.y * M.m[7] + p.z * M.m[11] + M.m[15];
        if (w <= 1e-5f) return false;
        ox = (x / w * 0.5f + 0.5f) * g_w;
        oy = (1.0f - (y / w * 0.5f + 0.5f)) * g_h;
        ow = w;
        return true;
    };
    int best = -1; float bestDepth = 1e30f;
    for (const auto& in : g_scene_insts) {
        if (!g_layer_visible[in.layer]) continue;
        const SceneModelGPU& sm = g_scene_models[in.model];
        if (!sm.ok) continue;
        Mat4 M = mul(mul(in.world, sceneRot), viewProj);
        Vec3 c{sm.center[0], sm.center[1], sm.center[2]};
        float cx, cy, cw;
        if (!proj_pt(M, c, cx, cy, cw)) continue;
        float sr = 4.0f;
        Vec3 axes[3] = {{sm.radius, 0, 0}, {0, sm.radius, 0}, {0, 0, sm.radius}};
        for (auto& a : axes) {
            float ox, oy, ow;
            if (proj_pt(M, {c.x + a.x, c.y + a.y, c.z + a.z}, ox, oy, ow))
                sr = std::max(sr, std::hypot(ox - cx, oy - cy));
        }
        float dd = std::hypot(static_cast<float>(sx) - cx, static_cast<float>(sy) - cy);
        if (dd <= sr && cw < bestDepth) { bestDepth = cw; best = in.model; }
    }
    return best;
}

// Draws the picked map model in a small inset panel in the bottom-right corner of
// the scene surface (a dark backing quad via the gizmo color pipeline, then the
// model framed on its own bounds with the reconstruction shader). The scene color
// is already in the backbuffer; depth is re-cleared for the inset.
void render_focus_inset() {
    if (!g_show_focus || g_focus_model < 0 || g_focus_model >= static_cast<int>(g_scene_models.size())) return;
    SceneModelGPU& sm = g_scene_models[g_focus_model];
    if (!sm.ok || !g_gizVs || !g_gizIl) return;

    int iw = std::clamp(static_cast<int>(g_w * 0.30f), 170, 460);
    int ih = std::clamp(static_cast<int>(g_h * 0.34f), 150, 400);
    if (iw > g_w) iw = g_w; if (ih > g_h) ih = g_h;
    int margin = 10;
    int ix = std::max(0, g_w - iw - margin), iy = std::max(0, g_h - ih - margin);

    ID3D11RenderTargetView* rt[] = {g_rtv.Get()};
    g_ctx->OMSetRenderTargets(1, rt, g_dsv.Get());
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    const float bf[4] = {0, 0, 0, 0};
    D3D11_VIEWPORT ivp{static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(iw), static_cast<float>(ih), 0, 1};
    g_ctx->RSSetViewports(1, &ivp);

    // 1) dark backing panel (NDC quad, uMVP = identity) via the gizmo color shader.
    {
        D3D11_MAPPED_SUBRESOURCE cm;
        if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
            CB cb{}; cb.mvp = identity();
            std::memcpy(cm.pData, &cb, sizeof cb);
            g_ctx->Unmap(g_cb.Get(), 0);
        }
        const float pr = 0.06f, pg = 0.07f, pb = 0.09f, pa = 0.92f;
        auto V = [&](float x, float y) { return ColVtx{x, y, 0.5f, pr, pg, pb, pa}; };
        ColVtx q[6] = {V(-1, -1), V(-1, 1), V(1, 1), V(-1, -1), V(1, 1), V(1, -1)};
        if (g_giz_vert_cap < 6 || !g_gizVb) {
            g_giz_vert_cap = 256;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = g_giz_vert_cap * sizeof(ColVtx);
            bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g_gizVb.Reset();
            if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_gizVb))) return;
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(g_gizVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, q, sizeof q);
            g_ctx->Unmap(g_gizVb.Get(), 0);
        }
        g_ctx->OMSetBlendState(g_blendAlpha.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
        g_ctx->RSSetState(g_rsSolid.Get());
        UINT stride = sizeof(ColVtx), off = 0;
        ID3D11Buffer* vbs[] = {g_gizVb.Get()};
        g_ctx->IASetInputLayout(g_gizIl.Get());
        g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_gizVs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_gizPs.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {g_cb.Get()};
        g_ctx->VSSetConstantBuffers(0, 1, cbs);
        g_ctx->Draw(6, 0);
    }

    // 2) the picked model, framed on its own bounds, sharing the scene orbit.
    float rad = sm.radius > 0 ? sm.radius : 1.0f;
    Vec3 c{sm.center[0], sm.center[1], sm.center[2]};
    float camDist = rad * 2.2f;
    Vec3 eye{c.x, c.y, c.z - camDist};
    Mat4 view = lookAt(eye, c, {0, 1, 0});
    Mat4 proj = perspective(0.9f, static_cast<float>(iw) / std::max(1, ih), rad * 0.02f, rad * 40.0f);
    Mat4 modelMat = mul(translate({-c.x, -c.y, -c.z}), mul(g_rot, translate(c)));
    Mat4 mvp = mul(modelMat, mul(view, proj));
    Vec3 L = norm({-0.4f, -0.8f, -0.4f});

    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    g_ctx->RSSetState(g_rsSolid.Get());
    g_ctx->IASetInputLayout(g_il.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    ID3D11SamplerState* samps[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, samps);
    UINT stride = sizeof(GVertex), off = 0;
    ID3D11Buffer* vbs[] = {sm.vb.Get()};
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(sm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    for (int pass = 0; pass < 2; ++pass) {
        bool effect = (pass == 1);
        g_ctx->OMSetDepthStencilState(effect ? g_dssNoWrite.Get() : g_dss.Get(), 0);
        for (const auto& s : sm.subs) {
            const MaterialGPU* mat = (s.matIndex < sm.mats.size()) ? &sm.mats[s.matIndex] : nullptr;
            bool isE = mat && mat->isEffect;
            if (isE != effect) continue;
            g_ctx->OMSetBlendState(effect ? (mat && mat->blend ? mat->blend.Get() : g_blendAlpha.Get())
                                          : g_blendOpaque.Get(),
                                   bf, 0xffffffff);
            CB cb{}; cb.exposure = g_light_intensity; cb.model = modelMat; cb.mvp = mvp;
            cb.lx = L.x; cb.ly = L.y; cb.lz = L.z; cb.hasSkin = 0.0f;
            cb.matKind = mat ? static_cast<float>(mat->kind) : 0.0f;
            cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
            cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
            cb.isEffect = isE ? 1.0f : 0.0f;
            cb.cutout = (mat && mat->cutout) ? 1.0f : 0.0f;
            cb.tintR = mat ? mat->tint[0] : 1.0f; cb.tintG = mat ? mat->tint[1] : 1.0f;
            cb.tintB = mat ? mat->tint[2] : 1.0f; cb.tintA = mat ? mat->tint[3] : 1.0f;
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof cb);
                g_ctx->Unmap(g_cb.Get(), 0);
            }
            ID3D11ShaderResourceView* srvs[2] = {mat ? mat->srv.Get() : nullptr, mat ? mat->srvNormal.Get() : nullptr};
            g_ctx->PSSetShaderResources(0, 2, srvs);
            if (effect) {
                // Back facets first, then front, so the inset's translucent parts
                // layer front-over-back like the main surface.
                g_ctx->RSSetState(g_rsCullFront.Get());
                g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
                g_ctx->RSSetState(g_rsCullBack.Get());
                g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
            } else {
                g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
            }
        }
        if (effect) g_ctx->RSSetState(g_rsSolid.Get());
    }

    // restore the full-surface viewport for the next frame.
    D3D11_VIEWPORT fvp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    g_ctx->RSSetViewports(1, &fvp);
}


} // namespace castlemist::render
