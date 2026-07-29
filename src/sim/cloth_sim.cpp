// cloth_sim.cpp - see cloth_sim.h.
#include "castlemist/sim/cloth_sim.h"

#include <algorithm>
#include <cmath>

#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>

namespace castlemist::cloth {

using castlemist::cloth::GrCloth;
using castlemist::cloth::ClothConstraint;
using castlemist::cloth::ClothWeld;
using castlemist::cloth::ClothObstacle;
using castlemist::cloth::ClothParams;
using castlemist::cloth::ClothBoneWeight;

// Build the reordered cloth (pinned verts first) + edge constraints from tris.
bool ClothSim::buildReordered(const ModelMeshCPU& mesh, const std::vector<char>& pinned)
{
    const uint32_t n = (uint32_t)mesh.vertices.size();
    if (n < 3 || mesh.indices.size() < 3) return false;

    // New order: locked (pinned) first, then free. Build both remaps.
    std::vector<uint32_t> toMesh;             // new idx -> mesh idx
    toMesh.reserve(n);
    std::vector<uint32_t> meshToNew(n, 0);
    for (uint32_t i = 0; i < n; ++i) if (pinned[i]) { meshToNew[i] = (uint32_t)toMesh.size(); toMesh.push_back(i); }
    const uint32_t numLocked = (uint32_t)toMesh.size();
    for (uint32_t i = 0; i < n; ++i) if (!pinned[i]) { meshToNew[i] = (uint32_t)toMesh.size(); toMesh.push_back(i); }

    // Rest positions + UVs in the new order.
    std::vector<glm::vec3> rest(n);
    std::vector<glm::vec2> uvs(n);
    for (uint32_t ni = 0; ni < n; ++ni) {
        const GVertex& v = mesh.vertices[toMesh[ni]];
        rest[ni] = glm::vec3(v.px, v.py, v.pz);
        uvs[ni]  = glm::vec2(v.u, v.v);
    }

    // Triangle index buffer remapped to the new order (drives normal rebuild).
    std::vector<uint32_t> idx;
    idx.reserve(mesh.indices.size());
    for (uint32_t i : mesh.indices) if (i < n) idx.push_back(meshToNew[i]);

    // Distance constraints from unique triangle edges (structural springs).
    // Symmetric relationship (flags bit0 set) for both ends unless one is locked.
    std::vector<ClothConstraint> cons;
    std::vector<uint64_t> seen;
    seen.reserve(idx.size());
    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32) | b;
    };
    cons.reserve(idx.size());
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        uint32_t tri[3] = { idx[t], idx[t + 1], idx[t + 2] };
        for (int e = 0; e < 3; ++e) {
            uint32_t a = tri[e], b = tri[(e + 1) % 3];
            if (a == b) continue;
            uint64_t k = edgeKey(a, b);
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
            seen.push_back(k);
            ClothConstraint c;
            c.a = (uint16_t)a; c.b = (uint16_t)b;
            c.flags = 1;                              // symmetric
            c.restLength = glm::length(rest[a] - rest[b]);
            cons.push_back(c);
        }
    }
    if (cons.empty()) return false;

    ClothParams params;                               // sensible demo defaults
    params.iterations       = 8;
    params.windResistance   = 0.35f;
    params.stretchStiffness = 1.0f;
    params.compressStiffness= 0.6f;

    m_cloth.init(rest, numLocked, std::move(cons), {}, {}, {}, std::move(idx), params, std::move(uvs));
    m_toMesh    = std::move(toMesh);
    m_numLocked = numLocked;
    m_active    = true;
    return true;
}

bool ClothSim::buildFromMesh(const ModelMeshCPU& mesh, int pinAxis, float pinFraction)
{
    clear();
    const uint32_t n = (uint32_t)mesh.vertices.size();
    if (n < 3) return false;
    if (pinAxis < 0 || pinAxis > 2) pinAxis = 2;
    pinFraction = std::clamp(pinFraction, 0.01f, 0.9f);

    auto axis = [&](const GVertex& v) {
        return pinAxis == 0 ? v.px : (pinAxis == 1 ? v.py : v.pz);
    };
    float mn = axis(mesh.vertices[0]), mx = mn;
    for (const GVertex& v : mesh.vertices) { float a = axis(v); mn = std::min(mn, a); mx = std::max(mx, a); }
    if (mx - mn < 1e-6f) return false;                // degenerate / flat along axis

    // Pin the top band: verts within pinFraction of the max extent.
    const float thresh = mx - (mx - mn) * pinFraction;
    std::vector<char> pinned(n, 0);
    uint32_t nPinned = 0;
    for (uint32_t i = 0; i < n; ++i) if (axis(mesh.vertices[i]) >= thresh) { pinned[i] = 1; ++nPinned; }
    if (nPinned == 0 || nPinned == n) return false;   // nothing to hang, or all pinned

    return buildReordered(mesh, pinned);
}

bool ClothSim::buildFromClothData(const std::vector<GVertex>&       meshVerts,
                                 const std::vector<uint32_t>&      meshIndices,
                                 uint32_t                          numLocked,
                                 std::vector<ClothConstraint>      constraints,
                                 std::vector<ClothWeld>            welds,
                                 std::vector<ClothObstacle>        obstacles,
                                 const ClothParams&                params)
{
    clear();
    const uint32_t n = (uint32_t)meshVerts.size();
    if (n < 3 || constraints.empty()) return false;

    std::vector<glm::vec3> rest(n);
    std::vector<glm::vec2> uvs(n);
    for (uint32_t i = 0; i < n; ++i) {
        rest[i] = glm::vec3(meshVerts[i].px, meshVerts[i].py, meshVerts[i].pz);
        uvs[i]  = glm::vec2(meshVerts[i].u, meshVerts[i].v);
    }
    std::vector<uint32_t> idx;
    idx.reserve(meshIndices.size());
    for (uint32_t i : meshIndices) if (i < n) idx.push_back(i);

    m_cloth.init(rest, numLocked, std::move(constraints), std::move(welds), {},
                 std::move(obstacles), std::move(idx), params, std::move(uvs));
    m_toMesh.resize(n);
    for (uint32_t i = 0; i < n; ++i) m_toMesh[i] = i;   // file path keeps mesh order
    m_numLocked = numLocked;
    m_active    = true;
    return true;
}

// Combine authored cloth pieces into one solver. GW2 stores each piece with its
// pinned verts first; we gather every piece's pinned prefix to the global front
// (GrCloth needs one locked prefix), remap the file constraints + indices, and
// assemble a renderable proxy mesh with per-material draw ranges.
bool ClothSim::buildFromPieces(const std::vector<ClothPieceCPU>& pieces)
{
    clear();
    if (pieces.empty()) return false;

    // Per-piece base offset into a naive concatenation of all proxy verts.
    std::vector<uint32_t> base(pieces.size());
    uint32_t total = 0;
    for (size_t i = 0; i < pieces.size(); ++i) { base[i] = total; total += (uint32_t)pieces[i].vertices.size(); }
    if (total < 3) return false;

    // pinned[g] = this concatenated vert is a locked anchor (front prefix of its piece).
    std::vector<char> pinned(total, 0);
    std::vector<std::pair<int, uint32_t>> src(total);   // g -> (piece, local)
    for (size_t i = 0; i < pieces.size(); ++i) {
        uint32_t lock = std::min<uint32_t>(pieces[i].lockCount, (uint32_t)pieces[i].vertices.size());
        for (uint32_t l = 0; l < pieces[i].vertices.size(); ++l) {
            src[base[i] + l] = {(int)i, l};
            if (l < lock) pinned[base[i] + l] = 1;
        }
    }

    // New solver order: all pinned verts first, then all free verts.
    std::vector<uint32_t> toGlobal; toGlobal.reserve(total);
    std::vector<uint32_t> gToNew(total, 0);
    for (uint32_t g = 0; g < total; ++g) if (pinned[g]) { gToNew[g] = (uint32_t)toGlobal.size(); toGlobal.push_back(g); }
    const uint32_t numLocked = (uint32_t)toGlobal.size();
    for (uint32_t g = 0; g < total; ++g) if (!pinned[g]) { gToNew[g] = (uint32_t)toGlobal.size(); toGlobal.push_back(g); }

    // Rest positions + proxy verts + UVs in solver order.
    std::vector<glm::vec3> rest(total);
    std::vector<glm::vec2> uvs(total);
    m_proxy.resize(total);
    for (uint32_t n = 0; n < total; ++n) {
        auto [pi, lo] = src[toGlobal[n]];
        const GVertex& v = pieces[pi].vertices[lo];
        rest[n] = glm::vec3(v.px, v.py, v.pz);
        uvs[n]  = glm::vec2(v.u, v.v);
        m_proxy[n] = v;
    }

    // File constraints (remapped to solver order); relationship 2 = weld.
    std::vector<castlemist::cloth::ClothConstraint> cons;
    std::vector<castlemist::cloth::ClothWeld> welds;
    float maxGravity = 0.0f;
    for (size_t i = 0; i < pieces.size(); ++i) {
        maxGravity = std::max(maxGravity, pieces[i].gravity);
        for (const auto& e : pieces[i].edges) {
            if (e.a >= pieces[i].vertices.size() || e.b >= pieces[i].vertices.size()) continue;
            uint32_t na = gToNew[base[i] + e.a], nb = gToNew[base[i] + e.b];
            if (e.rel == 2) welds.push_back(castlemist::cloth::ClothWeld{(uint16_t)na, (uint16_t)nb});
            else {
                castlemist::cloth::ClothConstraint c;
                c.a = (uint16_t)na; c.b = (uint16_t)nb; c.flags = 1; c.restLength = e.rest;
                cons.push_back(c);
            }
        }
    }
    if (cons.empty()) { clear(); return false; }

    // Proxy triangle index buffer (solver order) + per-piece material draw ranges.
    m_proxyIdx.clear();
    m_ranges.clear();
    m_clothMats.clear();
    for (size_t i = 0; i < pieces.size(); ++i) {
        uint32_t start = (uint32_t)m_proxyIdx.size();
        for (uint32_t idx : pieces[i].indices)
            if (idx < pieces[i].vertices.size()) m_proxyIdx.push_back(gToNew[base[i] + idx]);
        uint32_t cnt = (uint32_t)m_proxyIdx.size() - start;
        if (cnt) m_ranges.push_back({pieces[i].materialIndex, start, cnt});
        if (std::find(m_clothMats.begin(), m_clothMats.end(), pieces[i].materialIndex) == m_clothMats.end())
            m_clothMats.push_back(pieces[i].materialIndex);
    }

    ClothParams params;
    params.iterations       = 10;
    params.windResistance   = 0.35f;
    params.gravityScale     = maxGravity > 0.01f ? maxGravity : 1.0f;
    params.stretchStiffness = 1.0f;
    params.compressStiffness= 0.6f;

    std::vector<uint32_t> idxForNormals = m_proxyIdx;   // copy: init() moves it
    m_cloth.init(rest, numLocked, std::move(cons), std::move(welds), {}, {},
                 std::move(idxForNormals), params, std::move(uvs));
    m_numLocked  = numLocked;
    m_fileDriven = true;
    m_active     = true;
    return true;
}

void ClothSim::displacementStats(float& maxDisp, float& meanDisp) const
{
    maxDisp = 0.0f; meanDisp = 0.0f;
    if (!m_active) return;
    const std::vector<glm::vec3>& pos = m_cloth.positions();
    const std::vector<glm::vec3>& rest = m_cloth.restPositions();
    size_t n = std::min(pos.size(), rest.size());
    if (!n) return;
    double sum = 0.0; float mx = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float d = glm::length(pos[i] - rest[i]);
        if (d > mx) mx = d; sum += d;
    }
    maxDisp = mx; meanDisp = (float)(sum / n);
}

void ClothSim::writeBackProxy()
{
    if (!m_fileDriven || !m_active) return;
    const std::vector<glm::vec3>& pos = m_cloth.positions();
    const std::vector<glm::vec3>& nrm = m_cloth.normals();
    for (size_t i = 0; i < m_proxy.size() && i < pos.size(); ++i) {
        m_proxy[i].px = pos[i].x; m_proxy[i].py = pos[i].y; m_proxy[i].pz = pos[i].z;
        if (i < nrm.size()) { m_proxy[i].nx = nrm[i].x; m_proxy[i].ny = nrm[i].y; m_proxy[i].nz = nrm[i].z; }
    }
}

void ClothSim::clear()
{
    m_toMesh.clear();
    m_numLocked = 0;
    m_active = false;
    m_fileDriven = false;
    m_proxy.clear();
    m_proxyIdx.clear();
    m_ranges.clear();
    m_clothMats.clear();
}

void ClothSim::update(float dt, const float windWorld[3])
{
    if (!m_active) return;
    static const std::vector<glm::mat4> kNoBones;      // empty => locked verts stay put
    glm::vec3 wind = windWorld ? glm::vec3(windWorld[0], windWorld[1], windWorld[2]) : glm::vec3(0.0f);
    m_cloth.update(dt, kNoBones, glm::mat4(1.0f), wind);
}

void ClothSim::writeBack(std::vector<GVertex>& verts) const
{
    if (!m_active) return;
    const std::vector<glm::vec3>& pos = m_cloth.positions();
    const std::vector<glm::vec3>& nrm = m_cloth.normals();
    const uint32_t n = (uint32_t)m_toMesh.size();
    for (uint32_t ni = 0; ni < n; ++ni) {
        uint32_t mi = m_toMesh[ni];
        if (mi >= verts.size()) continue;
        GVertex& v = verts[mi];
        v.px = pos[ni].x; v.py = pos[ni].y; v.pz = pos[ni].z;
        if (ni < nrm.size()) { v.nx = nrm[ni].x; v.ny = nrm[ni].y; v.nz = nrm[ni].z; }
    }
}

} // namespace castlemist::cloth
