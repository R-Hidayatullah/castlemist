#ifndef GW2_MODEL_RENDERER_H
#define GW2_MODEL_RENDERER_H

#include <windows.h>

#include "castlemist/extract/entry_extractor.h" // ModelPreview

/// Direct3D 11 orbit-camera viewer for GW2 .modl models. Its own device +
/// swapchain + depth buffer, bound to a dedicated child window (a sibling of the
/// castlemist::gfx image surface). Shaders are our own runtime-compiled HLSL (ported from
/// tools/viewer/gw2viewer.cpp) -- NOT the game's DXBC blobs.
namespace castlemist::render {

enum class RenderMode {
    Full,      // albedo * lambert + normal maps + effect blending (reconstructed HLSL)
    Plain,     // untextured flat lambert (no textures, white)
    Wireframe, // wireframe, untextured
    GameShader, // the GAME's own bgfx DXBC shaders per material (from the AMAT package)
};

bool initialize(HWND target_window);
void shutdown();
void on_resize(int width, int height);

/// Uploads a model (vertex/index buffers + per-material textures) and frames it.
void set_model(const ModelPreview& model);
void clear_model();

/// --- map / multi-model scene -----------------------------------------------
/// Map content layers (each independently toggleable).
enum SceneLayer { LAYER_PROP = 0, LAYER_TERRAIN = 1, LAYER_COLLISION = 2, LAYER_ZONE = 3, LAYER_COUNT = 4 };

/// One placed model instance: which model (index into the models vector) at a
/// world position/rotation(Euler radians)/uniform scale, tagged with its layer.
struct SceneInstance {
    int model = 0;
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};
    float scale = 1.0f;
    int layer = LAYER_PROP;
};

/// Uploads a set of unique models + a list of instances, and renders them all at
/// their world transforms (a coordinated map scene) with the orbit camera. The
/// same surface/camera controls as the single-model path apply.
void set_scene(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances);
/// Appends more models + instances to the current scene (for lazily-added layers
/// like zone models); instance.model is relative to the appended models list.
void add_scene_models(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances);
void clear_scene();
bool scene_active();

/// Builds the real GAME (bgfx DXBC) materials for the current map scene from a set
/// of ModelPreviews whose game shaders were extracted (want_game). `models` is
/// index-aligned with the models passed to set_scene (props/terrain/collision).
/// Call this after augmenting the map's models with game shaders to enable the map
/// "Shader" render mode. Idempotent. Zone models added via add_scene_models keep
/// the reconstruction fallback. scene_game_ready() reports whether any built.
void build_scene_game_materials(const std::vector<ModelPreview>& models);
bool scene_game_ready();

/// --- map instance picking + inset preview -----------------------------------
/// Picks the map prop under a surface pixel (screen-space nearest hit); returns the
/// scene model index or -1. set_scene_focus shows that model in a small inset panel
/// overlaid on the bottom-right of the map surface (the inset shares the scene
/// orbit). set_show_focus toggles the inset; scene_focus() is the current pick.
int pick_scene_instance(int sx, int sy);
void set_scene_focus(int model_index);   // -1 clears the pick
int scene_focus();
void set_show_focus(bool on);
bool show_focus();

/// Per-layer render visibility (default: prop + terrain on, collision + zone off).
void set_layer_visible(int layer, bool visible);
bool layer_visible(int layer);

void set_mode(RenderMode mode);

/// --- per-submesh LOD + texture-size selection (single-model path) -----------
/// GW2 meshes carry LOD0 (full detail) plus optional lower-detail LOD index sets
/// over the same verts. These let the UI pick which LOD each submesh draws and
/// whether it samples the full or half-res ("reduced") texture. `sub < 0` targets
/// every submesh (the global/overall control); a specific index overrides one.
int submesh_count();
const char* submesh_label(int i);   // e.g. "mesh 0 (mat1, 2080 tris, 3 LODs)"
int submesh_lod_count(int i);       // number of LODs available for submesh i (>=1)
int max_lod_count();                // max LOD count across all submeshes
void set_submesh_lod(int sub, int lod);
int submesh_lod(int i);
void set_submesh_tex_reduced(int sub, bool reduced);
bool submesh_tex_reduced(int i);

/// Toggles the reconstructed deferred light pre-pass used by GameShader mode (real
/// directional shading through the game's DXBC materials). Off = flat light-buffer
/// stand-in (the old look). Takes effect on the next render; no model reload needed.
void set_lightprepass(bool on);
bool lightprepass();

/// Overall light intensity for the Full/Plain reconstruction shading (multiplies the
/// multi-directional environment light). 1.0 = balanced default; UI "Light" slider.
void set_light_intensity(float v);
float light_intensity();

/// Camera-following "headlight" for the single-model view: when on, the key light
/// (reconstruction) and the game SH sun (Shader mode) point from a camera-relative
/// direction, so the viewer-facing side stays lit however the model is orbited.
/// The angle slider [0..1] swings it from directly behind the camera (0 = brightest
/// front) up and over the model (1 = grazing / dark front). Map scenes are
/// unaffected (they keep their real env sun).
void set_light_follow(bool on);
bool light_follow();
void set_light_angle(float v);   // 0..1
float light_angle();

/// Light a lone model/skin with GW2's OWN preview rig -- the fixed 8-light studio
/// setup its Equipment Preview and paper-doll windows use (the game does not light
/// those from the map either; it builds a private scene and forces this rig plus a
/// constant backlight). Reproduced from the client and verified against a capture
/// to 2e-5, so it is the game-accurate answer for a single model rather than a
/// tuned approximation. Feeds both the forward GameShader materials and the
/// deferred light pre-pass, so the two stay consistent.
///
/// ON by default. Only applies when no map env rig is active -- map scenes keep
/// their real `env` chunk lighting. Turn it off to fall back to the previous
/// hand-calibrated daylight stand-in.
void set_preview_rig(bool on);
bool preview_rig();

/// Environment lighting rig for the GameShader (real DXBC) path: drives the sun +
/// SH-ambient uniforms of the game's own lighting shader from a real map's `env`
/// chunk (MapEnvRig), so a model/skin is lit by e.g. Lion's Arch's actual light
/// instead of the hardcoded daylight stand-in. Setting a present rig applies it
/// immediately; clear() reverts to the default. A map scene applies its own rig
/// automatically. rig.present==false is treated as clear().
void set_environment_rig(const MapEnvRig& rig);
void clear_environment_rig();
bool environment_rig_active();

/// --- Blender-style interactive transform gizmo (single-model path) ----------
/// A draggable gizmo edits an object transform (translate / rotate / scale)
/// applied to the model on top of the orbit view. A ground grid + colored world
/// axes (X red, Y green, Z blue) give spatial reference. In Rotate mode the gizmo
/// shows tri-axis rings; Translate shows arrows; Scale shows boxed axes.
enum class GizmoMode { None = 0, Translate = 1, Rotate = 2, Scale = 3 };
void set_gizmo_mode(GizmoMode m);
GizmoMode gizmo_mode();
void set_show_grid(bool on);
bool show_grid();

/// The editable object transform (pivot = model center). Rotation is Euler degrees
/// (X,Y,Z), scale is per-axis. reset restores identity (pos 0, rot 0, scale 1).
void get_object_transform(float pos[3], float rot_deg[3], float scale[3]);
void set_object_transform(const float pos[3], const float rot_deg[3], const float scale[3]);
void reset_object_transform();

/// Mouse interaction, in model-surface client pixels. `gizmo_pick` hit-tests
/// without changing state (returns the hovered axis 0=X/1=Y/2=Z, 3=center/plane,
/// or -1). `gizmo_begin` grabs a handle if one is under the cursor (returns true
/// when grabbed, so the host suppresses orbit). `gizmo_update` applies the drag,
/// `gizmo_end` releases. `gizmo_active` is true while a handle is held.
int  gizmo_pick(int sx, int sy);
bool gizmo_begin(int sx, int sy);
void gizmo_update(int sx, int sy);
void gizmo_end();
bool gizmo_active();
void set_gizmo_hover(int axis);   // -1 = none; drives handle highlight

/// --- baked particle / light effects (MODL cloudData + lightData) -----------
/// Fire/spark/glow emitters and effect lights baked into the model. They run on
/// their own spawn timers so they animate on an idle model. When the current
/// model has effects, the host should keep pumping render() (WM_TIMER) so the
/// animation advances. Toggle hides/clears them.
void set_show_effects(bool on);
bool show_effects();
bool has_effects();   // true if the current model carries any clouds/lights
void effect_stats(int& clouds, int& emitters, int& lights, int& live); // debug counts

/// --- live cloth simulation (single-model reconstruction path) ---------------
/// Runs the reverse-engineered GW2 cloth solver on the current mesh: the top band
/// (GW2 Z-up) is pinned and the rest drapes under gravity + optional wind, with
/// constraint relaxation + real index-buffer normals. Draws into a dynamic vertex
/// buffer each frame, so the host should keep pumping render() while it's on.
/// (The MODL struct template doesn't expose the real ModelClothData chunk yet, so
/// this simulates the whole mesh with an auto-pinned edge rather than the game's
/// authored cloth pieces.) No effect in GameShader/map modes.
void set_cloth_enabled(bool on);
bool cloth_enabled();
bool has_cloth();                       // true when the current model can be simulated
void set_cloth_wind(float x, float y, float z);
/// Debug/verification: how far the mesh has deformed from its rest pose. verts =
/// simulated vertex count, locked = pinned (anchor) count, maxDisp/meanDisp =
/// max/mean per-vertex displacement from rest (0 until the sim has stepped).
void cloth_stats(int& verts, int& locked, float& maxDisp, float& meanDisp);

/// Toggles the bind-pose skeleton overlay (bones drawn as lines + joint crosses,
/// on top of the mesh). No effect on models without an inline rig.
void set_show_skeleton(bool show);
bool has_skeleton();

/// --- animation / posing ---------------------------------------------------
/// The skeleton overlay can be posed from a decoded Granny clip. Clip index -1 =
/// bind pose. Setting a clip or time re-poses the rig (bones with no matching
/// animation track keep their bind local transform). Playback advances the clip
/// time on each render() when playing (wrapping at the clip duration).
int  animation_count();               // number of decoded clips on the current model
const char* animation_name(int i);    // clip name (e.g. "zeropose"); "" if out of range
float animation_duration(int i);      // clip duration in seconds
bool animation_has_motion(int i);     // true if clip i has real keyframed motion (not a static zeropose)
int  first_motion_animation();        // first clip index with real motion, or -1 if none
void set_animation(int clip_index);   // -1 = bind pose
int  current_animation();             // active clip index (-1 = bind)
void set_anim_time(float seconds);    // scrub to an absolute time
float anim_time();                    // current playback time
void set_playing(bool playing);
bool is_playing();

/// Orbit / zoom, in the same feel as the reference viewer.
void orbit(float delta_yaw, float delta_pitch);
void zoom(float factor);
void reset_view();

void render();

/// Debug: save the current rendered frame to a 24-bit BMP (for headless checks).
void save_screenshot(const char* path);

} // namespace castlemist::render

#endif // GW2_MODEL_RENDERER_H
