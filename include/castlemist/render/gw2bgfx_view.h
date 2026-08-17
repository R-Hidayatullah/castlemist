#ifndef CASTLEMIST_GW2BGFX_VIEW_H
#define CASTLEMIST_GW2BGFX_VIEW_H

#include <cstdint>
#include <string>
#include <windows.h>

#include "castlemist/native/gw2dat.h"

/// @file
/// @brief The "Game 1:1" surface -- a second, independent 3D view that draws a
///        model with Guild Wars 2's own shaders through the bgfx the client
///        itself vendors.
///
/// This does not replace castlemist::render. That renderer stays exactly as it
/// was: its own Direct3D 11 device, the map scene, instancing, picking, the
/// gizmo, particles, cloth, the skeleton, LOD selection and its GameShader mode.
/// This surface is a reference view to hold beside it -- same model, drawn the
/// way the client would draw it, so the two can be compared directly.
///
/// It owns a separate bgfx device on its own child window. bgfx is a process
/// singleton, so it is initialised once, lazily, the first time the view is
/// opened, and torn down at shutdown.
///
/// The model is loaded straight out of the archive by MFT index rather than
/// from an already-built ModelPreview, deliberately: the whole point of the view
/// is that nothing between the .dat and the draw call has been reinterpreted.
namespace castlemist::gw2bgfxview {

/// @brief Whether this build has the bgfx surface compiled in.
///
/// False when the tree was configured without external/bgfx. Callers should
/// hide the mode rather than fail: every other entry point below is safe to
/// call regardless and does nothing when this is false.
bool available();

/// @brief Creates the bgfx device against @p target_window. Idempotent.
/// @return false if bgfx could not initialise; last_error() says why.
bool initialize(HWND target_window);

void shutdown();

/// @brief Call from the surface window's WM_SIZE handler.
void on_resize(int width, int height);

/// @brief Loads and frames one model.
/// @param mft_index Index into @p dat 's MFT list -- i.e. `baseId - 1`.
/// @return false and fills @p error if the entry is not a usable model.
bool set_model(Gw2Dat& dat, uint32_t mft_index, std::string& error);

void clear_model();
bool has_model();

/// @brief Orbit deltas in radians. Pitch is unclamped: the camera basis is
///        derived from the angles, so it rolls through the poles.
void orbit(float d_yaw, float d_pitch);
/// @brief Multiplies the orbit radius (>1 pulls back).
void zoom(float factor);
void reset_view();

/// @brief Model-space orientation trim in degrees, applied before the
///        Z-up -> Y-up conversion.
///
/// The archive does not agree with itself about which way up a geoset sits:
/// props arrive upright, while a character's armour pieces are stored in a
/// bind-pose space whose root rotation lives in a skeleton this view does not
/// read, and arrive inverted. This is the manual correction -- (180,0,0) stands
/// such a model back up.
void set_rotation_trim(float x_deg, float y_deg, float z_deg);
void rotation_trim(float out_deg[3]);

/// @brief Draw every surface two-sided rather than honouring the effect's cull
///        bits. On by default.
///
/// The client suppresses culling per surface when the runtime material word has
/// bit 0x4000 set (`BgfxShader_SelectEffect` guards the cull OR with it). That
/// word is built at load time from data this view does not reconstruct, so it
/// reads zero here -- meaning no surface is ever two-sided and every effect's
/// cull applies. `shaderPassFlags` bit 0 is set on essentially every GW2 effect,
/// so that is CULL_CCW on everything, and GW2's single-sided sheet geometry
/// (capes, tabards, skirts, hair cards) disappears when viewed from its back
/// side -- most obviously when orbiting underneath a character.
///
/// Turning this off restores true 1:1 culling, with that caveat.
void set_force_two_sided(bool on);
bool force_two_sided();

/// @brief Draws and presents one frame. Cheap no-op when no model is loaded.
void render();

/// @brief Human-readable summary of the last load ("9 draws from 9 geosets"),
///        or the reason it failed. Shown in the UI's status text.
const std::string& last_status();

} // namespace castlemist::gw2bgfxview

#endif // CASTLEMIST_GW2BGFX_VIEW_H
