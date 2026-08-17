/// @file
/// @brief The "Game 1:1" surface: GW2's own shaders, geometry and state, on a
///        bgfx device of its own, inside one of the app's child windows.
///
/// The drawing logic here is the same as tools/viewer/gw2bgfx/viewer.cpp -- the
/// AMAT shader blobs go to bgfx::createShader as stored, the effect's
/// renderState goes into the state word the client would have built, vertex
/// buffers upload without repacking, and the layout is what
/// GrFvf_BuildVertexLayout produces. What is new is the lifetime: the tool owns
/// a process, this owns a window, so resources are created and destroyed per
/// model rather than leaked until exit.
///
/// castlemist::render is untouched by this file.

#include "castlemist/render/gw2bgfx_view.h"

#ifndef CASTLEMIST_HAVE_BGFX

// Configured without external/bgfx. Everything still links; the UI asks
// available() and hides the mode.
namespace castlemist::gw2bgfxview {
static const std::string kUnavailable = "built without bgfx (external/bgfx missing)";
bool available() { return false; }
bool initialize(HWND) { return false; }
void shutdown() {}
void on_resize(int, int) {}
bool set_model(Gw2Dat&, uint32_t, std::string& error) { error = kUnavailable; return false; }
void clear_model() {}
bool has_model() { return false; }
void orbit(float, float) {}
void zoom(float) {}
void reset_view() {}
void set_rotation_trim(float, float, float) {}
void rotation_trim(float out[3]) { out[0] = out[1] = out[2] = 0.0f; }
void set_force_two_sided(bool) {}
bool force_two_sided() { return false; }
bool has_skeleton() { return false; }
int skeleton_bone_count() { return 0; }
int animation_count() { return 0; }
const char* animation_name(int) { return ""; }
float animation_duration(int) { return 0.0f; }
void set_animation(int) {}
int current_animation() { return -1; }
void set_anim_time(float) {}
float anim_time() { return 0.0f; }
void set_playing(bool) {}
bool is_playing() { return false; }
int skinned_draw_count() { return 0; }
void render() {}
const std::string& last_status() { return kUnavailable; }
} // namespace castlemist::gw2bgfxview

#else

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <span>
#include <vector>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#include "castlemist/format/struct_template.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/granny_pose.hpp"
#include "castlemist/native/gw2_atex.hpp"
#include "castlemist/native/gw2model.hpp"

#include "amat_load.h"
#include "bgfx_draw.h"
#include "gr_fvf.h"
#include "gr_token.h"

using namespace gw2bgfx;
namespace mdl = castlemist::model;

namespace castlemist::gw2bgfxview {
namespace {

struct Vec4 {
    float v[4];
};

/// The paper-doll studio rig, evaluated -- the values the engine supplies that
/// do not come out of the archive. Measured from the client; see
/// docs/research/gw2-engine-uniform-values.md.
const std::map<std::string, Vec4> kEngineUniforms = {
    {"shRed",   {{0.19360f, 0.08209f, 0.13942f, 0.66239f}}},
    {"shGreen", {{0.22712f, 0.09183f, 0.18630f, 0.65143f}}},
    {"shBlue",  {{0.29937f, 0.11538f, 0.25859f, 0.69094f}}},
    {"shSun",       {{-0.46890f, -0.60577f, -0.64279f, 0.0f}}},
    {"shSunColor",  {{0.95120f, 0.99645f, 1.05000f, 1.05f}}},
    {"shSunData",   {{-0.46890f, -0.60577f, -0.64279f, -1.0f}}},
    {"BacklightColor",     {{1.5f, 1.5f, 1.5f, 1.5f}}},
    {"BacklightDirection", {{0.66341f, 0.38302f, -0.64279f, 0.0f}}},
    // Shadowing off: zero here plus a white gSs15 collapses the chain to
    // "fully lit", the right answer with no shadow-caster pass.
    {"WorldToShadowA", {{0, 0, 0, 0}}},
    {"WorldToShadowB", {{0, 0, 0, 0}}},
    {"WorldToShadowC", {{0, 0, 0, 0}}},
    {"WorldToShadowD", {{0, 0, 0, 0}}},
    {"LightCount",            {{0, 0, 0, 0}}},
    {"LightPointAndSpotData", {{0, 0, 0, 0}}},
    // A VECTOR; broadcasting one value wrecks the specular exponent.
    {"LightBuffer",  {{0.25f, 4.0f, 1.0f / 128.0f, 128.0f}}},
    {"TexelOffset",  {{0, 0, 0, 0}}},
    {"AlphaRef",     {{0.25f, 0.25f, 0.25f, 0.25f}}},
    // fxclr defaults to 0 = "fully faded out", and the stipple-discard shaders
    // then discard every pixel.
    {"fxclr",          {{1, 1, 1, 1}}},
    {"StippleDensity", {{0, 0, 0, 0}}},
    {"StencilId",      {{0, 0, 0, 0}}},
    {"FogColorNearMinusFar", {{0, 0, 0, 0}}},
    {"FogColorFar",          {{0, 0, 0, 0}}},
    {"FogColorHeight",       {{0, 0, 0, 0}}},
    {"FogDepthCue",          {{0, 0, 0, 0}}},
    {"FogParam0",            {{0, 0, 0, 0}}},
    {"TexTransform0A", {{1, 0, 0, 0}}}, {"TexTransform0B", {{0, 1, 0, 0}}},
    {"TexTransform1A", {{1, 0, 0, 0}}}, {"TexTransform1B", {{0, 1, 0, 0}}},
    {"TexTransform2A", {{1, 0, 0, 0}}}, {"TexTransform2B", {{0, 1, 0, 0}}},
    {"TexTransform3A", {{1, 0, 0, 0}}}, {"TexTransform3B", {{0, 1, 0, 0}}},
};

/// Routes bgfx's diagnostics somewhere visible. Without this bgfx logs through
/// bx::debugPrintf, i.e. OutputDebugString, and a failed init says nothing at
/// all. Note BX_TRACE is compiled out unless bgfx was built with
/// BX_CONFIG_DEBUG=1, so in a release build only fatal() ever fires.
struct Callback : public bgfx::CallbackI {
    virtual ~Callback() {}
    void fatal(const char* path, uint16_t line, bgfx::Fatal::Enum code, const char* str) override {
        std::fprintf(stderr, "[gw2bgfx FATAL %d] %s:%u: %s\n", (int)code, path, line, str);
    }
    void traceVargs(const char* path, uint16_t line, const char* fmt, va_list ap) override {
        std::fprintf(stderr, "[gw2bgfx] %s:%u: ", path, line);
        std::vfprintf(stderr, fmt, ap);
    }
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void screenShot(const char*, uint32_t, uint32_t, uint32_t, const void*, uint32_t, bool) override {}
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
};
Callback g_callback;

/// One drawable submesh, fully resolved.
struct Draw {
    bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    std::vector<BgfxBlobUniform> vsU, psU;
    std::vector<std::pair<uint8_t, bgfx::TextureHandle>> textures;
    std::map<std::string, Vec4> matConsts;
    /// The state the client would compose for a single-sided surface -- the
    /// effect's cull bits included.
    uint64_t state = 0;
    /// The same state composed with the engine's two-sided bit set, i.e. with
    /// the cull OR skipped. See State::forceTwoSided.
    uint64_t stateTwoSided = 0;
    uint32_t indexCount = 0;

    /// @brief Skeleton bone index per BONE-BINDING SLOT; -1 where unresolved.
    ///
    /// This indirection is the whole trick of GW2 skinning and it is easy to get
    /// wrong. A vertex's `GR_FVF_GROUP` component is four RAW uint8s that index
    /// this geoset's own `boneBindings` table -- NOT the skeleton. Each binding
    /// is a token64 that names a bone, matched through
    /// `castlemist::model::tokenizeBoneName`. So `grbones` is uploaded per draw,
    /// indexed by binding slot, and holds only the bones this geoset actually
    /// touches (2..55 on real models) rather than the whole 294/302-bone rig.
    ///
    /// The engine agrees: `GrFvf.cpp` drops `GR_FVF_GROUP` entirely for geosets
    /// with more than 255 bone bindings, which is exactly the point at which a
    /// uint8 slot index would stop addressing a 255-entry `mat4[255]` palette.
    std::vector<int> boneSlots;

    /// True when the game would skin this draw: the FVF carries both weights and
    /// indices, a rig resolved, and the slot count fits the palette. Drives the
    /// vertex-shader variant, so it is decided at load, not per frame.
    bool skinned = false;
};

struct State {
    bool inited = false;
    HWND hwnd = nullptr;
    int width = 16, height = 16;
    bool sizeDirty = false;

    std::vector<Draw> draws;
    /// Owned textures, by fileId, so a model swap can free them. The 1x1
    /// stand-ins are tracked separately and outlive individual models.
    std::map<uint32_t, bgfx::TextureHandle> texByFileId;
    bgfx::TextureHandle texWhite = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle texCube = BGFX_INVALID_HANDLE;
    /// bgfx dedupes uniforms by name internally, but the host still needs a
    /// handle per name to call setUniform. Kept for the device's lifetime.
    std::map<std::string, bgfx::UniformHandle> uniforms;

    /// Bounding-sphere centre in the MODEL's own space, kept so the render-space
    /// centre can be re-derived whenever `base` changes. Storing only the mapped
    /// result meant a trim change left the pivot behind in the old space.
    float centreModel[3] = {0, 0, 0};
    /// The same point through `base`. This is what the trackball pivots about.
    float centre[3] = {0, 0, 0};
    float radius = 1.0f;
    /// Model -> render space (axis conversion + the per-model trim), WITHOUT
    /// the trackball. `world` below is this with the rotation folded in.
    float base[16];
    float world[16];
    float rotTrim[3] = {0, 0, 0};

    /// Trackball orientation, accumulated as a matrix.
    ///
    /// Not Euler yaw/pitch. With angles the camera's up vector has to be
    /// derived from the pitch, and past +-90 deg it inverts: horizontal drags
    /// suddenly spin the other way and the model appears to jump around. This
    /// is the same model castlemist::render uses -- rotate the OBJECT by a
    /// composed increment and leave the camera still -- so the two views
    /// respond to the mouse identically, which is the point of having both.
    float rot[16];
    /// Camera distance as a multiple of the bounding radius, like
    /// castlemist::render's g_dist, so zoom feels the same in both views.
    float distMul = 3.0f;

    /// Draw every surface two-sided instead of honouring the effect's cull bits.
    ///
    /// The client decides this per surface, from the runtime material word's
    /// 0x4000 bit, and that word cannot be rebuilt from the archive yet (see the
    /// note at the `surf.materialFlags` assignment in setModel). With it stuck at
    /// zero NOTHING is two-sided, so every effect's cull applies -- and since
    /// `shaderPassFlags` bit 0 is set on essentially every GW2 effect, that is
    /// CULL_CCW everywhere. GW2 armour is full of single-sided sheet geometry
    /// (capes, tabards, skirts, loincloths, hair cards), and a sheet whose back
    /// faces are culled simply is not there from the other side: orbit under a
    /// character and the underside of the skirt or cape vanishes.
    ///
    /// Defaulting this on is the lesser error. Drawing a genuinely single-sided
    /// surface two-sided costs some fill rate and can show interior facets;
    /// culling a genuinely two-sided one deletes geometry outright, and a
    /// reference view you cannot orbit under is not much of a reference. Toggle
    /// it off (middle-click the surface) for true 1:1 culling.
    bool forceTwoSided = true;

    /// @name Animation
    /// @{
    /// The resolved rig. Owns the bone names and float arrays that `poseBones`
    /// points into, so it must not be reassigned while `poseBones` is alive.
    mdl::Skeleton skel;
    /// Views into `skel.bones`, built once per model (see granny_pose.hpp).
    std::vector<castlemist::granny::PoseBone> poseBones;
    /// Decoded clips. Filtered to the valid ones, in file order -- the SAME
    /// filter castlemist::render's ModelPreview applies, so clip N means the
    /// same clip in both views and one UI selection can drive both.
    std::vector<castlemist::granny::Anim> clips;
    /// Track-name lookup for the selected clip; rebuilt on selection, not per
    /// frame (a 400-track rig would otherwise rehash every name every frame).
    std::unordered_map<std::string, int> trackByName;
    int clipIndex = -1;   ///< -1 = bind pose.
    float animTime = 0.0f;
    bool playing = false;
    /// Frame clock for `playing`. bgfx has no per-frame delta of its own here.
    uint64_t lastTickMs = 0;
    /// Per-bone model-space pose, recomputed once per frame and shared by every
    /// draw -- the rig is posed once, not once per geoset.
    std::vector<castlemist::granny::PoseXform> pose;
    int skinnedDraws = 0;
    /// @}

    std::string status;
};
State g;

std::vector<uint8_t> decomp(Gw2Dat& dat, uint32_t idx) {
    const MftData& e = dat.mft_data_list[idx];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0) return s;
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
    return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
}

bgfx::UniformHandle uniformFor(const BgfxBlobUniform& u) {
    auto it = g.uniforms.find(u.name);
    if (it != g.uniforms.end()) return it->second;

    bgfx::UniformType::Enum type = bgfx::UniformType::Vec4;
    if (u.isSampler())      type = bgfx::UniformType::Sampler;
    else if (u.type() == 3) type = bgfx::UniformType::Mat3;
    else if (u.type() == 4) type = bgfx::UniformType::Mat4;

    bgfx::UniformHandle h = bgfx::createUniform(u.name.c_str(), type, std::max<uint8_t>(1, u.num));
    g.uniforms[u.name] = h;
    return h;
}

void destroyDraws() {
    for (Draw& d : g.draws) {
        if (bgfx::isValid(d.vb)) bgfx::destroy(d.vb);
        if (bgfx::isValid(d.ib)) bgfx::destroy(d.ib);
        if (bgfx::isValid(d.program)) bgfx::destroy(d.program);
    }
    g.draws.clear();
    // Textures are shared between draws via the fileId cache, so they are freed
    // here rather than per draw. The 1x1 stand-ins are not in this map.
    for (auto& kv : g.texByFileId)
        if (bgfx::isValid(kv.second) && kv.second.idx != g.texWhite.idx) bgfx::destroy(kv.second);
    g.texByFileId.clear();

    // The rig belongs to the model, not the device: drop it with the draws so a
    // model swap cannot leave the next one posed by the previous one's clips, or
    // leave `poseBones` pointing into a freed bone array.
    g.poseBones.clear();
    g.skel = mdl::Skeleton{};
    g.clips.clear();
    g.trackByName.clear();
    g.pose.clear();
    g.clipIndex = -1;
    g.animTime = 0.0f;
    g.playing = false;
    g.skinnedDraws = 0;
}

/// Model -> render space. GW2 authors Z-up; the view is Y-up. Doing the change
/// of basis once, in the world matrix, hands the shader geometry already in the
/// renderer's space.
void buildBase() {
    float axisFix[16];
    bx::mtxIdentity(axisFix);
    axisFix[0] = 1.0f; axisFix[1] = 0.0f; axisFix[2]  = 0.0f;  // e_x -> ( 1, 0, 0)
    axisFix[4] = 0.0f; axisFix[5] = 0.0f; axisFix[6]  = -1.0f; // e_y -> ( 0, 0,-1)
    axisFix[8] = 0.0f; axisFix[9] = 1.0f; axisFix[10] = 0.0f;  // e_z -> ( 0, 1, 0)

    float trim[16];
    bx::mtxRotateXYZ(trim, bx::toRad(g.rotTrim[0]), bx::toRad(g.rotTrim[1]), bx::toRad(g.rotTrim[2]));
    bx::mtxMul(g.base, trim, axisFix);

    // The pivot moves with the basis. Re-deriving it here rather than at load
    // time is the whole point: changing the trim (the right-click flip) rebuilds
    // `base`, and a centre still measured through the OLD base leaves the
    // trackball rotating about a point the model no longer occupies -- the model
    // then swings in a wide arc across the viewport instead of turning in place.
    const bx::Vec3 c =
        bx::mul(bx::Vec3(g.centreModel[0], g.centreModel[1], g.centreModel[2]), g.base);
    g.centre[0] = c.x;
    g.centre[1] = c.y;
    g.centre[2] = c.z;
}

/// Folds the trackball into the world matrix, rotating about the model's own
/// centre rather than the origin -- a model whose bounds sit far off origin
/// (GW2 armour sits ~38 units below it) would otherwise swing around the scene
/// instead of turning in place.
void buildWorld() {
    float toOrigin[16], back[16], tmp[16], spin[16];
    bx::mtxTranslate(toOrigin, -g.centre[0], -g.centre[1], -g.centre[2]);
    bx::mtxTranslate(back, g.centre[0], g.centre[1], g.centre[2]);
    bx::mtxMul(tmp, toOrigin, g.rot);
    bx::mtxMul(spin, tmp, back);
    bx::mtxMul(g.world, g.base, spin);
}

} // namespace

bool available() { return true; }

bool initialize(HWND target_window) {
    if (g.inited) return true;
    if (!target_window) { g.status = "no surface window"; return false; }

    RECT rc{};
    GetClientRect(target_window, &rc);
    g.hwnd = target_window;
    g.width = std::max<int>(16, rc.right - rc.left);
    g.height = std::max<int>(16, rc.bottom - rc.top);

    bgfx::Init init;
    // Direct3D 11 explicitly: the backend the client ships, so the one whose
    // state translation this is matching.
    init.type = bgfx::RendererType::Direct3D11;
    init.resolution.width = (uint32_t)g.width;
    init.resolution.height = (uint32_t)g.height;
    init.resolution.reset = BGFX_RESET_VSYNC;
    init.platformData.nwh = target_window;
    init.callback = &g_callback;
    if (!bgfx::init(init)) {
        g.status = "bgfx::init failed (see stderr)";
        return false;
    }
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20242bff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, uint16_t(g.width), uint16_t(g.height));

    // Shared stand-ins for engine globals we have no offline source for. White
    // at gSs15 means "unshadowed"; the reflection cube is neutral grey, not
    // white, or every metal becomes a mirror.
    {
        const bgfx::Memory* mem = bgfx::alloc(4);
        mem->data[0] = mem->data[1] = mem->data[2] = 255; mem->data[3] = 255;
        g.texWhite = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, mem);

        const bgfx::Memory* cube = bgfx::alloc(6 * 4);
        for (int f = 0; f < 6; ++f) {
            cube->data[f * 4 + 0] = cube->data[f * 4 + 1] = cube->data[f * 4 + 2] = 64;
            cube->data[f * 4 + 3] = 255;
        }
        g.texCube = bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::RGBA8, 0, cube);
    }

    bx::mtxIdentity(g.rot);
    buildBase();
    buildWorld();
    g.inited = true;
    g.status = "bgfx ready";
    return true;
}

void shutdown() {
    if (!g.inited) return;
    destroyDraws();
    for (auto& kv : g.uniforms)
        if (bgfx::isValid(kv.second)) bgfx::destroy(kv.second);
    g.uniforms.clear();
    if (bgfx::isValid(g.texWhite)) bgfx::destroy(g.texWhite);
    if (bgfx::isValid(g.texCube)) bgfx::destroy(g.texCube);
    g.texWhite = BGFX_INVALID_HANDLE;
    g.texCube = BGFX_INVALID_HANDLE;
    bgfx::shutdown();
    g.inited = false;
    g.hwnd = nullptr;
}

void on_resize(int width, int height) {
    if (width <= 0 || height <= 0) return; // minimised: resetting to 0x0 kills the swap chain
    if (width == g.width && height == g.height) return;
    g.width = width;
    g.height = height;
    g.sizeDirty = true;
}

void clear_model() {
    if (!g.inited) return;
    destroyDraws();
    g.status = "no model";
}

bool has_model() { return !g.draws.empty(); }

void orbit(float d_yaw, float d_pitch) {
    // Compose the drag as a rotation applied AFTER the current orientation --
    // the same free trackball castlemist::render::orbit uses. No clamped axis
    // and no gimbal lock, so a drag means the same thing whichever way the
    // model is already facing.
    float ry[16], rx[16], inc[16], out[16];
    bx::mtxRotateY(ry, d_yaw);
    bx::mtxRotateX(rx, d_pitch);
    bx::mtxMul(inc, ry, rx);
    bx::mtxMul(out, g.rot, inc);
    std::memcpy(g.rot, out, sizeof(out));
    buildWorld();
}

void zoom(float factor) { g.distMul = std::clamp(g.distMul * factor, 0.2f, 20.0f); }

void reset_view() {
    bx::mtxIdentity(g.rot);
    g.distMul = 3.0f;
    buildWorld();
}

void set_rotation_trim(float x_deg, float y_deg, float z_deg) {
    g.rotTrim[0] = x_deg;
    g.rotTrim[1] = y_deg;
    g.rotTrim[2] = z_deg;
    if (g.inited) { buildBase(); buildWorld(); }
}

void rotation_trim(float out_deg[3]) {
    out_deg[0] = g.rotTrim[0];
    out_deg[1] = g.rotTrim[1];
    out_deg[2] = g.rotTrim[2];
}

// Both states are composed at load, so this is just a pick at submit -- no
// reload, and it can be flipped between frames.
void set_force_two_sided(bool on) { g.forceTwoSided = on; }
bool force_two_sided() { return g.forceTwoSided; }

bool has_skeleton() { return !g.skel.bones.empty(); }
int skeleton_bone_count() { return (int)g.skel.bones.size(); }
int skinned_draw_count() { return g.skinnedDraws; }
int animation_count() { return (int)g.clips.size(); }

const char* animation_name(int i) {
    return (i >= 0 && i < (int)g.clips.size()) ? g.clips[i].name.c_str() : "";
}
float animation_duration(int i) {
    return (i >= 0 && i < (int)g.clips.size()) ? g.clips[i].duration : 0.0f;
}
int current_animation() { return g.clipIndex; }

void set_animation(int clip_index) {
    if (clip_index < -1 || clip_index >= (int)g.clips.size()) clip_index = -1;
    g.clipIndex = clip_index;
    g.animTime = 0.0f;
    g.lastTickMs = GetTickCount64();
    // Cache the clip's track lookup here rather than in render(): a 400-track rig
    // would otherwise rehash every track name on every frame.
    g.trackByName.clear();
    if (clip_index >= 0) g.trackByName = castlemist::granny::trackIndexByName(g.clips[clip_index]);
}

void set_anim_time(float seconds) { g.animTime = seconds; }
float anim_time() { return g.animTime; }

void set_playing(bool playing) {
    g.playing = playing;
    // Reset the clock on every transition, so a pause does not bank elapsed wall
    // time and jump the pose forward when playback resumes.
    g.lastTickMs = GetTickCount64();
}
bool is_playing() { return g.playing; }

const std::string& last_status() { return g.status; }

bool set_model(Gw2Dat& dat, uint32_t mft_index, std::string& error) {
    if (!g.inited) { error = "bgfx surface not initialised"; return false; }
    destroyDraws();

    auto tplPtr = castlemist::tpl::get_or_auto_load();
    if (!tplPtr) {
        error = "no struct template loaded (File -> Load Struct JSON...)";
        g.status = error;
        return false;
    }
    const nlohmann::json& tpl = *tplPtr;

    if (mft_index >= dat.mft_data_list.size()) { error = "MFT index out of range"; g.status = error; return false; }

    std::vector<mdl::GeosetRaw> geosets;
    mdl::Model model;
    try {
        std::vector<uint8_t> modlBytes = decomp(dat, mft_index);
        mdl::Extractor ex(modlBytes, tpl);
        model = ex.extract();
        geosets = ex.extractGeosetsRaw();
    } catch (const std::exception& e) {
        error = std::string("model load failed: ") + e.what();
        g.status = error;
        return false;
    }
    if (geosets.empty()) { error = "model has no geosets"; g.status = error; return false; }

    // ------------------------------------------------------------------
    // The rig. Inline when the MODL carries skeleton data; otherwise the SKEL
    // chunk's `fileReference` names another model's rig and we follow it, the
    // same resolution build_model_preview does. Many rigged assets have
    // NEITHER: an armour piece binds to the skeleton the character assembly
    // supplies at runtime, and the piece does not reference it. Those keep their
    // bone bindings but cannot be posed here, and draw unskinned.
    // ------------------------------------------------------------------
    mdl::Model extRig;
    const mdl::Skeleton* srcSkel = &model.skeleton;
    if (model.skeleton.bones.empty() && model.skeleton.externalRef != 0) {
        try {
            uint32_t rigBase = get_by_base_id(dat, model.skeleton.externalRef);
            if (rigBase && rigBase - 1 < dat.mft_data_list.size()) {
                std::vector<uint8_t> rigBytes = decomp(dat, rigBase - 1);
                extRig = mdl::Extractor(rigBytes, tpl).extract();
                if (!extRig.skeleton.bones.empty()) srcSkel = &extRig.skeleton;
            }
        } catch (const std::exception&) { /* no rig: draw unskinned */ }
    }
    g.skel = *srcSkel;
    g.poseBones.resize(g.skel.bones.size());
    for (size_t i = 0; i < g.skel.bones.size(); ++i) {
        const mdl::Bone& b = g.skel.bones[i];
        g.poseBones[i] = {b.name.c_str(), b.parent, b.localPos, b.localQuat, b.scaleShear, b.invWorld};
    }

    // Decode the embedded clips. `ptrSize` is per clip and must be passed
    // through: a 64-bit packfile's blob reads Duration out of the Name
    // pointer's high dword otherwise (see granny_anim.hpp). Keeping only the
    // valid ones matches ModelPreview's filter, which is what keeps clip
    // indices identical between this view and castlemist::render.
    for (const mdl::AnimClip& c : model.anim.clips) {
        if (c.rawGranny.empty()) continue;
        castlemist::granny::Anim a =
            castlemist::granny::parse(c.rawGranny.data(), c.rawGranny.size(), c.ptrSize);
        if (a.valid) g.clips.push_back(std::move(a));
    }

    // boneBindings token64 -> skeleton index, via the engine's own bone-name
    // tokenizer. Built once for the model and shared by every geoset.
    std::unordered_map<uint64_t, int> tokMap;
    tokMap.reserve(g.skel.bones.size() * 2);
    for (size_t i = 0; i < g.skel.bones.size(); ++i)
        tokMap[mdl::tokenizeBoneName(g.skel.bones[i].name)] = (int)i;

    // Bounding sphere, for framing.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& gs : geosets)
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], gs.minB[i]);
            hi[i] = std::max(hi[i], gs.maxB[i]);
        }
    g.centreModel[0] = (lo[0] + hi[0]) * 0.5f;
    g.centreModel[1] = (lo[1] + hi[1]) * 0.5f;
    g.centreModel[2] = (lo[2] + hi[2]) * 0.5f;
    g.radius = 0.0f;
    for (int i = 0; i < 3; ++i) g.radius = std::max(g.radius, (hi[i] - lo[i]) * 0.5f);
    if (!(g.radius > 0.0f)) g.radius = 1.0f;

    // buildBase maps centreModel through the new basis. The pivot is measured
    // through `base` only -- never through the trackball -- or it would chase
    // its own result.
    buildBase();
    // A new model gets a fresh orientation; leaving the old trackball on would
    // show the next model at whatever angle the last one was left at.
    bx::mtxIdentity(g.rot);
    g.distMul = 3.0f;
    buildWorld();

    // Textures. base - 1: get_by_base_id returns a 1-based baseId, so reading
    // `base` lands on the next archive entry -- which parses as a neighbouring
    // ATEX just often enough to bind the wrong image silently.
    auto loadTexture = [&](uint32_t fileId) -> bgfx::TextureHandle {
        auto it = g.texByFileId.find(fileId);
        if (it != g.texByFileId.end()) return it->second;
        bgfx::TextureHandle h = BGFX_INVALID_HANDLE;
        uint32_t base = get_by_base_id(dat, fileId);
        if (base && base - 1 < dat.mft_data_list.size()) {
            try {
                std::vector<uint8_t> bytes = decomp(dat, base - 1);
                castlemist::atex::Texture t = castlemist::atex::parse(bytes.data(), bytes.size());
                castlemist::atex::Image im = castlemist::atex::decode(t, 0);
                if (im.width > 0 && im.height > 0) {
                    const bgfx::Memory* mem = bgfx::copy(im.rgba.data(), (uint32_t)im.rgba.size());
                    h = bgfx::createTexture2D((uint16_t)im.width, (uint16_t)im.height, false, 1,
                                              bgfx::TextureFormat::RGBA8, 0, mem);
                }
            } catch (const std::exception&) { /* fall through to the stand-in */ }
        }
        if (!bgfx::isValid(h)) h = g.texWhite;
        g.texByFileId[fileId] = h;
        return h;
    };

    // Opaque render mode -- a render-mode token, not a material id, and the one
    // the paper-doll writes when not fading.
    const uint64_t effectToken = 0x914C6A8A883B1EEull;
    const int maxQuality = 4;

    std::map<uint32_t, AmatPackage> amatByMaterial;
    int skipped = 0;

    for (const mdl::GeosetRaw& gs : geosets) {
        if (gs.vertexBytes.empty() || gs.indices.empty()) { ++skipped; continue; }

        bgfx::VertexLayout layout = grFvfBuildVertexLayout(gs.fvf);
        if (!grFvfStrideMatches(gs.fvf, layout)) { ++skipped; continue; }

        const mdl::Material* mat = nullptr;
        for (const auto& m : model.materials)
            if (m.index == gs.materialIndex) { mat = &m; break; }
        if (!mat) { ++skipped; continue; }

        auto ai = amatByMaterial.find(gs.materialIndex);
        if (ai == amatByMaterial.end()) {
            AmatPackage pkg;
            uint32_t fnBase = mat->materialFile ? get_by_base_id(dat, mat->materialFile) : 0;
            if (fnBase && fnBase - 1 < dat.mft_data_list.size()) {
                try {
                    std::vector<uint8_t> amatBytes = decomp(dat, fnBase - 1);
                    pkg = convertAmat(mdl::Extractor(amatBytes, tpl).extractAmatTree());
                } catch (const std::exception& e) { pkg.error = e.what(); }
            } else {
                pkg.error = "material has no AMAT file";
            }
            ai = amatByMaterial.emplace(gs.materialIndex, std::move(pkg)).first;
        }
        const AmatPackage& pkg = ai->second;
        if (!pkg.ok()) { ++skipped; continue; }

        const int tech = amatSelectTechnique(pkg, maxQuality);

        // Skinning needs BOTH halves of the vertex feed: `GR_FVF_WEIGHTS` (4 x
        // uint8 normalized) and `GR_FVF_GROUP` (4 x uint8 RAW slot indices).
        // Testing `!= 0` on the pair treated a weights-only geoset as skinned
        // and bound the game's skinned vertex shader to a buffer with no bone
        // indices at all. That combination is not hypothetical: `GrFvf.cpp`
        // DROPS `GR_FVF_GROUP` when a geoset has more than 255 bone bindings,
        // leaving exactly weights-without-indices.
        const bool hasSkinFeed = (gs.fvf & GR_FVF_WEIGHTS) && (gs.fvf & GR_FVF_GROUP);

        // Resolve this geoset's binding slots to rig bones. A slot count past
        // the palette's 255 entries cannot be addressed by a uint8 index, which
        // is the same limit the engine enforces by dropping GROUP.
        std::vector<int> boneSlots;
        if (hasSkinFeed && !g.skel.bones.empty() && gs.boneBindings.size() <= 255) {
            boneSlots.assign(gs.boneBindings.size(), -1);
            for (size_t k = 0; k < gs.boneBindings.size(); ++k) {
                auto it = tokMap.find(gs.boneBindings[k]);
                if (it != tokMap.end()) boneSlots[k] = it->second;
            }
        }
        // An unresolvable rig means the palette would be all identity, so ask
        // for the plain variant instead and let the geometry draw in bind pose
        // rather than binding a skinned shader to a palette that says nothing.
        const bool skinned = !boneSlots.empty();
        // The skinned feed is the one the draw loop reaches via bits 0x2|0x4
        // (weights + indices) -- variant 1, the ONLY variant whose vertex shader
        // declares `grbones`. Passing 0x80 here asked for variant 2, which does
        // not read the palette at all: the mesh would have drawn in bind pose no
        // matter how correct the uploaded matrices were. See GrVsVariant.
        const uint32_t variant =
            vsVariantFromMeshFlags(skinned ? (GR_FVF_WEIGHTS | GR_FVF_GROUP) : 0u, 0u, false);

        // Pass 0 is the one that paints; later passes need a depth prepass we
        // do not run.
        AmatSelection sel = amatSelectEffect(pkg, tech, 0, effectToken, variant);
        if (!sel.ok) { ++skipped; continue; }

        const AmatShaderBinary& vsBin = pkg.shaders[sel.vertexShaderIndex].dx11Shader;
        const AmatShaderBinary& psBin = pkg.shaders[sel.pixelShaderIndex].dx11Shader;

        bgfx::ShaderHandle vsh = bgfx::createShader(bgfx::copy(vsBin.data.data(), (uint32_t)vsBin.data.size()));
        bgfx::ShaderHandle fsh = bgfx::createShader(bgfx::copy(psBin.data.data(), (uint32_t)psBin.data.size()));
        if (!bgfx::isValid(vsh) || !bgfx::isValid(fsh)) { ++skipped; continue; }

        Draw d;
        d.program = bgfx::createProgram(vsh, fsh, true);
        if (!bgfx::isValid(d.program)) { ++skipped; continue; }

        d.vsU = parseBgfxBlobUniforms(vsBin.data);
        d.psU = parseBgfxBlobUniforms(psBin.data);
        for (const auto& u : d.vsU) uniformFor(u);
        for (const auto& u : d.psU) uniformFor(u);

        for (const auto& s : psBin.samplers) {
            bgfx::TextureHandle h;
            if (s.textureIndex < mat->textures.size()) h = loadTexture(mat->textures[s.textureIndex].fileId);
            else if (s.textureSlot == 13)              h = g.texCube;
            else                                       h = g.texWhite;
            d.textures.emplace_back((uint8_t)s.textureSlot, h);
        }

        // MODL material constants bind by NAME: the token32 decodes straight to
        // the uniform's name (base-23, not a hash).
        for (const auto& cst : mat->constants) {
            std::string name = tokenDecode32(cst.name);
            if (!name.empty())
                d.matConsts[name] = Vec4{{cst.value[0], cst.value[1], cst.value[2], cst.value[3]}};
        }

        GrSurfaceState surf;
        surf.materialToken = effectToken;
        // NOT mat->materialFlags. That is ModelMaterialDataV*::materialFlags, a
        // file-format field; GrSurfaceState::materialFlags is the runtime word
        // *(surface->material + 28), which BgfxDraw_ComputeDepthState reads as a
        // state override mask (0x200 = force DEPTH_TEST_GREATER, 0x800 = kill
        // the RGB write mask). Feeding the file field in reads 0xA08 on every
        // material of some models: nothing is drawn at all. Zero means "no
        // overrides", which for pass 0 is LEQUAL + depth write + RGB|A.
        surf.materialFlags = 0;
        d.state = grComposeDrawState(*sel.effect, 0, surf).state;

        // The same composition with the engine's two-sided bit, for
        // State::forceTwoSided. Composing it rather than masking the cull field
        // out of `d.state` afterwards keeps this on the client's own path:
        // BgfxShader_SelectEffect (0x140BFDC40) guards the cull OR with
        // `(materialFlags & 0x4000) == 0` and ORs `effect.renderState` in
        // unconditionally afterwards, so masking would also strip any cull bits
        // that came from renderState -- which the client would have kept.
        // 0x4000 touches nothing else: neither grWriteMask (0x800 / 0x400) nor
        // grComputeDepthState (0x80 / 0x100 / 0x200 / 0x1000 / 0x2000) reads it.
        GrSurfaceState surfTwoSided = surf;
        surfTwoSided.materialFlags = 0x4000u;
        d.stateTwoSided = grComposeDrawState(*sel.effect, 0, surfTwoSided).state;

        d.boneSlots = std::move(boneSlots);
        d.skinned = skinned;
        if (skinned) ++g.skinnedDraws;

        const bgfx::Memory* vmem = bgfx::copy(gs.vertexBytes.data(), (uint32_t)gs.vertexBytes.size());
        d.vb = bgfx::createVertexBuffer(vmem, layout);
        const bgfx::Memory* imem = bgfx::copy(gs.indices.data(), (uint32_t)(gs.indices.size() * 2));
        d.ib = bgfx::createIndexBuffer(imem);
        d.indexCount = (uint32_t)gs.indices.size();
        g.draws.push_back(std::move(d));
    }

    if (g.draws.empty()) {
        error = "no drawable geosets (all " + std::to_string(geosets.size()) + " skipped)";
        g.status = error;
        return false;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%zu draws from %zu geosets%s%s", g.draws.size(), geosets.size(),
                  skipped ? (" (" + std::to_string(skipped) + " skipped)").c_str() : "",
                  g.skel.bones.empty()
                      ? (g.clips.empty() ? "" : " -- clips present but no rig in this file")
                      : (" -- rig " + std::to_string(g.skel.bones.size()) + " bones, " +
                         std::to_string(g.skinnedDraws) + " skinned, " +
                         std::to_string(g.clips.size()) + " clips").c_str());
    g.status = buf;
    return true;
}

void render() {
    if (!g.inited) return;

    if (g.sizeDirty) {
        bgfx::reset((uint32_t)g.width, (uint32_t)g.height, BGFX_RESET_VSYNC);
        g.sizeDirty = false;
    }
    bgfx::setViewRect(0, 0, 0, uint16_t(g.width), uint16_t(g.height));
    bgfx::touch(0);

    if (g.draws.empty()) { bgfx::frame(); return; }

    // The camera does not move: it sits back along -Z with a plain +Y up, and
    // the trackball turns the model instead (see State::rot). That is what
    // castlemist::render does, and it is why the up vector can be a constant --
    // there is no pole for it to flip across.
    const float camDist = g.radius * g.distMul;
    const float eye[3] = {g.centre[0], g.centre[1], g.centre[2] - camDist};
    const float up[3] = {0.0f, 1.0f, 0.0f};

    float view[16], proj[16], viewProj[16];
    bx::mtxLookAt(view, bx::Vec3(eye[0], eye[1], eye[2]),
                  bx::Vec3(g.centre[0], g.centre[1], g.centre[2]), bx::Vec3(up[0], up[1], up[2]));
    // Near/far bracketed to the model so the depth buffer keeps its precision.
    // Generous on the far side: the trackball can swing a long model's far end
    // well past the centre, and clipping it looks like geometry going missing.
    const float zn = std::max(camDist - g.radius * 2.0f, g.radius * 0.02f);
    const float zf = camDist + g.radius * 6.0f;
    const float aspect = float(g.width) / float(g.height > 0 ? g.height : 1);
    bx::mtxProj(proj, 60.0f, aspect, zn, zf, bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(viewProj, view, proj);

    float worldView[16];
    bx::mtxMul(worldView, g.world, view);

    // GW2's shaders want the transpose of what bx builds: bx is row-vector
    // (`v * M`), GW2's HLSL multiplies `mul(M, v)`. Uploaded as bx builds them,
    // every vertex lands off screen.
    float viewProjT[16], worldT[16], worldViewT[16];
    bx::mtxTranspose(viewProjT, viewProj);
    bx::mtxTranspose(worldT, g.world);
    bx::mtxTranspose(worldViewT, worldView);

    // ------------------------------------------------------------------
    // Pose the rig once for the whole frame. Every skinned draw reads the same
    // per-bone result and only differs in which slots it picks out of it.
    //
    // The palette is MODEL-space: a skin matrix takes a bind-space vertex to
    // animated model space, and `World` then does model -> render exactly as it
    // does for static geometry. So nothing about the camera or the axis fix has
    // to change to support skinning.
    // ------------------------------------------------------------------
    const bool posed = (g.clipIndex >= 0 && g.clipIndex < (int)g.clips.size() && !g.poseBones.empty());
    if (posed) {
        const castlemist::granny::Anim& clip = g.clips[g.clipIndex];
        if (g.playing) {
            const uint64_t now = GetTickCount64();
            if (g.lastTickMs != 0 && now > g.lastTickMs)
                g.animTime += float(now - g.lastTickMs) * 0.001f;
            g.lastTickMs = now;
        }
        // Wrap rather than clamp, so a clip loops instead of freezing on its
        // last key. granny_anim.hpp's sampler replicates boundary knots and does
        // not loop by itself -- that is a playback decision, not a curve one.
        float t = g.animTime;
        if (clip.duration > 1e-6f) {
            t = std::fmod(t, clip.duration);
            if (t < 0.0f) t += clip.duration;
        }
        castlemist::granny::composePose(g.poseBones, &clip, t, g.pose, &g.trackByName);
    }

    // Scratch for one draw's palette, reused across draws and frames.
    std::vector<float> palette;

    for (const Draw& d : g.draws) {
        auto setAll = [&](const std::vector<BgfxBlobUniform>& list) {
            for (const auto& u : list) {
                if (u.isSampler()) continue;
                bgfx::UniformHandle h = uniformFor(u);
                if (u.name == "ViewProjection") { bgfx::setUniform(h, viewProjT); continue; }
                if (u.name == "World")          { bgfx::setUniform(h, worldT); continue; }
                if (u.name == "WorldView")      { bgfx::setUniform(h, worldViewT); continue; }
                // The engine's bone palette: `mat4 grbones[255]`, global param
                // index 115 (GrGetGlobalParamIndex, table at 0x141BF9660). Read
                // straight off the client's own embedded shaders, whose uniform
                // table records grbones as type 4 / num 255 / regCount 1020
                // (= 255 x 4 float4 rows) -- not inferred.
                //
                // Indexed by BONE-BINDING SLOT, not by rig bone; see
                // Draw::boneSlots. Only the slots this geoset uses are written,
                // and only as many entries as the shader declared: uploading the
                // full 255 when the shader asked for fewer would overrun its
                // constant buffer.
                if (u.name == "grbones") {
                    const uint16_t cap = std::max<uint8_t>(1, u.num);
                    const size_t n = std::min<size_t>(d.boneSlots.size(), cap);
                    palette.assign((size_t)cap * 16, 0.0f);
                    for (size_t s = 0; s < (size_t)cap; ++s) {
                        float* m = palette.data() + s * 16;
                        const int bone = (s < n) ? d.boneSlots[s] : -1;
                        if (!posed || bone < 0 || bone >= (int)g.pose.size()) {
                            m[0] = m[5] = m[10] = m[15] = 1.0f; // identity -> bind pose
                            continue;
                        }
                        // Transposed, because GW2's HLSL multiplies mul(M, v) --
                        // the same reason World/ViewProjection go up transposed
                        // above. Uploading the row-vector form skins every vertex
                        // to garbage.
                        castlemist::granny::skinMatrixColumnMajor(g.poseBones[bone], g.pose[bone], m);
                    }
                    bgfx::setUniform(h, palette.data(), cap);
                    continue;
                }
                if (u.name == "CameraPosition") {
                    const float v[4] = {eye[0], eye[1], eye[2], 1.0f};
                    bgfx::setUniform(h, v);
                    continue;
                }
                if (u.name == "Time") {
                    const float t = float(GetTickCount64() % 100000) * 0.001f;
                    const float v[4] = {t, t, t, t};
                    bgfx::setUniform(h, v);
                    continue;
                }
                auto mc = d.matConsts.find(u.name);
                if (mc != d.matConsts.end()) { bgfx::setUniform(h, mc->second.v); continue; }
                auto eg = kEngineUniforms.find(u.name);
                if (eg != kEngineUniforms.end()) { bgfx::setUniform(h, eg->second.v); continue; }
            }
        };
        setAll(d.vsU);
        setAll(d.psU);

        for (size_t i = 0; i < d.textures.size(); ++i) {
            const uint8_t slot = d.textures[i].first;
            for (const auto& u : d.psU) {
                if (!u.isSampler() || u.regIndex != slot) continue;
                bgfx::setTexture(slot, uniformFor(u), d.textures[i].second);
                break;
            }
        }

        bgfx::setVertexBuffer(0, d.vb);
        bgfx::setIndexBuffer(d.ib, 0, d.indexCount);
        bgfx::setState(g.forceTwoSided ? d.stateTwoSided : d.state);
        bgfx::submit(0, d.program, 0, BGFX_DISCARD_ALL);
    }

    bgfx::frame();
}

} // namespace castlemist::gw2bgfxview

#endif // CASTLEMIST_HAVE_BGFX
