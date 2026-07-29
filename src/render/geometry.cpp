/// @file
/// @brief Uploading a ModelPreview or a MapScene to the GPU, plus the LOD/submesh selectors.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace castlemist::render {

void clear_model() {
    clear_effects();
    g_vb.Reset();
    g_ib.Reset();
    g_vb_skinned.Reset();
    g_cpu_verts.clear();
    g_vb_cloth.Reset();
    g_cloth.clear();
    g_cloth_enabled = false;
    g_cloth_rest.clear();
    g_cloth_lod0.clear();
    g_cloth_work.clear();
    g_cloth_pieces.clear();
    g_cloth_file_active = false;
    g_vb_clothproxy.Reset();
    g_ib_clothproxy.Reset();
    g_cloth_hide_mat.clear();
    g_render_verts_per_mat.clear();
    g_mats.clear();
    g_subs.clear();
    g_game_mats.clear();
    g_game_any_ok = false;
    g_has_model = false;
    g_skelVb.Reset();
    g_skel_vert_count = 0;
    g_skel_vert_cap = 0;
    g_has_skeleton = false;
    g_joints.clear();
    g_clips.clear();
    g_clip_index = -1;
    g_anim_time = 0.0f;
    g_anim_playing = false;
    g_skin_ok = false;
    g_scene_models.clear();
    g_scene_insts.clear();
    g_scene_mode = false;
}

// Animated world transform of a bone: p_model = lin * p_local + pos (column-vec).

void set_model(const ModelPreview& model) {
    clear_model();
    if (!g_dev || model.meshes.empty()) return;

    std::vector<GVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> lod0idx;   // LOD0 triangles only (for the cloth sim)
    int meshOrdinal = 0;
    for (const auto& m : model.meshes) {
        uint32_t voff = static_cast<uint32_t>(verts.size());
        SubMesh s;
        s.matIndex = m.materialIndex;
        s.hasSkin = m.hasSkin;
        // Submesh centroid (model space), for the game-shader transparent sort.
        double cx = 0, cy = 0, cz = 0;
        for (const auto& v : m.vertices) { cx += v.px; cy += v.py; cz += v.pz; }
        if (!m.vertices.empty()) {
            s.center[0] = static_cast<float>(cx / m.vertices.size());
            s.center[1] = static_cast<float>(cy / m.vertices.size());
            s.center[2] = static_cast<float>(cz / m.vertices.size());
        }
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        // Pack LOD0 then each extra LOD contiguously into the shared index buffer,
        // recording a (start,count) range per LOD. All LODs index the same verts.
        auto packLod = [&](const std::vector<uint32_t>& src) {
            UINT start = static_cast<UINT>(indices.size());
            for (uint32_t i : src) indices.push_back(voff + i);
            s.lodRanges.emplace_back(start, static_cast<UINT>(indices.size()) - start);
        };
        packLod(m.indices);                       // LOD0
        for (uint32_t i : m.indices) lod0idx.push_back(voff + i);   // cloth uses LOD0 topology
        for (const auto& lod : m.lodIndices)      // LOD1..N (skip empty ones so they don't blank the mesh)
            if (!lod.empty()) packLod(lod);
        if (s.lodRanges.empty() || s.lodRanges[0].second == 0) { ++meshOrdinal; continue; }
        s.lod = 0;
        s.indexStart = s.lodRanges[0].first;
        s.indexCount = s.lodRanges[0].second;
        char lbl[96];
        std::snprintf(lbl, sizeof lbl, "mesh %d (mat%u, %u tris, %zu LOD%s)", meshOrdinal, m.materialIndex,
                      s.lodRanges[0].second / 3, s.lodRanges.size(), s.lodRanges.size() == 1 ? "" : "s");
        s.label = lbl;
        g_subs.push_back(std::move(s));
        ++meshOrdinal;
    }
    if (verts.empty() || indices.empty()) {
        g_subs.clear();
        return;
    }

    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
    sd.pSysMem = verts.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g_vb))) { clear_model(); return; }
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    sd.pSysMem = indices.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g_ib))) { clear_model(); return; }

    // Materials (indexed by material.index).
    uint32_t maxIdx = 0;
    for (const auto& m : model.materials) maxIdx = std::max(maxIdx, m.index);
    g_mats.resize(model.materials.empty() ? 0 : maxIdx + 1);
    for (const auto& m : model.materials) {
        MaterialGPU g;
        for (int k = 0; k < 4; ++k) g.tint[k] = m.tint[k];
        g.isEffect = m.isEffect;
        g.kind = m.kind;
        if (m.diffuseTex >= 0 && m.diffuseTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.diffuseTex];
            g.srv = make_srv(t.rgba, t.width, t.height);
            g.srvReduced = make_srv_half(t.rgba, t.width, t.height);
            g.cutout = t.hasCutout;
        }
        if (!m.isEffect && m.normalTex >= 0 && m.normalTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.normalTex];
            g.srvNormal = make_srv(t.rgba, t.width, t.height);
            g.srvNormalReduced = make_srv_half(t.rgba, t.width, t.height);
        }
        if (m.hasRenderState) g.blend = make_blend_state_from_bgfx(m.renderState);
        if (m.index < g_mats.size()) g_mats[m.index] = std::move(g);
    }

    // Keep the rest mesh + LOD0 topology for the live cloth sim (built lazily
    // when the Cloth toggle is switched on). A DYNAMIC buffer receives each
    // simulated frame; reused for any model regardless of skinning.
    g_cloth.clear();
    g_cloth_enabled = false;
    g_cloth_file_active = false;
    g_cloth_rest = verts;
    g_cloth_lod0 = std::move(lod0idx);
    g_cloth_work.clear();
    g_cloth_pieces = model.clothPieces;   // authored cloth (file-driven path)
    // Render-mesh vertex count per material -> tells a "direct" cloth piece (its
    // proxy IS the visible geometry, no/low static mesh) from a coarse cage piece
    // (a small proxy driving a big shared-material render mesh via a runtime bind
    // we don't reproduce). Only direct pieces are simulated + drawn.
    g_render_verts_per_mat.clear();
    for (const auto& mm : model.meshes) {
        if (mm.materialIndex >= g_render_verts_per_mat.size())
            g_render_verts_per_mat.resize(mm.materialIndex + 1, 0);
        g_render_verts_per_mat[mm.materialIndex] += (uint32_t)mm.vertices.size();
    }
    g_vb_cloth.Reset();
    g_vb_clothproxy.Reset();
    g_ib_clothproxy.Reset();
    g_cloth_hide_mat.clear();
    {
        D3D11_BUFFER_DESC cbd{};
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cbd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
        if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_vb_cloth))) g_vb_cloth.Reset();
    }

    g_center = {model.center[0], model.center[1], model.center[2]};
    g_radius = model.radius > 1e-3f ? model.radius : 1.0f;
    g_has_model = true;
    build_game_materials(model); // real bgfx DXBC shaders for the "Shader" mode
    build_skeleton(model);
    capture_effects(model);      // baked particle clouds + effect lights
    // Skinning is possible whenever there's an inline rig (palette is dynamic).
    g_skin_ok = !g_joints.empty() && g_bonePaletteSRV;
    // For the game-shader path we CPU-skin, so keep a rest-pose vertex copy + a
    // DYNAMIC buffer to skin into (only for skinned models).
    if (g_skin_ok) {
        g_cpu_verts = verts;
        D3D11_BUFFER_DESC sbd{};
        sbd.Usage = D3D11_USAGE_DYNAMIC;
        sbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        sbd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
        if (FAILED(g_dev->CreateBuffer(&sbd, nullptr, &g_vb_skinned))) g_vb_skinned.Reset();
    }
    // Fresh model: clear any gizmo edit so the new mesh loads at identity.
    for (int i = 0; i < 3; ++i) { g_obj_pos[i] = 0; g_obj_rot[i] = 0; g_obj_scale[i] = 1; }
    g_gizmo_dragging = false; g_gizmo_axis = -1;
    reset_view();
}


bool upload_scene_model(const ModelPreview& model, SceneModelGPU& out) {
    if (!g_dev || model.meshes.empty()) return false;
    std::vector<GVertex> verts;
    std::vector<uint32_t> indices;
    for (const auto& m : model.meshes) {
        uint32_t voff = static_cast<uint32_t>(verts.size());
        SubMesh s;
        s.indexStart = static_cast<UINT>(indices.size());
        s.matIndex = m.materialIndex;
        s.hasSkin = false; // props render static (no per-scene skinning)
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        for (uint32_t i : m.indices) indices.push_back(voff + i);
        s.indexCount = static_cast<UINT>(indices.size()) - s.indexStart;
        if (s.indexCount) out.subs.push_back(s);
    }
    if (verts.empty() || indices.empty()) return false;

    // Model-space bounds (for map picking + inset framing).
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& v : verts) {
        lo[0] = std::min(lo[0], v.px); hi[0] = std::max(hi[0], v.px);
        lo[1] = std::min(lo[1], v.py); hi[1] = std::max(hi[1], v.py);
        lo[2] = std::min(lo[2], v.pz); hi[2] = std::max(hi[2], v.pz);
    }
    out.center[0] = (lo[0] + hi[0]) * 0.5f; out.center[1] = (lo[1] + hi[1]) * 0.5f;
    out.center[2] = (lo[2] + hi[2]) * 0.5f;
    float ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2];
    out.radius = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
    if (!(out.radius > 1e-4f)) out.radius = 1.0f;

    D3D11_BUFFER_DESC bd{}; D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
    sd.pSysMem = verts.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.vb))) return false;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER; bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    sd.pSysMem = indices.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.ib))) return false;

    uint32_t maxIdx = 0;
    for (const auto& m : model.materials) maxIdx = std::max(maxIdx, m.index);
    out.mats.resize(model.materials.empty() ? 0 : maxIdx + 1);
    for (const auto& m : model.materials) {
        MaterialGPU g;
        for (int k = 0; k < 4; ++k) g.tint[k] = m.tint[k];
        g.isEffect = m.isEffect;
        g.kind = m.kind;
        if (m.diffuseTex >= 0 && m.diffuseTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.diffuseTex];
            g.srv = make_srv(t.rgba, t.width, t.height);
            g.cutout = t.hasCutout;
        }
        if (!m.isEffect && m.normalTex >= 0 && m.normalTex < static_cast<int>(model.textures.size())) {
            const auto& t = model.textures[m.normalTex];
            g.srvNormal = make_srv(t.rgba, t.width, t.height);
        }
        if (m.hasRenderState) g.blend = make_blend_state_from_bgfx(m.renderState);
        if (m.index < out.mats.size()) out.mats[m.index] = std::move(g);
    }
    out.ok = true;
    return true;
}

void clear_scene() {
    g_scene_models.clear();
    g_scene_insts.clear();
    g_scene_mode = false;
    g_focus_model = -1;
}

void set_scene(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances) {
    clear_model();
    clear_scene();
    if (!g_dev || models.empty() || instances.empty()) return;

    g_scene_models.resize(models.size());
    for (size_t i = 0; i < models.size(); ++i) upload_scene_model(models[i], g_scene_models[i]);

    // Two bounding boxes: `plo/phi` = PROPS only, `lo/hi` = props + terrain. The
    // terrain plane spans the WHOLE map rect (often 6k-30k units wide) while the
    // actual content (props/buildings) clusters in a small part of it, so framing
    // on the combined box pushes the camera so far out that the props render tiny
    // -- the "peta daratan terlihat dikompres" symptom. So we frame on the PROP
    // box when there are props, and only fall back to the terrain box for a
    // terrain-only map. The terrain still renders full-size; it just doesn't drag
    // the camera out. COLLISION/ZONE overlays never drive framing (a stray havk
    // volume could blow the box up).
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    float plo[3] = {1e30f, 1e30f, 1e30f}, phi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& in : instances) {
        if (in.model < 0 || in.model >= static_cast<int>(g_scene_models.size())) continue;
        if (!g_scene_models[in.model].ok) continue;
        SceneInstGPU g;
        g.model = in.model;
        g.world = sceneWorld(in.pos, in.rot, in.scale);
        g.layer = in.layer;
        g_scene_insts.push_back(g);
        if (in.layer == LAYER_COLLISION || in.layer == LAYER_ZONE) continue;
        const ModelPreview& mp = models[in.model];
        float r = mp.radius * in.scale;
        // Extra guard: ignore non-finite / absurd extents outright instead of
        // letting one bad instance poison the whole bounding box.
        if (!std::isfinite(r) || r > 200000.0f) continue;
        for (int k = 0; k < 3; ++k) {
            float c = in.pos[k] + mp.center[k] * in.scale;
            if (!std::isfinite(c)) continue;
            lo[k] = std::min(lo[k], c - r);
            hi[k] = std::max(hi[k], c + r);
            if (in.layer == LAYER_PROP) {
                plo[k] = std::min(plo[k], c - r);
                phi[k] = std::max(phi[k], c + r);
            }
        }
    }
    if (g_scene_insts.empty()) return;

    // Prefer the prop box; fall back to the full (prop+terrain) box if no props.
    bool haveProps = plo[0] <= phi[0];
    const float* flo = haveProps ? plo : lo;
    const float* fhi = haveProps ? phi : hi;
    g_scene_center = {(flo[0]+fhi[0])*0.5f, (flo[1]+fhi[1])*0.5f, (flo[2]+fhi[2])*0.5f};
    float ext[3] = {fhi[0]-flo[0], fhi[1]-flo[1], fhi[2]-flo[2]};
    g_scene_radius = 0.5f * std::sqrt(ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2]);
    if (g_scene_radius < 1e-2f) g_scene_radius = 1.0f;
    g_scene_mode = true;
    reset_view();
}

// Appends more models + instances (e.g. a lazily-loaded zone layer) to the live
// scene without disturbing the camera. instance.model is 0-based over `models`.
void add_scene_models(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances) {
    if (!g_dev || !g_scene_mode || models.empty() || instances.empty()) return;
    int base = static_cast<int>(g_scene_models.size());
    g_scene_models.resize(g_scene_models.size() + models.size());
    for (size_t i = 0; i < models.size(); ++i) upload_scene_model(models[i], g_scene_models[base + i]);
    for (const auto& in : instances) {
        if (in.model < 0 || in.model >= static_cast<int>(models.size())) continue;
        int gm = base + in.model;
        if (!g_scene_models[gm].ok) continue;
        SceneInstGPU g;
        g.model = gm;
        g.world = sceneWorld(in.pos, in.rot, in.scale);
        g.layer = in.layer;
        g_scene_insts.push_back(g);
    }
}

bool scene_active() { return g_scene_mode; }

// Builds GPU game (bgfx DXBC) materials for the scene models from a set of
// ModelPreviews whose gameMaterials were extracted (want_game). `models` is
// index-aligned with the FIRST models uploaded by set_scene (props/terrain/
// collision); zone models appended later are left on the reconstruction fallback.
// Idempotent: a model already built is skipped. Enables the map "Shader" mode.

bool scene_game_ready() {
    for (const auto& sm : g_scene_models) if (sm.gameOk) return true;
    return false;
}

// --- map focus/inset preview API (forwarded to the file-local implementations) ---
int scene_pick(int sx, int sy);
int pick_scene_instance(int sx, int sy) { return scene_pick(sx, sy); }
void set_scene_focus(int model_index) {
    g_focus_model = (model_index >= 0 && model_index < static_cast<int>(g_scene_models.size())) ? model_index : -1;
}
int scene_focus() { return g_focus_model; }
void set_show_focus(bool on) { g_show_focus = on; }
bool show_focus() { return g_show_focus; }

void set_layer_visible(int layer, bool visible) { if (layer >= 0 && layer < LAYER_COUNT) g_layer_visible[layer] = visible; }
bool layer_visible(int layer) { return (layer >= 0 && layer < LAYER_COUNT) ? g_layer_visible[layer] : false; }

void set_mode(RenderMode mode) { g_mode = mode; }

// --- per-submesh LOD + texture-size selection (single-model path) -----------
int submesh_count() { return static_cast<int>(g_subs.size()); }
const char* submesh_label(int i) {
    return (i >= 0 && i < static_cast<int>(g_subs.size())) ? g_subs[i].label.c_str() : "";
}
int submesh_lod_count(int i) {
    return (i >= 0 && i < static_cast<int>(g_subs.size())) ? static_cast<int>(g_subs[i].lodRanges.size()) : 0;
}
int max_lod_count() {
    int m = 0;
    for (const auto& s : g_subs) m = std::max(m, static_cast<int>(s.lodRanges.size()));
    return m;
}
static void apply_lod(SubMesh& s, int lod) {
    if (s.lodRanges.empty()) return;
    s.lod = std::clamp(lod, 0, static_cast<int>(s.lodRanges.size()) - 1);
    s.indexStart = s.lodRanges[s.lod].first;
    s.indexCount = s.lodRanges[s.lod].second;
}
// sub < 0 => apply to every submesh (global). Out-of-range LODs clamp per submesh.
void set_submesh_lod(int sub, int lod) {
    if (sub < 0) { for (auto& s : g_subs) apply_lod(s, lod); return; }
    if (sub < static_cast<int>(g_subs.size())) apply_lod(g_subs[sub], lod);
}
int submesh_lod(int i) {
    return (i >= 0 && i < static_cast<int>(g_subs.size())) ? g_subs[i].lod : 0;
}
void set_submesh_tex_reduced(int sub, bool reduced) {
    if (sub < 0) { for (auto& s : g_subs) s.texReduced = reduced; return; }
    if (sub < static_cast<int>(g_subs.size())) g_subs[sub].texReduced = reduced;
}
bool submesh_tex_reduced(int i) {
    return (i >= 0 && i < static_cast<int>(g_subs.size())) && g_subs[i].texReduced;
}


} // namespace castlemist::render
