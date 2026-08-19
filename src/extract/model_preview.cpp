/// @file
/// @brief MODL packfile to renderable ModelPreview.

#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <span>
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/core/packfile.h"

namespace castlemist::extract {

std::vector<uint8_t> load_modl_bytes_by_fileid(Gw2Dat& dat, uint32_t fileId); // defined below
std::shared_ptr<ModelPreview> build_model_preview(const std::vector<uint8_t>& modl_bytes, Gw2Dat& dat,
                                                   const nlohmann::json& tpl, bool want_game) {
    castlemist::model::Model model;
    try {
        model = castlemist::model::Extractor(modl_bytes, tpl).extract();
    } catch (const std::exception&) {
        return nullptr;
    }
    if (model.meshes.empty()) {
        return nullptr;
    }

    auto out = std::make_shared<ModelPreview>();

    // Resolve an external skeleton reference up front, before the mesh loop, so
    // BOTH the vertex skin bindings and the joint list bind to the right rig. A
    // .modl with no inline skeleton points at another model's rig by fileId
    // (Skeleton.fileReference); Granny keys bones by name, so we load that rig,
    // then re-map every mesh's boneBindings token -> external bone index (gw2model
    // resolved them against the absent inline skeleton, leaving them all -1).
    // Without this the mesh stays unskinned AND gets no joints -- the visible
    // "shared skeleton" failure. `skel` then feeds the joint list below.
    castlemist::model::Model extRig;                              // storage kept alive for the joints section
    const castlemist::model::Skeleton* skel = &model.skeleton;
    if (model.skeleton.bones.empty() && model.skeleton.externalRef != 0) {
        try {
            std::vector<uint8_t> rigBytes = load_modl_bytes_by_fileid(dat, model.skeleton.externalRef);
            if (!rigBytes.empty()) {
                extRig = castlemist::model::Extractor(rigBytes, tpl).extract();
                if (!extRig.skeleton.bones.empty()) {
                    skel = &extRig.skeleton;
                    std::unordered_map<uint64_t, int> tokMap;
                    for (size_t i = 0; i < extRig.skeleton.bones.size(); ++i)
                        tokMap[castlemist::model::tokenizeBoneName(extRig.skeleton.bones[i].name)] = (int)i;
                    for (auto& mesh : model.meshes) {
                        mesh.boneBindingSkelIndex.assign(mesh.boneBindings.size(), -1);
                        for (size_t k = 0; k < mesh.boneBindings.size(); ++k) {
                            auto it = tokMap.find(mesh.boneBindings[k]);
                            if (it != tokMap.end()) mesh.boneBindingSkelIndex[k] = it->second;
                        }
                    }
                }
            }
        } catch (const std::exception&) { /* leave unresolved; info panel still shows the ref */ }
    }

    std::map<uint32_t, int> tex_cache; // fileId -> index into out->textures (-1 = tried, failed)
    auto get_texture = [&](uint32_t fileId) -> int {
        if (fileId == 0) return -1;
        auto it = tex_cache.find(fileId);
        if (it != tex_cache.end()) return it->second;
        ModelTextureCPU tex;
        int idx = -1;
        if (decode_texture_by_fileid(dat, fileId, tex)) {
            idx = static_cast<int>(out->textures.size());
            out->textures.push_back(std::move(tex));
        }
        tex_cache[fileId] = idx;
        return idx;
    };

    // Materials: decode textures, classify diffuse vs normal, detect effects.
    for (const auto& m : model.materials) {
        ModelMaterialCPU mat;
        mat.index = m.index;
        mat.textureFileIds = m.textureFileIds();
        long bestDiffuseArea = -1, bestNormalArea = -1;
        for (uint32_t fid : mat.textureFileIds) {
            int ti = get_texture(fid);
            if (ti < 0) continue;
            const ModelTextureCPU& t = out->textures[ti];
            long area = static_cast<long>(t.width) * t.height;
            if (t.isNormal) {
                if (area > bestNormalArea) { bestNormalArea = area; mat.normalTex = ti; }
            } else {
                if (area > bestDiffuseArea) { bestDiffuseArea = area; mat.diffuseTex = ti; }
            }
        }
        // MODL-side transparency hint for the RECONSTRUCTION path only (the
        // game-shader path below overwrites `isEffect` from the real bgfx blend
        // word). `sortLayer` does not exist on ModelMaterialDataV65 -- the field is
        // `sortOrder` -- so the old `m.sortLayer > 0` half of this test was dead and
        // the hint never fired. Use sortOrder, which is what the file actually
        // carries; materialFlags bit 0 alone is not enough (verified on 291977: the
        // solid jade rods carry it too).
        bool flaggedTranslucent = (m.materialFlags & 0x1u) != 0 && (m.sortOrder > 0 || m.sortLayer > 0);
        if (flaggedTranslucent) {
            mat.isEffect = true;
        } else if (mat.diffuseTex >= 0 && !want_game) {
            // Pixel guess ONLY on the fast map-scene path (want_game=false), where
            // we won't get the material's real bgfx render state. On the
            // single-model path the authoritative blend word below decides, so we
            // skip this leaky heuristic -- it was the source of opaque weapon/armor/
            // mount skins being wrongly forced into the effect glow shader.
            mat.isEffect = looks_like_effect(out->textures[mat.diffuseTex].rgba);
        }
        // A single plausible [0,1] color constant becomes the material tint.
        if (m.constants.size() == 1) {
            const float* v = m.constants[0].value;
            if (v[0] >= 0 && v[0] <= 1 && v[1] >= 0 && v[1] <= 1 && v[2] >= 0 && v[2] <= 1) {
                for (int k = 0; k < 4; ++k) mat.tint[k] = v[k];
            }
        }
        bool matIsEffect = mat.isEffect; // captured before the move below

        // Real game shaders (bgfx DXBC from the material's AMAT), for the
        // "Shader" render mode. Only for the single-model preview path -- the
        // map scene loads many props and skips this to stay fast. Pass the effect
        // flag so translucent materials pick the AMAT's transparent variant.
        GameMaterial gm;
        if (want_game) {
            gm = extract_game_material(dat, tpl, m, get_texture, matIsEffect);
        }
        if (gm.ok) {
            // Prefer the diffuse/albedo texture the real PS actually samples
            // (resolved from its own sampler bindings) over the "biggest
            // non-normal-format texture this material references" guess
            // above -- a material can reference textures (masks, lightmaps,
            // detail maps) that the largest-area heuristic can pick by
            // mistake even though the shader never binds them as albedo.
            int bestSlotTex = -1;
            long bestSlotArea = -1;
            for (const auto& s : gm.samplers) {
                if (s.global != 0 || s.gameTex < 0) continue;
                const ModelTextureCPU& t = out->textures[s.gameTex];
                if (t.isNormal) continue;
                long area = static_cast<long>(t.width) * t.height;
                if (area > bestSlotArea) { bestSlotArea = area; bestSlotTex = s.gameTex; }
            }
            if (bestSlotTex >= 0) mat.diffuseTex = bestSlotTex;
            mat.renderState = gm.renderState;
            mat.hasRenderState = true;
            // The real shader's own bgfx blend word is authoritative: nonzero =>
            // the material is genuinely drawn blended (glow / glass / foliage);
            // zero => it is opaque. Trust it in BOTH directions -- combined with
            // the MODL translucent flag as an OR (either real signal counts), but
            // NO pixel guessing -- so an opaque skin whose diffuse merely *looks*
            // masky is no longer rendered as an effect blob.
            uint32_t blendBits = static_cast<uint32_t>((gm.renderState >> 12) & 0xffffu);
            mat.isEffect = (blendBits != 0) || flaggedTranslucent;
        }

        out->materials.push_back(std::move(mat));
        if (want_game) out->gameMaterials.push_back(std::move(gm));
    }

    // Meshes: convert to GVertex + compute overall bounds.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& src : model.meshes) {
        ModelMeshCPU mesh;
        mesh.materialIndex = src.materialIndex;
        mesh.fvf = src.fvf;
        mesh.vertexCount = src.vertexCount;
        mesh.hasTangents = src.hasTangents;
        // Smooth-skin only if the mesh has per-vertex blend data AND its bindings
        // resolved to bones.
        int resolved = 0;
        for (int bi : src.boneBindingSkelIndex) if (bi >= 0) ++resolved;
        const auto& bmap = src.boneBindingSkelIndex;
        bool smoothSkin = src.hasSkin && !bmap.empty() && resolved > 0;
        // Rigid single-bone attach: weapon blades / rigid props carry no per-vertex
        // weights (hasSkin=false), but the whole sub-mesh binds to ONE bone via its
        // single boneBinding. Without this they get pinned to bone 0 (root) and drift
        // off / separate from the hand as the rig animates. Bind every vertex to that
        // bone so the piece follows its attach bone (assembling into the pose).
        int rigidBone = -1;
        if (!smoothSkin && resolved > 0)
            for (int bi : bmap) if (bi >= 0) { rigidBone = bi; break; }
        mesh.hasSkin = smoothSkin || (rigidBone >= 0);
        castlemist::model::describeFvf(src.fvf, &mesh.stride);
        mesh.vertices.reserve(src.vertices.size());
        for (const auto& v : src.vertices) {
            GVertex g{v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.tx, v.ty, v.tz, v.bx, v.by, v.bz, v.u, v.v};
            for (int c = 0; c < 7; ++c) { g.uv1[c][0] = v.uv[c][0]; g.uv1[c][1] = v.uv[c][1]; }
            if (smoothSkin) {
                for (int c = 0; c < 4; ++c) {
                    int raw = v.boneIdx[c];
                    int sk = (raw >= 0 && raw < static_cast<int>(bmap.size())) ? bmap[raw] : -1;
                    g.bidx[c] = static_cast<uint32_t>(sk >= 0 ? sk : 0);
                    g.bwt[c] = (sk >= 0) ? v.boneWt[c] : 0.0f;
                }
                float s = g.bwt[0] + g.bwt[1] + g.bwt[2] + g.bwt[3];
                if (s > 1e-6f) { for (int c = 0; c < 4; ++c) g.bwt[c] /= s; }
                else { g.bwt[0] = 1; g.bwt[1] = g.bwt[2] = g.bwt[3] = 0; }
            } else if (rigidBone >= 0) {
                g.bidx[0] = static_cast<uint32_t>(rigidBone);
                g.bidx[1] = g.bidx[2] = g.bidx[3] = 0;
                g.bwt[0] = 1.0f; g.bwt[1] = g.bwt[2] = g.bwt[3] = 0.0f;
            }
            mesh.vertices.push_back(g);
            lo[0] = std::min(lo[0], v.px); lo[1] = std::min(lo[1], v.py); lo[2] = std::min(lo[2], v.pz);
            hi[0] = std::max(hi[0], v.px); hi[1] = std::max(hi[1], v.py); hi[2] = std::max(hi[2], v.pz);
        }
        mesh.indices = src.indices;
        mesh.lodIndices = src.lods;   // extra LOD index buffers (same verts)
        out->totalVerts += static_cast<uint32_t>(mesh.vertices.size());
        out->totalTris += static_cast<uint32_t>(mesh.indices.size() / 3);
        out->meshes.push_back(std::move(mesh));
    }
    if (out->totalVerts == 0) {
        return nullptr;
    }

    // Authored cloth pieces (ModelFileData.clothData) -> CPU proxy meshes for the
    // file-driven cloth sim. Each carries its own low-res mesh + constraints + pins.
    for (const auto& cp : model.clothPieces) {
        if (cp.verts.empty() || cp.edges.empty()) continue;
        ClothPieceCPU d;
        d.materialIndex = cp.materialIndex;
        d.lockCount = cp.lockCount;
        d.gravity = cp.gravity; d.drag = cp.drag; d.wind = cp.wind;
        d.rigidness = cp.rigidness; d.translateWeight = cp.translateWeight;
        d.vertices.reserve(cp.verts.size());
        for (const auto& v : cp.verts) {
            GVertex g{v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.tx, v.ty, v.tz, v.bx, v.by, v.bz, v.u, v.v};
            for (int c = 0; c < 7; ++c) { g.uv1[c][0] = v.uv[c][0]; g.uv1[c][1] = v.uv[c][1]; }
            d.vertices.push_back(g);
        }
        d.indices = cp.indices;
        d.edges.reserve(cp.edges.size());
        for (const auto& e : cp.edges) d.edges.push_back({e.a, e.b, e.relationship, e.restLength});
        out->clothPieces.push_back(std::move(d));
    }

    // Skeleton (bind pose) + embedded-animation metadata. `skel` is the inline rig,
    // or the external rig resolved up front (see the fileReference block above).
    out->skeletonVersion = skel->fileVersion;
    out->skeletonType = skel->skelDataType;
    out->externalSkeletonRef = model.skeleton.externalRef;
    out->joints.reserve(skel->bones.size());
    for (const auto& b : skel->bones) {
        ModelJoint j;
        j.pos[0] = b.worldPos[0]; j.pos[1] = b.worldPos[1]; j.pos[2] = b.worldPos[2];
        j.parent = b.parent;
        j.name = b.name;
        for (int k = 0; k < 3; ++k) j.localPos[k] = b.localPos[k];
        for (int k = 0; k < 4; ++k) j.localQuat[k] = b.localQuat[k];
        for (int k = 0; k < 9; ++k) j.localScale[k] = b.scaleShear[k];
        for (int k = 0; k < 16; ++k) j.invWorld[k] = b.invWorld[k];
        out->joints.push_back(std::move(j));
    }
    out->hasAnimation = model.anim.present;
    out->animationVersion = model.anim.present ? static_cast<int>(model.anim.chunkVersion) : -1;
    out->animationType = model.anim.typeKey;
    out->animationModelRef = model.anim.modelReference;
    for (const auto& imp : model.anim.imports) out->animationImports.push_back(imp.fileId);

    // Pull in the external animation banks before decoding. A geometry MODL
    // usually carries one static "zeropose" and names the files holding its real
    // locomotion in ModelFileAnimationBank.imports; without this the model looks
    // rigged but has nothing to play. Same shape as the external-rig resolution
    // above, one level over: reference by fileId, followed through the DAT.
    // Cheap when there is nothing to do -- the call returns immediately on an
    // empty import list.
    castlemist::model::resolveAnimImports(model, tpl, [&](uint32_t fileId) {
        return load_modl_bytes_by_fileid(dat, fileId);
    });

    for (const auto& c : model.anim.clips) {
        out->animationTokens.push_back(c.token);
        if (!c.rawGranny.empty()) {
            castlemist::granny::Anim clip = castlemist::granny::parse(c.rawGranny.data(), c.rawGranny.size(), c.ptrSize);
            if (clip.valid) {
                out->animClips.push_back(std::move(clip));
                out->animClipBank.push_back(c.bankFileId);
            }
        }
    }

    // Particle clouds + effect lights. Resolve each bone token64 to a joint index
    // via the same name->token hash the skeleton uses, so emitters/lights attach
    // to the right bone (bind-pose position).
    {
        const auto& fx = model.effects;
        std::unordered_map<uint64_t, int> tokToJoint;
        for (size_t j = 0; j < model.skeleton.bones.size(); ++j)
            tokToJoint[castlemist::model::tokenizeBoneName(model.skeleton.bones[j].name)] = static_cast<int>(j);
        auto resolveBone = [&](uint64_t tok) -> int {
            if (!tok) return -1;
            auto it = tokToJoint.find(tok);
            return it == tokToJoint.end() ? -1 : it->second;
        };
        auto copy2 = [](const float s[2], float d[2]) { d[0]=s[0]; d[1]=s[1]; };
        for (const auto& c : fx.clouds) {
            ParticleCloudCPU pc;
            pc.materialIndex = c.materialIndex; pc.flags = c.flags; pc.fvf = c.fvf; pc.drag = c.drag;
            for (int k = 0; k < 3; ++k) { pc.acceleration[k]=c.acceleration[k]; pc.velocity[k]=c.velocity[k]; }
            pc.emitterIndices = c.emitterIndices;
            pc.boneJoint = resolveBone(c.bone);
            out->clouds.push_back(std::move(pc));
        }
        for (const auto& e : fx.emitters) {
            ParticleEmitterCPU em;
            em.spawnPeriod = e.spawnPeriod; em.spawnProbability = e.spawnProbability;
            copy2(e.spawnGroupSize, em.spawnGroupSize); copy2(e.spawnRadius, em.spawnRadius); copy2(e.lifetime, em.lifetime);
            em.spawnShape = e.spawnShape; em.drag = e.drag;
            for (int r=0;r<4;++r){ copy2(e.velocity[r],em.velocity[r]); copy2(e.acceleration[r],em.acceleration[r]); }
            for (int r=0;r<2;++r) for (int c=0;c<4;++c){ em.colorBegin[r][c]=e.colorBegin[r][c]; em.colorEnd[r][c]=e.colorEnd[r][c]; }
            em.colorPeriod = e.colorPeriod; copy2(e.colorFalloff, em.colorFalloff);
            copy2(e.spawnWindEmit, em.spawnWindEmit); copy2(e.spawnWindSpeed, em.spawnWindSpeed); em.windInfluence = e.windInfluence;
            em.opacityCurve = e.opacityCurve; em.scaleCurve = e.scaleCurve;
            em.opacityCurvePreset = e.opacityCurvePreset; em.scaleCurvePreset = e.scaleCurvePreset;
            em.emitterFlags = e.emitterFlags; em.flags = e.flags;
            for (int k=0;k<12;++k) em.transform[k]=e.transform[k]; em.hasTransform = e.hasTransform;
            em.hasPlane = e.hasPlane; em.alignmentType = e.alignmentType;
            for (int k=0;k<3;++k) em.alignmentDir[k]=e.alignmentDir[k];
            copy2(e.rotationInitial, em.rotationInitial); copy2(e.rotationChange, em.rotationChange); em.rotationDrag = e.rotationDrag;
            for (int r=0;r<2;++r){ copy2(e.scaleInitial[r],em.scaleInitial[r]); copy2(e.scaleChange[r],em.scaleChange[r]); }
            for (int k=0;k<4;++k) em.texCoordRect[k]=e.texCoordRect[k];
            em.flipbook.present = e.flipbook.present; em.flipbook.columns = e.flipbook.columns;
            em.flipbook.rows = e.flipbook.rows; em.flipbook.count = e.flipbook.count;
            em.flipbook.start = e.flipbook.start; em.flipbook.fps = e.flipbook.fps;
            em.hasMesh = e.hasMesh; em.meshFileId = e.meshFileId;
            em.boneJoint = -1; // emitters attach via their owning cloud's bone; kept per-emitter if set
            out->emitters.push_back(std::move(em));
        }
        for (const auto& L : fx.lights) {
            EffectLightCPU lc;
            for (int k=0;k<3;++k) lc.color[k]=L.color[k];
            lc.intensity = L.intensity; lc.nearDistance = L.nearDistance; lc.farDistance = L.farDistance;
            lc.boneJoint = resolveBone(L.bone);
            if (lc.boneJoint >= 0 && lc.boneJoint < (int)out->joints.size())
                for (int k=0;k<3;++k) lc.pos[k] = out->joints[lc.boneJoint].pos[k];
            out->effectLights.push_back(std::move(lc));
        }
    }

    for (int i = 0; i < 3; ++i) out->center[i] = (lo[i] + hi[i]) * 0.5f;
    float ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    out->radius = 0.5f * std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
    if (out->radius < 1e-3f) out->radius = 1.0f;
    return out;
}

// Convenience overload for the single-model preview path, which (by design,
// see decompress_raw_entry) has no shared Gw2Dat to reuse -- it opens its own
// short-lived one. Fine when called once; NEVER call this in a loop over many
// models (that's exactly the per-model MFT re-parse this file used to do).
std::shared_ptr<ModelPreview> build_model_preview(const std::vector<uint8_t>& modl_bytes, const std::string& dat_path,
                                                   const nlohmann::json& tpl, bool want_game) {
    Gw2Dat dat;
    try {
        load_dat_file(dat, dat_path);
    } catch (const std::exception&) {
        // Leave `dat` at its empty default state: get_by_base_id/decode_texture_by_fileid
        // simply fail to resolve any texture against empty tables, so the model still
        // builds (untextured) instead of the whole preview failing.
    }
    return build_model_preview(modl_bytes, dat, tpl, want_game);
}

// Decompress a MODL entry addressed by fileId (mirrors decode_texture_by_fileid
// but returns the raw decompressed packfile bytes).
std::vector<uint8_t> load_modl_bytes_by_fileid(Gw2Dat& dat, uint32_t fileId) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) return {};
    const MftData& e = dat.mft_data_list[base - 1];
    try {
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
        if (e.compression_flag == 0) return stripped;
        if (stripped.size() < 8) return {};
        uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
        return castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
    } catch (const std::exception&) {
        return {};
    }
}

// Builds a lit grey ground surface from the terrain height-map grid. Vertices
// are placed in the same world space as the props (GW2 is Z-up: X/Y horizontal,
// Z = height), so the terrain lines up under the placed models. Normals come
// from height central-differences for shading. No terrain textures yet.
// hasWaterZ/waterZ: the real water plane, parsed from PackMapCollideV16::

} // namespace castlemist::extract
