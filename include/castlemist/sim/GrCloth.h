// GrCloth.h
// ---------------------------------------------------------------------------
// Faithful CPU reconstruction of Guild Wars 2's cloth simulation system,
// reverse-engineered from Gw2-64.exe (Arena\Engine\Gr\GrCloth.cpp).
//
// The engine's cloth solver is a classic Verlet / Position-Based-Dynamics
// integrator (Jakobsen "Advanced Character Physics" style):
//
//   1. Skin the "locked" verts to the skeleton   (verts driven by animation)
//   2. Accumulate external accelerations          (gravity + wind + inertia)
//   3. Verlet-integrate the free verts            (fixed 30 Hz timestep)
//   4. Relax N iterations:
//        a. distance constraints (edges) + weld    -> GrCloth: solveEdges
//        b. obstacle collisions (sphere / capsule)  -> GrCloth: collide*
//   5. Recompute normals / tangents / bitangents
//   6. Interpolate between physics frames -> output vertex buffer
//
// Mapping to the binary (imagebase 0x140000000):
//   sub_140A8FE50  GrCloth::Simulate      (force accum + verlet + iteration loop)
//   sub_140A90F30  solveEdges (clamped)   (distance constraints + weld pass)
//   sub_140A91210  solveEdges (max-dist)  (distance constraints, stretch clamp)
//   sub_140A922A0  collide sphere         (obstacle type 0)
//   sub_140A91950  collide capsule        (obstacle type 1, swept-sphere/segment)
//   sub_140A92A50  GrCloth::Finalize      (frame-fraction lerp + skin to output)
//
// This reconstruction favours readability over bit-exactness with the SIMD
// disassembly, but keeps every tunable constant found in the binary.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

namespace castlemist::cloth {

// ---- constants lifted verbatim from GrCloth.cpp -------------------------------
namespace cloth_consts {
    // Physics runs on a fixed step; the game advances by real dt and steps the
    // solver whenever the accumulator (a1+668) crosses a full frame.
    constexpr float kFixedDt      = 0.033333335f;   // 1/30 s
    constexpr float kFixedDtSq    = 0.0011111113f;  // (1/30)^2, force -> displacement
    // Verlet velocity damping: drag = 0.99999899 - windResistance*0.7
    constexpr float kDragBase     = 0.99999899f;
    constexpr float kDragWindMul  = 0.69999897f;
    // Gravity magnitude in GW2 world units (inch-ish). Applied along -Z (Z-up).
    constexpr float kGravity      = 384.0f;
    // Wind smoothing / obstacle push factor used in the verlet accumulate.
    constexpr float kForceScale   = 0.0011111113f;
    // "collapsed edge" epsilon and generic tiny epsilon from the asserts.
    constexpr float kEps          = 1e-6f;
    constexpr float kEps12        = 1e-12f;
}

// Edge/distance constraint. In the file these are packed as four u16 words
// (8 bytes): [idxA, idxB, flags, restLenHalf]. See sub_140A90F30.
struct ClothConstraint {
    uint16_t a;            // particle index A
    uint16_t b;            // particle index B
    uint16_t flags;        // bit0 (v4[2]): 0 = asymmetric (A locked side), 1 = symmetric
    float    restLength;   // decoded from the half-float word v4[3]
};

// A "weld" pairing: particle `dst` is snapped to `src` every relax pass.
// GR_CLOTH_CONSTRAINT_RELATIONSHIP_WELD. Packed after the edge list.
struct ClothWeld {
    uint16_t src;
    uint16_t dst;
};

// Collision proxy bound to a skeleton bone. sub_140A922A0 / sub_140A91950.
enum class ObstacleType : uint32_t {
    Sphere  = 0,   // GR_OBSTACLE_TYPE_SPHERE
    Capsule = 1,   // GR_OBSTACLE_TYPE_CAPSULE
};

struct ClothObstacle {
    ObstacleType type   = ObstacleType::Sphere;
    int          bone   = -1;      // bone this obstacle rides on
    float        radius = 0.0f;
    float        length = 0.0f;    // capsule segment length (centre-to-centre + 2r)
    // Local transform relative to its bone. World transform is rebuilt per frame
    // from boneMatrices[bone] * localTransform (see rebuildObstacles()).
    glm::mat4    local  = glm::mat4(1.0f);
    glm::mat4    world  = glm::mat4(1.0f);      // filled each frame
    glm::mat4    worldInv = glm::mat4(1.0f);    // filled each frame
};

// Per-vertex skinning influence used to drive the locked/anchor verts and to
// build the "target" rest pose the free verts are pulled toward.
struct ClothBoneWeight {
    uint16_t bone[4] = {0,0,0,0};
    float    weight[4] = {0,0,0,0};
};

// Material / behaviour parameters. These live scattered in the runtime object
// (a1+512, +516, +520, +532, +544, +560, +576 ...) — grouped here for clarity.
struct ClothParams {
    float stretchStiffness  = 0.9f;   // xmm arg a2 to solveEdges (dist > restLen)
    float compressStiffness = 0.5f;   // xmm arg a3 to solveEdges (dist < restLen)
    float weldBlend         = 0.5f;   // xmm arg a4: split for asymmetric edges
    float windResistance    = 0.3f;   // a1+512, clamped [0,1] -> drag
    float windCoefficient   = 1.0f;   // a1+544, wind acceleration multiplier
    float gravityScale      = 1.0f;   // a1+516
    float inertiaMass       = 1.0f;   // a1+520, divides inertial accel   [1,10]
    float motionFollow      = 0.0f;   // a1+540, inertia *= (1-this)      [0,1]
    int   iterations        = 4;      // a1+532, relax passes per step
    bool  useMaxDistSolver  = false;  // a1+576 bit6: pick sub_140A91210 vs 90F30
    bool  computeNormals    = true;   // a1+576 bit4: rebuild tangent frame
};

// ---------------------------------------------------------------------------
// GrCloth: one simulated cloth mesh.
// ---------------------------------------------------------------------------
class GrCloth {
public:
    // Load static topology. `numLocked` verts at the front of the array are
    // driven by the skeleton and never integrated (m_numLockedVerts, a1+672).
    //
    // `indices` is the mesh triangle list (3 per face). When supplied, normals
    // are rebuilt area-weighted from real faces (matching the engine's
    // face-frame blend in sub_140A97CB0). `uvs` (per vertex) additionally
    // enables tangent/bitangent reconstruction (Lengyel's method) so callers
    // get the same N/T/B frame the game maintains (m_normals/tangents/bitangents).
    // Pass empty `indices` to fall back to the edge-graph normal approximation.
    void init(const std::vector<glm::vec3>& restPositions,
              uint32_t                        numLocked,
              std::vector<ClothConstraint>    constraints,
              std::vector<ClothWeld>          welds,
              std::vector<ClothBoneWeight>    boneWeights,
              std::vector<ClothObstacle>      obstacles,
              std::vector<uint32_t>           indices,
              const ClothParams&              params,
              std::vector<glm::vec2>          uvs = {});

    // Advance the simulation by `dt` real seconds. `boneMatrices` are the
    // current skeleton world matrices (used to skin locked verts + obstacles).
    // `modelWorld` is the character's world transform this frame; its delta vs.
    // last frame is turned into an inertial acceleration (a1+788..836 block).
    // `windWorld` is the world-space wind vector.
    void update(float dt,
                const std::vector<glm::mat4>& boneMatrices,
                const glm::mat4&              modelWorld,
                const glm::vec3&              windWorld);

    // Simulated positions (rest-frame). Size == restPositions.size().
    const std::vector<glm::vec3>& positions()  const { return m_pos; }
    const std::vector<glm::vec3>& restPositions() const { return m_rest; }
    const std::vector<glm::vec3>& normals()    const { return m_normals; }
    // Tangent frame; populated only when UVs were supplied to init().
    const std::vector<glm::vec3>& tangents()   const { return m_tangents; }
    const std::vector<glm::vec3>& bitangents() const { return m_bitangents; }

    // Decode a GW2 half-float exactly as sub_140A90F30 does (for loaders).
    static float halfToFloat(uint16_t h);

private:
    void skinLockedVerts(const std::vector<glm::mat4>& bones);
    void rebuildObstacles(const std::vector<glm::mat4>& bones);
    void accumulateInertia(const glm::mat4& modelWorld);
    void integrate(const glm::vec3& accel);
    void solveEdges();               // dispatches 90F30 / 91210
    void solveWeld();
    void collide();                  // all obstacles
    void collideSphere (const ClothObstacle& o);
    void collideCapsule(const ClothObstacle& o);
    void recomputeNormals();          // from index buffer (or edge fallback)
    void recomputeTangentFrame();     // Lengyel N/T/B from indices + UVs

    // ---- topology (immutable after init) ----
    std::vector<glm::vec3>       m_rest;        // m_restPositions (a1+1016)
    std::vector<ClothConstraint> m_constraints;
    std::vector<ClothWeld>       m_welds;
    std::vector<ClothBoneWeight> m_weights;
    std::vector<ClothObstacle>   m_obstacles;
    std::vector<uint32_t>        m_indices;     // triangle list (mesh faces)
    std::vector<glm::vec2>       m_uvs;         // per-vertex UVs (optional)
    uint32_t                     m_numLocked = 0;
    ClothParams                  m_params;

    // ---- state (double-buffered for Verlet) ----
    std::vector<glm::vec3>       m_pos;         // a1+256, current
    std::vector<glm::vec3>       m_prev;        // a1+248, previous
    std::vector<glm::vec3>       m_normals;     // a1+1076
    std::vector<glm::vec3>       m_tangents;    // a1+1140
    std::vector<glm::vec3>       m_bitangents;  // a1+1204

    // ---- inertia bookkeeping (a1+788..+836, +564..+572) ----
    glm::mat4  m_lastModelWorld = glm::mat4(1.0f);
    glm::vec3  m_inertialAccum  = glm::vec3(0.0f);
    bool       m_haveLastWorld  = false;

    // ---- fixed-step accumulator (a1+668) ----
    float      m_accumTime = 0.0f;
    bool       m_initialised = false;
};

} // namespace castlemist::cloth
