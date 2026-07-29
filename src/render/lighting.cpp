/// @file
/// @brief The deferred light pre-pass, tone mapping and the light-rig controls.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace castlemist::render {

void set_lightprepass(bool on) { g_lightprepass_on = on; }
bool lightprepass() { return g_lightprepass_on; }

void set_light_intensity(float v) { g_light_intensity = v < 0.0f ? 0.0f : (v > 4.0f ? 4.0f : v); }
float light_intensity() { return g_light_intensity; }

void apply_env_uniforms(); // fwd (updates the game SH sun from the headlight)
void set_light_follow(bool on) {
    g_light_follow = on;
    if (g_game_uniforms_ready) apply_env_uniforms();
}
bool light_follow() { return g_light_follow; }
void set_light_angle(float v) {
    g_light_angle = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    if (g_game_uniforms_ready) apply_env_uniforms();
}
float light_angle() { return g_light_angle; }


void post_process_relight() {
    if (!g_post_ready) return;
    ID3D11RenderTargetView* rt[] = {g_rtv.Get()};
    g_ctx->OMSetRenderTargets(1, rt, nullptr);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rsSolid.Get());
    g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
    const float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_postCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float p[4] = {g_post_exp, g_post_amb, 1.0f / g_post_gamma, 0.0f};
        std::memcpy(m.pData, p, sizeof p);
        g_ctx->Unmap(g_postCB.Get(), 0);
    }
    g_ctx->IASetInputLayout(nullptr);
    ID3D11Buffer* nvb[1] = {nullptr}; UINT zs = 0, zo = 0;
    g_ctx->IASetVertexBuffers(0, 1, nvb, &zs, &zo);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_postVS.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_postPS.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_postCB.Get()};
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srv[] = {g_sceneSRV.Get()};
    g_ctx->PSSetShaderResources(0, 1, srv);
    ID3D11SamplerState* smp[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, smp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nil[] = {nullptr};
    g_ctx->PSSetShaderResources(0, 1, nil); // avoid RTV/SRV bind hazard next frame
}

// Light pre-pass (Option 2): rebuilds the screen-space light-accumulation buffer
// the deferred game materials sample as gSs14. Pass 1 renders world normals into
// g_nrmTex (same camera as render_game, so screen positions align pixel-for-pixel);
// pass 2 is a fullscreen shader that turns those normals into ambient + sun*N.L and
// writes g_lightTex. render_game then binds g_lightSRV at each material's light slot.
// `vbUse` is the (possibly CPU-skinned) vertex buffer, shared with render_game so
// the normals match the drawn geometry exactly.
void render_light_prepass(ID3D11Buffer* vbUse) {
    if (!g_nrmRTV || !g_lightRTV || !g_nrmVs || !g_nrmPs || !g_lightVs || !g_lightPs || !g_ib) return;

    // Same camera as render_game -- literally the same call, so the normal buffer
    // lines up with the material pass pixel for pixel.
    Vec3 eye; Mat4 view, proj;
    main_camera(eye, view, proj);
    // Include the editable object (gizmo) transform so the normal/light buffer
    // tracks the mesh when it's moved/rotated/scaled -- must match render_game.
    Mat4 model = gizmo_identity() ? giz_view_transform() : mul(object_matrix(), giz_view_transform());
    Mat4 mvp = mul(model, mul(view, proj));

    // Fill CB (uMVP + uModel + uHasTex are read by the normal shader; hasTex is
    // re-written per submesh below so the alpha cutout can be enabled per material).
    CB cb{};
    cb.mvp = mvp;
    cb.model = model;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_cb.Get(), 0);
    }

    // Refill the SH lighting cbuffer from the shared rig (g_game_vals) so the light
    // buffer PSLight uses the exact same shRed/shGreen/shBlue/shSun/shSunColor the
    // forward-lit materials bind -- one consistent, game-accurate SH lighting model.
    if (g_lightCB) {
        float sh[24] = {0};
        auto put = [&](int i, const char* n, float a, float b, float c, float d) {
            float v[4] = {a, b, c, d};
            auto it = g_game_vals.find(n);
            if (it != g_game_vals.end())
                for (size_t k = 0; k < 4 && k < it->second.size(); ++k) v[k] = it->second[k];
            sh[i * 4] = v[0]; sh[i * 4 + 1] = v[1]; sh[i * 4 + 2] = v[2]; sh[i * 4 + 3] = v[3];
        };
        put(0, "shRed", 0, 0.10f, 0, 0.32f);
        put(1, "shGreen", 0, 0.11f, 0, 0.35f);
        put(2, "shBlue", 0, 0.13f, 0, 0.40f);
        put(3, "shSun", 0.45f, 0.80f, 0.40f, 0);
        put(4, "shSunColor", 0.92f, 0.86f, 0.79f, 1);
        sh[20] = kLightBufferEncode;  // encParams.x -- pairs with the LightBuffer decode
        D3D11_MAPPED_SUBRESOURCE lms;
        if (SUCCEEDED(g_ctx->Map(g_lightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &lms))) {
            std::memcpy(lms.pData, sh, sizeof sh);
            g_ctx->Unmap(g_lightCB.Get(), 0);
        }
    }

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    const float bf[4] = {0, 0, 0, 0};

    // --- pass 1: world-normal G-buffer (own depth via g_dsv; re-cleared before the
    //     material pass). Background clears to N=0 (0.5,0.5,0.5) -> ambient-only. ---
    const float nclr[4] = {0.5f, 0.5f, 0.5f, 0.0f};
    g_ctx->ClearRenderTargetView(g_nrmRTV.Get(), nclr);
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* nrt[] = {g_nrmRTV.Get()};
    g_ctx->OMSetRenderTargets(1, nrt, g_dsv.Get());
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rsSolid.Get());
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    UINT stride = sizeof(GVertex), off = 0;
    ID3D11Buffer* vbs[] = {vbUse};
    g_ctx->IASetInputLayout(g_il.Get()); // GVertex layout; VSNrm reads a subset (POSITION+NORMAL)
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_nrmVs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_nrmPs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* nsamp[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, nsamp);
    // Opaque submeshes only: translucent/coplanar planes (billboards, glass) must not
    // write the normal/depth buffer -- they SAMPLE the light buffer of the opaque
    // surface behind them. Writing them here just corrupts the light with z-fight.
    for (const auto& s : g_subs) {
        if (s.matIndex < g_game_mats.size() && g_game_mats[s.matIndex].ok && g_game_mats[s.matIndex].blend)
            continue;
        // Bind this submesh's albedo so PSNrm can run the game's alpha cutout, which
        // the real prepass also does. srv[0] is the AMAT's ss0 = albedo, the same
        // texture whose alpha the game's prepass PS tests.
        ID3D11ShaderResourceView* dif = nullptr;
        bool cut = false;
        if (s.matIndex < g_game_mats.size() && g_game_mats[s.matIndex].ok) {
            dif = g_game_mats[s.matIndex].srv[0].Get();
            cut = g_game_mats[s.matIndex].cutout;
        }
        cb.hasTex = (dif && cut) ? 1.0f : 0.0f;
        cb.cutout = cut ? 1.0f : 0.0f;
        if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &cb, sizeof cb);
            g_ctx->Unmap(g_cb.Get(), 0);
        }
        ID3D11ShaderResourceView* dsrv[] = {dif};
        g_ctx->PSSetShaderResources(0, 1, dsrv);
        g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
    }
    ID3D11ShaderResourceView* nodif[] = {nullptr};
    g_ctx->PSSetShaderResources(0, 1, nodif); // t0 becomes an RTV again in pass 2

    // --- pass 2: fullscreen lighting -> g_lightTex ---
    ID3D11RenderTargetView* lrt[] = {g_lightRTV.Get()};
    g_ctx->OMSetRenderTargets(1, lrt, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->IASetInputLayout(nullptr);
    ID3D11Buffer* nvb[1] = {nullptr}; UINT zs = 0, zo = 0;
    g_ctx->IASetVertexBuffers(0, 1, nvb, &zs, &zo);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_lightVs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_lightPs.Get(), nullptr, 0);
    ID3D11Buffer* lcbs[] = {g_lightCB.Get()};
    g_ctx->PSSetConstantBuffers(0, 1, lcbs);
    ID3D11ShaderResourceView* nsrv[] = {g_nrmSRV.Get()};
    g_ctx->PSSetShaderResources(0, 1, nsrv);
    ID3D11SamplerState* smp[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, smp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nil[] = {nullptr};
    g_ctx->PSSetShaderResources(0, 1, nil); // release g_nrmSRV before it's an RTV again
    debug_dump_light_buffer();
}

// GW2_LIGHTDBG=1: print percentiles of the light-accumulation buffer to stderr.
// The light buffer is the one value that decides how bright every deferred material
// comes out, and it is invisible from a screenshot, so being able to compare it
// against the game's own measured distribution (p1 0.32 / p50 0.98 / max 1.28,
// taken from sample1 RT 6826) is what turns "looks too dark" into a number.
void debug_dump_light_buffer() {
    if (!std::getenv("GW2_LIGHTDBG") || !g_lightTex || !g_dev || !g_ctx) return;
    D3D11_TEXTURE2D_DESC td{};
    g_lightTex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg))) return;
    g_ctx->CopyResource(stg.Get(), g_lightTex.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    std::vector<float> lum;
    lum.reserve(static_cast<size_t>(td.Width) * td.Height / 16);
    const bool isHalf = (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (UINT y = 0; y < td.Height; y += 4) {
        const uint8_t* row = static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch;
        for (UINT x = 0; x < td.Width; x += 4) {
            float r, g, b;
            if (isHalf) {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
                auto h2f = [](uint16_t h) {
                    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
                    if (e == 0) return (s ? -1.f : 1.f) * std::ldexp(static_cast<float>(m), -24);
                    if (e == 31) return (s ? -1.f : 1.f) * 1e30f;
                    return (s ? -1.f : 1.f) * std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25);
                };
                r = h2f(p[0]); g = h2f(p[1]); b = h2f(p[2]);
            } else {
                const uint8_t* p = row + static_cast<size_t>(x) * 4;
                r = p[0] / 255.f; g = p[1] / 255.f; b = p[2] / 255.f;
            }
            lum.push_back(0.299f * r + 0.587f * g + 0.114f * b);
        }
    }
    g_ctx->Unmap(stg.Get(), 0);
    if (lum.empty()) return;
    std::sort(lum.begin(), lum.end());
    auto q = [&](double p) { return lum[static_cast<size_t>(p * (lum.size() - 1))]; };
    std::fprintf(stderr,
                 "[lightbuf] n=%zu fmt=%s p1=%.3f p10=%.3f p25=%.3f p50=%.3f p90=%.3f max=%.3f"
                 "   (game: p1=0.32 p10=0.49 p25=0.78 p50=0.98 p90=1.06 max=1.28)\n",
                 lum.size(), isHalf ? "F16" : "U8",
                 q(0.01), q(0.10), q(0.25), q(0.50), q(0.90), lum.back());
}

// Draws the single model with each submesh's own GAME bgfx VS+PS (opaque pass
// then translucent back-to-front). Reuses g_vb/g_ib (GVertex). Camera matches
// the reconstruction path so the two modes frame identically. Assumes the RTV /
// depth / viewport are already bound by render(). `vbUse` is the (possibly
// CPU-skinned) vertex buffer chosen by the caller.

void render_scene_light_prepass(const Mat4& sceneRot, const Mat4& VP) {
    if (!g_nrmRTV || !g_lightRTV || !g_nrmVs || !g_nrmPs || !g_lightVs || !g_lightPs) return;

    // SH lighting cbuffer (identical rig to the single-model prepass).
    if (g_lightCB) {
        float sh[24] = {0};
        auto put = [&](int i, const char* n, float a, float b, float c, float d) {
            float v[4] = {a, b, c, d};
            auto it = g_game_vals.find(n);
            if (it != g_game_vals.end())
                for (size_t k = 0; k < 4 && k < it->second.size(); ++k) v[k] = it->second[k];
            sh[i * 4] = v[0]; sh[i * 4 + 1] = v[1]; sh[i * 4 + 2] = v[2]; sh[i * 4 + 3] = v[3];
        };
        put(0, "shRed", 0, 0.10f, 0, 0.32f);
        put(1, "shGreen", 0, 0.11f, 0, 0.35f);
        put(2, "shBlue", 0, 0.13f, 0, 0.40f);
        put(3, "shSun", 0.45f, 0.80f, 0.40f, 0);
        put(4, "shSunColor", 0.92f, 0.86f, 0.79f, 1);
        sh[20] = kLightBufferEncode;  // encParams.x -- pairs with the LightBuffer decode
        D3D11_MAPPED_SUBRESOURCE lms;
        if (SUCCEEDED(g_ctx->Map(g_lightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &lms))) {
            std::memcpy(lms.pData, sh, sizeof sh);
            g_ctx->Unmap(g_lightCB.Get(), 0);
        }
    }

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    const float bf[4] = {0, 0, 0, 0};

    // pass 1: world-normal G-buffer over every visible opaque instance.
    const float nclr[4] = {0.5f, 0.5f, 0.5f, 0.0f};
    g_ctx->ClearRenderTargetView(g_nrmRTV.Get(), nclr);
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* nrt[] = {g_nrmRTV.Get()};
    g_ctx->OMSetRenderTargets(1, nrt, g_dsv.Get());
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rsSolid.Get());
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->IASetInputLayout(g_il.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_nrmVs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_nrmPs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);   // PSNrm reads uHasTex for the alpha cutout
    ID3D11SamplerState* nsamp[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, nsamp);
    UINT stride = sizeof(GVertex), off = 0;
    for (const auto& in : g_scene_insts) {
        if (!g_layer_visible[in.layer]) continue;
        const SceneModelGPU& sm = g_scene_models[in.model];
        if (!sm.ok) continue;
        Mat4 modelMat = mul(in.world, sceneRot);
        Mat4 mvp = mul(in.world, VP);
        CB cb{}; cb.mvp = mvp; cb.model = modelMat;
        ID3D11Buffer* vbs[] = {sm.vb.Get()};
        g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        g_ctx->IASetIndexBuffer(sm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        for (const auto& s : sm.subs) {
            // Skip translucent surfaces (they sample the light buffer, not write it).
            bool blendMat = (sm.gameOk && s.matIndex < sm.gameMats.size() && sm.gameMats[s.matIndex].blend) ||
                            (s.matIndex < sm.mats.size() && sm.mats[s.matIndex].blend);
            if (blendMat) continue;
            // Same cutout the game's prepass runs, so masked props punch real holes
            // in the normal buffer instead of lighting a solid quad.
            ID3D11ShaderResourceView* dif = nullptr;
            bool cut = false;
            if (sm.gameOk && s.matIndex < sm.gameMats.size() && sm.gameMats[s.matIndex].ok) {
                dif = sm.gameMats[s.matIndex].srv[0].Get();
                cut = sm.gameMats[s.matIndex].cutout;
            } else if (s.matIndex < sm.mats.size()) {
                dif = sm.mats[s.matIndex].srv.Get();
                cut = sm.mats[s.matIndex].cutout;
            }
            cb.hasTex = (dif && cut) ? 1.0f : 0.0f;
            cb.cutout = cut ? 1.0f : 0.0f;
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof cb);
                g_ctx->Unmap(g_cb.Get(), 0);
            }
            ID3D11ShaderResourceView* dsrv[] = {dif};
            g_ctx->PSSetShaderResources(0, 1, dsrv);
            g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
        }
    }
    ID3D11ShaderResourceView* nodif[] = {nullptr};
    g_ctx->PSSetShaderResources(0, 1, nodif); // t0 becomes an RTV again in pass 2

    // pass 2: fullscreen lighting -> g_lightTex.
    ID3D11RenderTargetView* lrt[] = {g_lightRTV.Get()};
    g_ctx->OMSetRenderTargets(1, lrt, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->IASetInputLayout(nullptr);
    ID3D11Buffer* nvb[1] = {nullptr}; UINT zs = 0, zo = 0;
    g_ctx->IASetVertexBuffers(0, 1, nvb, &zs, &zo);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_lightVs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_lightPs.Get(), nullptr, 0);
    ID3D11Buffer* lcbs[] = {g_lightCB.Get()};
    g_ctx->PSSetConstantBuffers(0, 1, lcbs);
    ID3D11ShaderResourceView* nsrv[] = {g_nrmSRV.Get()};
    g_ctx->PSSetShaderResources(0, 1, nsrv);
    ID3D11SamplerState* smp[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, smp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nil[] = {nullptr};
    g_ctx->PSSetShaderResources(0, 1, nil);
}

// Renders the whole map scene with each model's real GAME (bgfx DXBC) materials
// -- the map counterpart of render_game. Game-capable models draw with their game
// VS+PS; models without game shaders (e.g. some terrain) fall back to the
// reconstruction shader into the same target. Everything renders into the
// offscreen HDR target and is relit to the backbuffer (like the single model).

} // namespace castlemist::render
