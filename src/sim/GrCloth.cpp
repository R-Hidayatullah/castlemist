// GrCloth.cpp
// ---------------------------------------------------------------------------
// See GrCloth.h for the high-level description and the map to the GW2 binary.
// ---------------------------------------------------------------------------
#include "castlemist/sim/GrCloth.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/geometric.hpp>          // dot, length, normalize
#include <glm/gtc/matrix_inverse.hpp> // affineInverse

namespace castlemist::cloth {

using namespace cloth_consts;

// ---------------------------------------------------------------------------
// Half-float decode. Bit-for-bit port of the routine embedded in sub_140A90F30
// / sub_140A91210 (they decode edge.restLength from a u16 on the fly).
// ---------------------------------------------------------------------------
float GrCloth::halfToFloat(uint16_t h)
{
    uint32_t mant = h & 0x3FF;
    int      exp;
    if ((h & 0x7C00) != 0) {                 // normalised
        exp = (h >> 10) & 0x1F;
    } else if (mant != 0) {                  // subnormal -> normalise
        exp = 1;
        do { --exp; mant <<= 1; } while ((mant & 0x400) == 0);
        mant &= 0x3FF;
    } else {                                 // zero
        exp = -112;                          // (exp + 112) == 0
    }
    uint32_t bits = (mant << 13)
                  | (uint32_t(exp + 112) << 23)
                  | ((uint32_t(h) << 16) & 0x80000000u);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// ---------------------------------------------------------------------------
void GrCloth::init(const std::vector<glm::vec3>& restPositions,
                   uint32_t                        numLocked,
                   std::vector<ClothConstraint>    constraints,
                   std::vector<ClothWeld>          welds,
                   std::vector<ClothBoneWeight>    boneWeights,
                   std::vector<ClothObstacle>      obstacles,
                   std::vector<uint32_t>           indices,
                   const ClothParams&              params,
                   std::vector<glm::vec2>          uvs)
{
    m_rest        = restPositions;
    m_numLocked   = std::min<uint32_t>(numLocked, (uint32_t)restPositions.size());
    m_constraints = std::move(constraints);
    m_welds       = std::move(welds);
    m_weights     = std::move(boneWeights);
    m_obstacles   = std::move(obstacles);
    m_indices     = std::move(indices);
    m_uvs         = std::move(uvs);
    m_params      = params;

    m_pos     = restPositions;   // start at rest pose
    m_prev    = restPositions;   // zero initial velocity
    m_normals.assign(restPositions.size(), glm::vec3(0, 0, 1));
    // Tangent frame is only meaningful with UVs; size lazily otherwise.
    if (m_uvs.size() == restPositions.size()) {
        m_tangents.assign(restPositions.size(), glm::vec3(1, 0, 0));
        m_bitangents.assign(restPositions.size(), glm::vec3(0, 1, 0));
    }

    m_accumTime      = 0.0f;
    m_inertialAccum  = glm::vec3(0.0f);
    m_haveLastWorld  = false;
    m_initialised    = true;
}

// ---------------------------------------------------------------------------
// Fixed-step driver. Mirrors the accumulator gate at the top of sub_140A8FE50:
// only when (a1+668) drops below 0 do we run one physics frame and add 1/30.
// ---------------------------------------------------------------------------
void GrCloth::update(float dt,
                     const std::vector<glm::mat4>& boneMatrices,
                     const glm::mat4&              modelWorld,
                     const glm::vec3&              windWorld)
{
    if (!m_initialised) return;

    accumulateInertia(modelWorld);       // build model-space inertial force
    skinLockedVerts(boneMatrices);       // drive anchor verts from skeleton
    rebuildObstacles(boneMatrices);      // world transforms for collision

    m_accumTime -= dt;
    // Run at most a few catch-up steps to avoid the spiral of death.
    int guard = 4;
    while (m_accumTime <= 0.0f && guard-- > 0) {
        m_accumTime += kFixedDt;

        // --- external acceleration in the cloth's (model) frame ---
        // gravity is applied down (-Z, GW2 is Z-up); wind and inertia added.
        glm::vec3 gravity(0.0f, 0.0f, -kGravity * m_params.gravityScale);
        glm::vec3 wind = windWorld * m_params.windCoefficient;
        glm::vec3 accel = (gravity + wind + m_inertialAccum) * kFixedDtSq;

        integrate(accel);   // writes new->m_pos, old->m_prev (in-place Verlet)

        for (int it = 0; it < m_params.iterations; ++it) {
            solveEdges();
            solveWeld();
            collide();
        }

        m_inertialAccum = glm::vec3(0.0f); // consumed (a1+564..572 cleared)
    }

    if (m_params.computeNormals) {
        recomputeNormals();
        if (m_uvs.size() == m_pos.size() && !m_indices.empty())
            recomputeTangentFrame();
    }
}

// ---------------------------------------------------------------------------
// Locked verts (index < m_numLocked) follow the skeleton exactly; they are the
// Dirichlet boundary of the sim. Linear blend skin from their bone weights.
// ---------------------------------------------------------------------------
void GrCloth::skinLockedVerts(const std::vector<glm::mat4>& bones)
{
    if (bones.empty()) return;
    for (uint32_t i = 0; i < m_numLocked && i < m_weights.size(); ++i) {
        const ClothBoneWeight& w = m_weights[i];
        glm::vec3 skinned(0.0f);
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            if (w.weight[k] <= 0.0f) continue;
            uint16_t b = w.bone[k];
            if (b >= bones.size()) continue;
            skinned += w.weight[k] * glm::vec3(bones[b] * glm::vec4(m_rest[i], 1.0f));
            wsum    += w.weight[k];
        }
        if (wsum > 0.0f) {
            glm::vec3 p = skinned / wsum;
            m_pos[i]  = p;
            m_prev[i] = p;   // locked verts carry no Verlet velocity
        }
    }
}

// ---------------------------------------------------------------------------
void GrCloth::rebuildObstacles(const std::vector<glm::mat4>& bones)
{
    for (ClothObstacle& o : m_obstacles) {
        glm::mat4 boneM = (o.bone >= 0 && o.bone < (int)bones.size())
                        ? bones[o.bone] : glm::mat4(1.0f);
        o.world    = boneM * o.local;
        o.worldInv = glm::affineInverse(o.world);
    }
}

// ---------------------------------------------------------------------------
// Turn the frame-to-frame change of the character's world transform into a
// pseudo-force in cloth space, so the cloth lags behind sudden movement /
// rotation. Corresponds to the a1+788..+836 matrix cache and a1+564..+572
// accumulator in sub_140A8FE50.
// ---------------------------------------------------------------------------
void GrCloth::accumulateInertia(const glm::mat4& modelWorld)
{
    if (!m_haveLastWorld) {
        m_lastModelWorld = modelWorld;
        m_haveLastWorld  = true;
        return;
    }
    // Displacement of the origin, expressed in the *new* model frame.
    glm::vec3 prevOrigin(m_lastModelWorld[3]);
    glm::vec3 curOrigin (modelWorld[3]);
    glm::vec3 worldDelta = prevOrigin - curOrigin;      // opposite of motion

    glm::mat4 invCur = glm::affineInverse(modelWorld);
    glm::vec3 localDelta = glm::mat3(invCur) * worldDelta;

    // Match the binary's inertia accumulation (sub_140A8FE50 @0x140a903a0):
    //   accum += dir * (-speed) / inertiaMass * (1 - motionFollow)
    // i.e. -(delta/dt) / inertiaMass * (1 - motionFollow). localDelta already
    // holds prev-cur (= -delta), so the sign is baked in.
    float mass   = std::max(m_params.inertiaMass, 1.0f);      // obj+520, clamp[1,10]
    float follow = std::clamp(m_params.motionFollow, 0.0f, 1.0f); // obj+540
    m_inertialAccum += localDelta * (1.0f / std::max(kFixedDt, kEps))
                     * ((1.0f - follow) / mass);
    m_lastModelWorld = modelWorld;
}

// ---------------------------------------------------------------------------
// Verlet integration of the free verts. new = cur + drag*(cur - prev) + accel.
// (sub_140A8FE50 inner loop; buffer roles alternate via the swap in update()).
// ---------------------------------------------------------------------------
void GrCloth::integrate(const glm::vec3& accel)
{
    float wr   = std::clamp(m_params.windResistance, 0.0f, 1.0f);
    float drag = kDragBase - wr * kDragWindMul;

    for (size_t i = m_numLocked; i < m_pos.size(); ++i) {
        glm::vec3 cur  = m_pos[i];
        glm::vec3 prev = m_prev[i];
        glm::vec3 nxt  = cur + drag * (cur - prev) + accel;
        // In-place Verlet: new position becomes current, old current becomes
        // previous. (The binary writes to a second buffer and swaps; equivalent.)
        m_prev[i] = cur;
        m_pos[i]  = nxt;
    }
}

// ---------------------------------------------------------------------------
// Distance constraints. Port of sub_140A90F30 (clamped) and sub_140A91210
// (max-distance) — both share the structure, differing only in how the
// correction magnitude is clamped. Ends with the weld copy pass.
// ---------------------------------------------------------------------------
void GrCloth::solveEdges()
{
    const float kStretch  = m_params.stretchStiffness;   // xmm a2
    const float kCompress = m_params.compressStiffness;  // xmm a3
    const float kWeld     = m_params.weldBlend;          // xmm a4

    for (const ClothConstraint& c : m_constraints) {
        if (c.a >= m_pos.size() || c.b >= m_pos.size()) continue;

        glm::vec3 pa = m_pos[c.a];
        glm::vec3 pb = m_pos[c.b];
        glm::vec3 d  = pb - pa;
        float len2   = glm::dot(d, d);
        if (len2 < kEps12) continue;            // collapsed edge guard
        float len    = std::sqrt(len2);

        // Stiffness picked by stretch vs. compression (v33 select in 90F30).
        float k = (len > c.restLength) ? kStretch : kCompress;

        // Correction vector pulling the edge back to rest length.
        glm::vec3 corr = d * (k * (len - c.restLength) / len);

        // flags bit0 clear -> asymmetric: split by weldBlend (locked side moves
        // less). Set -> symmetric 50/50. (v4[2] test in the binary.)
        float wa, wb;
        if (c.flags & 1) { wa = 0.5f;         wb = 0.5f; }
        else             { wa = 1.0f - kWeld; wb = kWeld; }

        if (c.a >= m_numLocked) m_pos[c.a] = pa + corr * wa;
        if (c.b >= m_numLocked) m_pos[c.b] = pb - corr * wb;
    }
}

// ---------------------------------------------------------------------------
// Weld pass: snap dst particle onto src (GR_..RELATIONSHIP_WELD). This is the
// tail loop of both edge solvers copying position words vert->vert.
// ---------------------------------------------------------------------------
void GrCloth::solveWeld()
{
    for (const ClothWeld& w : m_welds) {
        if (w.src < m_pos.size() && w.dst < m_pos.size())
            m_pos[w.dst] = m_pos[w.src];
    }
}

// ---------------------------------------------------------------------------
void GrCloth::collide()
{
    for (const ClothObstacle& o : m_obstacles) {
        if (o.type == ObstacleType::Sphere) collideSphere(o);
        else                                collideCapsule(o);
    }
}

// ---------------------------------------------------------------------------
// Sphere obstacle. Port of sub_140A922A0: push any particle inside the sphere
// out to its surface. (The binary works in the obstacle's local frame; here we
// use world space with the obstacle's world centre for clarity.)
// ---------------------------------------------------------------------------
void GrCloth::collideSphere(const ClothObstacle& o)
{
    glm::vec3 c = glm::vec3(o.world[3]);
    float r  = o.radius;
    float r2 = r * r;
    for (size_t i = m_numLocked; i < m_pos.size(); ++i) {
        glm::vec3 d = m_pos[i] - c;
        float dist2 = glm::dot(d, d);
        if (dist2 < r2) {
            float dist = std::sqrt(dist2);
            glm::vec3 n = (dist > kEps) ? d / (dist + kEps) : glm::vec3(0, 0, 1);
            m_pos[i] = c + n * r;      // project to surface
        }
    }
}

// ---------------------------------------------------------------------------
// Capsule obstacle. Port of sub_140A91950: closest point on the capsule's core
// segment, push out to radius if the particle is inside. The binary solves a
// swept quadratic against the two end-spheres and the cylinder; the result is
// the same nearest-point projection reconstructed here.
// ---------------------------------------------------------------------------
void GrCloth::collideCapsule(const ClothObstacle& o)
{
    float r  = o.radius;
    float r2 = r * r;
    float half = std::max(0.0f, (o.length - 2.0f * r) * 0.5f);

    // Capsule core segment endpoints in world space. Local axis is +Z (matches
    // the v86 midpoint the binary derives from length along the third column).
    glm::vec3 c    = glm::vec3(o.world[3]);
    glm::vec3 axis = glm::normalize(glm::vec3(o.world[2]));
    glm::vec3 p0 = c - axis * half;
    glm::vec3 p1 = c + axis * half;
    glm::vec3 seg = p1 - p0;
    float segLen2 = std::max(glm::dot(seg, seg), kEps);

    for (size_t i = m_numLocked; i < m_pos.size(); ++i) {
        glm::vec3 p = m_pos[i];
        float t = glm::clamp(glm::dot(p - p0, seg) / segLen2, 0.0f, 1.0f);
        glm::vec3 nearest = p0 + t * seg;
        glm::vec3 d = p - nearest;
        float dist2 = glm::dot(d, d);
        if (dist2 < r2) {
            float dist = std::sqrt(dist2);
            glm::vec3 n = (dist > kEps) ? d / dist : glm::vec3(0, 0, 1);
            m_pos[i] = nearest + n * r;
        }
    }
}

// ---------------------------------------------------------------------------
// Rebuild smooth per-vertex normals from the real mesh triangle list.
//
// This is the viewer-side equivalent of the engine's face-frame blend
// (sub_140A97CB0): the game precomputes, per vertex, the set of adjacent faces
// and a weight, then sums the face tangent frames. Area-weighted accumulation
// (un-normalised cross product) is the standard, deterministic realisation of
// that same weighted blend and needs no baked adjacency table.
//
// If no index buffer was supplied we fall back to the old edge-graph estimate
// so lighting still works on meshes that ship without face indices.
// ---------------------------------------------------------------------------
void GrCloth::recomputeNormals()
{
    std::fill(m_normals.begin(), m_normals.end(), glm::vec3(0.0f));

    if (!m_indices.empty()) {
        const size_t triCount = m_indices.size() / 3;
        for (size_t t = 0; t < triCount; ++t) {
            uint32_t i0 = m_indices[3 * t + 0];
            uint32_t i1 = m_indices[3 * t + 1];
            uint32_t i2 = m_indices[3 * t + 2];
            if (i0 >= m_pos.size() || i1 >= m_pos.size() || i2 >= m_pos.size())
                continue;
            const glm::vec3& p0 = m_pos[i0];
            const glm::vec3& p1 = m_pos[i1];
            const glm::vec3& p2 = m_pos[i2];
            // Un-normalised => magnitude == 2*triArea, giving area weighting.
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            m_normals[i0] += fn;
            m_normals[i1] += fn;
            m_normals[i2] += fn;
        }
    } else {
        // Fallback: edge-graph estimate (no real faces available).
        for (const ClothConstraint& c : m_constraints) {
            if (c.a >= m_pos.size() || c.b >= m_pos.size()) continue;
            glm::vec3 e = m_pos[c.b] - m_pos[c.a];
            glm::vec3 ref = (std::fabs(e.z) < 0.99f) ? glm::vec3(0, 0, 1)
                                                     : glm::vec3(1, 0, 0);
            glm::vec3 n = glm::cross(e, ref);
            m_normals[c.a] += n;
            m_normals[c.b] += n;
        }
    }

    for (glm::vec3& n : m_normals) {
        float l = glm::length(n);
        n = (l > kEps) ? n / l : glm::vec3(0, 0, 1);
    }
}

// ---------------------------------------------------------------------------
// Reconstruct the tangent/bitangent frame from the triangle list + UVs
// (Lengyel, "Computing Tangent Space Basis Vectors"). The game keeps a full
// N/T/B frame per vert (m_normals/m_tangents/m_bitangents, rebuilt by
// sub_140A98DD0); this reproduces it deterministically from mesh data.
//
// Assumes recomputeNormals() has already run this frame.
// ---------------------------------------------------------------------------
void GrCloth::recomputeTangentFrame()
{
    const size_t n = m_pos.size();
    if (m_tangents.size()   != n) m_tangents.assign(n, glm::vec3(1, 0, 0));
    if (m_bitangents.size() != n) m_bitangents.assign(n, glm::vec3(0, 1, 0));

    std::vector<glm::vec3> tan1(n, glm::vec3(0.0f));  // d/du accumulators
    std::vector<glm::vec3> tan2(n, glm::vec3(0.0f));  // d/dv accumulators

    const size_t triCount = m_indices.size() / 3;
    for (size_t t = 0; t < triCount; ++t) {
        uint32_t i0 = m_indices[3 * t + 0];
        uint32_t i1 = m_indices[3 * t + 1];
        uint32_t i2 = m_indices[3 * t + 2];
        if (i0 >= n || i1 >= n || i2 >= n) continue;

        const glm::vec3& p0 = m_pos[i0];
        const glm::vec3& p1 = m_pos[i1];
        const glm::vec3& p2 = m_pos[i2];
        const glm::vec2& w0 = m_uvs[i0];
        const glm::vec2& w1 = m_uvs[i1];
        const glm::vec2& w2 = m_uvs[i2];

        glm::vec3 e1 = p1 - p0, e2 = p2 - p0;
        glm::vec2 d1 = w1 - w0, d2 = w2 - w0;

        float denom = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(denom) < kEps) continue;      // degenerate UVs
        float r = 1.0f / denom;

        glm::vec3 sdir = (e1 * d2.y - e2 * d1.y) * r;
        glm::vec3 tdir = (e2 * d1.x - e1 * d2.x) * r;

        tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
        tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
    }

    for (size_t i = 0; i < n; ++i) {
        glm::vec3 nrm = m_normals[i];
        glm::vec3 t   = tan1[i];
        // Gram-Schmidt orthonormalise the tangent against the normal.
        glm::vec3 tangent = t - nrm * glm::dot(nrm, t);
        float tl = glm::length(tangent);
        tangent = (tl > kEps) ? tangent / tl : glm::vec3(1, 0, 0);
        // Handedness picks the bitangent sign (mirrored UV islands).
        float handed = (glm::dot(glm::cross(nrm, tangent), tan2[i]) < 0.0f) ? -1.0f : 1.0f;
        m_tangents[i]   = tangent;
        m_bitangents[i] = glm::cross(nrm, tangent) * handed;
    }
}

} // namespace castlemist::cloth
