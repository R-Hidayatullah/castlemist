/// @file
/// @brief Driving the reverse-engineered GW2 cloth solver from the displayed mesh.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace castlemist::render {

// --- live cloth simulation --------------------------------------------------
// Step the solver one frame and stream the deformed verts into g_vb_cloth.
// Returns the buffer to draw with, or nullptr when cloth isn't running.
ID3D11Buffer* cloth_step_and_upload() {
    if (!g_cloth_enabled || !g_cloth.active() || !g_vb_cloth || !g_ctx) return nullptr;
    // The solver has its own fixed 30 Hz accumulator; feed an approximate frame dt.
    g_cloth.update(1.0f / 60.0f, g_cloth_wind);
    if (g_cloth_work.size() != g_cloth_rest.size()) g_cloth_work = g_cloth_rest;
    g_cloth.writeBack(g_cloth_work);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_vb_cloth.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return nullptr;
    std::memcpy(ms.pData, g_cloth_work.data(), g_cloth_work.size() * sizeof(GVertex));
    g_ctx->Unmap(g_vb_cloth.Get(), 0);
    return g_vb_cloth.Get();
}

bool has_cloth() {
    return g_has_model && (!g_cloth_pieces.empty() || (g_cloth_rest.size() >= 3 && g_cloth_lod0.size() >= 3));
}
bool cloth_enabled() { return g_cloth_enabled; }
void set_cloth_wind(float x, float y, float z) { g_cloth_wind[0] = x; g_cloth_wind[1] = y; g_cloth_wind[2] = z; }

// Draw the file-driven cloth proxy meshes: step the sim, stream the deformed
// proxy verts, and draw each piece's triangles textured with its material. Uses
// the reconstruction VS/PS/input-layout/cbuffer already bound by render(); drawn
// after the normal submesh passes so it doesn't disturb their VB/IB binding.
void render_cloth_proxy(const Mat4& model, const Mat4& mvp, const Vec3& L) {
    if (!g_cloth_file_active || !g_vb_clothproxy || !g_ib_clothproxy || !g_ctx) return;
    g_cloth.update(1.0f / 60.0f, g_cloth_wind);
    g_cloth.writeBackProxy();
    const std::vector<GVertex>& pv = g_cloth.proxyVertices();
    if (pv.empty()) return;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_vb_clothproxy.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, pv.data(), pv.size() * sizeof(GVertex));
        g_ctx->Unmap(g_vb_clothproxy.Get(), 0);
    }
    UINT stride = sizeof(GVertex), off = 0;
    ID3D11Buffer* vbs[] = {g_vb_clothproxy.Get()};
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(g_ib_clothproxy.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    const float bf0[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf0, 0xffffffff);
    bool textured = (g_mode == RenderMode::Full);
    for (const auto& r : g_cloth.proxyRanges()) {
        const MaterialGPU* mat = (r.materialIndex < g_mats.size()) ? &g_mats[r.materialIndex] : nullptr;
        CB cb{}; cb.exposure = g_light_intensity; cb.model = model; cb.mvp = mvp;
        cb.lx = L.x; cb.ly = L.y; cb.lz = L.z;
        cb.hasSkin = 0.0f;
        cb.hasTex = (textured && mat && mat->srv) ? 1.0f : 0.0f;
        cb.hasNormal = (textured && mat && mat->srvNormal) ? 1.0f : 0.0f;
        cb.isEffect = 0.0f;
        cb.tintR = mat ? mat->tint[0] : 1.0f; cb.tintG = mat ? mat->tint[1] : 1.0f;
        cb.tintB = mat ? mat->tint[2] : 1.0f; cb.tintA = 1.0f;
        if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &cb, sizeof cb); g_ctx->Unmap(g_cb.Get(), 0);
        }
        ID3D11ShaderResourceView* srvs[2] = {textured && mat ? mat->srv.Get() : nullptr,
                                             textured && mat ? mat->srvNormal.Get() : nullptr};
        g_ctx->PSSetShaderResources(0, 2, srvs);
        g_ctx->DrawIndexed(r.indexCount, r.indexStart, 0);
    }
}

void cloth_stats(int& verts, int& locked, float& maxDisp, float& meanDisp) {
    verts = static_cast<int>(g_cloth.vertexCount());
    locked = static_cast<int>(g_cloth.lockedCount());
    maxDisp = 0.0f; meanDisp = 0.0f;
    if (g_cloth_enabled) g_cloth.displacementStats(maxDisp, meanDisp);
}

void set_cloth_enabled(bool on) {
    if (on == g_cloth_enabled) return;
    if (on) {
        // Keep only "direct" cloth pieces: the proxy IS the visible geometry
        // (its material has no static render mesh, or the proxy roughly matches
        // that mesh's vert count). Coarse cage pieces (a tiny proxy sharing a
        // material with a big body mesh) are skipped -- drawing them would hide
        // non-cloth geometry or show a blocky overlay, and the hi-res follow they
        // drive uses a runtime bind we don't reproduce.
        auto renderVerts = [&](uint32_t m) -> uint32_t {
            return m < g_render_verts_per_mat.size() ? g_render_verts_per_mat[m] : 0;
        };
        std::vector<ClothPieceCPU> direct;
        for (const auto& p : g_cloth_pieces) {
            uint32_t rv = renderVerts(p.materialIndex);
            if (rv == 0 || (uint32_t)p.vertices.size() * 2 >= rv) direct.push_back(p);
        }
        // Preferred: authored file cloth (real proxy meshes + pins + constraints).
        if (!direct.empty() && g_cloth.buildFromPieces(direct)) {
            const auto& pv = g_cloth.proxyVertices();
            const auto& pi = g_cloth.proxyIndices();
            g_vb_clothproxy.Reset(); g_ib_clothproxy.Reset();
            bool ok = !pv.empty() && !pi.empty();
            if (ok) {
                D3D11_BUFFER_DESC vbd{};
                vbd.Usage = D3D11_USAGE_DYNAMIC; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                vbd.ByteWidth = (UINT)(pv.size() * sizeof(GVertex));
                if (FAILED(g_dev->CreateBuffer(&vbd, nullptr, &g_vb_clothproxy))) ok = false;
                D3D11_BUFFER_DESC ibd{}; D3D11_SUBRESOURCE_DATA isd{};
                ibd.Usage = D3D11_USAGE_IMMUTABLE; ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
                ibd.ByteWidth = (UINT)(pi.size() * sizeof(uint32_t)); isd.pSysMem = pi.data();
                if (ok && FAILED(g_dev->CreateBuffer(&ibd, &isd, &g_ib_clothproxy))) ok = false;
            }
            if (ok) {
                // Hide the static render submeshes a direct proxy replaces (only
                // where such a mesh exists -- rv>0; rv==0 materials have nothing).
                uint32_t maxMat = 0; for (uint32_t m : g_cloth.clothMaterials()) maxMat = std::max(maxMat, m);
                g_cloth_hide_mat.assign(maxMat + 1, 0);
                for (uint32_t m : g_cloth.clothMaterials())
                    if (renderVerts(m) > 0) g_cloth_hide_mat[m] = 1;
                g_cloth_file_active = true;
                g_cloth_enabled = true;
                return;
            }
            g_cloth.clear();   // proxy-buffer creation failed -> fall through to demo
        }
        // Fallback demo: auto-pin the whole render mesh as one cloth.
        if (g_cloth_rest.size() < 3 || g_cloth_lod0.size() < 3 || !g_vb_cloth) return;
        ModelMeshCPU mm;
        mm.vertices = g_cloth_rest;
        mm.indices  = g_cloth_lod0;
        if (!g_cloth.buildFromMesh(mm, /*pinAxis=Z (GW2 Z-up)*/ 2, 0.12f)) { g_cloth.clear(); return; }
        g_cloth_work = g_cloth_rest;
        g_cloth_file_active = false;
        g_cloth_enabled = true;
    } else {
        g_cloth_enabled = false;
        g_cloth_file_active = false;
        g_cloth.clear();
    }
}


} // namespace castlemist::render
