// ModelClothData.cpp
// ---------------------------------------------------------------------------
// Reader that turns a parsed (offset-fixed-up) GW2 cloth chunk into the runtime
// inputs consumed by castlemist::cloth::GrCloth. See ModelClothData.h for the byte layout and
// its reverse-engineering provenance.
// ---------------------------------------------------------------------------
#include "castlemist/sim/ModelClothData.h"

#include <algorithm>

namespace castlemist::cloth {

// Fill ClothParams from the on-disk float/flag fields, clamping exactly like the
// engine setter sub_140A97120 (arg->offset mapping proven at call site 0x140ce27b4).
ClothParams loadClothParams(const ModelClothDataView& cloth)
{
    ClothParams p;
    p.windResistance   = std::clamp(cloth.rigidness(),   0.0f, 1.0f);   // -> obj+512
    p.gravityScale     = std::clamp(cloth.gravityCoef(), -2.0f, 2.0f);  // -> obj+516
    p.windCoefficient  = std::clamp(cloth.dragCoef(),    -2.0f, 2.0f);  // -> obj+544
    int it = cloth.iterations();
    p.iterations       = it < 1 ? 1 : (it > 8 ? 8 : it);               // -> obj+532
    p.useMaxDistSolver = ((cloth.flags2() >> 6) & 1) != 0;             // -> obj+576 bit6

    // solveEdges stiffness (xmm1/2/3 at 0x140a90849). +524/+528 store 1-x.
    p.stretchStiffness  = std::clamp(1.0f - cloth.stretchRaw(),  0.0f, 1.0f);  // -> obj+524 (a2, stretched)
    p.compressStiffness = std::clamp(1.0f - cloth.compressRaw(), 0.3f, 1.0f);  // -> obj+528 (a3, compressed)
    p.weldBlend         = std::clamp(cloth.weldRaw(),            0.0f, 1.0f);  // -> obj+536 (a4, asym split)

    // inertia response (sub_140A8FE50 @0x140a903a0): accum /= mass, *= (1-follow).
    p.inertiaMass       = std::clamp(cloth.inertiaMass(),  1.0f, 10.0f);      // -> obj+520
    p.motionFollow      = std::clamp(cloth.motionFollow(), 0.0f, 1.0f);       // -> obj+540
    return p;
}

// The engine builds the runtime edge buffer with sub_140A95430: it copies the
// file constraints verbatim (distance/relationship/idxA/idxB) and treats the
// contiguous run of relationship==2 records at the tail as welds. We split the
// same way here. distance is a half-float -> decode with GrCloth::halfToFloat.
ClothLoadResult loadModelClothData(const ModelClothDataView&  cloth,
                                   const ModelClothChunkView& chunk,
                                   int                        lod,
                                   BoneResolver               resolveBone,
                                   void*                      resolveUser)
{
    ClothLoadResult out;
    out.params = loadClothParams(cloth);

    const uint32_t vertCount = cloth.vertexCount();

    // ---- constraints + welds ------------------------------------------------
    const uint32_t              cCount = cloth.constraintCount(lod);
    const ModelClothConstraint* cPtr   = cloth.constraints(lod);
    out.constraints.reserve(cCount);
    for (uint32_t i = 0; i < cCount && cPtr; ++i) {
        const ModelClothConstraint& c = cPtr[i];
        if (c.relationship == CLOTH_REL_WELD) {
            // Weld: snap vertIndexB onto vertIndexA every relax pass.
            out.welds.push_back(ClothWeld{ c.vertIndexA, c.vertIndexB });
        } else {
            ClothConstraint e;
            e.a          = c.vertIndexA;
            e.b          = c.vertIndexB;
            // flags bit0: symmetric(1) vs asymmetric(0) split, matching the
            // solver's v4[2] test (see GrCloth::solveEdges).
            e.flags      = (c.relationship == CLOTH_REL_SYMMETRIC) ? 1 : 0;
            e.restLength = GrCloth::halfToFloat(c.distance);
            out.constraints.push_back(e);
        }
    }

    // ---- per-vertex bone weights (drives locked/anchor verts) ---------------
    // The front run of verts that carry any bone weight are the locked verts
    // (m_numLockedVerts). Vertices with an empty group are free-simulated.
    const ModelClothBoneWeightGroup* groups = cloth.boneWeightGroups();
    out.boneWeights.assign(vertCount, ClothBoneWeight{});
    uint32_t locked = 0;
    bool countingLocked = true;
    if (groups) {
        for (uint32_t v = 0; v < vertCount; ++v) {
            const ModelClothBoneWeightGroup& g = groups[v];
            ClothBoneWeight& w = out.boneWeights[v];
            uint32_t n = g.count < 4 ? g.count : 4;   // engine packs up to 4
            for (uint32_t k = 0; k < n && g.entries; ++k) {
                int idx = resolveBone ? resolveBone(g.entries[k].bone, resolveUser) : -1;
                w.bone[k]   = (idx >= 0) ? (uint16_t)idx : 0;
                w.weight[k] = g.entries[k].weight / 255.0f;
            }
            if (countingLocked) {
                if (g.count > 0) ++locked;      // still in the locked prefix
                else             countingLocked = false;
            }
        }
    }
    out.numLocked = locked;

    // Soft locks partially pin verts; expose their weight through the bone-weight
    // slot so callers can blend. (The solver treats weight 1.0 as fully locked.)
    const ModelClothSoftLock* sl = cloth.softLocks();
    for (uint32_t i = 0; i < cloth.softLockCount() && sl; ++i) {
        if (sl[i].vertIndex < out.boneWeights.size()) {
            // store soft weight in slot 3 as a hint; caller decides how to use it
            out.boneWeights[sl[i].vertIndex].weight[3] = sl[i].weight / 255.0f;
        }
    }

    // ---- obstacles (resolve this cloth's obstacle indices into the chunk) ----
    const uint32_t* obIdx = cloth.obstacleIndices();
    const uint32_t  obN   = cloth.obstacleIndexCount();
    const ModelClothObstacle* obArr = chunk.obstacles();
    const uint32_t  obMax = chunk.obstacleCount();
    out.obstacles.reserve(obN);
    for (uint32_t i = 0; i < obN && obIdx && obArr; ++i) {
        uint32_t gi = obIdx[i];
        if (gi >= obMax) continue;                 // matches the fileData assert
        const ModelClothObstacle& src = obArr[gi];

        ClothObstacle o;
        o.type   = (src.type == CLOTH_OBSTACLE_CAPSULE) ? ObstacleType::Capsule
                                                        : ObstacleType::Sphere;
        o.bone   = resolveBone ? resolveBone(src.bone, resolveUser) : -1;
        o.radius = src.width * 0.5f;
        o.length = src.length;
        // local transform: caller multiplies by the bound bone matrix each frame.
        // We seed translation from the confirmed 16-byte block; orientation is
        // left identity until the transform semantics are fully resolved.
        o.local  = glm::mat4(1.0f);
        o.local[3] = glm::vec4(src.transform[0], src.transform[1], src.transform[2], 1.0f);
        out.obstacles.push_back(o);
    }

    return out;
}

} // namespace castlemist::cloth
