/// @file
/// @brief The single-model render entry point and the orbit camera.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>

namespace castlemist::render {

void orbit(float dyaw, float dpitch) {
    // Free trackball: compose the drag as world-space rotations (yaw about Y,
    // pitch about X) applied *after* the current orientation. No clamped axis
    // and no gimbal lock, so the model tumbles freely in every direction.
    Mat4 inc = mul(rotY(dyaw), rotX(dpitch));
    g_rot = mul(g_rot, inc);
}

void zoom(float factor) { g_dist = std::clamp(g_dist * factor, 0.2f, 20.0f); }

void reset_view() {
    g_rot = identity();
    g_dist = 1.7f;
}

// Draws every scene instance at its world transform. Orbit camera around the
// scene center (reusing g_rot/g_dist so the existing drag/zoom controls work).

void render_game(ID3D11Buffer* vbUse) {
    g_ctx->RSSetState(g_rsSolid.Get());

    Vec3 eye; Mat4 view, proj;
    main_camera(eye, view, proj); // shared with the light pre-pass and the grid
    // Include the editable object (gizmo) transform so the game-shader mesh
    // follows the gizmo just like the reconstruction path and skeleton overlay.
    Mat4 model = gizmo_identity() ? giz_view_transform() : mul(object_matrix(), giz_view_transform());
    Mat4 vp = mul(view, proj);
    Mat4 mvp = mul(model, vp);
    Mat4 wv = mul(model, view);

    // bgfx uniforms are column-major -> store the transpose of each row-major matrix.
    auto pM = [&](const char* n, const Mat4& m) {
        Mat4 t{};
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t.m[i * 4 + j] = m.m[j * 4 + i];
        g_game_vals[n] = std::vector<float>(t.m, t.m + 16);
    };
    pM("World", model); pM("ViewProjection", vp); pM("WorldViewProjection", mvp); pM("WorldView", wv);
    // GW2's SKINNED vertex shaders declare `View` where the plain ones declare
    // `World` (same cbuffer offset). This path prefers a variant that does not
    // read `grbones`, so it normally binds the plain one -- but a material whose
    // every variant is skinned falls back to one that does, and then `View` was
    // simply never supplied. Feeding it costs nothing and removes the hole.
    pM("View", view);
    g_game_vals["CameraPosition"] = std::vector<float>{eye.x, eye.y, eye.z, 1};
    // ScreenDims = (1/w, 1/h, w, h) -- RECIPROCALS FIRST. Measured directly in the
    // capture: a 1344x756 frame binds (0.000744, 0.001323, 1344, 756) (sample1 eid
    // 1650, cb0[3]).
    //
    // The order is not cosmetic. Every DEFERRED material -- which is what GW2 uses
    // for ordinary world geometry -- fetches its lighting with
    //     uv = (screenPos.xy + TexelOffset.x) * ScreenDims.xy
    //     light = tex2D(gSs14, uv)
    // so .xy MUST be the reciprocals that turn a pixel coordinate into a [0,1] UV.
    // With (w, h) there instead, uv came out around 1344x the correct value, so every
    // pixel of every deferred material sampled a clamped corner texel of the light
    // buffer -- one constant value for the whole model. That is what made these
    // materials render dark and flat, and it is the reason a 1.9x post exposure used
    // to be needed to make anything visible at all.
    g_game_vals["ScreenDims"] =
        std::vector<float>{1.0f / std::max(1, g_w), 1.0f / std::max(1, g_h), (float)g_w, (float)g_h};
    // Wall-clock time for animated material effects (scrolling lava/energy, etc.).
    double tsec = g_qpc_freq.QuadPart ? double(now_qpc() - g_qpc_start.QuadPart) / g_qpc_freq.QuadPart : 0.0;
    float tf = static_cast<float>(tsec);
    g_game_vals["Time"] = std::vector<float>{tf, tf, tf, tf};

    for (auto& g : g_game_mats) {
        if (!g.ok) continue;
        game_update_cb(g.vcb.Get(), g.vsU, g.vsCB, g_game_vals, g.vsConsts);
        game_update_cb(g.pcb.Get(), g.psU, g.psCB, g_game_vals, g.psConsts);
    }

    const float bf[4] = {0, 0, 0, 0};

    // Not every material yields a game shader. Its AMAT may be missing, its BGFX
    // chunk may hold nothing but depth-only and prepass effects, or the DXBC may
    // fail to create. Those submeshes used to be skipped outright, so the geometry
    // simply WAS NOT THERE -- a model rendered with holes, or (when every material
    // failed) not at all. Draw them with the reconstruction material instead: a
    // textured approximation is the right answer for "this material has no game
    // shader", an invisible mesh never is.
    {
        std::vector<const SubMesh*> fallback;
        for (const auto& s : g_subs)
            if (s.matIndex >= g_game_mats.size() || !g_game_mats[s.matIndex].ok) fallback.push_back(&s);
        if (!fallback.empty() && g_vs && g_ps && g_il) {
            UINT rstride = sizeof(GVertex), roff = 0;
            ID3D11Buffer* rvbs[] = {vbUse};
            g_ctx->IASetInputLayout(g_il.Get());
            g_ctx->IASetVertexBuffers(0, 1, rvbs, &rstride, &roff);
            g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
            g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
            ID3D11Buffer* rcbs[] = {g_cb.Get()};
            g_ctx->VSSetConstantBuffers(0, 1, rcbs);
            g_ctx->PSSetConstantBuffers(0, 1, rcbs);
            ID3D11SamplerState* rsamp[] = {g_samp.Get()};
            g_ctx->PSSetSamplers(0, 1, rsamp);
            g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
            Vec3 L = recon_light_dir();
            for (const SubMesh* s : fallback) {
                const MaterialGPU* mat = (s->matIndex < g_mats.size()) ? &g_mats[s->matIndex] : nullptr;
                // vbUse is already CPU-skinned for the game VS, so the reconstruction
                // VS must NOT skin it a second time.
                CB cb{};
                cb.exposure = g_light_intensity;
                cb.lightMode = g_light_follow ? 1.0f : 0.0f;
                cb.model = model; cb.mvp = mvp;
                cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
                cb.hasSkin = 0.0f;
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
                bool eff = mat && mat->isEffect;
                g_ctx->OMSetBlendState(eff ? (mat->blend ? mat->blend.Get() : g_blendAlpha.Get())
                                           : g_blendOpaque.Get(), bf, 0xffffffff);
                ID3D11ShaderResourceView* rsrv[2] = {mat ? mat->srv.Get() : nullptr,
                                                     mat ? mat->srvNormal.Get() : nullptr};
                g_ctx->PSSetShaderResources(0, 2, rsrv);
                g_ctx->DrawIndexed(s->indexCount, s->indexStart, 0);
            }
        }
    }

    // The game VS wants pre-skinned vertices (it has no bone inputs). `vbUse` was
    // chosen by render() (CPU-skinned when a rig is posed) and shared with the
    // light pre-pass so both see identical geometry.
    UINT stride = sizeof(GVertex), off = 0;
    ID3D11Buffer* vbs[] = {vbUse};
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11SamplerState* samps[16];
    for (int i = 0; i < 16; i++) samps[i] = g_samp.Get();
    g_ctx->PSSetSamplers(0, 16, samps);

    auto drawSub = [&](const SubMesh& s) {
        if (s.matIndex >= g_game_mats.size()) return;
        GameMatGPU& g = g_game_mats[s.matIndex];
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
        // Bind our reconstructed light buffer at the deferred light-buffer slot(s)
        // so `albedo * light` gets real directional shading instead of a flat grey.
        if (g_lightprepass_on && g_lightSRV && g.lightSlots) {
            for (int i = 0; i < 16; i++)
                if (g.lightSlots & (1u << i)) srvs[i] = g_lightSRV.Get();
        }
        g_ctx->PSSetShaderResources(0, 16, srvs);
        g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
    };

    // Opaque first, each with its own effect's write mask (see make_opaque_blend_state).
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    for (const auto& s : g_subs) {
        if (s.matIndex >= g_game_mats.size()) continue;
        const GameMatGPU& gm = g_game_mats[s.matIndex];
        if (!gm.ok || gm.blend) continue;
        g_ctx->OMSetBlendState(gm.opaqueBlend ? gm.opaqueBlend.Get() : g_blendOpaque.Get(), bf, 0xffffffff);
        drawSub(s);
    }

    // Translucent back-to-front (blend from the AMAT bgfx state), depth read-only.
    std::vector<const SubMesh*> tsubs;
    for (const auto& s : g_subs)
        if (s.matIndex < g_game_mats.size() && g_game_mats[s.matIndex].ok && g_game_mats[s.matIndex].blend)
            tsubs.push_back(&s);
    if (!tsubs.empty()) {
        auto worldZ = [&](const SubMesh* s) {
            const float* p = s->center;
            float wx = p[0] * model.m[0] + p[1] * model.m[4] + p[2] * model.m[8] + model.m[12];
            float wy = p[0] * model.m[1] + p[1] * model.m[5] + p[2] * model.m[9] + model.m[13];
            float wz = p[0] * model.m[2] + p[1] * model.m[6] + p[2] * model.m[10] + model.m[14];
            float dx = wx - eye.x, dy = wy - eye.y, dz = wz - eye.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::sort(tsubs.begin(), tsubs.end(),
                  [&](const SubMesh* a, const SubMesh* b) { return worldZ(a) > worldZ(b); });
        // Depth write per material, from its effect's shaderPassFlags (0x40 = off).
        // Glass/energy panes (AMAT 57224) clear it; an alpha-blended CUTOUT material
        // like AMAT 511755's eff13 keeps it and must still write depth.
        for (const SubMesh* s : tsubs) {
            const GameMatGPU& gm = g_game_mats[s->matIndex];
            g_ctx->OMSetDepthStencilState(gm.depthWrite ? g_dss.Get() : g_dssNoWrite.Get(), 0);
            g_ctx->OMSetBlendState(gm.blend.Get(), bf, 0xffffffff);
            drawSub(*s);
        }
        g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    }
}

// Scene-wide world-normal light pre-pass for the map GameShader mode. Same idea
// as render_light_prepass but iterating every visible opaque instance so the
// screen-space light buffer (gSs14) covers the whole map. Fills g_lightTex.

void render() {
    if (!g_ctx || !g_rtv) return;

    // Advance playback (real time via QPC, so it's frame-rate independent) and
    // re-pose the rig. A WM_TIMER in the host keeps invalidating us while playing.
    bool clipActive = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
    if (g_anim_playing && clipActive) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt = g_qpc_freq.QuadPart ? double(now.QuadPart - g_qpc_last.QuadPart) / g_qpc_freq.QuadPart : 0.0;
        g_qpc_last = now;
        // Real clips wrap at their duration.
        float dur = g_clips[g_clip_index].duration;
        if (dur > 1e-4f) {
            g_anim_time += static_cast<float>(dt);
            if (g_anim_time > dur) g_anim_time = std::fmod(g_anim_time, dur);
        }
        rebuild_skel_lines();
    }

    // Advance the particle simulation every frame (independent of anim playback)
    // so baked fire/glow effects animate even on an idle model. dt from a
    // dedicated QPC anchor so it isn't disturbed by anim scrubbing.
    {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float fdt = 0;
        if (g_fx_qpc_init && g_qpc_freq.QuadPart)
            fdt = static_cast<float>(double(now.QuadPart - g_fx_qpc_last.QuadPart) / g_qpc_freq.QuadPart);
        g_fx_qpc_last = now; g_fx_qpc_init = true;
        update_particles(fdt);
    }

    const float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
    g_ctx->ClearRenderTargetView(g_rtv.Get(), clear);
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* rtvs[] = {g_rtv.Get()};
    g_ctx->OMSetRenderTargets(1, rtvs, g_dsv.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    g_ctx->RSSetViewports(1, &vp);

    if (g_scene_mode) {
        // GameShader map mode: draw with the real bgfx materials (+ relight) when
        // any scene model has game shaders built; otherwise the reconstruction path.
        if (g_mode == RenderMode::GameShader && scene_game_ready())
            render_scene_game();
        else
            render_scene();
        render_focus_inset(); // small preview of the picked prop (no-op if none)
        g_swap->Present(1, 0);
        return;
    }

    if (!g_has_model || !g_vb || !g_ib) {
        g_swap->Present(1, 0);
        return;
    }

    // GameShader mode draws each submesh with its own game bgfx VS+PS; the
    // reconstruction (Full/Plain/Wireframe) path is skipped. Falls back to the
    // reconstruction draw if no material had usable game shaders.
    bool useGame = (g_mode == RenderMode::GameShader) && g_game_any_ok;
    bool wire = (g_mode == RenderMode::Wireframe);
    bool textured = (g_mode == RenderMode::Full);
    if (!useGame) g_ctx->RSSetState(wire ? g_rsWire.Get() : g_rsSolid.Get());

    // Fixed camera looking at the model center; all rotation lives in the model
    // matrix (free trackball) so there is no locked axis or pole to stall at.
    // main_camera is shared with render_game / render_light_prepass / the grid, so
    // every pass sharing g_dsv writes comparable depth (see main_camera).
    Vec3 eye; Mat4 view, proj;
    main_camera(eye, view, proj);
    // View transform (centering + orbit); the gizmo/grid draw with just this,
    // the model additionally with its editable object transform on top.
    Mat4 vt = mul(translate({-g_center.x, -g_center.y, -g_center.z}), mul(g_rot, translate(g_center)));
    Mat4 gizMVP = mul(vt, mul(view, proj));
    Mat4 model = gizmo_identity() ? vt : mul(object_matrix(), vt);
    Mat4 mvp = mul(model, mul(view, proj));
    Vec3 L = recon_light_dir();                       // headlight travel dir (or fixed world)
    float lightMode = g_light_follow ? 1.0f : 0.0f;   // 1 => envLight uses the camera headlight
    const float bf[4] = {0, 0, 0, 0};

    if (useGame) {
        // CPU-skin once (the game VS take pre-skinned verts); the same buffer feeds
        // both the light pre-pass and the material draw so their geometry matches.
        bool posed = (g_clip_index >= 0 && g_clip_index < static_cast<int>(g_clips.size()));
        ID3D11Buffer* vbUse = g_vb.Get();
        if (g_skin_ok && g_vb_skinned && posed && cpu_skin_into(g_vb_skinned.Get())) vbUse = g_vb_skinned.Get();

        // Reconstruct the screen-space light-accumulation buffer (Option 2) so the
        // deferred materials get real directional shading. Renders to its own
        // targets; render_game's scene target/depth are (re)bound right after.
        if (g_lightprepass_on) render_light_prepass(vbUse);

        // Game shaders render correct-but-dark (no offline deferred light buffer):
        // draw them into the offscreen HDR target, then relight onto the backbuffer.
        if (g_post_ready && g_sceneRTV && g_sceneSRV) {
            const float sclr[4] = {0.10f, 0.11f, 0.13f, 1.0f};
            g_ctx->ClearRenderTargetView(g_sceneRTV.Get(), sclr);
            if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            ID3D11RenderTargetView* srt[] = {g_sceneRTV.Get()};
            g_ctx->OMSetRenderTargets(1, srt, g_dsv.Get());
            g_ctx->RSSetViewports(1, &vp);
            render_game(vbUse);
            post_process_relight();
        } else {
            // No post target: the prepass left its own RT/depth bound -- restore the
            // backbuffer (and re-clear the depth the prepass overwrote) before drawing.
            const float sclr[4] = {0.10f, 0.11f, 0.13f, 1.0f};
            g_ctx->ClearRenderTargetView(g_rtv.Get(), sclr);
            if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            ID3D11RenderTargetView* brt[] = {g_rtv.Get()};
            g_ctx->OMSetRenderTargets(1, brt, g_dsv.Get());
            g_ctx->RSSetViewports(1, &vp);
            render_game(vbUse);
        }
    } else {
    UINT stride = sizeof(GVertex), off = 0;
    // Live cloth: step the sim + stream deformed verts, and draw those instead of
    // the static rest mesh (falls back to g_vb if the sim isn't running).
    ID3D11Buffer* vbRecon = g_vb.Get();
    // Demo cloth (whole-mesh auto-pin) streams into g_vb_cloth and replaces the
    // render mesh. File-driven cloth instead keeps the render mesh and draws its
    // proxy separately (see render_cloth_proxy), so don't replace the VB here.
    if (g_cloth_enabled && !g_cloth_file_active) { ID3D11Buffer* c = cloth_step_and_upload(); if (c) vbRecon = c; }
    ID3D11Buffer* vbs[] = {vbRecon};
    g_ctx->IASetInputLayout(g_il.Get());
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    update_bone_palette();
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* bsrv[] = {g_bonePaletteSRV.Get()};
    g_ctx->VSSetShaderResources(2, 1, bsrv); // bone palette StructuredBuffer (t2)
    ID3D11SamplerState* samps[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, samps);

    // Full mode does the two-pass opaque/effect split; Plain/Wireframe draw
    // everything once, opaque and untextured. The effect pass is gathered and
    // sorted back-to-front (by distance from the camera) before drawing, and
    // each material draws with its OWN resolved blend state when known --
    // drawing translucent submeshes in raw storage order with one hardcoded
    // blend mode is what made overlapping transparent textures look
    // "berantakan" (out of order).
    struct EffectDraw {
        const SubMesh* sub;
        const MaterialGPU* mat;
        float viewDepthSq;
    };
    int passes = textured ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        bool effectPass = textured && (pass == 1);
        g_ctx->OMSetDepthStencilState(effectPass ? g_dssNoWrite.Get() : g_dss.Get(), 0);
        if (!effectPass) g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);

        std::vector<EffectDraw> effectDraws;
        for (const auto& s : g_subs) {
            // File-driven cloth: hide the static submesh a cloth piece replaces
            // (its animated proxy is drawn by render_cloth_proxy instead).
            if (g_cloth_file_active && s.matIndex < g_cloth_hide_mat.size() && g_cloth_hide_mat[s.matIndex]) continue;
            const MaterialGPU* mat = (s.matIndex < g_mats.size()) ? &g_mats[s.matIndex] : nullptr;
            bool isEffect = mat && mat->isEffect;
            if (textured && isEffect != effectPass) continue;
            if (effectPass) {
                Vec3 wp = transformPoint({s.center[0], s.center[1], s.center[2]}, model);
                Vec3 d = sub(wp, eye);
                effectDraws.push_back({&s, mat, dot(d, d)});
                continue;
            }

            CB cb{}; cb.exposure = g_light_intensity; cb.lightMode = lightMode;
            cb.model = model;
            cb.mvp = mvp;
            cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
            cb.hasSkin = (s.hasSkin && g_skin_ok) ? 1.0f : 0.0f;
            if (textured) {
                cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
                cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
                cb.isEffect = isEffect ? 1.0f : 0.0f;
                cb.cutout = (mat && mat->cutout) ? 1.0f : 0.0f;
                cb.tintR = mat ? mat->tint[0] : 1.0f;
                cb.tintG = mat ? mat->tint[1] : 1.0f;
                cb.tintB = mat ? mat->tint[2] : 1.0f;
                cb.tintA = mat ? mat->tint[3] : 1.0f;
            } else {
                cb.hasTex = 0.0f; cb.hasNormal = 0.0f; cb.isEffect = 0.0f; cb.cutout = 0.0f;
                cb.tintR = cb.tintG = cb.tintB = cb.tintA = 1.0f;
            }
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof cb);
                g_ctx->Unmap(g_cb.Get(), 0);
            }
            auto diff = mat ? (s.texReduced && mat->srvReduced ? mat->srvReduced.Get() : mat->srv.Get()) : nullptr;
            auto nrm = mat ? (s.texReduced && mat->srvNormalReduced ? mat->srvNormalReduced.Get() : mat->srvNormal.Get()) : nullptr;
            ID3D11ShaderResourceView* srvs[2] = {textured ? diff : nullptr, textured ? nrm : nullptr};
            g_ctx->PSSetShaderResources(0, 2, srvs);
            g_ctx->DrawIndexed(s.indexCount, s.indexStart, 0);
        }

        if (!effectPass) continue;

        std::sort(effectDraws.begin(), effectDraws.end(),
                  [](const EffectDraw& a, const EffectDraw& b) { return a.viewDepthSq > b.viewDepthSq; });

        for (const auto& d : effectDraws) {
            const MaterialGPU* mat = d.mat;
            g_ctx->OMSetBlendState(mat && mat->blend ? mat->blend.Get() : g_blendAlpha.Get(), bf, 0xffffffff);
            CB cb{}; cb.exposure = g_light_intensity; cb.lightMode = lightMode;
            cb.model = model;
            cb.mvp = mvp;
            cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
            cb.hasSkin = (d.sub->hasSkin && g_skin_ok) ? 1.0f : 0.0f;
            cb.hasTex = (mat && mat->srv) ? 1.0f : 0.0f;
            cb.hasNormal = (mat && mat->srvNormal) ? 1.0f : 0.0f;
            cb.isEffect = (mat && mat->isEffect) ? 1.0f : 0.0f;
            cb.cutout = (mat && mat->cutout) ? 1.0f : 0.0f;
            cb.tintR = mat ? mat->tint[0] : 1.0f;
            cb.tintG = mat ? mat->tint[1] : 1.0f;
            cb.tintB = mat ? mat->tint[2] : 1.0f;
            cb.tintA = mat ? mat->tint[3] : 1.0f;
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof cb);
                g_ctx->Unmap(g_cb.Get(), 0);
            }
            auto diff = mat ? (d.sub->texReduced && mat->srvReduced ? mat->srvReduced.Get() : mat->srv.Get()) : nullptr;
            auto nrm = mat ? (d.sub->texReduced && mat->srvNormalReduced ? mat->srvNormalReduced.Get() : mat->srvNormal.Get()) : nullptr;
            ID3D11ShaderResourceView* srvs[2] = {diff, nrm};
            g_ctx->PSSetShaderResources(0, 2, srvs);
            // Two-pass draw: far (back) facets first, then near (front) facets on
            // top -> the near side layers over the far side instead of both smearing
            // together (fixes "depan-belakang jadi satu"). Same triangles total as
            // the old CULL_NONE draw, just ordered; harmless for additive materials.
            g_ctx->RSSetState(g_rsCullFront.Get());
            g_ctx->DrawIndexed(d.sub->indexCount, d.sub->indexStart, 0);
            g_ctx->RSSetState(g_rsCullBack.Get());
            g_ctx->DrawIndexed(d.sub->indexCount, d.sub->indexStart, 0);
        }
        g_ctx->RSSetState(g_rsSolid.Get()); // restore two-sided for later passes
    }
    // File-driven cloth: draw the simulated proxy cape(s) last, using the
    // reconstruction VS/PS/input-layout still bound above.
    render_cloth_proxy(model, mvp, L);
    } // end reconstruction path (!useGame)

    // Skeleton overlay: bind-pose bones as green lines, drawn over the mesh with
    // depth test off so the rig stays visible regardless of orientation. Reuses
    // g_cb (uMVP is its leading field, already holding the current mvp).
    if (g_show_skeleton && g_has_skeleton && g_skelVb && g_lineVs && g_linePs && g_lineIl) {
        g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
        g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
        g_ctx->RSSetState(g_rsSolid.Get());
        UINT lstride = sizeof(LineVtx), loff = 0;
        ID3D11Buffer* lvbs[] = {g_skelVb.Get()};
        g_ctx->IASetInputLayout(g_lineIl.Get());
        g_ctx->IASetVertexBuffers(0, 1, lvbs, &lstride, &loff);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        g_ctx->VSSetShader(g_lineVs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_linePs.Get(), nullptr, 0);
        ID3D11Buffer* lcbs[] = {g_cb.Get()};
        g_ctx->VSSetConstantBuffers(0, 1, lcbs);
        // g_cb still holds the last submesh's CB; its mvp is the shared model mvp.
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            CB cb{}; cb.exposure = g_light_intensity;
            cb.mvp = mvp;
            std::memcpy(ms.pData, &cb, sizeof cb);
            g_ctx->Unmap(g_cb.Get(), 0);
        }
        g_ctx->Draw(g_skel_vert_count, 0);
    }

    // Baked particle effects (fire/glow/infusion) + effect lights, drawn on top
    // with additive blending. Bind the backbuffer + the model's depth so they are
    // correctly occluded by geometry in front of them.
    if (g_show_effects && effects_present()) {
        ID3D11RenderTargetView* frt[] = {g_rtv.Get()};
        g_ctx->OMSetRenderTargets(1, frt, g_dsv.Get());
        g_ctx->RSSetViewports(1, &vp);
        render_particles(mvp);
    }

    // Blender-style gizmo + ground grid overlay (single-model path only).
    if (g_gizmo_mode != GizmoMode::None || g_show_grid) {
        ID3D11RenderTargetView* grt[] = {g_rtv.Get()};
        g_ctx->OMSetRenderTargets(1, grt, g_dsv.Get());
        g_ctx->RSSetViewports(1, &vp);
        draw_gizmo(gizMVP);
    }

    g_swap->Present(1, 0);
}

void save_screenshot(const char* path) {
    if (!g_swap || !g_dev || !g_ctx) return;
    render(); // ensure the latest frame is drawn
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC td; back->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg))) return;
    g_ctx->CopyResource(stg.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    int w = (int)td.Width, h = (int)td.Height;
    int rowBytes = w * 3, pad = (4 - (rowBytes & 3)) & 3, stride = rowBytes + pad;
    uint32_t dataSize = stride * h, fileSize = 54 + dataSize;
    std::vector<uint8_t> bmp(fileSize, 0);
    uint8_t hdr[54] = {'B','M'};
    std::memcpy(hdr + 2, &fileSize, 4); uint32_t off = 54; std::memcpy(hdr + 10, &off, 4);
    uint32_t ih = 40; std::memcpy(hdr + 14, &ih, 4); std::memcpy(hdr + 18, &w, 4); std::memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bpp = 24; std::memcpy(hdr + 26, &planes, 2); std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(bmp.data(), hdr, 54);
    const uint8_t* src = static_cast<const uint8_t*>(ms.pData);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + (size_t)(h - 1 - y) * ms.RowPitch; // BMP is bottom-up
        uint8_t* dst = bmp.data() + 54 + (size_t)y * stride;
        for (int x = 0; x < w; ++x) { dst[x*3+0] = row[x*4+2]; dst[x*3+1] = row[x*4+1]; dst[x*3+2] = row[x*4+0]; }
    }
    g_ctx->Unmap(stg.Get(), 0);
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(bmp.data(), 1, bmp.size(), f); std::fclose(f); }
}

// ==========================================================================
// Gizmo public API
// ==========================================================================

} // namespace castlemist::render
