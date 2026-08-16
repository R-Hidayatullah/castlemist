// gw2model.hpp -- extract renderable geometry + material textures from a
// decompressed GW2 .modl packfile, driven by the same type registry the
// BinaryParser uses (templates/gw2_packfile.json).
//
// Path (verified against the registry + real models):
//   GEOM chunk -> ModelFileGeometryV1.meshes[] -> ModelMeshDataV66
//                   .materialIndex, .geometry -> ModelMeshGeometryV1
//                       .verts (ModelMeshVertexDataV1: vertexCount,
//                                mesh=PackVertexType{ fvf, vertices[byte] })
//                       .indices (ModelMeshIndexDataV1: indices[word])
//   MODL chunk -> ModelFileDataV*.permutations[0].materials[] ->
//                   ModelMaterialDataV*.textures[] -> ModelTextureDataV*.filename
//
// The FVF bitmask decode follows GW2_Vertex_FVF_Notes.md (GrFvf).
#ifndef GW2MODEL_HPP
#define GW2MODEL_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace castlemist::model {

using nlohmann::json;

struct Vertex {
    float px, py, pz;         // offset 0
    float nx, ny, nz;         // 12
    float tx, ty, tz; // tangent   (from FVF tangent-frame, else 0)  // 24
    float bx, by, bz; // bitangent (from FVF tangent-frame, else 0)  // 36
    float u, v;               // 48  UV0 (TEXCOORD0) — kept named for gw2viewer
    float uv[7][2];           // 56  UV1..UV7 (TEXCOORD1..7); channel c>=1 -> uv[c-1]
    // GrFvf supports 8 UV channels (bits 8-15); absent channels copy UV0.
    uint8_t boneIdx[4] = {0, 0, 0, 0};   // BlendIndices (FVF 0x4): index into Mesh.boneBindings
    float   boneWt[4] = {1, 0, 0, 0};    // BlendWeights (FVF 0x2): normalized [0,1]
};

struct Mesh {
    uint32_t fvf = 0;
    uint32_t vertexCount = 0;
    uint32_t materialIndex = 0;
    bool hasTangents = false;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;         // LOD0 (full detail) index buffer
    // Additional lower-detail LOD index buffers (LOD1, LOD2, ...), each indexing
    // the SAME `vertices`. Empty when the mesh has no extra LODs. GW2 stores these
    // in ModelMeshGeometryV1.lods[]; LOD0 is `indices` above.
    std::vector<std::vector<uint32_t>> lods;
    float minB[3] = {0, 0, 0}, maxB[3] = {0, 0, 0};
    // Per-mesh bone binding table: a vertex's BlendIndices index into this, and
    // each entry is a token64 identifying a skeleton bone (matched by name hash).
    std::vector<uint64_t> boneBindings;
    std::vector<int> boneBindingSkelIndex; // boneBindings[k] -> skeleton bone index (-1 = unresolved)
    bool hasSkin = false;                 // FVF carries blend weights + indices
};

// GW2 bone-name -> token64 (reverse of engine sub_140E3B5E0, ModelBones.cpp).
// The engine tokenizes a bone name by 5-bit-packing its letters (a-z/A-Z -> 1..26,
// LSB-first, up to 12 chars) and, if the name ends in two decimal digits, storing
// that number in the top nibble (bits 60-63). The "bone:" display prefix is
// stripped first. Verified: matches every boneBindings entry on real models.
inline uint64_t tokenizeBoneName(const std::string& name) {
    const char* s = name.c_str();
    size_t n = name.size();
    // Strip the namespace prefix up to and including the LAST ':' and tokenize only
    // the leaf. GW2 bone names are namespaced ("bone:COG", "Rig1:bone:BCOG" for a
    // merged multi-rig model, "ignore:FK_LowTeeth"/"actionpoint:MountA" for facial /
    // helper bones); the engine keys on the leaf, and only the leaf is unique. A prior
    // "strip up to last 'bone:'" left non-"bone:" namespaces mis-tokenized: their long
    // prefixes overflowed the 12-char/60-bit pack and COLLIDED (e.g. "ignore:FK_LowTeeth"
    // aliased "ignore:FK_LowlidR"), so a mesh vertex bound to the jaw/teeth bone resolved
    // to the wrong bone and the mouth geometry skinned onto the body. Verified: fileId
    // 1762011 (springer) 88/101 -> 101/101 bindings resolve with 0 collisions; fileId
    // 904350 (Marjory & Kasmeer) 200/203 -> 203/203, 0 collisions. Unprefixed names
    // (rfind == npos) are unchanged.
    if (size_t p = name.rfind(':'); p != std::string::npos) { s += p + 1; n -= p + 1; }
    size_t end = n;
    uint64_t top = 0;
    if (n >= 2) {
        unsigned char last = (unsigned char)s[n - 1], sec = (unsigned char)s[n - 2];
        if (last >= '0' && last <= '9' && sec >= '0' && sec <= '9') {
            end = n - 2;
            top = ((uint64_t)last + 10ull * (uint64_t)sec) & 0xFull; // engine's v6<<60 keeps low nibble
        }
    }
    uint64_t tok = top << 60;
    int i = 0;
    for (size_t k = 0; k < end && i < 60; ++k, i += 5) {
        unsigned char c = (unsigned char)s[k];
        uint64_t val = (c >= 'a' && c <= 'z') ? (c - 96) : (c >= 'A' && c <= 'Z') ? (c - 64) : 0;
        tok |= val << i;
    }
    return tok;
}

struct MatTexture {
    uint32_t fileId = 0;    // decoded texture reference
    uint64_t token = 0;     // sampler token (binds to the shader's AmatSamplerConstant)
    uint32_t flags = 0;     // textureFlags
    uint8_t  uvIndex = 0;   // uvPSInputIndex (which UV channel)
};
struct MatConstant {
    uint32_t name = 0;      // token32 hash of the constant name
    float    value[4] = {0, 0, 0, 0};
};
struct Material {
    uint32_t index = 0;
    // The material's own token64 -- the key the engine matches against
    // AmatEffectV1::token to choose which shader in the AMAT draws this material
    // (sub_140BFAD20, passed as `materialToken64` from the draw object at +64).
    // Without it, effect selection has to guess from shader content.
    uint64_t token = 0;
    uint32_t materialId = 0;    // selects the built-in shader (0..57) or 58=custom
    uint32_t materialFile = 0;  // fileId of the material's .amat/GRMT file (0 if none)
    uint32_t materialFlags = 0;
    uint32_t sortOrder = 0;     // draw/render order
    uint32_t sortLayer = 0;     // transparent/sort hint
    std::vector<MatTexture> textures;
    std::vector<MatConstant> constants;
    // convenience: just the fileIds, file order
    std::vector<uint32_t> textureFileIds() const {
        std::vector<uint32_t> v; for (auto& t : textures) v.push_back(t.fileId); return v;
    }
};

// --- skeleton (bind pose) ---
// One rig bone. Bind-pose joint origin in model space (worldPos) is what the
// viewer draws; localPos/localQuat/scaleShear + parent are kept so an animation
// system can later re-pose the hierarchy per frame.
struct Bone {
    std::string name;
    int32_t parent = -1;                                  // index into bones[]; <0 or out-of-range = root
    float localPos[3] = {0, 0, 0};                        // LocalTransform.Position
    float localQuat[4] = {0, 0, 0, 1};                    // LocalTransform.Orientation (x,y,z,w)
    float scaleShear[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};    // LocalTransform.ScaleShear (row-major 3x3)
    float invWorld[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // InverseWorld4x4 (bind, model->bone), row-major
    float worldPos[3] = {0, 0, 0};                        // bind-pose joint origin in model space
};
struct Skeleton {
    bool present = false;
    int fileVersion = -1;             // ModelFileSkeletonV0/V1 chunk version (0 or 1)
    std::string skelDataType;         // resolved ModelSkeletonDataV* type key
    uint32_t externalRef = 0;         // fileId of a referenced external skeleton (0 = inline)
    std::vector<Bone> bones;
};

// --- embedded animation bank (MODL ANIM chunk) ---
// The actual keyframe curves are Granny-compressed (not decoded here); we expose
// the chunk version and the per-clip token so a UI can list/select clips and a
// future decoder can fill in the curves.
struct AnimClip {
    uint64_t token = 0;               // token64 name hash of the clip
    float moveSpeed = 0;              // ModelAnimationData.moveSpeed (if present)
    // Raw serialized Granny animation blob (self-relative pointers unresolved)
    // + the pointer-fixup table. Decoded lazily by the granny reader.
    std::vector<uint8_t> rawGranny;
    std::vector<uint32_t> fixups;     // byte offsets into rawGranny holding pointers
    size_t grannyFileOffset = 0;      // absolute offset of rawGranny[0] within the packfile
    int ptrSize = 4;                  // blob pointer width (4=32-bit packfile, 8=64-bit)
};
struct AnimInfo {
    bool present = false;
    uint16_t chunkVersion = 0;        // ANIM chunk version (selects the format variant)
    std::string typeKey;              // resolved ModelFileAnimation*/Bank* type key
    std::vector<AnimClip> clips;
};

// --- particle / effect system (MODL cloudData + lightData) ---
// GW2 bakes runtime visual effects into the model file: particle clouds
// (fire/spark/smoke/infusion shimmer), effect lights (glow), streaks and
// lightning. These run on their own timers, so they animate even on an idle,
// un-posed model. Parsed straight from ModelFileData.cloudData/lightData.
//
// KEY (reverse-engineered from GrCloud.cpp, sub_140ACA4B0 / sub_140AC5A40):
//  * every emitter float2 is a {base, span} range, sampled as base + rand01()*span
//    (engine helper sub_140E12DE0: r.x + rand()*r.y), NOT {min,max}.
//  * velocity[4]/acceleration[4] = (dirX, dirY, dirZ, magnitude) each {base,span};
//    direction is normalized then scaled by magnitude.
//  * colorBegin[0]/colorEnd[0] = base RGBA, [1] = random span added per-particle.
//  * spawnShape: 0 cylinder, 1 disc, 2 line, 3 box, 4/5 mesh-surface, 6 point.
struct ParticleFlipbook {
    bool present = false;
    uint8_t columns = 1, count = 1, rows = 1, start = 0;
    float fps = 0;
};
struct ParticleEmitter {
    // spawning
    float spawnPeriod = 0, spawnProbability = 1;
    float spawnGroupSize[2] = {1, 0};   // {base, span}
    float spawnRadius[2] = {0, 0};
    uint8_t spawnShape = 6;
    float lifetime[2] = {1, 0};
    // motion (each row = {base, span})
    float velocity[4][2] = {};          // dirX, dirY, dirZ, speed
    float acceleration[4][2] = {};      // dirX, dirY, dirZ, magnitude
    float drag = 0;
    // color
    float colorBegin[2][4] = {};        // [0] base rgba, [1] span rgba (span=0 on old float4 layout)
    float colorEnd[2][4] = {};
    float colorPeriod = 0;
    float colorFalloff[2] = {0, 0};
    // wind: sand/dust/leaves blown by the environment wind. windInfluence (byte)
    // scales how strongly the global wind pushes the particle; spawnWindSpeed
    // {base,span} is an initial wind-aligned kick, spawnWindEmit a spawn bias.
    float spawnWindEmit[2] = {0, 0}, spawnWindSpeed[2] = {0, 0};
    uint8_t windInfluence = 0;
    // lifetime curves: keys (t in [0,1], value); empty -> use preset / linear
    std::vector<std::pair<float, float>> opacityCurve;
    std::vector<std::pair<float, float>> scaleCurve;
    uint32_t opacityCurvePreset = 0, scaleCurvePreset = 0;
    uint32_t emitterFlags = 0, flags = 0;
    uint64_t bone = 0;                   // attachment bone (token64), 0 = model origin
    float transform[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0}; // ModelMatrix43 (3 rows of float4)
    bool hasTransform = false;
    // plane (billboard) emitter
    bool hasPlane = false;
    uint8_t alignmentType = 0;
    float alignmentDir[3] = {0, 0, 1};
    float rotationInitial[2] = {0, 0}, rotationChange[2] = {0, 0}, rotationDrag = 0;
    float scaleInitial[2][2] = {{1,0},{1,0}}; // 2 axes, each {base, span}
    float scaleChange[2][2] = {};
    float texCoordRect[4] = {0, 0, 1, 1};
    ParticleFlipbook flipbook;
    // mesh emitter
    bool hasMesh = false;
    uint32_t meshFileId = 0;
};
struct ParticleCloud {
    uint32_t materialIndex = 0, flags = 0, fvf = 0;
    uint64_t bone = 0;
    float acceleration[3] = {0, 0, 0};  // external/gravity accel on the whole cloud
    float velocity[3] = {0, 0, 0};
    float drag = 0;
    std::vector<uint32_t> emitterIndices; // -> indices into CloudSystem.emitters
};
struct EffectLight {
    uint64_t bone = 0;
    float color[3] = {1, 1, 1};         // 0..1
    float intensity = 1, nearDistance = 0, farDistance = 1;
};
struct CloudSystem {
    std::vector<ParticleCloud> clouds;
    std::vector<ParticleEmitter> emitters;
    std::vector<EffectLight> lights;
    bool present() const { return !clouds.empty() || !lights.empty(); }
};

// One authored cloth simulation piece (ModelFileData.clothData[i]). GW2 cloth is
// a self-contained low-res proxy: it carries its OWN vertex mesh (positions +
// tangent frame), triangle indices, and distance constraints. `lockCount` verts
// at the FRONT are skeleton-driven anchors (the runtime treats the front prefix
// as locked); the rest are simulated. `materialIndex` binds the piece to the
// render submesh(es) it deforms (via barycentric/binding at draw time).
struct ClothEdge {
    uint16_t a = 0, b = 0;
    uint16_t relationship = 0;   // 0/1 = distance edge, 2 = weld (GR_..RELATIONSHIP_WELD)
    float    restLength = 0.0f;  // decoded from the half-float `distance`
};
struct ClothPiece {
    uint32_t materialIndex = 0;
    uint32_t lockCount = 0;
    float drag = 0, gravity = 0, wind = 0, weight = 1, rigidness = 0, translateWeight = 0;
    std::vector<Vertex>   verts;    // the cloth's own proxy mesh (locked verts first)
    std::vector<uint32_t> indices;  // proxy triangle list
    std::vector<ClothEdge> edges;   // lod0Constraints
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    Skeleton skeleton;
    AnimInfo anim;
    CloudSystem effects;
    std::vector<ClothPiece> clothPieces;   // ModelFileData.clothData
};

// --- AMAT (bgfx shader package) extraction ---
struct AmatSamplerBind {
    uint64_t token = 0;
    uint32_t stateIndex = 0;   // -> AmatTree::samplerStates[] (bgfx sampler flags)
    /// @brief Which material texture to bind.
    ///
    /// `0xFFFFFFFF` is meaningful, not "absent": the engine's uniform-count
    /// assert (BgfxShader.cpp:336) subtracts exactly these, because a sampler
    /// with textureIndex -1 consumes no bgfx uniform. Getting that wrong
    /// shifts every later sampler binding by one.
    uint32_t textureIndex = 0;
    uint32_t textureSlot = 0;  // -> sampler register (t#/s#)
};
struct AmatShader {
    bool isPixel = false;
    std::vector<uint8_t> dxbc;  // bgfx VSH/FSH blob (contains DXBC)
    std::vector<AmatSamplerBind> samplers;
    // Ordered list of material-constant name tokens (AmatShaderConstant.token,
    // token32). Same order as the shader's material-constant uniforms in the bgfx
    // uniform table, so index i pairs constantTokens[i] <-> the i-th non-global
    // uniform. A MODL MatConstant whose token matches one of these binds its value
    // to that uniform (no name-hash needed: token-to-token match).
    std::vector<uint32_t> constantTokens;

    // ---- role classification (filled by classifyShader) --------------------
    // An AMAT holds every shader the engine may bind for this material across the
    // whole frame, not just the one that paints it: the deferred NORMAL PREPASS
    // (frame pass 1), depth-only and depth-peel passes, and the real COLOUR pass
    // (frame pass 4). Only the colour pass may be used to draw the model; picking
    // a prepass renders the world-space normal as RGB, which is the flat
    // green/yellow/blue "normal map" look. See classifyShader for how each is told
    // apart.
    bool writesNormalEncode = false; // outputs normal*0.5+0.5 -> G-buffer prepass, NOT a colour pass
    bool hasShLighting = false;      // uniform table carries the SH sun/ambient rig -> real lit material
    bool samplesSlot0 = false;       // binds a material texture at t0 (depth-only passes do not)
    bool usesBones = false;          // vertex shader reads the `grbones` matrix palette (GPU skinning)
    bool hasDiscard = false;         // contains a `discard` (alpha cutout / stipple fade)
    // The pixel shader's SV_TARGET alpha is a CONSTANT -- `mov o0.w, cb0[k].x` (the
    // engine's StencilId / submodel id) or a literal -- rather than a computed
    // opacity. Such a shader must never be blended by a SRC_ALPHA-family factor: the
    // id is not coverage, so the whole surface would come out uniformly see-through.
    // See dxbcAlphaIsConstant.
    bool alphaIsConstant = false;
};
struct AmatSet {
    int vsIndex = -1, psIndex = -1;      // best OPAQUE colour effect
    uint64_t renderState = 0;            // bgfx 64-bit state word of the picked opaque effect
    uint32_t passFlags = 0;              // shaderPassFlags of the picked opaque effect
    // best BLENDED colour effect, if the AMAT defines one whose renderState carries
    // real blend bits (see isBlendState).
    bool hasTrans = false;
    int transVsIndex = -1, transPsIndex = -1;
    uint64_t transRenderState = 0;
    uint32_t transPassFlags = 0;         // shaderPassFlags of the picked blended effect
    std::vector<AmatShader> shaders;     // full shader list
    // Does this material's own deferred NORMAL PREPASS cut geometry away? True only
    // when the AMAT actually ships a prepass shader containing a `discard`. This is
    // the authority on whether a reconstructed prepass should alpha-test: guessing it
    // from the albedo's alpha histogram misfires on the many materials that store a
    // GLOSS map in alpha, and punching those full of holes corrupts the light buffer.
    bool prepassCutout = false;
    bool hasPrepass = false;             // a normal-prepass shader was found at all
    /// @brief The effect was chosen by the engine's token rule, not by heuristic.
    ///
    /// False means this AMAT offered no effect matching the material's token, the
    /// remap chain, or the default -- so the content heuristic picked instead.
    /// Tracking it is what makes "how often are we still guessing?" measurable.
    bool tokenMatched = false;
    /// @name Structure counts (for surveying the archive, not for rendering)
    /// @{
    uint32_t techniqueCount = 0;  ///< quality levels present
    uint32_t maxPassCount = 0;    ///< largest `passes[]` across techniques
    uint32_t effectCount = 0;     ///< total effects across every technique/pass
    int selectedQuality = -1;     ///< ::amatQualityRank of the tier selection used
    /// @}
    std::string error;
};

// --- AMAT, whole tree ------------------------------------------------------
//
// `AmatSet` above is a *decision*: it walks the tree and hands back one
// vs/ps pair chosen by content heuristics. That is the right shape for the
// reconstruction renderer, which has no material token to work with.
//
// `AmatTree` is the tree itself, unflattened, so a renderer that DOES have the
// MODL material's token can run the engine's own selection over it -- technique
// by quality, effect by token64 with the remap chain, vertex shader by the
// mesh-derived variant id. See docs/research/gw2-amat-draw-state.md and
// tools/viewer/gw2bgfx.
//
// Field names and nesting are the packfile schema's; the layouts were
// cross-checked against the runtime structs the client indexes (AmatEffectV1 is
// 36 bytes: 8+8+4+4+12).

/// @brief `AmatVertexShaderVariantV1`. `variant` is the small integer 0..4 the
///        draw loop derives from mesh flags, NOT a Token.
struct AmatVsVariantRaw {
    uint32_t variant = 0;
    uint32_t vertexShaderIndex = 0;
};

/// @brief `AmatEffectV1`.
struct AmatEffectRaw {
    uint64_t token = 0;            ///< The engine's selection key.
    uint64_t renderState = 0;      ///< PARTIAL bgfx state: blend + ALPHA_REF only.
    uint32_t shaderPassFlags = 0;  ///< Write-mask / depth / cull bits.
    uint32_t pixelShaderIndex = 0;
    std::vector<AmatVsVariantRaw> vertexShaderVariants;
};

/// @brief `AmatPassV1`.
struct AmatPassRaw { std::vector<AmatEffectRaw> effects; };

/// @brief `AmatTechniqueV1` -- a quality level, not a frame pass.
struct AmatTechniqueRaw {
    uint32_t quality = 0;          ///< token32 decoding to ultra/high/medium/low
    std::vector<AmatPassRaw> passes;
};

/// @brief `AmatShaderBinaryV1`.
struct AmatShaderBinaryRaw {
    std::vector<uint8_t> data;              ///< bgfx blob: 'VSH'/'FSH' + 0x0b, then DXBC
    std::vector<uint32_t> constants;        ///< one token32 per non-sampler uniform
    std::vector<AmatSamplerBind> samplers;  ///< one per sampler uniform
};

/// @brief `AmatShaderV1`.
///
/// The second binary is `osxShader` (Metal), not a shading-model variant; the
/// client selects it on a platform flag that is clear on the D3D11 backend.
struct AmatShaderRaw {
    bool isPixelShader = false;
    AmatShaderBinaryRaw dx11Shader;
    AmatShaderBinaryRaw osxShader;
};

/// @brief `AmatMaterialV1` -- the AMAT's whole BGFX chunk.
struct AmatTree {
    std::vector<AmatShaderRaw> shaders;
    std::vector<uint32_t> samplerStates;   ///< AmatSamplerStateV1::state, bgfx sampler flags
    std::vector<AmatTechniqueRaw> techniques;
    std::string error;
    bool ok() const { return error.empty() && !shaders.empty(); }
};

// --- Geometry, undecoded ---------------------------------------------------

/// @brief One geoset with its vertex buffer left exactly as the file stores it.
///
/// `Mesh` above decodes into the fat 144-byte ::Vertex, which is what the
/// reconstruction renderer wants. This does not decode at all, because it does
/// not have to: `GrFvf_CreateVertexLayout` asserts, in the shipping client,
/// that `DDI_STRIDE(fvf) == vertexLayout->CalcStride()`, so the bytes in the
/// file are already in GPU layout. Repacking them can only lose information --
/// packed tangent frames and half-precision UVs both widen on the way through
/// ::Vertex.
struct GeosetRaw {
    uint32_t fvf = 0;
    uint32_t vertexCount = 0;
    uint32_t materialIndex = 0;
    /// @brief `vertexCount * DDI_STRIDE(fvf)` bytes, verbatim.
    std::vector<uint8_t> vertexBytes;
    /// @brief LOD0 indices, 16-bit as stored. Index 0 of ::lodIndices.
    std::vector<uint16_t> indices;
    /// @brief LOD1..N over the same vertices. Empty entries are kept so the
    ///        LOD numbering stays meaningful.
    std::vector<std::vector<uint16_t>> lodIndices;
    /// @brief token64 per bone slot; vertex BlendIndices index into this.
    std::vector<uint64_t> boneBindings;
    float minB[3] = {0, 0, 0}, maxB[3] = {0, 0, 0};

    /// @brief Byte stride implied by the buffer, or 0 when the file disagrees
    ///        with itself. Compare against `grFvfDdiStride(fvf)`.
    uint32_t stride() const {
        return vertexCount ? (uint32_t)(vertexBytes.size() / vertexCount) : 0;
    }
};
// shaderPassFlags decoded from the engine's own draw path (Gw2-64
// sub_140AAFDB0, BgfxDraw.cpp). It is a bag of RENDER-TARGET/DEPTH bits, which
// is why it never worked as an opaque/transparent or colour/prepass label:
//
//   0x0004  colour write mask = 0   -> depth-only pass, never a colour shader
//   0x0008  do not write RGB
//   0x0020  depth test off
//   0x0040  depth write off         (the translucent marker)
//   0x4000  do not write ALPHA      -> D3D11 write mask 7
//   0x8000  depth bias -65          (decal push)
//
// The engine builds the write mask as
//     (flags & 4) ? 0 : ((noRGB ? 0 : 7) | (noAlpha ? 0 : 8))
// and takes blending purely from the effect's renderState.
enum : uint32_t {
    kAmatPassNoColor = 0x0004,
    kAmatPassNoRGB   = 0x0008,
    kAmatPassNoDepthTest = 0x0020,
    kAmatPassNoDepthWrite = 0x0040,
    kAmatPassNoAlpha = 0x4000,
    kAmatPassDepthBias = 0x8000,
};
// The old note, kept because the conclusion still stands for effect SELECTION:
// bit 0x4000 marks the colour effects in some AMATs (135429:
// colour=0xc041, prepass=0x8041) and is absent from the colour effects in others
// (643276: colour=0x1). Measured over 40 AMATs / 999 pixel shaders it disagreed
// with the shaders' own content 48 times out of 80. The split is therefore taken
// from the effect's bgfx renderState blend bits instead -- see isBlendState.
inline bool isBlendState(uint64_t st) { return ((st >> 12) & 0xffffu) != 0; }
// BGFX_STATE_BLEND_MASK -- the four blend-factor nibbles at BLEND_SHIFT 12.
inline constexpr uint64_t kBgfxBlendMask = 0x000000000ffff000ull;
// Does the COLOUR (rgb) side of this blend read the source alpha? Only these
// factors turn the pixel shader's o0.w into coverage; ONE/INV_SRC_COLOR-style
// premultiplied blends do not care what alpha holds. Table indices match
// bgfx's s_blendFactor: 5 = SRC_ALPHA, 6 = INV_SRC_ALPHA, 11 = SRC_ALPHA_SAT.
inline bool blendFactorUsesSrcAlpha(uint32_t f) { return f == 5 || f == 6 || f == 11; }
inline bool blendReadsSrcAlpha(uint64_t st) {
    uint32_t blend = (uint32_t)((st >> 12) & 0xffffu);
    return blendFactorUsesSrcAlpha(blend & 0xf) || blendFactorUsesSrcAlpha((blend >> 4) & 0xf);
}

// --- effect selection by token (the engine's own rule) ---------------------
//
// `sub_140BFAD20` (BgfxShader.cpp ~1069-1200) picks the effect inside
// `techniques[quality].passes[passIdx]` by looking its **token** up
// (`sub_140BF4630(pass, token, &idx)`) -- never by sampler count or shader
// content. The token comes from the draw object at +64, which is
// `ModelMaterialDataV*::token`.
//
// A token that is not present in the pass falls through a hardcoded remap chain
// and finally to the default below. That default is not a rare edge case: in
// every AMAT sampled it is `effects[0]`, and on AMAT 19896 it resolves to
// pixelShaderIndex 3 -- independently confirmed as that material's real colour
// shader. So honouring the default alone already beats guessing.
inline constexpr uint64_t kAmatDefaultEffectToken = 183330ull;

/// @brief Decode a base-23 packed GW2 token32 to its lowercase name.
///
/// Engine: `Token::Decode` (sub_140E3B360, Token.cpp:30). The alphabet has 23
/// letters -- no j/q/z -- and the subtraction wraps unsigned exactly as there.
/// Verified against known values: 805394902 -> "high", 881522146 -> "medium",
/// 805317257 -> "low".
inline std::string decodeToken23(uint32_t token) {
    static const char* kAlpha = "abcdefghiklmnopvrstuwxy";
    uint32_t v = token - 0x30000000u;
    std::string out;
    while (v) { out.push_back(kAlpha[v % 23]); v /= 23; }
    return out;
}

/// @brief Rank an AMAT technique's quality token; higher is better.
///
/// `techniques[]` are QUALITY LEVELS, not passes (selector `sub_140BFBFD0`
/// against GR_SHADER_QUALITIES; the game picks one via `m_techniqueIndex` from
/// the graphics settings). A viewer has no such slider, so it takes the best on
/// offer. Unknown tokens rank lowest but stay usable.
inline int amatQualityRank(uint32_t qualityToken) {
    const std::string q = decodeToken23(qualityToken);
    if (q == "ultra")  return 4;
    if (q == "high")   return 3;
    if (q == "medium") return 2;
    if (q == "low")    return 1;
    return 0;
}

/// @brief One hop of the engine's hardcoded effect-token fallback chain.
/// @return The token to try next, or ::kAmatDefaultEffectToken to stop.
inline uint64_t amatRemapEffectToken(uint64_t t) {
    switch (t) {
        case 0x1481311ECull:       return 0x51C59061370654ull;
        case 0x1779028991ECull:    return 0x5DE40A268A40A4ull;
        case 0x2C26C0B64F711ECull: return 0x6584D816C9EEull;
        default:                   return kAmatDefaultEffectToken;
    }
}

// The uniforms that only a forward-lit GW2 material shader declares: the SH
// ambient rig plus the sun. Verified across 40 AMATs -- 291 shaders carry these
// and not one of them is a normal prepass.
inline bool isShLightingUniform(const std::string& n) {
    return n == "shRed" || n == "shGreen" || n == "shBlue" ||
           n == "shSun" || n == "shSunColor" || n == "shSunData";
}


// --- shader role classification -------------------------------------------
//
// Walks the DXBC container of a bgfx blob and reports whether its pixel shader
// ends by writing an ENCODED WORLD NORMAL, i.e.
//
//     mad o0.xyz, r?.xyzx, l(0.5,0.5,0.5,0.0), l(0.5,0.5,0.5,0.0)
//
// which is the signature of GW2's deferred normal G-buffer prepass (frame pass 1
// in a real capture; its output feeds the light-accumulation pass that every
// colour material then samples). Such a shader must never be chosen to draw a
// model -- doing so paints the normal buffer, which reads as flat green/yellow.
//
// Detected on the raw SHEX token stream rather than by disassembling: an
// immediate32 4-component operand is 16 bytes of float, and the two literals in
// that `mad` are separated by exactly one 4-byte operand token, so the pattern is
// two copies of {0.5,0.5,0.5,0.0} exactly 20 bytes apart. Validated against
// D3DDisassemble output over 999 pixel shaders from 40 AMATs: 91 prepass, 908
// not, ZERO disagreements.
inline bool dxbcWritesNormalEncode(const uint8_t* d, size_t n) {
    if (!d || n < 32 || std::memcmp(d, "DXBC", 4) != 0) return false;
    uint32_t chunkCount;
    std::memcpy(&chunkCount, d + 28, 4);
    if (chunkCount > 32 || 32 + (size_t)chunkCount * 4 > n) return false;
    static const uint8_t kHalfHalfHalfZero[16] = {
        0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f,
        0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x00};
    for (uint32_t i = 0; i < chunkCount; ++i) {
        uint32_t off;
        std::memcpy(&off, d + 32 + i * 4, 4);
        if ((size_t)off + 8 > n) continue;
        if (std::memcmp(d + off, "SHEX", 4) != 0 && std::memcmp(d + off, "SHDR", 4) != 0) continue;
        uint32_t sz;
        std::memcpy(&sz, d + off + 4, 4);
        const uint8_t* b = d + off + 8;
        if ((size_t)off + 8 + sz > n) sz = (uint32_t)(n - off - 8);
        if (sz < 36) continue;
        for (size_t p = 0; p + 36 <= sz; ++p) {
            if (std::memcmp(b + p, kHalfHalfHalfZero, 16) == 0 &&
                std::memcmp(b + p + 20, kHalfHalfHalfZero, 16) == 0)
                return true;
        }
    }
    return false;
}

// Does this shader contain a `discard`? Walks the SHEX/SHDR token stream (each
// instruction token carries its own length in DWORDs at bits 24..30) looking for
// D3D10_SB_OPCODE_DISCARD. Validated against D3DDisassemble over 2198 shaders from
// 30 AMATs: 530 with a discard, 1668 without, zero disagreements.
//
// Used to decide whether a material's NORMAL PREPASS really cuts geometry away.
// That cannot be inferred from the albedo's alpha histogram: plenty of GW2 materials
// put a GLOSS map in alpha (the shader feeds it to `pow` as a specular exponent), and
// a gloss map is mostly-low and near-binary, so a histogram test calls it a coverage
// mask and the prepass then punches the surface full of holes.
inline bool dxbcHasDiscard(const uint8_t* d, size_t n) {
    if (!d || n < 32 || std::memcmp(d, "DXBC", 4) != 0) return false;
    uint32_t chunkCount;
    std::memcpy(&chunkCount, d + 28, 4);
    if (chunkCount > 32 || 32 + (size_t)chunkCount * 4 > n) return false;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        uint32_t off;
        std::memcpy(&off, d + 32 + i * 4, 4);
        if ((size_t)off + 8 > n) continue;
        if (std::memcmp(d + off, "SHEX", 4) != 0 && std::memcmp(d + off, "SHDR", 4) != 0) continue;
        uint32_t sz;
        std::memcpy(&sz, d + off + 4, 4);
        if ((size_t)off + 8 + sz > n) sz = (uint32_t)(n - off - 8);
        const uint8_t* b = d + off + 8;
        size_t words = sz / 4;
        for (size_t w = 2; w < words;) {   // skip the version + length header tokens
            uint32_t tok;
            std::memcpy(&tok, b + w * 4, 4);
            uint32_t op = tok & 0x7ff;
            uint32_t len = (tok >> 24) & 0x7f;
            if (len == 0) break;
            if (op == 13) return true;     // D3D10_SB_OPCODE_DISCARD
            w += len;
        }
    }
    return false;
}

// Is this pixel shader's SV_TARGET alpha a CONSTANT rather than a computed opacity?
//
// GW2's ordinary world materials end with `mov o0.w, cb0[k].x` -- the engine's
// StencilId / submodel id, an IDENTIFIER the later passes read, not coverage.
// Confirmed against a real frame (sample1.rdc, material pass eid 1650, PS 17726:
// instruction 33 is exactly `mov o0.w, cb0[1].x`) where the game draws that shader
// with BLENDING DISABLED and write mask 15. Blending such a shader by a
// SRC_ALPHA-family factor multiplies the whole surface by an id -- typically a
// fraction near zero -- and the model turns uniformly see-through. That is the
// "everything went transparent" failure, and this is the test that tells the two
// kinds of alpha apart. A shader that computes its alpha (glass fresnel, particle
// falloff, an alpha-cutout mask) is genuinely translucent and keeps its blend.
//
// Walks the SHEX/SHDR token stream: every instruction whose FIRST (destination)
// operand is an OUTPUT register with the .w component in its write mask must be a
// `mov` sourced from a constant buffer or an immediate. Anything else -- a `mul`,
// a `mad`, a `mov` from a temp -- means alpha is computed. A shader that never
// writes o#.w leaves alpha undefined, which is equally not an opacity, so it
// counts as constant too. Anything the scanner cannot decode with certainty
// (extended operand tokens, non-immediate register indices) returns false, i.e.
// keeps the material's declared blend -- the conservative direction.
inline bool dxbcAlphaIsConstant(const uint8_t* d, size_t n) {
    if (!d || n < 32 || std::memcmp(d, "DXBC", 4) != 0) return false;
    uint32_t chunkCount;
    std::memcpy(&chunkCount, d + 28, 4);
    if (chunkCount > 32 || 32 + (size_t)chunkCount * 4 > n) return false;
    enum : uint32_t { kOpMov = 54, kOpDclOutput = 101, kOpDclOutputSgv = 102, kOpDclOutputSiv = 103 };
    enum : uint32_t { kTypeImmediate32 = 4, kTypeOutput = 2, kTypeConstantBuffer = 8 };
    for (uint32_t i = 0; i < chunkCount; ++i) {
        uint32_t off;
        std::memcpy(&off, d + 32 + i * 4, 4);
        if ((size_t)off + 8 > n) continue;
        if (std::memcmp(d + off, "SHEX", 4) != 0 && std::memcmp(d + off, "SHDR", 4) != 0) continue;
        uint32_t sz;
        std::memcpy(&sz, d + off + 4, 4);
        if ((size_t)off + 8 + sz > n) sz = (uint32_t)(n - off - 8);
        const uint8_t* b = d + off + 8;
        size_t words = sz / 4;
        auto word = [&](size_t w) { uint32_t v = 0; std::memcpy(&v, b + w * 4, 4); return v; };
        for (size_t w = 2; w + 1 < words;) {   // skip the version + length header tokens
            uint32_t tok = word(w);
            uint32_t op = tok & 0x7ff;
            uint32_t len = (tok >> 24) & 0x7f;
            if (len == 0 || w + len > words) break;
            if (op == kOpDclOutput || op == kOpDclOutputSgv || op == kOpDclOutputSiv) { w += len; continue; }
            uint32_t dst = word(w + 1);
            uint32_t dstType = (dst >> 12) & 0xff;
            uint32_t selMode = (dst >> 2) & 3;
            uint32_t mask = (dst >> 4) & 0xf;
            // Not a write to o#.w -> irrelevant to the alpha question.
            if (dstType != kTypeOutput || selMode != 0 || !(mask & 0x8)) { w += len; continue; }
            if (op != kOpMov) return false;              // alpha is computed
            if (dst & 0x80000000u) return false;         // extended dest token: cannot locate the source
            if (((dst >> 20) & 3) != 1) return false;    // expect exactly one register index
            if (((dst >> 22) & 7) != 0) return false;    // ... encoded as a plain immediate32
            if (w + 3 >= words) return false;
            uint32_t src = word(w + 3);                  // opcode, dest token, dest index, source token
            uint32_t srcType = (src >> 12) & 0xff;
            if (srcType != kTypeConstantBuffer && srcType != kTypeImmediate32) return false;
            w += len;
        }
    }
    return true;
}

// Reads a bgfx shader blob's uniform-name table and locates the inner DXBC, then
// fills in the AmatShader's role flags. Mirrors the blob layout parsed in
// castlemist's game_shader.cpp::parse_bgfx (magic+ver, hashes, uniform table,
// codeSize, DXBC, attrs, constBufSize) but reads only what classification needs.
inline void classifyShader(AmatShader& sh) {
    const std::vector<uint8_t>& d = sh.dxbc;
    for (const auto& b : sh.samplers)
        if (b.textureSlot == 0) { sh.samplesSlot0 = true; break; }
    if (d.size() < 8 || (d[0] != 'V' && d[0] != 'F' && d[0] != 'C')) return;
    size_t p = 3;
    int ver = d[p++];
    p += 4;
    if (ver >= 6) p += 4;
    if (p + 2 > d.size()) return;
    uint16_t cnt = (uint16_t)(d[p] | (d[p + 1] << 8));
    p += 2;
    for (int i = 0; i < cnt; ++i) {
        if (p >= d.size()) return;
        int nl = d[p++];
        if (p + (size_t)nl > d.size()) return;
        std::string nm((const char*)&d[p], (size_t)nl);
        p += nl;
        if (p + 6 > d.size()) return;
        p += 2;      // type, pad
        p += 2 + 2;  // reg, regCount
        if (ver >= 8) p += 2;
        if (ver >= 10) p += 2;
        if (isShLightingUniform(nm)) sh.hasShLighting = true;
        if (nm == "grbones") sh.usesBones = true;
    }
    if (p + 4 > d.size()) return;
    uint32_t cs = (uint32_t)(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24));
    p += 4;
    if (p + cs > d.size()) return;
    sh.writesNormalEncode = dxbcWritesNormalEncode(d.data() + p, cs);
    sh.hasDiscard = dxbcHasDiscard(d.data() + p, cs);
    sh.alphaIsConstant = dxbcAlphaIsConstant(d.data() + p, cs);
}

// --------------------------------------------------------------------------
class Extractor {
public:
    Extractor(const std::vector<uint8_t>& data, const json& tpl)
        : d_(data.data()), n_(data.size()), tpl_(tpl) {
        if (n_ < 12 || d_[0] != 'P' || d_[1] != 'F') throw std::runtime_error("not a PF packfile");
        types_ = tpl_.contains("types") ? &tpl_["types"] : nullptr;
        if (!types_) throw std::runtime_error("template missing 'types'");
        uint16_t pfv = rd16(2);
        ptr_ = (pfv & 4) ? 8 : 4;
        char c[5] = {0};
        std::memcpy(c, d_ + 8, 4);
        container_ = c;
    }

    Model extract() {
        Model m;
        size_t geom = findChunk("GEOM"), modl = findChunk("MODL");
        if (modl) parseMaterials(modl, m);
        if (geom) parseGeometry(geom, m);
        try { if (modl) parseCloth(modl, m); } catch (const std::exception&) { /* cloth is optional */ }
        try { parseSkeleton(m); } catch (const std::exception&) { /* skeleton is optional */ }
        try { parseAnim(m); }     catch (const std::exception&) { /* animation is optional */ }
        try { parseEffects(m); }  catch (const std::exception&) { /* effects are optional */ }
        resolveBoneBindings(m);
        return m;
    }

    // Map zone model: a zone's def model placed at the zone footprint centroid.
    // (Approximation of GW2's zone scatter system -- one representative placement
    // per zone rather than the full per-instance scatter.)
    struct MapZoneInst {
        uint32_t fileId = 0;
        float pos[3] = {0, 0, 0};
        float scale = 1.0f;
    };

    std::vector<MapZoneInst> parseMapZones() {
        std::vector<MapZoneInst> out;
        std::string root; uint16_t ver = 0;
        size_t z = findChunk("zon2", &root, &ver);
        if (!z || root.empty()) return out;
        size_t off; json f;

        // zoneDefArray: token -> def model fileId.
        std::unordered_map<uint32_t, uint32_t> defFile;
        if (fieldOffset(root, "zoneDefArray", off, f)) {
            std::string dt = f["element"].value("struct", std::string());
            int ds = typeSize(dt);
            uint32_t n = 0; size_t base = arrayAt(z + off, n);
            for (uint32_t i = 0; base && ds > 0 && i < n; ++i) {
                size_t e = base + (size_t)i * ds;
                size_t o; json ff;
                uint32_t tok = fieldOffset(dt, "token", o, ff) ? rd32(e + o) : 0;
                uint32_t fid = decodeFilename(dt, e);
                if (tok) defFile[tok] = fid;
            }
        }
        // zoneArray: each zone -> its def model at the polygon centroid + zPos.
        if (fieldOffset(root, "zoneArray", off, f)) {
            std::string zt = f["element"].value("struct", std::string());
            int zs = typeSize(zt);
            uint32_t n = 0; size_t base = arrayAt(z + off, n);
            for (uint32_t i = 0; base && zs > 0 && i < n; ++i) {
                size_t e = base + (size_t)i * zs;
                size_t o; json ff;
                uint32_t tok = fieldOffset(zt, "defToken", o, ff) ? rd32(e + o) : 0;
                auto it = defFile.find(tok);
                if (it == defFile.end() || !it->second) continue;
                float zpos = fieldOffset(zt, "zPos", o, ff) ? rdf(e + o) : 0;
                float cx = 0, cy = 0; uint32_t vc = 0;
                if (fieldOffset(zt, "vertices", o, ff)) {
                    uint32_t vn = 0; size_t vb = arrayAt(e + o, vn);
                    for (uint32_t v = 0; vb && v < vn; ++v) { cx += rdf(vb + 8u*v); cy += rdf(vb + 8u*v + 4); ++vc; }
                }
                MapZoneInst mz;
                mz.fileId = it->second;
                mz.pos[0] = vc ? cx / vc : 0; mz.pos[1] = vc ? cy / vc : 0; mz.pos[2] = zpos;
                out.push_back(mz);
            }
        }
        return out;
    }

    // Map collision: the Havok collision meshes (walls/floors/ramps) stored in the
    // `havk` chunk as plain vertex+index geometry, plus the real water level.
    struct MapCollision {
        bool present = false;
        float waterZ = 0; bool hasWater = false;
        std::vector<float> verts;      // x,y,z triples (Z-up, world space)
        std::vector<uint32_t> indices; // triangle list
    };

    MapCollision parseMapCollision() {
        MapCollision out;
        std::string root; uint16_t ver = 0;
        size_t h = findChunk("havk", &root, &ver);
        if (!h || root.empty()) return out;
        size_t off; json f;
        if (fieldOffset(root, "waterSurfaceZ", off, f)) { out.waterZ = rdf(h + off); out.hasWater = true; }
        if (!fieldOffset(root, "collisions", off, f)) return out;
        std::string ct = f["element"].value("struct", std::string());
        int cs = typeSize(ct);
        uint32_t cn = 0; size_t cbase = arrayAt(h + off, cn);
        if (cn > 100000) cn = 100000;
        for (uint32_t i = 0; cbase && cs > 0 && i < cn; ++i) {
            size_t ce = cbase + (size_t)i * cs;
            size_t vo, io; json vf, iff;
            if (!fieldOffset(ct, "vertices", vo, vf) || !fieldOffset(ct, "indices", io, iff)) continue;
            uint32_t vn = 0; size_t vbase = arrayAt(ce + vo, vn);
            uint32_t in = 0; size_t ibase = arrayAt(ce + io, in);
            if (!vbase || !ibase || !vn || !in) continue;
            uint32_t voff = (uint32_t)(out.verts.size() / 3);
            for (uint32_t v = 0; v < vn; ++v) {
                out.verts.push_back(rdf(vbase + 12u * v));
                out.verts.push_back(rdf(vbase + 12u * v + 4));
                out.verts.push_back(rdf(vbase + 12u * v + 8));
            }
            for (uint32_t k = 0; k < in; ++k) out.indices.push_back(voff + rd16(ibase + 2u * k));
        }
        out.present = !out.verts.empty() && !out.indices.empty();
        return out;
    }

    // Map environment lighting: the dominant sun + accumulated sky/fill lights of
    // the `env` chunk's day preset. Reconstructs the real per-map light rig the
    // game feeds its lighting shaders. Path (all template-navigated, so field
    // packing is exact): env -> dataGlobal -> PackMapEnvDataGlobalV* -> lighting[]
    // (time-of-day presets) -> [preset].lights[] (ptr array) ->
    // PackMapEnvDataLightV*{ color byte3, intensity float, direction float3 }.
    struct MapEnvLight {
        bool  present = false;
        float sunDir[3] = {0.45f, 0.80f, 0.40f};
        float sunColor[3] = {1.0f, 0.91f, 0.75f};
        float sunIntensity = 1.0f;
        float fillColor[3] = {0.49f, 0.68f, 0.97f};
        float fillIntensity = 0.3f;
        int   lightCount = 0;
    };

    // preset: a specific time-of-day index (0..2), or -1 (default) to AUTO-select
    // the brightest preset -- i.e. the one whose dominant light is strongest, which
    // is the daytime rig (night/off presets have near-zero or all-zero intensities).
    MapEnvLight parseMapEnv(int preset = -1) {
        MapEnvLight out;
        std::string root; uint16_t ver = 0;
        size_t env = findChunk("env", &root, &ver);
        if (!env || root.empty()) return out;
        size_t off; json f;
        if (!fieldOffset(root, "dataGlobal", off, f)) return out;
        size_t g = follow(env + off);
        if (!g) return out;
        std::string globalType = resolveVariant("PackMapEnvDataGlobal", ver);
        if (globalType.empty() || !fieldOffset(globalType, "lighting", off, f)) return out;
        std::string lightingType = f["element"].value("struct", std::string());
        int lightingSize = typeSize(lightingType);
        uint32_t nPresets = 0; size_t lbase = arrayAt(g + off, nPresets);
        if (!lbase || lightingSize <= 0 || nPresets == 0) return out;

        size_t loff; json lf;
        if (!fieldOffset(lightingType, "lights", loff, lf)) return out;
        std::string lightType = lf["element"].value("struct", std::string());
        size_t lco, ico, dco; json cf, iff, df;
        if (!fieldOffset(lightType, "color", lco, cf) ||
            !fieldOffset(lightType, "intensity", ico, iff) ||
            !fieldOffset(lightType, "direction", dco, df)) return out;

        struct L { float c[3], dir[3], inten; };
        // Read one preset's lights into `lights`; returns the max intensity found.
        auto readPreset = [&](uint32_t pi, std::vector<L>& lights) -> float {
            lights.clear();
            size_t elem = lbase + (size_t)pi * lightingSize;
            uint32_t nLights = 0; size_t pbase = arrayAt(elem + loff, nLights);
            if (!pbase || nLights == 0) return -1;
            if (nLights > 256) nLights = 256;
            float mx = -1;
            for (uint32_t i = 0; i < nLights; ++i) {
                size_t lp = follow(pbase + (size_t)i * ptr_);
                if (!lp) continue;
                L l;
                try {
                    l.c[0] = rd8(lp + lco) / 255.f; l.c[1] = rd8(lp + lco + 1) / 255.f; l.c[2] = rd8(lp + lco + 2) / 255.f;
                    l.inten = rdf(lp + ico);
                    l.dir[0] = rdf(lp + dco); l.dir[1] = rdf(lp + dco + 4); l.dir[2] = rdf(lp + dco + 8);
                } catch (const std::exception&) { continue; }
                if (!(l.inten == l.inten)) continue; // NaN guard
                lights.push_back(l);
                if (l.inten > mx) mx = l.inten;
            }
            return mx;
        };

        std::vector<L> lights;
        if (preset >= 0 && (uint32_t)preset < nPresets) {
            readPreset((uint32_t)preset, lights);
        } else {
            // Auto: pick the brightest preset (daytime).
            float best = -1; uint32_t bestPi = 0; std::vector<L> tmp;
            for (uint32_t pi = 0; pi < nPresets; ++pi) {
                float mx = readPreset(pi, tmp);
                if (mx > best) { best = mx; bestPi = pi; }
            }
            readPreset(bestPi, lights);
        }

        int bestIdx = -1; float bestI = -1;
        for (int i = 0; i < (int)lights.size(); ++i)
            if (lights[i].inten > bestI) { bestI = lights[i].inten; bestIdx = i; }
        if (bestIdx < 0) return out;

        const L& sun = lights[bestIdx];
        for (int k = 0; k < 3; ++k) { out.sunColor[k] = sun.c[k]; out.sunDir[k] = sun.dir[k]; }
        out.sunIntensity = sun.inten;
        float len = std::sqrt(out.sunDir[0]*out.sunDir[0] + out.sunDir[1]*out.sunDir[1] + out.sunDir[2]*out.sunDir[2]);
        if (len > 1e-6f) for (int k = 0; k < 3; ++k) out.sunDir[k] /= len;

        float fc[3] = {0, 0, 0}, fw = 0;
        for (int i = 0; i < (int)lights.size(); ++i) {
            if (i == bestIdx) continue;
            float w = lights[i].inten > 0 ? lights[i].inten : 0;
            fc[0] += lights[i].c[0]*w; fc[1] += lights[i].c[1]*w; fc[2] += lights[i].c[2]*w; fw += w;
        }
        if (fw > 1e-6f) { for (int k = 0; k < 3; ++k) out.fillColor[k] = fc[k] / fw; out.fillIntensity = fw; }
        else out.fillIntensity = 0;
        out.lightCount = (int)lights.size();
        out.present = true;
        return out;
    }

    // Map prop placement: one placed model instance in an `area` (map) packfile.
    struct MapProp {
        uint32_t fileId = 0;   // model .modl fileId (0 = none)
        float pos[3] = {0, 0, 0};
        float rot[3] = {0, 0, 0}; // Euler angles (radians)
        float scale = 1.0f;
        float bounds[4] = {0, 0, 0, 0};
    };

    // Map terrain: a height-map grid (GW2 is Z-up, so heights are the Z axis).
    struct MapTerrain {
        bool present = false;
        uint32_t dimX = 0, dimY = 0;      // grid dimensions (tiles)
        float swapDistance = 0;
        uint32_t vertsPerChunkSide = 0;
        std::vector<float> heights;       // row-major height samples
        float rect[4] = {0, 0, 0, 0};     // map world rect (x0,y0,x1,y1) from the parm chunk
        bool hasRect = false;
    };

    MapTerrain parseTerrain() {
        MapTerrain out;
        // Map world rect (terrain placement) from the parm chunk.
        {
            std::string proot; uint16_t pver = 0;
            size_t parm = findChunk("parm", &proot, &pver);
            size_t po; json pf;
            if (parm && !proot.empty() && fieldOffset(proot, "rect", po, pf)) {
                for (int k = 0; k < 4; ++k) out.rect[k] = rdf(parm + po + 4 * k);
                out.hasRect = true;
            }
        }
        std::string root; uint16_t ver = 0;
        size_t trn = findChunk("trn", &root, &ver);
        if (!trn || root.empty()) return out;
        size_t off; json f;
        if (fieldOffset(root, "dims", off, f)) { out.dimX = rd32(trn + off); out.dimY = rd32(trn + off + 4); }
        if (fieldOffset(root, "swapDistance", off, f)) out.swapDistance = rdf(trn + off);
        if (fieldOffset(root, "verticesPerChunkSide", off, f)) out.vertsPerChunkSide = rd32(trn + off);
        if (fieldOffset(root, "heightMapArray", off, f)) {
            uint32_t n = 0; size_t base = arrayAt(trn + off, n);
            if (n > 16u * 1024 * 1024) n = 16u * 1024 * 1024;
            out.heights.reserve(n);
            for (uint32_t i = 0; base && i < n; ++i) out.heights.push_back(rdf(base + 4u * i));
        }
        out.present = !out.heights.empty() && out.dimX > 0 && out.dimY > 0;
        return out;
    }

    // Parse the `prp2` chunk of an `area` packfile -> the list of placed props
    // (model + world transform). This is what turns a map into a coordinated set
    // of 3D models. Handles both propArray (single placements) and
    // propInstanceArray (a prop reused at many transforms).
    std::vector<MapProp> parseMapProps() {
        std::vector<MapProp> out;
        std::string root; uint16_t ver = 0;
        size_t prp = findChunk("prp2", &root, &ver);
        if (!prp || root.empty()) return out;

        auto readProp = [&](const std::string& objType, size_t e, MapProp& p) {
            size_t o; json f;
            p.fileId = decodeFilename(objType, e);
            if (fieldOffset(objType, "position", o, f)) for (int k=0;k<3;++k) p.pos[k] = rdf(e+o+4*k);
            if (fieldOffset(objType, "rotation", o, f)) for (int k=0;k<3;++k) p.rot[k] = rdf(e+o+4*k);
            if (fieldOffset(objType, "scale", o, f)) p.scale = rdf(e+o);
            if (fieldOffset(objType, "bounds", o, f)) for (int k=0;k<4;++k) p.bounds[k] = rdf(e+o+4*k);
        };

        size_t off; json fj;
        // propArray -- one model instance each.
        if (fieldOffset(root, "propArray", off, fj)) {
            std::string objType = fj["element"].value("struct", std::string());
            int objSize = typeSize(objType);
            uint32_t n = 0; size_t base = arrayAt(prp + off, n);
            for (uint32_t i = 0; base && objSize > 0 && i < n; ++i) {
                MapProp p; readProp(objType, base + (size_t)i * objSize, p);
                if (p.fileId) out.push_back(p);
            }
        }
        // propInstanceArray -- a base prop plus a list of extra transforms.
        if (fieldOffset(root, "propInstanceArray", off, fj)) {
            std::string instType = fj["element"].value("struct", std::string());
            int instSize = typeSize(instType);
            uint32_t n = 0; size_t base = arrayAt(prp + off, n);
            for (uint32_t i = 0; base && instSize > 0 && i < n; ++i) {
                size_t e = base + (size_t)i * instSize;
                MapProp p; readProp(instType, e, p);
                if (!p.fileId) continue;
                out.push_back(p); // the base placement
                // Extra transforms[] (position/rotation/scale) reusing the same model.
                size_t to; json tf;
                if (fieldOffset(instType, "transforms", to, tf)) {
                    std::string trType = tf["element"].value("struct", std::string());
                    int trSize = typeSize(trType);
                    uint32_t tn = 0; size_t tb = arrayAt(e + to, tn);
                    for (uint32_t j = 0; tb && trSize > 0 && j < tn; ++j) {
                        size_t te = tb + (size_t)j * trSize;
                        MapProp q; q.fileId = p.fileId;
                        size_t o; json f;
                        if (fieldOffset(trType, "position", o, f)) for (int k=0;k<3;++k) q.pos[k] = rdf(te+o+4*k);
                        if (fieldOffset(trType, "rotation", o, f)) for (int k=0;k<3;++k) q.rot[k] = rdf(te+o+4*k);
                        if (fieldOffset(trType, "scale", o, f)) q.scale = rdf(te+o);
                        out.push_back(q);
                    }
                }
            }
        }
        return out;
    }

    // Walk the BGFX chunk (AmatMaterialV*) of an AMAT packfile: extract every
    // shader's DXBC + sampler bindings, then resolve the effect that draws this
    // material to its vertex+pixel shader pair -- by the engine's token rule
    // where the AMAT allows it, else by shader content.
    /// @brief Resolve an AMAT to the shader pair that draws one material.
    /// @param materialToken `ModelMaterialDataV*::token` -- the key the engine
    ///        matches against `AmatEffectV1::token`. 0 = unknown, which still
    ///        tries the engine's default token before any heuristic.
    AmatSet extractAmat(uint64_t materialToken = 0) {
        AmatSet out;
        std::string root; uint16_t ver = 0;
        size_t bgfx = findChunk("BGFX", &root, &ver);
        if (!bgfx || root.empty()) { out.error = "no BGFX chunk / schema"; return out; }
        size_t off; json fj;

        // shaders[] (inline AmatShaderV*)
        if (fieldOffset(root, "shaders", off, fj)) {
            uint32_t n = 0; size_t base = arrayAt(bgfx + off, n);
            std::string shType = fj["element"].value("struct", std::string());
            int shSize = typeSize(shType);
            for (uint32_t i = 0; base && i < n; ++i) {
                size_t se = base + (size_t)i * shSize;
                AmatShader sh;
                size_t o; json f;
                if (fieldOffset(shType, "isPixelShader", o, f)) sh.isPixel = rd32(se + o) != 0;
                if (fieldOffset(shType, "dx11Shader", o, f)) {
                    std::string bin = f.value("type", std::string());
                    size_t bs = se + o, o2; json f2;
                    if (fieldOffset(bin, "data", o2, f2)) {
                        uint32_t dn = 0; size_t db = arrayAt(bs + o2, dn);
                        if (db && dn && db + dn <= n_) sh.dxbc.assign(d_ + db, d_ + db + dn);
                    }
                    if (fieldOffset(bin, "samplers", o2, f2)) {
                        std::string smT = f2["element"].value("struct", std::string());
                        int smSize = typeSize(smT);
                        uint32_t sn = 0; size_t sb = arrayAt(bs + o2, sn);
                        for (uint32_t j = 0; sb && j < sn; ++j) {
                            size_t se2 = sb + (size_t)j * smSize; size_t o3; json f3;
                            AmatSamplerBind b;
                            // token64 is a fixed 8 bytes, independent of the packfile's
                            // pointer width -- rd64, not rdPtr. (AMATs happen to be
                            // 64-bit PF v5 so rdPtr read it correctly, but relying on
                            // that couples an 8-byte field to an unrelated property.)
                            if (fieldOffset(smT, "token", o3, f3)) b.token = rd64(se2 + o3);
                            if (fieldOffset(smT, "textureIndex", o3, f3)) b.textureIndex = rd32(se2 + o3);
                            if (fieldOffset(smT, "textureSlot", o3, f3)) b.textureSlot = rd32(se2 + o3);
                            sh.samplers.push_back(b);
                        }
                    }
                    // constants[] (AmatShaderConstantV*) -- ordered token32 list that
                    // pairs with the shader's material-constant uniforms.
                    if (fieldOffset(bin, "constants", o2, f2)) {
                        std::string cT = f2["element"].value("struct", std::string());
                        int cSize = typeSize(cT);
                        uint32_t cn = 0; size_t cb2 = arrayAt(bs + o2, cn);
                        for (uint32_t j = 0; cb2 && cSize > 0 && j < cn; ++j) {
                            size_t ce = cb2 + (size_t)j * cSize; size_t o3; json f3;
                            if (fieldOffset(cT, "token", o3, f3)) sh.constantTokens.push_back(rd32(ce + o3));
                        }
                    }
                }
                classifyShader(sh);
                out.shaders.push_back(std::move(sh));
            }
        }

        // Enumerate ALL techniques[].passes[].effects[] and pick the effect
        // whose pixel shader has the MOST sampler bindings -- that's the color
        // pass (depth/shadow/pick pre-passes have few or no textures).
        auto iter = [&](const std::string& type, size_t start, const char* field,
                        std::string& elemType, int& elemSize, uint32_t& count) -> size_t {
            size_t o; json f; count = 0;
            if (!fieldOffset(type, field, o, f)) return 0;
            size_t base = arrayAt(start + o, count);
            elemType = f["element"].value("struct", std::string());
            elemSize = typeSize(elemType);
            return base;
        };
        // One plausible colour effect (see the tier selection after the walk).
        struct Cand {
            int ps, vs;
            uint64_t rstate;
            uint32_t pflags; // effect's shaderPassFlags (write mask / depth bits)
            int nsamp;   // texture bindings -> permutation richness
            bool lit;    // pixel shader declares the SH sun/ambient rig
            bool blend;  // renderState carries real bgfx blend bits
            uint64_t tok;    // AmatEffectV1::token -- the engine's selection key
            int qrank;       // quality of the technique this came from (higher = better)
        };
        std::vector<Cand> cands;
        std::string tT, pT, eT, vT; int tS = 0, pS = 0, eS = 0, vS = 0; uint32_t tN = 0, pN = 0, eN = 0, vN = 0;
        size_t tb = iter(root, bgfx, "techniques", tT, tS, tN);
        for (uint32_t ti = 0; tb && ti < tN; ++ti) {
            // techniques[] are quality levels; remember which one each candidate
            // came from so the tier filter below can keep only the best.
            int qrank = 0;
            { size_t qo; json qf;
              if (fieldOffset(tT, "quality", qo, qf)) qrank = amatQualityRank(rd32(tb + (size_t)ti * tS + qo)); }
            size_t pb = iter(tT, tb + (size_t)ti * tS, "passes", pT, pS, pN);
            out.techniqueCount = tN;
            if (pN > out.maxPassCount) out.maxPassCount = pN;
            for (uint32_t pi = 0; pb && pi < pN; ++pi) {
                size_t eb = iter(pT, pb + (size_t)pi * pS, "effects", eT, eS, eN);
                out.effectCount += eN;
                for (uint32_t ei = 0; eb && ei < eN; ++ei) {
                    size_t ee = eb + (size_t)ei * eS; size_t o; json f;
                    int ps = -1, vs = -1;
                    if (fieldOffset(eT, "pixelShaderIndex", o, f)) ps = (int)rd32(ee + o);
                    uint32_t spf = 0;
                    if (fieldOffset(eT, "shaderPassFlags", o, f)) spf = rd32(ee + o);
                    uint64_t etok = 0;
                    if (fieldOffset(eT, "token", o, f)) etok = rd64(ee + o);
                    if (getenv("AMATDBG")) { uint64_t rsv=0; size_t ro; json rf;
                        if (fieldOffset(eT,"renderState",ro,rf)) rsv=rd64(ee+ro);
                        std::fprintf(stderr,"[amat] tech%u pass%u eff%u ps=%d renderState=0x%llx passFlags=0x%x\n",
                            ti,pi,ei,ps,(unsigned long long)rsv,spf); }
                    // An effect lists several vertex-shader VARIANTS -- the same
                    // material compiled for different vertex feeds (static, GPU-skinned
                    // via the `grbones` palette, instanced, ...). Taking variant 0
                    // blindly can land on the skinned one, whose bone palette we cannot
                    // fill: the renderer CPU-skins into a plain position/normal/uv
                    // buffer before drawing, so the STATIC variant is the correct pair.
                    // Prefer a variant that does not read `grbones`, and only fall back
                    // to the first one if every variant does.
                    size_t vb = iter(eT, ee, "vertexShaderVariants", vT, vS, vN);
                    size_t vsel = vb;
                    if (vb && vN && fieldOffset(vT, "vertexShaderIndex", o, f)) {
                        vs = (int)rd32(vb + o);
                        for (uint32_t vi = 0; vi < vN; ++vi) {
                            int cand = (int)rd32(vb + (size_t)vi * vS + o);
                            if (cand < 0 || cand >= (int)out.shaders.size()) continue;
                            if (!out.shaders[cand].usesBones) { vs = cand; vsel = vb + (size_t)vi * vS; break; }
                        }
                    }
                    if (ps >= 0 && ps < (int)out.shaders.size() && vs >= 0) {
                        int ns = (int)out.shaders[ps].samplers.size();
                        uint64_t eff=0, var=0; size_t eo=0, vo=0; bool ef=false, vf=false;
                        if ((ef=fieldOffset(eT, "renderState", o, f))) { eo=o; eff=rd64(ee + o); }
                        if (vsel && (vf=fieldOffset(vT, "renderState", o, f))) { vo=o; var=rd64(vsel + o); }
                        uint64_t rstate = eff ? eff : var; // prefer whichever carries a nonzero bgfx state
                        // An AMAT carries every shader the engine binds for this
                        // material across the WHOLE frame, not just the one that paints
                        // it. Collect the plausible colour candidates here and choose
                        // between them after the walk (see the tier logic below); a
                        // shader is out of the running entirely if it
                        //   * writes an encoded world normal -> deferred G-buffer
                        //     prepass. Drawing with it paints normals as RGB: the flat
                        //     green/yellow look. AMAT 135429's prepass (ps 22) samples
                        //     slot 0 and binds three textures, so every "most samplers"
                        //     heuristic picked it over the real lit shader.
                        //   * has no slot-0 sampler -> depth-only / depth-peel pass.
                        //     Choosing one renders the model INVISIBLE.
                        // hasShLighting overrides writesNormalEncode. The byte scan is a
                        // pattern match, so a long colour shader can trip it by chance
                        // (AMAT 626493 ps 16 is a real backlit material whose 128
                        // instructions happen to contain the literal pair mid-body). A
                        // shader carrying the SH rig is a colour shader by construction --
                        // 291 of 291 such shaders across 40 AMATs are -- so trusting the
                        // rig over the pattern removes that false positive without losing
                        // a single true prepass (all 91 in the same sample are unlit).
                        const AmatShader& psh = out.shaders[ps];
                        // shaderPassFlags & 0x4 = "colour write mask 0". The engine
                        // submits those effects with no render-target write at all
                        // (Gw2-64 sub_140AAFDB0), i.e. they are depth-only /
                        // shadow / pick passes. Drawing with one paints nothing --
                        // or, once its output is no longer masked off, garbage.
                        // AMAT 511755 ships two of them (eff05/eff08, flags 0x5)
                        // that pass every content-based test below, so this has to
                        // be filtered on the flag.
                        // An effect can declare a SRC_ALPHA blend over a pixel shader
                        // whose alpha is a constant StencilId rather than an opacity.
                        // Blending that multiplies the whole surface by an id -- which
                        // is what turned solid geometry uniformly see-through. The
                        // engine draws exactly this combination with blending OFF
                        // (sample1.rdc material pass, PS 17726 `mov o0.w, cb0[1].x`,
                        // blend disabled, mask 15), so drop the blend nibbles and let
                        // the effect be what it really is: opaque.
                        if (psh.alphaIsConstant && blendReadsSrcAlpha(rstate)) rstate &= ~kBgfxBlendMask;
                        if (spf & kAmatPassNoColor) { /* depth-only pass */ }
                        else if (psh.samplesSlot0 && (psh.hasShLighting || !psh.writesNormalEncode))
                            cands.push_back({ps, vs, rstate, spf, ns, psh.hasShLighting, isBlendState(rstate), etok, qrank});
                        if (getenv("AMATDBG")) std::fprintf(stderr,
                            "[amat] effT=%s ef=%d@%zu=0x%llx  varT=%s vf=%d@%zu=0x%llx blend=%d "
                            "cand=%d normalPrepass=%d sh=%d slot0=%d nsamp=%d passFlags=0x%x\n",
                            eT.c_str(),ef,eo,(unsigned long long)eff, vT.c_str(),vf,vo,(unsigned long long)var,
                            (int)isBlendState(rstate), (int)(psh.samplesSlot0 && !psh.writesNormalEncode),
                            (int)psh.writesNormalEncode,(int)psh.hasShLighting,(int)psh.samplesSlot0,ns,spf);
                    }
                }
            }
        }

        // Choose between the candidates in two TIERS.
        //
        // Tier 1 -- any candidate whose pixel shader declares the SH sun/ambient rig.
        // That rig is what GW2's real forward material pass runs, and measured over
        // 40 AMATs / 999 pixel shaders not one shader carrying it is a prepass, so it
        // is a safe positive identification of "the shader that paints this material".
        //
        // Tier 2 -- only when the AMAT has no lit shader at all (unlit/effect
        // materials: additive particles, holograms, UI). Then fall back to sampler
        // count, as before. Restricting the fallback this way matters because the
        // leftovers are not harmless: AMAT 135429's ps 3 samples slot 0, is not a
        // normal prepass, and yet writes (depth, depth*depth, 0, 1) -- a shadow /
        // linear-depth moment pass that would draw the model as a flat depth ramp.
        // Ask the material's own normal-prepass shader whether it cuts. If several
        // prepass variants exist, any one that discards means the material is a
        // cutout material (the variants differ by quality, not by coverage).
        for (const auto& sh : out.shaders) {
            if (!sh.writesNormalEncode || sh.hasShLighting) continue;
            out.hasPrepass = true;
            if (sh.hasDiscard) { out.prepassCutout = true; break; }
        }

        // Engine-order effect selection. `sub_140BFAD20` matches the material's own
        // token against the pass's effects; a miss walks the hardcoded remap chain
        // and then lands on the default token. Do the same here and hand the tier
        // logic below only the effects the engine would actually have considered.
        // A materialToken of 0 (caller has none) still tries the default, which is
        // strictly better than going straight to content heuristics.
        // The engine resolves the technique FIRST (by quality) and only then looks
        // the effect up by token inside that technique's pass. Order matters: an
        // AMAT repeats the same effect tokens at every quality level with different
        // shaders -- AMAT 19896 carries the default token 183330 in all three
        // (ps 3 / 46 / 94) -- so matching across a pooled candidate list would let a
        // `low` shader win the tie on sampler count.
        int bestQ = -1;
        for (const auto& c : cands) if (c.qrank > bestQ) bestQ = c.qrank;
        out.selectedQuality = bestQ;
        std::vector<Cand> tier;
        for (const auto& c : cands) if (c.qrank == bestQ) tier.push_back(c);

        std::vector<Cand> picked;
        for (uint64_t want = materialToken, hop = 0; hop < 3 && picked.empty(); ++hop) {
            if (want) for (const auto& c : tier) if (c.tok == want) picked.push_back(c);
            if (!picked.empty()) break;
            uint64_t next = amatRemapEffectToken(want);
            if (next == want) break;
            want = next;
        }
        out.tokenMatched = !picked.empty();
        // No token match at all -> the AMAT genuinely has nothing the engine's rule
        // would select, so fall back to the content heuristic within the same tier.
        const std::vector<Cand>& sel = picked.empty() ? tier : picked;

        bool anyLit = false;
        for (const auto& c : sel) if (c.lit) { anyLit = true; break; }
        auto better = [&](const Cand& a, const Cand& b) {
            if (a.lit != b.lit) return a.lit;          // lit always wins
            return a.nsamp > b.nsamp;                  // then the richest permutation
        };
        const Cand* bestOpaque = nullptr;
        const Cand* bestBlend = nullptr;
        for (const auto& c : sel) {
            if (anyLit && !c.lit) continue;
            const Cand*& slot = c.blend ? bestBlend : bestOpaque;
            if (!slot || better(c, *slot)) slot = &c;
        }
        // The opaque/blended split comes from the effect's own bgfx renderState blend
        // bits, NOT from shaderPassFlags -- see the isBlendState note above.
        if (bestOpaque) {
            out.psIndex = bestOpaque->ps; out.vsIndex = bestOpaque->vs;
            out.renderState = bestOpaque->rstate; out.passFlags = bestOpaque->pflags;
        }
        if (bestBlend) {
            out.hasTrans = true;
            out.transPsIndex = bestBlend->ps; out.transVsIndex = bestBlend->vs;
            out.transRenderState = bestBlend->rstate; out.transPassFlags = bestBlend->pflags;
        }
        // Many AMATs define only one colour family. If it happens to be the blended
        // one, it is still this material's shader -- use it for the opaque path too
        // rather than leaving the material unrenderable.
        if (out.psIndex < 0 && out.hasTrans) {
            out.psIndex = out.transPsIndex; out.vsIndex = out.transVsIndex;
            out.renderState = out.transRenderState; out.passFlags = out.transPassFlags;
        }
        return out;
    }

    // -----------------------------------------------------------------------
    /// @brief Parse the AMAT's BGFX chunk into the whole technique/pass/effect
    ///        tree, deciding nothing.
    ///
    /// The counterpart to ::extractAmat, which flattens the same data down to
    /// one heuristically chosen vs/ps pair. Use this when you have the MODL
    /// material's token64 and can run the engine's own selection instead.
    // -----------------------------------------------------------------------
    AmatTree extractAmatTree() {
        AmatTree out;
        std::string root; uint16_t ver = 0;
        size_t bgfx = findChunk("BGFX", &root, &ver);
        if (!bgfx || root.empty()) { out.error = "no BGFX chunk / schema"; return out; }
        size_t off; json fj;

        // Reads an array_ptr field: returns the element base, and fills the
        // element type name, element stride and count. Zero base means empty.
        auto arrayField = [&](const std::string& type, size_t start, const char* field,
                              std::string& elemType, int& elemSize, uint32_t& count) -> size_t {
            size_t o; json f; count = 0; elemSize = 0; elemType.clear();
            if (!fieldOffset(type, field, o, f)) return 0;
            size_t base = arrayAt(start + o, count);
            if (f.contains("element") && f["element"].is_object())
                elemType = f["element"].value("struct", std::string());
            elemSize = elemType.empty() ? 0 : typeSize(elemType);
            return base;
        };

        // --- shaders[] -----------------------------------------------------
        {
            std::string shType; int shSize = 0; uint32_t n = 0;
            size_t base = arrayField(root, bgfx, "shaders", shType, shSize, n);

            // One AmatShaderBinaryV* -> AmatShaderBinaryRaw.
            auto readBinary = [&](const std::string& binType, size_t bs, AmatShaderBinaryRaw& dst) {
                size_t o; json f;
                if (fieldOffset(binType, "data", o, f)) {
                    uint32_t dn = 0; size_t db = arrayAt(bs + o, dn);
                    if (db && dn && db + dn <= n_) dst.data.assign(d_ + db, d_ + db + dn);
                }
                {
                    std::string cT; int cS = 0; uint32_t cn = 0;
                    size_t cb = arrayField(binType, bs, "constants", cT, cS, cn);
                    size_t o3; json f3;
                    for (uint32_t j = 0; cb && cS > 0 && j < cn; ++j)
                        if (fieldOffset(cT, "token", o3, f3))
                            dst.constants.push_back(rd32(cb + (size_t)j * cS + o3));
                }
                {
                    std::string sT; int sS = 0; uint32_t sn = 0;
                    size_t sb = arrayField(binType, bs, "samplers", sT, sS, sn);
                    for (uint32_t j = 0; sb && sS > 0 && j < sn; ++j) {
                        size_t se = sb + (size_t)j * sS, o3; json f3;
                        AmatSamplerBind b;
                        // token64 is a fixed 8 bytes regardless of the packfile's
                        // pointer width, so rd64 rather than rdPtr.
                        if (fieldOffset(sT, "token", o3, f3))        b.token = rd64(se + o3);
                        if (fieldOffset(sT, "stateIndex", o3, f3))   b.stateIndex = rd32(se + o3);
                        if (fieldOffset(sT, "textureIndex", o3, f3)) b.textureIndex = rd32(se + o3);
                        if (fieldOffset(sT, "textureSlot", o3, f3))  b.textureSlot = rd32(se + o3);
                        dst.samplers.push_back(b);
                    }
                }
            };

            for (uint32_t i = 0; base && shSize > 0 && i < n; ++i) {
                size_t se = base + (size_t)i * shSize;
                AmatShaderRaw sh;
                size_t o; json f;
                if (fieldOffset(shType, "isPixelShader", o, f)) sh.isPixelShader = rd32(se + o) != 0;
                if (fieldOffset(shType, "dx11Shader", o, f))
                    readBinary(f.value("type", std::string()), se + o, sh.dx11Shader);
                if (fieldOffset(shType, "osxShader", o, f))
                    readBinary(f.value("type", std::string()), se + o, sh.osxShader);
                out.shaders.push_back(std::move(sh));
            }
        }

        // --- samplers[] (the shared sampler-state table) ---------------------
        {
            std::string sT; int sS = 0; uint32_t n = 0;
            size_t base = arrayField(root, bgfx, "samplers", sT, sS, n);
            size_t o; json f;
            for (uint32_t i = 0; base && sS > 0 && i < n; ++i)
                out.samplerStates.push_back(fieldOffset(sT, "state", o, f)
                                                ? rd32(base + (size_t)i * sS + o) : 0);
        }

        // --- techniques[] -> passes[] -> effects[] -> vertexShaderVariants[] --
        {
            std::string tT; int tS = 0; uint32_t tN = 0;
            size_t tb = arrayField(root, bgfx, "techniques", tT, tS, tN);
            for (uint32_t ti = 0; tb && tS > 0 && ti < tN; ++ti) {
                size_t te = tb + (size_t)ti * tS;
                AmatTechniqueRaw tech;
                size_t o; json f;
                if (fieldOffset(tT, "quality", o, f)) tech.quality = rd32(te + o);

                std::string pT; int pS = 0; uint32_t pN = 0;
                size_t pb = arrayField(tT, te, "passes", pT, pS, pN);
                for (uint32_t pi = 0; pb && pS > 0 && pi < pN; ++pi) {
                    size_t pe = pb + (size_t)pi * pS;
                    AmatPassRaw pass;

                    std::string eT; int eS = 0; uint32_t eN = 0;
                    size_t eb = arrayField(pT, pe, "effects", eT, eS, eN);
                    for (uint32_t ei = 0; eb && eS > 0 && ei < eN; ++ei) {
                        size_t ee = eb + (size_t)ei * eS;
                        AmatEffectRaw eff;
                        if (fieldOffset(eT, "token", o, f))            eff.token = rd64(ee + o);
                        if (fieldOffset(eT, "renderState", o, f))      eff.renderState = rd64(ee + o);
                        if (fieldOffset(eT, "shaderPassFlags", o, f))  eff.shaderPassFlags = rd32(ee + o);
                        if (fieldOffset(eT, "pixelShaderIndex", o, f)) eff.pixelShaderIndex = rd32(ee + o);

                        std::string vT; int vS = 0; uint32_t vN = 0;
                        size_t vb = arrayField(eT, ee, "vertexShaderVariants", vT, vS, vN);
                        for (uint32_t vi = 0; vb && vS > 0 && vi < vN; ++vi) {
                            size_t ve = vb + (size_t)vi * vS;
                            AmatVsVariantRaw v;
                            if (fieldOffset(vT, "variant", o, f))           v.variant = rd32(ve + o);
                            if (fieldOffset(vT, "vertexShaderIndex", o, f)) v.vertexShaderIndex = rd32(ve + o);
                            eff.vertexShaderVariants.push_back(v);
                        }
                        pass.effects.push_back(std::move(eff));
                    }
                    tech.passes.push_back(std::move(pass));
                }
                out.techniques.push_back(std::move(tech));
            }
        }

        if (out.shaders.empty()) out.error = "BGFX chunk carried no shaders";
        return out;
    }

    // -----------------------------------------------------------------------
    /// @brief Every geoset with its vertex buffer left undecoded.
    ///
    /// Same walk as ::extract's geometry pass, stopping before
    /// `decodeVertices`. See ::GeosetRaw for why not decoding is the point.
    // -----------------------------------------------------------------------
    std::vector<GeosetRaw> extractGeosetsRaw() {
        std::vector<GeosetRaw> out;
        std::string root; uint16_t ver = 0;
        size_t geomStart = findChunk("GEOM", &root, &ver);
        if (!geomStart || root.empty()) return out;
        size_t off; json fj;
        if (!fieldOffset(root, "meshes", off, fj)) return out;
        uint32_t count = 0;
        size_t base = arrayAt(geomStart + off, count);
        if (!base) return out;
        std::string meshType = fj["element"].value("struct", std::string());

        for (uint32_t i = 0; i < count; ++i) {
            size_t meshStart = follow(base + (size_t)i * ptr_);
            if (!meshStart) continue;
            try {
                out.push_back(parseGeosetRaw(meshType, meshStart));
            } catch (const std::exception&) { /* skip a bad geoset, keep going */ }
        }
        return out;
    }

private:
    // Reads one ModelMeshIndexDataV* into a 16-bit index vector.
    std::vector<uint16_t> readIndexData(const std::string& idxType, size_t s) {
        std::vector<uint16_t> idx;
        size_t o; json f;
        if (!fieldOffset(idxType, "indices", o, f)) return idx;
        uint32_t n = 0; size_t b = arrayAt(s + o, n);
        idx.reserve(n);
        for (uint32_t i = 0; b && i < n; ++i) idx.push_back(rd16(b + 2u * i));
        return idx;
    }

    GeosetRaw parseGeosetRaw(const std::string& meshType, size_t s) {
        GeosetRaw g;
        size_t off; json fj;
        if (fieldOffset(meshType, "materialIndex", off, fj)) g.materialIndex = rd32(s + off);
        if (fieldOffset(meshType, "minBound", off, fj)) for (int i=0;i<3;++i) g.minB[i]=rdf(s+off+4*i);
        if (fieldOffset(meshType, "maxBound", off, fj)) for (int i=0;i<3;++i) g.maxB[i]=rdf(s+off+4*i);
        if (fieldOffset(meshType, "boneBindings", off, fj)) {
            uint32_t bc = 0; size_t bb = arrayAt(s + off, bc);
            if (bc > 4096) bc = 4096;
            for (uint32_t i = 0; bb && i < bc; ++i) g.boneBindings.push_back(rd64(bb + 8u * i));
        }

        if (!fieldOffset(meshType, "geometry", off, fj)) throw std::runtime_error("no geometry field");
        size_t geo = follow(s + off);
        if (!geo) throw std::runtime_error("null geometry");
        std::string geoType = fj["target"].value("struct", std::string());

        // verts: inline ModelMeshVertexDataV* { vertexCount, mesh: PackVertexType }
        if (!fieldOffset(geoType, "verts", off, fj)) throw std::runtime_error("no verts");
        std::string vType = fj.value("type", std::string());
        size_t vs = geo + off, voff; json vfj;
        if (fieldOffset(vType, "vertexCount", voff, vfj)) g.vertexCount = rd32(vs + voff);
        if (!fieldOffset(vType, "mesh", voff, vfj)) throw std::runtime_error("no PackVertexType");
        std::string pvt = vfj.value("type", std::string());
        size_t ps = vs + voff, poff; json pfj;
        if (fieldOffset(pvt, "fvf", poff, pfj)) g.fvf = rd32(ps + poff);
        // `vertices` is array_ptr<byte>: the count is a BYTE LENGTH, not a
        // vertex count.
        if (fieldOffset(pvt, "vertices", poff, pfj)) {
            uint32_t vbytes = 0;
            size_t vbase = arrayAt(ps + poff, vbytes);
            if (vbase && vbytes && vbase + vbytes <= n_)
                g.vertexBytes.assign(d_ + vbase, d_ + vbase + vbytes);
        }

        if (fieldOffset(geoType, "indices", off, fj))
            g.indices = readIndexData(fj.value("type", std::string()), geo + off);

        if (fieldOffset(geoType, "lods", off, fj)) {
            uint32_t lcount = 0;
            size_t lbase = arrayAt(geo + off, lcount);
            std::string lodElem = fj.contains("element") && fj["element"].is_object()
                                      ? fj["element"].value("struct", std::string()) : std::string();
            size_t lodStride = lodElem.empty() ? 0 : (size_t)typeSize(lodElem);
            if (lbase && lodStride && lcount <= 16)
                for (uint32_t k = 0; k < lcount; ++k)
                    g.lodIndices.push_back(readIndexData(lodElem, lbase + (size_t)k * lodStride));
        }
        return g;
    }

    const uint8_t* d_;
    size_t n_;
    const json& tpl_;
    const json* types_ = nullptr;
    int ptr_ = 8;
    std::string container_;

    // ---- low-level reads (bounds-checked) ----
    void chk(size_t pos, size_t k) const {
        if (pos + k > n_) throw std::runtime_error("read out of bounds");
    }
    uint8_t  rd8(size_t p)  const { chk(p, 1); return d_[p]; }
    uint16_t rd16(size_t p) const { chk(p, 2); return d_[p] | (d_[p + 1] << 8); }
    uint32_t rd32(size_t p) const { chk(p, 4); return d_[p] | (d_[p+1]<<8) | (d_[p+2]<<16) | ((uint32_t)d_[p+3]<<24); }
    uint64_t rd64(size_t p) const { return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4) << 32); }
    uint64_t rdPtr(size_t p) const {
        chk(p, ptr_);
        uint64_t v = 0;
        for (int i = 0; i < ptr_; ++i) v |= (uint64_t)d_[p + i] << (8 * i);
        return v;
    }
    float rdf(size_t p) const { uint32_t u = rd32(p); float f; std::memcpy(&f, &u, 4); return f; }
    static float half2float(uint16_t h) {
        uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF, out;
        if (e == 0) { if (m == 0) out = s << 31; else { while (!(m & 0x400)) { m <<= 1; --e; } ++e; m &= ~0x400u; out = (s<<31)|((e+112)<<23)|(m<<13); } }
        else if (e == 31) out = (s<<31)|0x7F800000|(m<<13);
        else out = (s<<31)|((e+112)<<23)|(m<<13);
        float f; std::memcpy(&f, &out, 4); return f;
    }

    // self-relative pointer at field position p -> absolute target (0 if null)
    size_t follow(size_t p) const { uint64_t s = rdPtr(p); return s ? p + (size_t)s : 0; }
    // array_ptr header at p: out count + base offset of elements (0 if empty)
    size_t arrayAt(size_t p, uint32_t& count) const {
        count = rd32(p);
        size_t pp = p + 4;
        uint64_t s = rdPtr(pp);
        return (s && count) ? pp + (size_t)s : 0;
    }

    // ---- template-driven type geometry ----
    int scalarSize(const std::string& k) const {
        static const std::pair<const char*, int> tbl[] = {
            {"byte",1},{"byte3",3},{"byte4",4},{"word",2},{"word3",6},{"dword",4},
            {"dword2",8},{"dword4",16},{"qword",8},{"float",4},{"float2",8},{"float3",12},
            {"float4",16},{"double",8},{"fileref",4},{"token32",4},{"token64",8}};
        for (auto& e : tbl) if (k == e.first) return e.second;
        return -1;
    }
    int fieldSize(const json& f) const {
        std::string k = f.value("kind", std::string("dword"));
        if (k == "ptr" || k == "char_ptr" || k == "wchar_ptr" || k == "filename") return ptr_;
        if (k == "array_ptr" || k == "ptr_array_ptr") return 4 + ptr_;
        if (k == "struct") return typeSize(f.value("type", std::string()));
        if (k == "array") {
            int cnt = f.value("count", 0);
            const json& el = f.contains("element") ? f["element"] : json("dword");
            int es = el.is_string() ? scalarSize(el.get<std::string>())
                                    : (el.is_object() && el.contains("struct") ? typeSize(el["struct"]) : 4);
            return cnt * (es < 0 ? 4 : es);
        }
        int s = scalarSize(k);
        return s < 0 ? 4 : s;
    }
    int typeSize(const std::string& typeName) const {
        if (!types_->contains(typeName)) return 0;
        int total = 0;
        for (const auto& f : (*types_)[typeName]["fields"]) total += fieldSize(f);
        return total;
    }
    // offset of a named field within a struct type + the field json
    bool fieldOffset(const std::string& typeName, const std::string& name, size_t& off, json& out) const {
        if (!types_->contains(typeName)) return false;
        size_t o = 0;
        for (const auto& f : (*types_)[typeName]["fields"]) {
            if (f.value("name", std::string()) == name) { off = o; out = f; return true; }
            o += fieldSize(f);
        }
        return false;
    }

    // Resolve a versioned struct name (`base` + "V<n>") to the exact variant for
    // `ver`, or the nearest lower version that exists (the template omits some
    // intermediate versions, e.g. GlobalV77). "" if none.
    std::string resolveVariant(const std::string& base, uint16_t ver) const {
        for (int v = (int)ver; v >= 0; --v) {
            std::string n = base + "V" + std::to_string(v);
            if (types_->contains(n)) return n;
        }
        return "";
    }

    // resolve the concrete versioned type key for a chunk fourcc
    std::string chunkType(const std::string& fourcc, uint16_t version) const {
        std::string vs = std::to_string(version);
        const json& ft = tpl_.contains("fileTypes") ? tpl_["fileTypes"] : json::object();
        if (ft.contains(container_) && ft[container_].contains(fourcc) && ft[container_][fourcc].contains(vs))
            return ft[container_][fourcc][vs].get<std::string>();
        const json& ch = tpl_.contains("chunks") ? tpl_["chunks"] : json::object();
        if (ch.contains(fourcc) && ch[fourcc].contains(vs)) return ch[fourcc][vs].get<std::string>();
        return "";
    }

    // walk chunks; return the data-start offset of the first matching fourcc (0 if none)
    size_t findChunk(const std::string& want, std::string* typeKeyOut = nullptr, uint16_t* verOut = nullptr) const {
        size_t pos = rd16(6); // headerSize
        while (pos + 16 <= n_) {
            char fourcc[5] = {0};
            std::memcpy(fourcc, d_ + pos, 4);
            uint32_t chunkSize = rd32(pos + 4);
            uint16_t version = rd16(pos + 8);
            uint16_t chunkHdr = rd16(pos + 10);
            if (want == fourcc) {
                if (typeKeyOut) *typeKeyOut = chunkType(fourcc, version);
                if (verOut) *verOut = version;
                return pos + chunkHdr;
            }
            size_t next = pos + 8 + chunkSize;
            if (next <= pos) break;
            pos = next;
        }
        return 0;
    }

    // ---- geometry ----
    void parseGeometry(size_t geomStart, Model& out) {
        std::string root; uint16_t ver;
        (void)findChunk("GEOM", &root, &ver);
        if (root.empty()) return;
        size_t off; json fj;
        if (!fieldOffset(root, "meshes", off, fj)) return;
        uint32_t count = 0;
        size_t base = arrayAt(geomStart + off, count);
        if (!base) return;
        // meshes is ptr_array_ptr: array of pointers to ModelMeshData*
        std::string meshType = fj["element"].value("struct", std::string());
        for (uint32_t i = 0; i < count; ++i) {
            size_t elemPtrPos = base + (size_t)i * ptr_;
            size_t meshStart = follow(elemPtrPos);
            if (!meshStart) continue;
            try {
                out.meshes.push_back(parseMesh(meshType, meshStart));
            } catch (const std::exception&) { /* skip a bad mesh, keep going */ }
        }
    }

    Mesh parseMesh(const std::string& meshType, size_t s) {
        Mesh mesh;
        size_t off; json fj;
        if (fieldOffset(meshType, "materialIndex", off, fj)) mesh.materialIndex = rd32(s + off);
        if (fieldOffset(meshType, "minBound", off, fj)) for (int i=0;i<3;++i) mesh.minB[i]=rdf(s+off+4*i);
        if (fieldOffset(meshType, "maxBound", off, fj)) for (int i=0;i<3;++i) mesh.maxB[i]=rdf(s+off+4*i);
        // boneBindings: token64[] -- vertex BlendIndices index into this table.
        if (fieldOffset(meshType, "boneBindings", off, fj)) {
            uint32_t bc = 0; size_t bb = arrayAt(s + off, bc);
            if (bc > 4096) bc = 4096;
            for (uint32_t i = 0; bb && i < bc; ++i) mesh.boneBindings.push_back(rd64(bb + 8u * i));
        }
        if (!fieldOffset(meshType, "geometry", off, fj)) throw std::runtime_error("no geometry field");
        size_t geo = follow(s + off);
        if (!geo) throw std::runtime_error("null geometry");
        std::string geoType = fj["target"].value("struct", std::string());
        parseGeom(geoType, geo, mesh);
        return mesh;
    }

    void parseGeom(const std::string& geoType, size_t g, Mesh& mesh) {
        size_t off; json fj;
        // verts (inline struct ModelMeshVertexDataV1)
        if (!fieldOffset(geoType, "verts", off, fj)) throw std::runtime_error("no verts");
        std::string vType = fj.value("type", std::string());
        size_t vs = g + off;
        size_t voff; json vfj;
        fieldOffset(vType, "vertexCount", voff, vfj);
        mesh.vertexCount = rd32(vs + voff);
        fieldOffset(vType, "mesh", voff, vfj); // PackVertexType (inline)
        std::string pvt = vfj.value("type", std::string());
        size_t ps = vs + voff;
        size_t poff; json pfj;
        fieldOffset(pvt, "fvf", poff, pfj);
        mesh.fvf = rd32(ps + poff);
        fieldOffset(pvt, "vertices", poff, pfj); // array_ptr<byte>, count = byte length
        uint32_t vbytes = 0;
        size_t vbase = arrayAt(ps + poff, vbytes);

        // indices (inline struct ModelMeshIndexDataV1)
        std::string iType;
        if (fieldOffset(geoType, "indices", off, fj)) {
            iType = fj.value("type", std::string());
            size_t is = g + off, ioff; json ifj;
            if (fieldOffset(iType, "indices", ioff, ifj)) {
                uint32_t icount = 0;
                size_t ibase = arrayAt(is + ioff, icount);
                if (ibase)
                    for (uint32_t i = 0; i < icount; ++i) mesh.indices.push_back(rd16(ibase + 2u * i));
            }
        }

        // Additional LOD index buffers: ModelMeshGeometryV1.lods is
        // array_ptr<ModelMeshIndexDataV1>, each an alternative (lower-detail) index
        // set over the same verts. LOD0 is `indices` above; lods[k] = LOD(k+1).
        if (fieldOffset(geoType, "lods", off, fj)) {
            uint32_t lcount = 0;
            size_t lbase = arrayAt(g + off, lcount);
            std::string lodElem = fj.contains("element") && fj["element"].is_object()
                                      ? fj["element"].value("struct", std::string())
                                      : std::string();
            size_t lodStride = lodElem.empty() ? 0 : (size_t)typeSize(lodElem);
            if (lbase && lodStride && lcount <= 16) {
                size_t lioff; json lifj;
                for (uint32_t k = 0; k < lcount; ++k) {
                    size_t elem = lbase + (size_t)k * lodStride;
                    std::vector<uint32_t> idx;
                    if (fieldOffset(lodElem, "indices", lioff, lifj)) {
                        uint32_t ic = 0; size_t ib = arrayAt(elem + lioff, ic);
                        if (ib) for (uint32_t i = 0; i < ic; ++i) idx.push_back(rd16(ib + 2u * i));
                    }
                    mesh.lods.push_back(std::move(idx)); // keep even if empty to preserve LOD numbering
                }
            }
        }

        if (vbase && mesh.vertexCount) decodeVertices(vbase, vbytes, mesh);
    }

    // Parse ModelFileData.clothData -> Model.clothPieces. Each piece carries its
    // own proxy vertex mesh (PackVertexType, decoded like the render geometry),
    // triangle indices, distance constraints and sim params. See ClothPiece.
    void parseCloth(size_t modl, Model& out) {
        std::string root; uint16_t ver = 0;
        (void)findChunk("MODL", &root, &ver);
        if (root.empty()) return;
        size_t off; json f;
        if (!fieldOffset(root, "clothData", off, f)) return;
        std::string ct = f.contains("element") && f["element"].is_object()
                             ? f["element"].value("struct", std::string()) : std::string();
        int cs = ct.empty() ? 0 : typeSize(ct);
        uint32_t n = 0; size_t base = arrayAt(modl + off, n);
        size_t o; json ff;
        for (uint32_t i = 0; base && cs > 0 && i < n; ++i) {
            size_t e = base + (size_t)i * cs;
            ClothPiece cp;
            if (fieldOffset(ct, "materialIndex", o, ff)) cp.materialIndex = rd32(e + o);
            if (fieldOffset(ct, "drag", o, ff))            cp.drag = rdf(e + o);
            if (fieldOffset(ct, "gravity", o, ff))         cp.gravity = rdf(e + o);
            if (fieldOffset(ct, "wind", o, ff))            cp.wind = rdf(e + o);
            if (fieldOffset(ct, "weight", o, ff))          cp.weight = rdf(e + o);
            if (fieldOffset(ct, "translateWeight", o, ff)) cp.translateWeight = rdf(e + o);
            if (fieldOffset(ct, "rigidness", o, ff))       cp.rigidness = (float)d_[e + o]; // byte
            if (fieldOffset(ct, "lockCount", o, ff))       cp.lockCount = rd16(e + o);

            // mesh: inline PackVertexType (fvf + vertices byte-array) -> decode.
            if (fieldOffset(ct, "mesh", o, ff)) {
                std::string pvt = ff.value("type", std::string());
                size_t ps = e + o, po; json pf;
                Mesh tmp;
                if (fieldOffset(pvt, "fvf", po, pf)) tmp.fvf = rd32(ps + po);
                if (fieldOffset(pvt, "vertices", po, pf)) {
                    uint32_t vb = 0; size_t vbase = arrayAt(ps + po, vb);
                    tmp.vertexCount = 0;                 // let decodeVertices derive vc = vbytes/stride
                    if (vbase && vb) decodeVertices(vbase, vb, tmp);
                }
                cp.verts = std::move(tmp.vertices);
            }
            // indices: array_ptr<word>
            if (fieldOffset(ct, "indices", o, ff)) {
                uint32_t ic = 0; size_t ib = arrayAt(e + o, ic);
                for (uint32_t k = 0; ib && k < ic; ++k) cp.indices.push_back(rd16(ib + 2u * k));
            }
            // lod0Constraints: array_ptr<ModelClothConstraintV*> {distance(half),relationship,vertIndexA,vertIndexB}
            if (fieldOffset(ct, "lod0Constraints", o, ff)) {
                std::string kt = ff.contains("element") && ff["element"].is_object()
                                     ? ff["element"].value("struct", std::string()) : std::string();
                int ks = kt.empty() ? 0 : typeSize(kt);
                uint32_t cn = 0; size_t cb = arrayAt(e + o, cn);
                size_t da = 0, ra = 0, va = 0, vbo = 0; json jj;
                bool okF = fieldOffset(kt, "distance", da, jj) && fieldOffset(kt, "relationship", ra, jj)
                        && fieldOffset(kt, "vertIndexA", va, jj) && fieldOffset(kt, "vertIndexB", vbo, jj);
                for (uint32_t c = 0; okF && cb && ks > 0 && c < cn; ++c) {
                    size_t ce = cb + (size_t)c * ks;
                    ClothEdge ed;
                    ed.restLength   = half2float(rd16(ce + da));
                    ed.relationship = rd16(ce + ra);
                    ed.a = rd16(ce + va); ed.b = rd16(ce + vbo);
                    cp.edges.push_back(ed);
                }
            }
            out.clothPieces.push_back(std::move(cp));
        }
    }

    // ---- FVF decode (GrFvf, GW2_Vertex_FVF_Notes.md) ----
    void decodeVertices(size_t base, uint32_t vbytes, Mesh& mesh) {
        uint32_t fvf = mesh.fvf;
        int stride = 0;
        int posOff = -1, nrmOff = -1, tfOff = -1, uvOff = -1, wtOff = -1, idxOff = -1;
        bool uvHalf = false;
        int uvOffN[8]; bool uvHalfN[8]; for(int c=0;c<8;++c){uvOffN[c]=-1;uvHalfN[c]=false;} // 8 UV channels (GrFvf TEXCOORD0..7)
        auto add = [&](int sz) { int o = stride; stride += sz; return o; };
        if (fvf & 0x1)  posOff = add((fvf & 0x08000000) ? 16 : (fvf & 0x10000000) ? 6 : 12);
        if (fvf & 0x2)  wtOff  = add(4);                      // blend weights (4 x u8 norm)
        if (fvf & 0x4)  idxOff = add(4);                      // blend indices (4 x u8 -> boneBindings)
        if (fvf & 0x8)  nrmOff = add((fvf & 0x04000000) ? 4 : 12); // normal
        if (fvf & 0x10) add(4);                               // color BGRA
        if (fvf & 0x20) add(12);                              // tangent
        if (fvf & 0x40) add(12);                              // bitangent
        if (fvf & 0x80) tfOff = add(12);                      // tangent frame (N,T,B as u8x4)
        for (int i = 0; i < 8; ++i) {
            // A UV channel is present if EITHER its F32 presence bit (0x100<<i)
            // OR its F16 bit (0x10000<<i) is set; the F16 bit alone means the
            // channel exists as 2xHalf (4B). (F32/F16 are mutually exclusive.)
            bool f32 = (fvf & (0x100u << i)) != 0;
            bool f16 = (fvf & (0x10000u << i)) != 0;
            if (f32 || f16) {
                bool h = f16;
                int o = add(h ? 4 : 8);
                if (i == 0) { uvOff = o; uvHalf = h; }
                uvOffN[i] = o; uvHalfN[i] = h;   // all 8 channels
            }
        }
        if (stride <= 0) return;
        mesh.hasTangents = (tfOff >= 0);
        mesh.hasSkin = (wtOff >= 0 && idxOff >= 0);
        uint32_t vc = vbytes / (uint32_t)stride;
        if (mesh.vertexCount && vc > mesh.vertexCount) vc = mesh.vertexCount;
        mesh.vertices.reserve(vc);
        auto u8n = [&](size_t p) { return d_[p] / 127.5f - 1.0f; }; // u8 [0,255] -> [-1,1]
        for (uint32_t i = 0; i < vc; ++i) {
            size_t v = base + (size_t)i * stride;
            Vertex out{};
            if (posOff >= 0) { out.px = rdf(v + posOff); out.py = rdf(v + posOff + 4); out.pz = rdf(v + posOff + 8); }
            if (tfOff >= 0) {
                // tangent frame: 3 x (4 x u8 norm) = Normal, Tangent, Bitangent
                out.nx = u8n(v + tfOff);     out.ny = u8n(v + tfOff + 1);  out.nz = u8n(v + tfOff + 2);
                out.tx = u8n(v + tfOff + 4); out.ty = u8n(v + tfOff + 5);  out.tz = u8n(v + tfOff + 6);
                out.bx = u8n(v + tfOff + 8); out.by = u8n(v + tfOff + 9);  out.bz = u8n(v + tfOff + 10);
            } else if (nrmOff >= 0) {
                out.nx = rdf(v + nrmOff); out.ny = rdf(v + nrmOff + 4); out.nz = rdf(v + nrmOff + 8);
            } else out.nz = 1.0f;
            // read all present UV channels (0..7); missing channels fall back to
            // UV0 so a shader that samples UV1..7 still gets sane coords.
            float uvs[8][2];
            for (int c = 0; c < 8; ++c) {
                if (uvOffN[c] >= 0) {
                    if (uvHalfN[c]) { uvs[c][0] = half2float(rd16(v + uvOffN[c])); uvs[c][1] = half2float(rd16(v + uvOffN[c] + 2)); }
                    else { uvs[c][0] = rdf(v + uvOffN[c]); uvs[c][1] = rdf(v + uvOffN[c] + 4); }
                } else { uvs[c][0] = 1e30f; uvs[c][1] = 1e30f; } // sentinel: fall back below
            }
            if (uvOffN[0] < 0) { uvs[0][0] = uvs[0][1] = 0; }
            out.u = uvs[0][0]; out.v = uvs[0][1];
            if (idxOff >= 0) for (int c = 0; c < 4; ++c) out.boneIdx[c] = d_[v + idxOff + c];
            if (wtOff >= 0) {
                int sum = 0; for (int c = 0; c < 4; ++c) sum += d_[v + wtOff + c];
                if (sum <= 0) { out.boneWt[0] = 1; for (int c = 1; c < 4; ++c) out.boneWt[c] = 0; }
                else for (int c = 0; c < 4; ++c) out.boneWt[c] = d_[v + wtOff + c] / (float)sum;
            }
            for (int c = 1; c < 8; ++c) {
                bool have = (uvOffN[c] >= 0);
                out.uv[c-1][0] = have ? uvs[c][0] : out.u;
                out.uv[c-1][1] = have ? uvs[c][1] : out.v;
            }
            mesh.vertices.push_back(out);
        }
    }

    // ---- materials / textures ----
    void parseMaterials(size_t modlStart, Model& out) {
        std::string root; (void)findChunk("MODL", &root);
        if (root.empty()) return;
        size_t off; json fj;
        if (!fieldOffset(root, "permutations", off, fj)) return;
        uint32_t pcount = 0;
        size_t pbase = arrayAt(modlStart + off, pcount);
        if (!pbase || !pcount) return;
        std::string permType = fj["element"].value("struct", std::string());
        // use permutation 0
        size_t perm = pbase; // array_ptr<struct>: elements are inline structs back-to-back
        int permSize = typeSize(permType);
        (void)permSize;
        size_t moff; json mfj;
        if (!fieldOffset(permType, "materials", moff, mfj)) return;
        uint32_t mcount = 0;
        size_t mbase = arrayAt(perm + moff, mcount); // ptr_array_ptr
        if (!mbase) return;
        std::string matType = mfj["element"].value("struct", std::string());
        for (uint32_t i = 0; i < mcount; ++i) {
            size_t matPtr = follow(mbase + (size_t)i * ptr_);
            if (!matPtr) continue;
            Material mat; mat.index = i;
            size_t off; json fj;
            // token64 is 8 bytes regardless of the packfile's pointer width, so read
            // it with rd64 -- rdPtr would truncate to 4 on a 32-bit-ptr MODL (v65 is).
            if (fieldOffset(matType, "token", off, fj))         mat.token         = rd64(matPtr + off);
            if (fieldOffset(matType, "materialId", off, fj))    mat.materialId    = rd32(matPtr + off);
            if (fieldOffset(matType, "materialFlags", off, fj)) mat.materialFlags = rd32(matPtr + off);
            if (fieldOffset(matType, "sortOrder", off, fj))     mat.sortOrder     = rd32(matPtr + off);
            if (fieldOffset(matType, "sortLayer", off, fj))     mat.sortLayer     = rd32(matPtr + off);
            mat.materialFile = decodeFilename(matType, matPtr); // the material's own 'filename' field

            // textures[] -- fileId + sampler token + flags + uv channel
            if (fieldOffset(matType, "textures", off, fj)) {
                uint32_t tcount = 0;
                size_t tbase = arrayAt(matPtr + off, tcount);
                std::string texType = fj["element"].value("struct", std::string());
                int texSize = typeSize(texType);
                for (uint32_t t = 0; tbase && t < tcount; ++t) {
                    size_t texElem = tbase + (size_t)t * texSize;
                    MatTexture mt;
                    mt.fileId = decodeFilename(texType, texElem);
                    size_t so; json sfj;
                    // MODL v65 is a 32-bit-pointer PF, so rdPtr truncated this 8-byte
                    // token64 to its low half (MODL 143917 texture 0 reads
                    // 0x0060B401_67531924, of which rdPtr returned only 0x67531924).
                    if (fieldOffset(texType, "token", so, sfj)) mt.token = rd64(texElem + so);
                    if (fieldOffset(texType, "textureFlags", so, sfj)) mt.flags = rd32(texElem + so);
                    if (fieldOffset(texType, "uvPSInputIndex", so, sfj)) mt.uvIndex = (uint8_t)d_[texElem + so];
                    mat.textures.push_back(mt);
                }
            }
            // constants[] -- tint/parameter float4 by token32 name
            if (fieldOffset(matType, "constants", off, fj)) {
                uint32_t ccount = 0;
                size_t cbase = arrayAt(matPtr + off, ccount);
                std::string cType = fj["element"].value("struct", std::string());
                int cSize = typeSize(cType);
                for (uint32_t c = 0; cbase && c < ccount; ++c) {
                    size_t cElem = cbase + (size_t)c * cSize;
                    MatConstant mc;
                    size_t so; json sfj;
                    if (fieldOffset(cType, "name", so, sfj)) mc.name = rd32(cElem + so);
                    if (fieldOffset(cType, "value", so, sfj)) for (int k = 0; k < 4; ++k) mc.value[k] = rdf(cElem + so + 4 * k);
                    mat.constants.push_back(mc);
                }
            }
            out.materials.push_back(std::move(mat));
        }
    }

    // decode a "filename" field of a struct -> fileId (0 if none/invalid)
    uint32_t decodeFilename(const std::string& typeName, size_t structStart) {
        size_t off; json fj;
        if (!fieldOffset(typeName, "filename", off, fj)) return 0;
        size_t t = follow(structStart + off);
        if (!t) return 0;
        uint16_t lo = rd16(t), hi = rd16(t + 2);
        uint16_t third = (t + 6 <= n_) ? rd16(t + 4) : 0;
        uint16_t fourth = (t + 8 <= n_) ? rd16(t + 6) : 0;
        if (lo < 0x100 || hi < 0x100 || (third != 0 && (third < 0x100 || fourth != 0))) return 0;
        return (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
    }

    // ---- particle clouds + effect lights (MODL cloudData / lightData) ----
    // small field-read helpers scoped to a struct type + base offset
    bool fOff(const std::string& t, const char* name, size_t base, size_t& abs, json& fj) const {
        size_t off; if (!fieldOffset(t, name, off, fj)) return false; abs = base + off; return true;
    }
    void rdFloat2(const std::string& t, const char* name, size_t base, float o[2]) const {
        size_t a; json fj; if (fOff(t, name, base, a, fj)) { o[0] = rdf(a); o[1] = rdf(a + 4); }
    }
    float rdFloat1(const std::string& t, const char* name, size_t base, float def = 0) const {
        size_t a; json fj; return fOff(t, name, base, a, fj) ? rdf(a) : def;
    }
    uint32_t rdDword(const std::string& t, const char* name, size_t base, uint32_t def = 0) const {
        size_t a; json fj; return fOff(t, name, base, a, fj) ? rd32(a) : def;
    }
    // read a lifetime curve (array_ptr<float2> of (t,value)) into keys
    void rdCurve(const std::string& emType, const char* name, size_t base,
                 std::vector<std::pair<float,float>>& keys) const {
        size_t a; json fj;
        if (!fOff(emType, name, base, a, fj)) return;
        size_t curve = follow(a);
        if (!curve) return;
        std::string ct = ptrTargetStruct(fj);           // ModelParticleCurveV*
        size_t ka; json kf;
        if (ct.empty() || !fOff(ct, "keys", curve, ka, kf)) return;
        uint32_t n = 0; size_t kb = arrayAt(ka, n);
        for (uint32_t i = 0; kb && i < n && i < 64; ++i)
            keys.emplace_back(rdf(kb + 8u*i), rdf(kb + 8u*i + 4));
    }

    void parseEffects(Model& out) {
        std::string root; uint16_t ver = 0;
        size_t modl = findChunk("MODL", &root, &ver);
        if (!modl || root.empty()) return;
        parseCloudData(root, modl, out);
        parseLightData(root, modl, out);
    }

    void parseCloudData(const std::string& root, size_t modl, Model& out) {
        size_t a; json fj;
        if (!fOff(root, "cloudData", modl, a, fj)) return;
        size_t cd = follow(a);
        if (!cd) return;
        std::string cdType = ptrTargetStruct(fj);         // ModelCloudDataV*
        if (cdType.empty()) return;

        // emitters[] first (clouds reference them by index)
        size_t ea; json ef;
        if (fOff(cdType, "emitters", cd, ea, ef)) {
            std::string emType = ef["element"].value("struct", std::string());
            int es = typeSize(emType);
            uint32_t n = 0; size_t base = arrayAt(ea, n);
            for (uint32_t i = 0; base && es > 0 && i < n && i < 256; ++i)
                out.effects.emitters.push_back(parseEmitter(emType, base + (size_t)i * es));
        }
        // clouds[]
        size_t ca; json cf;
        if (fOff(cdType, "clouds", cd, ca, cf)) {
            std::string clType = cf["element"].value("struct", std::string());
            int cs = typeSize(clType);
            uint32_t n = 0; size_t base = arrayAt(ca, n);
            for (uint32_t i = 0; base && cs > 0 && i < n && i < 128; ++i) {
                size_t e = base + (size_t)i * cs;
                ParticleCloud pc;
                pc.materialIndex = rdDword(clType, "materialIndex", e);
                pc.flags = rdDword(clType, "flags", e);
                pc.fvf = rdDword(clType, "fvf", e);
                pc.drag = rdFloat1(clType, "drag", e);
                size_t o; json f2;
                if (fOff(clType, "bone", e, o, f2)) pc.bone = rd64(o);
                if (fOff(clType, "acceleration", e, o, f2)) { pc.acceleration[0]=rdf(o); pc.acceleration[1]=rdf(o+4); pc.acceleration[2]=rdf(o+8); }
                if (fOff(clType, "velocity", e, o, f2))     { pc.velocity[0]=rdf(o); pc.velocity[1]=rdf(o+4); pc.velocity[2]=rdf(o+8); }
                if (fOff(clType, "emitterIndices", e, o, f2)) {
                    uint32_t en = 0; size_t eb = arrayAt(o, en);
                    for (uint32_t k = 0; eb && k < en && k < 256; ++k) pc.emitterIndices.push_back(rd32(eb + 4u*k));
                }
                out.effects.clouds.push_back(std::move(pc));
            }
        }
    }

    ParticleEmitter parseEmitter(const std::string& t, size_t e) {
        ParticleEmitter em;
        em.spawnPeriod = rdFloat1(t, "spawnPeriod", e);
        em.spawnProbability = rdFloat1(t, "spawnProbability", e, 1.0f);
        rdFloat2(t, "spawnGroupSize", e, em.spawnGroupSize);
        rdFloat2(t, "spawnRadius", e, em.spawnRadius);
        rdFloat2(t, "lifetime", e, em.lifetime);
        rdFloat2(t, "colorFalloff", e, em.colorFalloff);
        em.colorPeriod = rdFloat1(t, "colorPeriod", e);
        em.emitterFlags = rdDword(t, "emitterFlags", e);
        em.flags = rdDword(t, "flags", e);
        em.drag = rdFloat1(t, "drag", e);
        size_t o; json fj;
        if (fOff(t, "spawnShape", e, o, fj)) em.spawnShape = d_[o];
        if (fOff(t, "bone", e, o, fj)) em.bone = rd64(o);
        // velocity/acceleration: inline array[4] of float2
        if (fOff(t, "velocity", e, o, fj))
            for (int r = 0; r < 4; ++r) { em.velocity[r][0]=rdf(o+8*r); em.velocity[r][1]=rdf(o+8*r+4); }
        if (fOff(t, "acceleration", e, o, fj))
            for (int r = 0; r < 4; ++r) { em.acceleration[r][0]=rdf(o+8*r); em.acceleration[r][1]=rdf(o+8*r+4); }
        // colorBegin/colorEnd. Two layouts: V>=70 = array[2] float4 ({base,span});
        // V<=66 = a single float4 begin/end color (no random span).
        auto readColor = [&](const char* name, float dst[2][4]) {
            size_t co; json cf;
            if (!fOff(t, name, e, co, cf)) return;
            bool isArr = cf.value("kind", std::string()) == "array";
            if (isArr) for (int r = 0; r < 2; ++r) for (int c = 0; c < 4; ++c) dst[r][c] = rdf(co + 16*r + 4*c);
            else { for (int c = 0; c < 4; ++c) { dst[0][c] = rdf(co + 4*c); dst[1][c] = 0; } }
        };
        readColor("colorBegin", em.colorBegin);
        readColor("colorEnd", em.colorEnd);
        em.opacityCurvePreset = rdDword(t, "opacityCurvePreset", e);
        em.scaleCurvePreset = rdDword(t, "scaleCurvePreset", e);
        rdCurve(t, "opacityCurve", e, em.opacityCurve);
        rdCurve(t, "scaleCurve", e, em.scaleCurve);
        // wind (both layouts have these directly on the emitter)
        rdFloat2(t, "spawnWindEmit", e, em.spawnWindEmit);
        rdFloat2(t, "spawnWindSpeed", e, em.spawnWindSpeed);
        if (fOff(t, "windInfluence", e, o, fj)) em.windInfluence = d_[o];
        // transform (ptr -> ModelMatrix43, 3 rows of float4)
        if (fOff(t, "transform", e, o, fj)) {
            size_t m = follow(o);
            if (m) { for (int k = 0; k < 12; ++k) em.transform[k] = rdf(m + 4*k); em.hasTransform = true; }
        }
        // Billboard (plane) settings. V>=67: a planeEmitterSettings sub-struct.
        // V<=66: the same fields (alignment/rotation/scale/flipbook) live DIRECTLY
        // on the emitter -- read them from `t` at `e`.
        if (fOff(t, "planeEmitterSettings", e, o, fj)) {
            size_t p = follow(o);
            std::string pt = ptrTargetStruct(fj);
            if (p && !pt.empty()) parsePlane(pt, p, em);
        } else if (types_->contains(t) && (*types_)[t].contains("fields")) {
            bool hasDirectPlane = false;
            for (const auto& f : (*types_)[t]["fields"])
                if (f.value("name", std::string()) == "scaleInitial") { hasDirectPlane = true; break; }
            if (hasDirectPlane) parsePlane(t, e, em); // emitter itself carries the plane fields
        }
        // mesh emitter settings (mesh particles) -- V>=67 only
        if (fOff(t, "meshEmitterSettings", e, o, fj)) {
            size_t p = follow(o);
            std::string mt = ptrTargetStruct(fj);
            if (p && !mt.empty()) { em.hasMesh = true; em.meshFileId = decodeFilename(mt, p); }
        }
        return em;
    }

    void parsePlane(const std::string& t, size_t p, ParticleEmitter& em) {
        em.hasPlane = true;
        size_t o; json fj;
        if (fOff(t, "alignmentType", p, o, fj)) em.alignmentType = d_[o];
        if (fOff(t, "alignmentDir", p, o, fj)) { em.alignmentDir[0]=rdf(o); em.alignmentDir[1]=rdf(o+4); em.alignmentDir[2]=rdf(o+8); }
        rdFloat2(t, "rotationInitial", p, em.rotationInitial);
        rdFloat2(t, "rotationChange", p, em.rotationChange);
        em.rotationDrag = rdFloat1(t, "rotationDrag", p);
        if (fOff(t, "scaleInitial", p, o, fj))
            for (int r = 0; r < 2; ++r) { em.scaleInitial[r][0]=rdf(o+8*r); em.scaleInitial[r][1]=rdf(o+8*r+4); }
        if (fOff(t, "scaleChange", p, o, fj))
            for (int r = 0; r < 2; ++r) { em.scaleChange[r][0]=rdf(o+8*r); em.scaleChange[r][1]=rdf(o+8*r+4); }
        if (fOff(t, "texCoordRect", p, o, fj)) for (int k = 0; k < 4; ++k) em.texCoordRect[k] = rdf(o + 4*k);
        // flipbook
        if (fOff(t, "flipbook", p, o, fj)) {
            size_t fb = follow(o);
            std::string ft = ptrTargetStruct(fj);
            if (fb && !ft.empty()) {
                size_t a2; json f2;
                em.flipbook.present = true;
                if (fOff(ft, "columns", fb, a2, f2)) em.flipbook.columns = d_[a2];
                if (fOff(ft, "count", fb, a2, f2)) em.flipbook.count = d_[a2];
                if (fOff(ft, "rows", fb, a2, f2)) em.flipbook.rows = d_[a2];
                if (fOff(ft, "start", fb, a2, f2)) em.flipbook.start = d_[a2];
                em.flipbook.fps = rdFloat1(ft, "fps", fb);
            }
        }
    }

    void parseLightData(const std::string& root, size_t modl, Model& out) {
        size_t a; json fj;
        if (!fOff(root, "lightData", modl, a, fj)) return;
        size_t ld = follow(a);
        if (!ld) return;
        std::string ldType = ptrTargetStruct(fj);         // ModelLightDataV*
        if (ldType.empty()) return;
        size_t la; json lf;
        if (!fOff(ldType, "effectLights", ld, la, lf)) return;
        std::string lt = lf["element"].value("struct", std::string());
        int ls = typeSize(lt);
        uint32_t n = 0; size_t base = arrayAt(la, n);
        for (uint32_t i = 0; base && ls > 0 && i < n && i < 128; ++i) {
            size_t e = base + (size_t)i * ls;
            EffectLight L;
            size_t o; json f2;
            if (fOff(lt, "bone", e, o, f2)) L.bone = rd64(o);
            if (fOff(lt, "color", e, o, f2)) { L.color[0]=d_[o]/255.0f; L.color[1]=d_[o+1]/255.0f; L.color[2]=d_[o+2]/255.0f; }
            L.intensity = rdFloat1(lt, "intensity", e, 1.0f);
            L.nearDistance = rdFloat1(lt, "nearDistance", e);
            L.farDistance = rdFloat1(lt, "farDistance", e, 1.0f);
            out.effects.lights.push_back(L);
        }
    }

    // ---- skeleton ----
    std::string readCStr(size_t p, size_t cap = 256) const {
        std::string s;
        for (size_t i = 0; i < cap && p + i < n_; ++i) {
            char c = (char)d_[p + i];
            if (!c) break;
            s.push_back(c);
        }
        return s;
    }
    // struct target of a "ptr" field ("" if it isn't a struct ptr)
    static std::string ptrTargetStruct(const json& f) {
        if (f.contains("target") && f["target"].is_object() && f["target"].contains("struct"))
            return f["target"]["struct"].get<std::string>();
        return "";
    }
    // 3x3 inverse of a row-major matrix; false if singular.
    static bool inv3x3(const float m[9], float o[9]) {
        float a=m[0],b=m[1],c=m[2],d=m[3],e=m[4],f=m[5],g=m[6],h=m[7],i=m[8];
        float det = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g);
        if (std::abs(det) < 1e-20f) return false;
        float id = 1.0f/det;
        o[0]=(e*i-f*h)*id; o[1]=(c*h-b*i)*id; o[2]=(b*f-c*e)*id;
        o[3]=(f*g-d*i)*id; o[4]=(a*i-c*g)*id; o[5]=(c*d-a*f)*id;
        o[6]=(d*h-e*g)*id; o[7]=(b*g-a*h)*id; o[8]=(a*e-b*d)*id;
        return true;
    }

    void parseSkeleton(Model& out) {
        std::string root; uint16_t ver = 0;
        size_t skel = findChunk("SKEL", &root, &ver);
        if (!skel || root.empty()) return;
        out.skeleton.fileVersion = (int)ver;
        size_t off; json fj;
        // Optional external skeleton reference (this .modl uses another rig).
        if (fieldOffset(root, "fileReference", off, fj)) out.skeleton.externalRef = decodeFilenameAt(skel + off);
        if (!fieldOffset(root, "skeletonData", off, fj)) return;
        std::string sdType = ptrTargetStruct(fj);
        out.skeleton.skelDataType = sdType;
        size_t sd = follow(skel + off);
        if (!sd || sdType.empty()) return; // inline data absent (external rig)

        if (!fieldOffset(sdType, "grannyModel", off, fj)) return;
        std::string gmType = ptrTargetStruct(fj);
        size_t gm = follow(sd + off);
        if (!gm || gmType.empty()) return;

        if (!fieldOffset(gmType, "Skeleton", off, fj)) return;
        std::string gsType = ptrTargetStruct(fj);
        size_t gs = follow(gm + off);
        if (!gs || gsType.empty()) return;

        if (!fieldOffset(gsType, "Bones", off, fj)) return;
        std::string boneType = fj.contains("element") ? fj["element"].value("struct", std::string()) : std::string();
        if (boneType.empty()) return;
        int boneSize = typeSize(boneType);
        uint32_t bn = 0;
        size_t bbase = arrayAt(gs + off, bn);
        if (bn > 4096) bn = 4096; // sanity guard
        for (uint32_t i = 0; bbase && boneSize > 0 && i < bn; ++i) {
            size_t be = bbase + (size_t)i * boneSize;
            Bone bone;
            size_t o; json f;
            if (fieldOffset(boneType, "Name", o, f)) { size_t s = follow(be + o); if (s) bone.name = readCStr(s); }
            if (fieldOffset(boneType, "ParentIndex", o, f)) bone.parent = (int32_t)rd32(be + o);
            if (fieldOffset(boneType, "LocalTransform", o, f)) {
                std::string tt = f.value("type", std::string());
                size_t ts = be + o, to; json tf;
                if (fieldOffset(tt, "Position", to, tf))    for (int k=0;k<3;++k) bone.localPos[k]  = rdf(ts+to+4*k);
                if (fieldOffset(tt, "Orientation", to, tf)) for (int k=0;k<4;++k) bone.localQuat[k] = rdf(ts+to+4*k);
                if (fieldOffset(tt, "ScaleShear", to, tf))  for (int k=0;k<9;++k) bone.scaleShear[k]= rdf(ts+to+4*k);
            }
            if (fieldOffset(boneType, "InverseWorld4x4", o, f)) for (int k=0;k<16;++k) bone.invWorld[k] = rdf(be+o+4*k);
            out.skeleton.bones.push_back(std::move(bone));
        }
        computeBonePositions(out.skeleton);
        out.skeleton.present = !out.skeleton.bones.empty();
    }

    // decode a raw "filename" pointer field at position p (like decodeFilename but by offset)
    uint32_t decodeFilenameAt(size_t p) {
        size_t t = follow(p);
        if (!t) return 0;
        uint16_t lo = rd16(t), hi = rd16(t + 2);
        uint16_t third = (t + 6 <= n_) ? rd16(t + 4) : 0;
        uint16_t fourth = (t + 8 <= n_) ? rd16(t + 6) : 0;
        if (lo < 0x100 || hi < 0x100 || (third != 0 && (third < 0x100 || fourth != 0))) return 0;
        return (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
    }

    // Bind-pose joint origin = translation of inverse(InverseWorld). Each bone's
    // InverseWorld is the exact model->bone bind matrix used to skin the mesh, so
    // its inverse translation is the joint position in the very space the mesh is
    // in -- no hierarchy accumulation, guaranteed to line up with the geometry.
    // Falls back to hierarchical local-transform composition if a matrix is
    // singular (e.g. all-zero InverseWorld on a helper bone).
    void computeBonePositions(Skeleton& sk) {
        bool anyGood = false;
        std::vector<bool> ok(sk.bones.size(), false);
        for (size_t i = 0; i < sk.bones.size(); ++i) {
            const float* IW = sk.bones[i].invWorld;
            float R[9] = {IW[0],IW[1],IW[2], IW[4],IW[5],IW[6], IW[8],IW[9],IW[10]};
            float t[3] = {IW[12],IW[13],IW[14]};
            float Ri[9];
            if (inv3x3(R, Ri)) {
                // worldPos = -t * Ri (t as a row vector)
                for (int j = 0; j < 3; ++j)
                    sk.bones[i].worldPos[j] = -(t[0]*Ri[0+j] + t[1]*Ri[3+j] + t[2]*Ri[6+j]);
                ok[i] = anyGood = true;
            }
        }
        if (anyGood) {
            // Patch any singular bones with their parent's position (visual only).
            for (size_t i = 0; i < sk.bones.size(); ++i)
                if (!ok[i]) {
                    int p = sk.bones[i].parent;
                    if (p >= 0 && p < (int)sk.bones.size() && ok[p])
                        for (int j = 0; j < 3; ++j) sk.bones[i].worldPos[j] = sk.bones[p].worldPos[j];
                }
            return;
        }
        // No usable InverseWorld anywhere: compose local translations down the
        // hierarchy (rotation-free approximation; good enough to see the rig).
        for (size_t i = 0; i < sk.bones.size(); ++i) {
            int p = sk.bones[i].parent;
            float base[3] = {0,0,0};
            if (p >= 0 && p < (int)i) for (int j=0;j<3;++j) base[j] = sk.bones[p].worldPos[j];
            for (int j = 0; j < 3; ++j) sk.bones[i].worldPos[j] = base[j] + sk.bones[i].localPos[j];
        }
    }

    // ---- embedded animation bank ----
    void parseAnim(Model& out) {
        std::string root; uint16_t ver = 0;
        size_t anim = findChunk("ANIM", &root, &ver);
        if (!anim) return;
        out.anim.present = true;
        out.anim.chunkVersion = ver;
        out.anim.typeKey = root;
        if (root.empty()) return;
        size_t off; json fj;
        // The clip list ("animations") lives directly in older ModelFileAnimation*
        // / *Bank* types, but newer ModelFileAnimationV24/V25 wrap it in a `bank`
        // pointer -> ModelFileAnimationBankV*; follow that first when present.
        std::string listType = root;
        size_t listStart = anim;
        if (!fieldOffset(listType, "animations", off, fj)) {
            if (!fieldOffset(root, "bank", off, fj)) return;
            std::string bankType = ptrTargetStruct(fj);
            size_t bank = follow(anim + off);
            if (!bank || bankType.empty()) return;
            listType = bankType; listStart = bank;
            if (!fieldOffset(listType, "animations", off, fj)) return;
        }
        std::string clipType = fj.contains("element") ? fj["element"].value("struct", std::string()) : std::string();
        uint32_t an = 0;
        size_t abase = arrayAt(listStart + off, an);
        if (an > 8192) an = 8192;
        for (uint32_t i = 0; abase && i < an; ++i) {
            size_t clip = follow(abase + (size_t)i * ptr_); // ptr_array_ptr: array of pointers
            if (!clip) continue;
            AnimClip c;
            c.ptrSize = ptr_;   // granny blob pointers match the packfile's width
            size_t o; json f;
            if (!clipType.empty()) {
                if (fieldOffset(clipType, "token", o, f)) c.token = rd64(clip + o);
                if (fieldOffset(clipType, "moveSpeed", o, f)) c.moveSpeed = rdf(clip + o);
                // data (PackGrannyAnimationType*) -> raw granny blob + fixup table
                if (fieldOffset(clipType, "data", o, f)) {
                    std::string gt = f.value("type", std::string());
                    size_t gs = clip + o, go; json gf;
                    if (!gt.empty() && fieldOffset(gt, "animation", go, gf)) {
                        uint32_t bn2 = 0; size_t bb = arrayAt(gs + go, bn2);
                        if (bb && bn2 && bb + bn2 <= n_) {
                            c.rawGranny.assign(d_ + bb, d_ + bb + bn2);
                            c.grannyFileOffset = bb;
                        }
                    }
                    if (!gt.empty() && fieldOffset(gt, "pointers", go, gf)) {
                        uint32_t pn = 0; size_t pb = arrayAt(gs + go, pn);
                        for (uint32_t k = 0; pb && k < pn && pb + 4u*k + 4 <= n_; ++k)
                            c.fixups.push_back(rd32(pb + 4u*k));
                    }
                }
            }
            out.anim.clips.push_back(std::move(c));
        }
    }

    // Map every mesh's boneBindings token -> skeleton bone index, using the
    // engine's bone-name tokenizer. This is what lets the per-vertex BlendIndices
    // (which index boneBindings) resolve to skeleton bones for skinning.
    void resolveBoneBindings(Model& out) {
        if (out.skeleton.bones.empty()) return;
        std::unordered_map<uint64_t, int> tokMap;
        tokMap.reserve(out.skeleton.bones.size() * 2);
        for (size_t i = 0; i < out.skeleton.bones.size(); ++i)
            tokMap[tokenizeBoneName(out.skeleton.bones[i].name)] = (int)i;
        for (auto& mesh : out.meshes) {
            mesh.boneBindingSkelIndex.assign(mesh.boneBindings.size(), -1);
            for (size_t k = 0; k < mesh.boneBindings.size(); ++k) {
                auto it = tokMap.find(mesh.boneBindings[k]);
                if (it != tokMap.end()) mesh.boneBindingSkelIndex[k] = it->second;
            }
        }
    }
};

// Human-readable breakdown of a GrFvf bitmask: one label per present vertex
// component (with its byte size + offset) and the total per-vertex stride.
// Mirrors Extractor::decodeVertices exactly -- keep the two in sync.
inline std::vector<std::string> describeFvf(uint32_t fvf, int* strideOut = nullptr) {
    std::vector<std::string> comps;
    int stride = 0;
    auto add = [&](const char* label, int sz) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s (%dB @%d)", label, sz, stride);
        comps.emplace_back(buf);
        stride += sz;
    };
    if (fvf & 0x1)
        add((fvf & 0x08000000) ? "Position float4" : (fvf & 0x10000000) ? "Position half3" : "Position float3",
            (fvf & 0x08000000) ? 16 : (fvf & 0x10000000) ? 6 : 12);
    if (fvf & 0x2)  add("BlendWeights", 4);
    if (fvf & 0x4)  add("BlendIndices", 4);
    if (fvf & 0x8)  add((fvf & 0x04000000) ? "Normal packed" : "Normal float3", (fvf & 0x04000000) ? 4 : 12);
    if (fvf & 0x10) add("Color BGRA", 4);
    if (fvf & 0x20) add("Tangent float3", 12);
    if (fvf & 0x40) add("Bitangent float3", 12);
    if (fvf & 0x80) add("TangentFrame u8x12", 12);
    for (int i = 0; i < 8; ++i) {
        bool f32 = (fvf & (0x100u << i)) != 0;
        bool f16 = (fvf & (0x10000u << i)) != 0;
        if (f32 || f16) {
            char label[32];
            std::snprintf(label, sizeof label, "UV%d %s", i, f16 ? "half2" : "float2");
            add(label, f16 ? 4 : 8);
        }
    }
    if (strideOut) *strideOut = stride;
    return comps;
}

} // namespace castlemist::model
#endif
