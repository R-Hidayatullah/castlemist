// ModelClothData.h
// ---------------------------------------------------------------------------
// Byte-exact structures + reader for the GW2 MODL cloth chunk (ModelClothData),
// reverse-engineered from Gw2-64.exe. Feeds castlemist::cloth::GrCloth (see GrCloth.h).
//
// Provenance (imagebase 0x140000000):
//   sub_140CE1FF0  Model::AttachCloth  - consumes the parsed chunk; gives the
//                  root + per-cloth (222 B) + obstacle (50 B) byte offsets.
//   sub_140A95430  cloth constraint loader - proves the on-disk 8-byte
//                  ModelClothConstraint layout exactly (see below).
//   sub_140A956A0/96D10  weld / collision-response setup.
//   typeinfo tables @0x141ee9500.. / @0x141f08130.. - field names + sizes.
//
// Small records (constraint 8 B, soft-lock 3 B, bone-weight entry 9 B,
// obstacle 50 B) are declared as packed structs. The 222-byte per-cloth record
// and its chunk root are NOT fully named on purpose: only some of their fields
// are proven, so they are opaque blobs read through offset-exact accessors.
// This keeps every value we hand out byte-exact and never guesses padding.
//
// Confidence markers:  [C] confirmed byte-exact   [~] name known, offset inferred
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "castlemist/sim/GrCloth.h"     // castlemist::cloth::ClothConstraint / ClothWeld / ClothObstacle ...

namespace castlemist::cloth {

#pragma pack(push, 1)

// -----------------------------------------------------------------------------
// ModelClothConstraint - 8 bytes. FULLY CONFIRMED by sub_140A95430, which reads
// the file record from (base+6) backwards into the runtime edge:
//     runtime[3] = file[+0]  -> distance      (half-float rest length)
//     runtime[2] = file[+2]  -> relationship  (==2 => WELD)
//     runtime[0] = file[+4]  -> vertIndexA
//     runtime[1] = file[+6]  -> vertIndexB
// -----------------------------------------------------------------------------
enum ClothRelationship : uint16_t {
    CLOTH_REL_ASYMMETRIC = 0,   // one-sided edge (locked/heavier side barely moves)
    CLOTH_REL_SYMMETRIC  = 1,   // 50/50 distance edge
    CLOTH_REL_WELD       = 2,   // GR_CLOTH_CONSTRAINT_RELATIONSHIP_WELD (tail of list)
};

struct ModelClothConstraint {
    uint16_t distance;       // [C] @0  half-float rest length
    uint16_t relationship;   // [C] @2  ClothRelationship
    uint16_t vertIndexA;     // [C] @4
    uint16_t vertIndexB;     // [C] @6
};
static_assert(sizeof(ModelClothConstraint) == 8, "ModelClothConstraint must be 8 bytes");

// -----------------------------------------------------------------------------
// ModelClothObstacle - 50 bytes. Collision proxy bound to a bone. Offsets from
// the obstacle loop in sub_140CE1FF0 (v84 = obstacleArray + 50*idx). typeinfo
// field names: affinity, bone, type, response, flags, length, width, transform.
// -----------------------------------------------------------------------------
enum ClothObstacleType : uint8_t {
    CLOTH_OBSTACLE_SPHERE  = 0,
    CLOTH_OBSTACLE_CAPSULE = 1,
};

struct ModelClothObstacle {
    uint32_t affinity;        // [~] @0   affinity mask
    uint64_t bone;            // [C] @4   bone filename token (-> bone index)
    uint8_t  type;            // [C] @12  ClothObstacleType (via sub_140D4ECC0)
    uint8_t  response;        // [~] @13  collision response mode
    uint32_t flags;           // [~] @14  obstacle flags
    uint8_t  reserved_18[8];  //     @18  (unresolved fields)
    float    transform[4];    // [C] @26  16-byte block (orientation/position row)
    float    length;          // [C] @42  capsule length
    float    width;           // [C] @46  capsule width (radius = width * 0.5)
};
static_assert(sizeof(ModelClothObstacle) == 50, "ModelClothObstacle must be 50 bytes");

// Per-vertex skeleton influence group entry (9 B): {uint64 boneToken; uint8 w}.
struct ModelClothBoneWeightEntry {
    uint64_t bone;            // [C] @0  bone filename token
    uint8_t  weight;          // [C] @8  quantised weight (value / 255.0)
};
static_assert(sizeof(ModelClothBoneWeightEntry) == 9, "bone-weight entry must be 9 bytes");

// One per-vertex bone-weight group header (12 B): {uint32 count; <ptr @+4>}.
// The pointer straddles +4..+11 (unaligned) in the fixed-up struct.
struct ModelClothBoneWeightGroup {
    uint32_t count;                              // [C] @0
    const ModelClothBoneWeightEntry* entries;    // [C] @4 (unaligned)
};
static_assert(sizeof(ModelClothBoneWeightGroup) == 12, "bone-weight group must be 12 bytes");

// Soft lock (3 B): {uint8 weight/255; uint16 vertIndex}.  (sub_140CE1FF0 v70 loop)
struct ModelClothSoftLock {
    uint8_t  weight;         // [C] @0
    uint16_t vertIndex;      // [C] @1
};
static_assert(sizeof(ModelClothSoftLock) == 3, "ModelClothSoftLock must be 3 bytes");

#pragma pack(pop)

// -----------------------------------------------------------------------------
// Offset-exact views over the two opaque records. Every getter reads a byte
// offset proven from sub_140CE1FF0 (v15 = per-cloth, a2 = chunk root).
// -----------------------------------------------------------------------------
namespace cloth_off {
    // ModelClothData (222 B, stride 0xDE) confirmed field offsets:
    constexpr int kData_geoData          = 0;    // uint32 geometry/submodel index
    // --- simulation params (float block @4..@31) proven from the setter call
    //     site @0x140ce27b4.. feeding sub_140A97120 (which clamps + stores):
    constexpr int kData_rigidness        = 4;    // float -> +512 windResistance  clamp[0,1]
    constexpr int kData_gravityCoef      = 8;    // float -> +516 gravityScale     clamp[-2,2]
    constexpr int kData_param0C          = 12;   // float -> +524 stretchStiffness (stored 1-x) clamp[0,1]
    constexpr int kData_param10          = 16;   // float -> +536 weldBlend                     clamp[0,1]
    constexpr int kData_param14          = 20;   // float -> +528 compressStiffness (stored 1-x) clamp[0.3,1]
    constexpr int kData_param18          = 24;   // float -> +520 inertiaMass      clamp[1,10]
    constexpr int kData_dragCoef         = 28;   // float -> +544 windCoefficient  clamp[-2,2]
    constexpr int kData_fvf              = 32;   // uint32 vertex format
    constexpr int kData_vertexCount      = 60;   // uint32
    constexpr int kData_boneWeightGroups = 64;   // ptr -> ModelClothBoneWeightGroup[vertexCount]
    constexpr int kData_softLockCount    = 84;   // uint32
    constexpr int kData_softLocks        = 88;   // ptr -> ModelClothSoftLock[]
    constexpr int kData_constraintCount0 = 96;   // uint32  (LOD0)
    constexpr int kData_constraints0     = 100;  // ptr -> ModelClothConstraint[]
    constexpr int kData_constraintCount1 = 108;  // uint32  (LOD1)
    constexpr int kData_constraints1     = 112;  // ptr -> ModelClothConstraint[]
    constexpr int kData_obstacleIdxCount = 156;  // uint32
    constexpr int kData_obstacleIndices  = 160;  // ptr -> uint32[]
    constexpr int kData_flags2           = 208;  // uint8 flags (bit6->maxDistSolver, bit7)
    constexpr int kData_iterations       = 209;  // uint8  -> +532 iterations  clamp[1,8]
    constexpr int kData_paramD2          = 210;  // float  -> +540             clamp[0,1]
    constexpr int kData_rootBone         = 214;  // uint64 bone token
    constexpr int kData_size             = 222;

    // ModelClothChunk root confirmed field offsets:
    constexpr int kChunk_obstacleCount   = 20;   // uint32
    constexpr int kChunk_obstacles       = 24;   // ptr -> ModelClothObstacle[]
    constexpr int kChunk_clothCount      = 48;   // uint32
    constexpr int kChunk_cloths          = 52;   // ptr -> ModelClothData[] (stride 222)
}

// Thin reader over a fixed-up per-cloth record.
struct ModelClothDataView {
    const uint8_t* base = nullptr;
    template <typename T> T at(int off) const { T v; std::memcpy(&v, base + off, sizeof(T)); return v; }

    uint32_t geoData()          const { return at<uint32_t>(cloth_off::kData_geoData); }
    uint32_t vertexCount()      const { return at<uint32_t>(cloth_off::kData_vertexCount); }
    const ModelClothBoneWeightGroup* boneWeightGroups() const {
        return at<const ModelClothBoneWeightGroup*>(cloth_off::kData_boneWeightGroups); }
    uint32_t softLockCount()    const { return at<uint32_t>(cloth_off::kData_softLockCount); }
    const ModelClothSoftLock* softLocks() const {
        return at<const ModelClothSoftLock*>(cloth_off::kData_softLocks); }
    uint32_t constraintCount(int lod) const {
        return at<uint32_t>(lod ? cloth_off::kData_constraintCount1 : cloth_off::kData_constraintCount0); }
    const ModelClothConstraint* constraints(int lod) const {
        return at<const ModelClothConstraint*>(lod ? cloth_off::kData_constraints1 : cloth_off::kData_constraints0); }
    uint32_t obstacleIndexCount() const { return at<uint32_t>(cloth_off::kData_obstacleIdxCount); }
    const uint32_t* obstacleIndices() const { return at<const uint32_t*>(cloth_off::kData_obstacleIndices); }
    uint64_t rootBone()         const { return at<uint64_t>(cloth_off::kData_rootBone); }

    // Raw simulation-parameter fields (unclamped, as stored on disk).
    float    rigidness()   const { return at<float>(cloth_off::kData_rigidness); }
    float    gravityCoef() const { return at<float>(cloth_off::kData_gravityCoef); }
    float    dragCoef()    const { return at<float>(cloth_off::kData_dragCoef); }
    float    stretchRaw()  const { return at<float>(cloth_off::kData_param0C); } // -> +524
    float    weldRaw()     const { return at<float>(cloth_off::kData_param10); } // -> +536
    float    compressRaw() const { return at<float>(cloth_off::kData_param14); } // -> +528
    float    inertiaMass() const { return at<float>(cloth_off::kData_param18); } // -> +520
    float    motionFollow()const { return at<float>(cloth_off::kData_paramD2); } // -> +540
    uint8_t  flags2()      const { return at<uint8_t>(cloth_off::kData_flags2); }
    uint8_t  iterations()  const { return at<uint8_t>(cloth_off::kData_iterations); }
};

// Build the runtime ClothParams from a per-cloth record, applying the same
// clamps the engine's setter (sub_140A97120) uses. Constraint-solver stiffness
// (stretch/compress/weld) is mapped from the +524/+528/+536 obj fields, proven
// at the solveEdges call site 0x140a90849 (xmm1/2/3 <- obj+0x20C/0x210/0x218).
ClothParams loadClothParams(const ModelClothDataView& cloth);

// Thin reader over the fixed-up chunk root.
struct ModelClothChunkView {
    const uint8_t* base = nullptr;
    template <typename T> T at(int off) const { T v; std::memcpy(&v, base + off, sizeof(T)); return v; }

    uint32_t obstacleCount() const { return at<uint32_t>(cloth_off::kChunk_obstacleCount); }
    const ModelClothObstacle* obstacles() const {
        return at<const ModelClothObstacle*>(cloth_off::kChunk_obstacles); }
    uint32_t clothCount()    const { return at<uint32_t>(cloth_off::kChunk_clothCount); }
    ModelClothDataView cloth(uint32_t i) const {
        const uint8_t* arr = at<const uint8_t*>(cloth_off::kChunk_cloths);
        return ModelClothDataView{ arr + size_t(i) * cloth_off::kData_size };
    }
};

// -----------------------------------------------------------------------------
// Reader: convert one per-cloth record into the runtime inputs GrCloth expects.
// `restPositions` come from the referenced mesh geometry (geoData) and must be
// supplied by the caller. `boneNameToIndex` maps a bone filename token to a
// skeleton index (the game's sub_140CF8340); return -1 for unknown / no skel.
// -----------------------------------------------------------------------------
struct ClothLoadResult {
    uint32_t                     numLocked = 0;
    ClothParams                  params;         // filled via loadClothParams()
    std::vector<ClothConstraint> constraints;   // relationship 0/1
    std::vector<ClothWeld>       welds;          // relationship == 2
    std::vector<ClothBoneWeight> boneWeights;
    std::vector<ClothObstacle>   obstacles;
};

using BoneResolver = int (*)(uint64_t boneToken, void* user);

ClothLoadResult loadModelClothData(const ModelClothDataView&  cloth,
                                   const ModelClothChunkView& chunk,
                                   int                        lod = 0,
                                   BoneResolver               resolveBone = nullptr,
                                   void*                      resolveUser = nullptr);

} // namespace castlemist::cloth
