/// @file
/// @brief CPU-side model payload: geometry, materials, skeleton, effects, cloth.
/// @ingroup extract

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "castlemist/native/granny_anim.hpp"

/// @defgroup extract extract -- MFT entry to previewable payload
/// @brief Sniffs a decompressed .dat entry and builds whatever the UI can show.
///
/// Everything in this layer is CPU-side and thread-safe to build off the UI
/// thread: the renderer uploads the result later. That split is why the types
/// below carry raw `std::vector` pixel/vertex data rather than D3D objects.
/// @{

/// @brief One vertex, laid out exactly like `gw2viewer.cpp`'s GVertex.
///
/// The layout is load-bearing: the model renderer's input layout and the game's
/// own bgfx DXBC shaders both index into it by byte offset, so the field order
/// and the 144-byte stride must not change without updating
/// `castlemist::render::game_sem_off` and the reconstruction input layout together.
///
/// @verbatim
///   0   float3 position     POSITION
///   12  float3 normal       NORMAL
///   24  float3 tangent      TANGENT
///   36  float3 bitangent    BITANGENT
///   48  float2 uv0          TEXCOORD0
///   56  float2 uv1..uv7     TEXCOORD1..7
///   112 uint4  boneIndices  BLENDINDICES
///   128 float4 boneWeights  BLENDWEIGHT
///   144 (stride)
/// @endverbatim
struct GVertex {
    float px, py, pz;      ///< Position (offset 0).
    float nx, ny, nz;      ///< Normal (offset 12).
    float tx, ty, tz;      ///< Tangent (offset 24).
    float bx, by, bz;      ///< Bitangent (offset 36).
    float u, v;            ///< UV0 / TEXCOORD0 (offset 48).

    /// @brief UV1..UV7 (TEXCOORD1..7), offset 56.
    ///
    /// Decoded by gw2model (F16/F32 handled there); absent channels copy UV0.
    /// The game's material shaders sample detail and decal layers from these --
    /// binding UV0 everywhere is what made multi-UV materials render garbled.
    float uv1[7][2] = {};

    /// @brief Bone indices, already remapped to skeleton bone indices (offset 112).
    uint32_t bidx[4] = {0, 0, 0, 0};
    /// @brief Normalized bone weights (offset 128). Unskinned meshes get bone 0, weight 1.
    float bwt[4] = {1, 0, 0, 0};
};

/// @brief A decoded texture referenced by a model material, deduped by fileId.
struct ModelTextureCPU {
    uint32_t fileId = 0;         ///< Source dat fileId.
    /// @brief baseId of the MFT entry the pixels actually came from.
    ///
    /// Not simply `fileId`'s baseId: GW2 ships most textures as a full/reduced
    /// pair at consecutive indices and `decode_texture_by_fileid` picks whichever
    /// member the resolution preference asks for, so this names the entry that was
    /// really decoded -- which is what the texture panel must show (and navigate to).
    uint32_t baseId = 0;
    int width = 0;               ///< Pixels.
    int height = 0;              ///< Pixels.
    int mipCount = 0;            ///< Mip levels present in the source container.
    std::string fmt;             ///< Source format label, e.g. "DXT5", "3DCX/BC5".
    /// @brief Which colour channels actually carry data: "R", "RG", "RGB" or "RGBA".
    ///
    /// Measured from the decoded pixels rather than assumed from @ref fmt, because
    /// the container format is an upper bound only: a DXT5 whose alpha is uniformly
    /// opaque is an RGB texture stored in an RGBA format, and every BCn decode
    /// lands in an RGBA8888 buffer regardless. See `compute_channels`.
    std::string channels;
    std::vector<uint8_t> rgba;   ///< `width * height * 4` bytes, RGBA8888.
    bool isNormal = false;       ///< Classified as a normal map (BC5/3DC/ATI2).
    /// @brief The alpha channel is a real coverage MASK, so the game's alpha-test
    ///        cutout (`discard` where alpha < 0.25) applies to this texture.
    ///
    /// GW2's opaque material shaders cut out on the diffuse alpha rather than
    /// blending with it. But we cannot run that test unconditionally: a diffuse
    /// whose alpha is uniformly 0 (alpha simply unused by that material) would be
    /// erased completely. So this is set only when the alpha channel actually
    /// partitions the texture -- a non-trivial minority of texels below the
    /// threshold and a clear majority above it. See `compute_alpha_cutout`.
    bool hasCutout = false;
};

/// @brief A model material as the reconstruction renderer needs it.
struct ModelMaterialCPU {
    uint32_t index = 0;                   ///< Material slot, matches ModelMeshCPU::materialIndex.
    float tint[4] = {1, 1, 1, 1};         ///< Flat colour multiplier.
    bool isEffect = false;                ///< Additive/translucent effect material.
    int kind = 0;                         ///< 0 normal, 1 terrain, 2 water (procedural shading).
    int diffuseTex = -1;                  ///< Index into ModelPreview::textures (-1 = none).
    int normalTex = -1;                   ///< Index into ModelPreview::textures (-1 = none).
    std::vector<uint32_t> textureFileIds; ///< Every texture the material references (for the info panel).

    /// @brief Real bgfx blend-state word, carried over from game-shader extraction.
    ///
    /// Lets the *reconstruction* path build the material's own blend function
    /// instead of one shared guess for every "effect" material.
    uint64_t renderState = 0;
    /// @brief False when game-shader extraction did not run (map props) or did not resolve.
    bool hasRenderState = false;
};

/// @name Game-shader (real bgfx DXBC) material payload
/// @brief The game's own shaders, extracted so "Shader" mode can draw with them.
///
/// Built on the background thread because it needs the dat and the struct
/// template. All CPU-side: raw DXBC plus the bgfx uniform table and resolved
/// sampler bindings; the renderer creates the D3D shaders, input layout,
/// constant buffers and SRVs on the UI thread. Mirrors the pipeline in
/// `tools/viewer/gw2gsviewer.cpp`.
/// @{

/// @brief One bgfx uniform slot inside a shader's constant buffer.
struct GameShaderUniform {
    std::string name;  ///< bgfx uniform name.
    int type = 0;      ///< bgfx UniformType low nibble (0 = sampler, skipped in cbuffers).
    int stage = 0;     ///< 0 vertex, 1/2 fragment/sampler.
    int byteOff = 0;   ///< Byte offset into the cbuffer (bgfx `reg` is a byte offset).
    int vec4s = 0;     ///< Register count; each register is one float4.
};

/// @brief A texture binding resolved to a t#/s# register.
struct GameSamplerCPU {
    int slot = 0;      ///< The t#/s# register the pixel shader binds this texture to.
    int gameTex = -1;  ///< Index into ModelPreview::textures, or -1 for a global stand-in.
    int global = 0;    ///< 0 = material texture, 1 = white 1x1, 2 = grey env cubemap.
};

/// @brief A per-material constant written straight into the shader cbuffer.
///
/// Bound by token: the AMAT `constants[]` order pairs with the shader's
/// material uniforms, so no name hash is needed (the AMAT constant token is
/// *not* the bgfx uniform-name hash -- see the `gw2-uniform-hash` notes).
struct GameConstOverride {
    int byteOff = 0;              ///< Byte offset into the cbuffer.
    float value[4] = {0, 0, 0, 0};///< The value, as up to four floats.
};

/// @brief Everything needed to render one submesh with the game's own shaders.
struct GameMaterial {
    uint32_t index = 0;                 ///< Matches ModelMaterialCPU::index.
    bool ok = false;                    ///< False = fall back to the reconstruction shader.
    std::vector<uint8_t> vsDXBC;        ///< Vertex shader bytecode.
    std::vector<uint8_t> psDXBC;        ///< Pixel shader bytecode.
    std::vector<GameShaderUniform> vsUniforms; ///< Vertex-stage uniform table.
    std::vector<GameShaderUniform> psUniforms; ///< Pixel-stage uniform table.
    uint32_t vsConstBuf = 0;            ///< bgfx constant-buffer size marker (vertex).
    uint32_t psConstBuf = 0;            ///< bgfx constant-buffer size marker (pixel).
    std::vector<GameSamplerCPU> samplers;      ///< Resolved texture bindings.
    std::vector<GameConstOverride> vsConsts;   ///< Material constants for the vertex cbuffer.
    std::vector<GameConstOverride> psConsts;   ///< Material constants for the pixel cbuffer.
    uint64_t renderState = 0;           ///< bgfx 64-bit state word (blend / depth write).
    /// @brief The picked effect's `shaderPassFlags`.
    ///
    /// Decoded from the engine's draw path (Gw2-64 `sub_140AAFDB0`): 0x4 = no
    /// colour write, 0x8 = no RGB, 0x20 = depth test off, 0x40 = depth write off,
    /// 0x4000 = no ALPHA write, 0x8000 = decal depth bias. The render-target write
    /// mask is built from these, not hardcoded.
    uint32_t shaderPassFlags = 0;
    /// @brief The material's own normal prepass contains a `discard`.
    ///
    /// Authoritative answer to "should the reconstructed normal prepass alpha-test
    /// this material", read straight off the game's prepass shader. Replaces guessing
    /// from the albedo's alpha histogram, which misfires whenever alpha holds a gloss
    /// map instead of coverage.
    bool prepassCutout = false;
};
/// @}

/// @brief One drawable submesh: vertices, LOD index sets and its material slot.
struct ModelMeshCPU {
    std::vector<GVertex> vertices;                 ///< Shared vertex buffer for every LOD.
    std::vector<uint32_t> indices;                 ///< LOD0 (full detail).
    std::vector<std::vector<uint32_t>> lodIndices; ///< LOD1..N, indexing the same vertices.
    uint32_t materialIndex = 0;                    ///< Index into ModelPreview::materials.
    uint32_t fvf = 0;                              ///< Source flexible-vertex-format word.
    uint32_t vertexCount = 0;                      ///< Vertex count as declared by the file.
    int stride = 0;                                ///< Decoded per-vertex byte stride (from the FVF).
    bool hasTangents = false;                      ///< Tangent/bitangent channels were present.
    bool hasSkin = false;                          ///< Vertices carry resolved bone indices and weights.
};

/// @brief One rig joint in bind pose.
///
/// Enough for the renderer to draw the skeleton as a line list over the mesh
/// and to build a skinning palette. `invWorld` is the inverse of the joint's
/// bind world matrix, so `skin = invWorld * animatedWorld`.
struct ModelJoint {
    float pos[3] = {0, 0, 0};   ///< Bind-pose model-space origin.
    int parent = -1;            ///< Index into ModelPreview::joints; -1 for a root.
    std::string name;           ///< Joint name, when the file carries one.

    float localPos[3] = {0, 0, 0};        ///< Bind local translation.
    float localQuat[4] = {0, 0, 0, 1};    ///< Bind local rotation (xyzw).
    float localScale[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; ///< Bind local 3x3 scale/shear.
    /// @brief Row-major model-to-bone bind matrix (`InverseWorld4x4`).
    float invWorld[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

/// @name Baked particle / effect system
/// @brief GW2 bakes fire, spark and glow emitters into the model itself.
///
/// These run on their own timers, so they animate on an idle, un-posed model.
/// The structs mirror `castlemist::model::*` so this header stays free of nlohmann/json.
/// @{

/// @brief Flipbook (sprite-sheet) animation parameters for an emitter's texture.
struct ParticleFlipbookCPU {
    bool present = false;                     ///< Emitter uses a flipbook.
    uint8_t columns = 1, rows = 1;            ///< Sheet dimensions.
    uint8_t count = 1, start = 0;             ///< Frames used and first frame.
    float fps = 0;                            ///< Playback rate.
};

/// @brief One particle emitter.
///
/// Range-valued fields are `{base, span}` pairs: the sampled value is
/// `base + rand01() * span`, matching `GrCloud.cpp`.
struct ParticleEmitterCPU {
    float spawnPeriod = 0;              ///< Seconds between spawn groups.
    float spawnProbability = 1;         ///< Chance a due spawn actually fires.
    float spawnGroupSize[2] = {1, 0};   ///< Particles per spawn `{base, span}`.
    float spawnRadius[2] = {0, 0};      ///< Spawn shape radius `{base, span}`.
    float lifetime[2] = {1, 0};         ///< Particle lifetime `{base, span}`.
    uint8_t spawnShape = 6;             ///< Spawn volume shape id.
    float velocity[4][2] = {};          ///< Initial velocity per axis `{base, span}`.
    float acceleration[4][2] = {};      ///< Acceleration per axis `{base, span}`.
    float drag = 0;                     ///< Velocity damping.
    float colorBegin[2][4] = {};        ///< Start colour range.
    float colorEnd[2][4] = {};          ///< End colour range.
    float colorPeriod = 0;              ///< Colour cycle period.
    float colorFalloff[2] = {0, 0};     ///< Colour falloff `{base, span}`.
    float spawnWindEmit[2] = {0, 0};    ///< Wind-driven emission `{base, span}`.
    float spawnWindSpeed[2] = {0, 0};   ///< Wind-driven speed `{base, span}`.
    uint8_t windInfluence = 0;          ///< How strongly wind moves live particles.
    std::vector<std::pair<float, float>> opacityCurve; ///< (t, value) keys.
    std::vector<std::pair<float, float>> scaleCurve;   ///< (t, value) keys.
    uint32_t opacityCurvePreset = 0;    ///< Built-in curve id when no keys are stored.
    uint32_t scaleCurvePreset = 0;      ///< Built-in curve id when no keys are stored.
    uint32_t emitterFlags = 0;          ///< Emitter behaviour bits.
    uint32_t flags = 0;                 ///< Additional behaviour bits.
    float transform[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0}; ///< Emitter-to-model 3x4 transform.
    bool hasTransform = false;          ///< `transform` was authored.
    bool hasPlane = false;              ///< Emitter spawns on a plane.
    uint8_t alignmentType = 0;          ///< Billboard alignment mode.
    float alignmentDir[3] = {0, 0, 1};  ///< Alignment axis for directional modes.
    float rotationInitial[2] = {0, 0};  ///< Initial spin `{base, span}`.
    float rotationChange[2] = {0, 0};   ///< Spin rate `{base, span}`.
    float rotationDrag = 0;             ///< Spin damping.
    float scaleInitial[2][2] = {{1,0},{1,0}}; ///< Initial size per axis `{base, span}`.
    float scaleChange[2][2] = {};             ///< Size rate per axis `{base, span}`.
    float texCoordRect[4] = {0, 0, 1, 1};     ///< Sub-rect of the material texture.
    ParticleFlipbookCPU flipbook;       ///< Sprite-sheet animation, if any.
    bool hasMesh = false;               ///< Emitter spawns meshes rather than billboards.
    uint32_t meshFileId = 0;            ///< The mesh's fileId when hasMesh.
    int boneJoint = -1;                 ///< Resolved joint index (-1 = model origin).
};

/// @brief A group of emitters sharing a material and a bulk motion.
struct ParticleCloudCPU {
    uint32_t materialIndex = 0;             ///< Index into ModelPreview::materials.
    uint32_t flags = 0;                     ///< Cloud behaviour bits.
    uint32_t fvf = 0;                       ///< Source vertex format word.
    float acceleration[3] = {0, 0, 0};      ///< Cloud-wide acceleration.
    float velocity[3] = {0, 0, 0};          ///< Cloud-wide velocity.
    float drag = 0;                         ///< Cloud-wide damping.
    std::vector<uint32_t> emitterIndices;   ///< Indices into ModelPreview::emitters.
    int boneJoint = -1;                     ///< Resolved joint index (-1 = model origin).
};

/// @brief A baked point light that travels with the model.
struct EffectLightCPU {
    float color[3] = {1, 1, 1};   ///< Linear RGB.
    float intensity = 1;          ///< Scalar strength.
    float nearDistance = 0;       ///< Falloff start.
    float farDistance = 1;        ///< Falloff end.
    int boneJoint = -1;           ///< Resolved joint index (-1 = model origin).
    float pos[3] = {0, 0, 0};     ///< Resolved bind-pose model-space position.
};
/// @}

/// @brief One distance constraint of an authored cloth proxy.
struct ClothEdgeCPU {
    uint16_t a = 0;    ///< First vertex index.
    uint16_t b = 0;    ///< Second vertex index.
    uint16_t rel = 0;  ///< Relationship word (V33+ packs it beside a half-float distance).
    float rest = 0.0f; ///< Rest length.
};

/// @brief One authored cloth simulation piece (`ModelFileData.clothData`).
///
/// GW2 cloth is a self-contained low-resolution *proxy*: its own vertices,
/// triangle indices and distance constraints, with the first `lockCount`
/// vertices pinned to the skeleton. Bound to a render material through
/// `materialIndex`, so the proxy can be textured and the static submesh it
/// replaces can be hidden.
struct ClothPieceCPU {
    uint32_t materialIndex = 0;         ///< Index into ModelPreview::materials.
    uint32_t lockCount = 0;             ///< Pinned vertices, always the leading prefix.
    float gravity = 1.0f;               ///< Gravity scale.
    float drag = 0.5f;                  ///< Velocity damping.
    float wind = 0.5f;                  ///< Wind susceptibility.
    float rigidness = 0.0f;             ///< Constraint stiffness.
    float translateWeight = 0.0f;       ///< How much rig translation drags the cloth.
    std::vector<GVertex> vertices;      ///< Proxy vertices, locked prefix first.
    std::vector<uint32_t> indices;      ///< Proxy triangle list.
    std::vector<ClothEdgeCPU> edges;    ///< `lod0Constraints`.
};

/// @brief Everything needed to render and describe one model.
///
/// Produced entirely on a background thread; the renderer uploads it on the UI
/// thread. A `ModelPreview` is also the unit of a map scene's model list.
struct ModelPreview {
    std::vector<ModelMeshCPU> meshes;         ///< Submeshes.
    std::vector<ModelMaterialCPU> materials;  ///< Materials, indexed by ModelMeshCPU::materialIndex.
    std::vector<ModelTextureCPU> textures;    ///< Deduped decoded textures.
    std::vector<ClothPieceCPU> clothPieces;   ///< Authored cloth (`ModelFileData.clothData`).
    float center[3] = {0, 0, 0};              ///< Bounding-sphere centre, model space.
    float radius = 1.0f;                      ///< Bounding-sphere radius.
    uint32_t totalVerts = 0;                  ///< Sum over all submeshes.
    uint32_t totalTris = 0;                   ///< Sum over all submeshes (LOD0).

    /// @name Skeleton (bind pose)
    /// @{
    std::vector<ModelJoint> joints;   ///< Empty when the model has no inline rig.
    int skeletonVersion = -1;         ///< `ModelFileSkeleton` chunk version (-1 = none).
    std::string skeletonType;         ///< Resolved `ModelSkeletonDataV*` template key.
    uint32_t externalSkeletonRef = 0; ///< fileId of a referenced external rig (0 = inline/none).
    /// @}

    /// @name Embedded animation bank
    /// @brief Curves decoded from the Granny blob into @ref animClips.
    ///
    /// Constant and identity curves decode fully; keyframe formats are
    /// best-effort. Most embedded clips are the static "zeropose" -- real
    /// locomotion lives in external anim files.
    /// @{
    bool hasAnimation = false;             ///< An ANIM chunk was present and parsed.
    int animationVersion = -1;             ///< ANIM chunk version (selects the format variant).
    std::string animationType;             ///< Resolved `ModelFileAnimation*`/`Bank*` key.
    std::vector<uint64_t> animationTokens; ///< Per-clip token64 name hashes.
    std::vector<castlemist::granny::Anim> animClips;   ///< Decoded clips (name, duration, tracks).
    /// @}

    /// @brief Real game shaders per material, for the "Shader" render mode.
    ///
    /// Empty unless the entry was extracted for single-model preview; the map
    /// scene path skips extraction to stay fast and fills this in lazily via
    /// ::augment_map_scene_game. Indexed by GameMaterial::index.
    std::vector<GameMaterial> gameMaterials;

    std::vector<ParticleCloudCPU> clouds;        ///< Baked emitter groups (`cloudData`).
    std::vector<ParticleEmitterCPU> emitters;    ///< Baked emitters.
    std::vector<EffectLightCPU> effectLights;    ///< Baked effect lights (`lightData`).

    /// @brief True when the model carries any baked particle or light effect.
    bool hasEffects() const { return !clouds.empty() || !effectLights.empty(); }
};

/// @}
