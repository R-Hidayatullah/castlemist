#ifndef GW2_TEXTURE_PANEL_H
#define GW2_TEXTURE_PANEL_H

#include <cstdint>
#include <functional>
#include <windows.h>

#include "castlemist/extract/model_types.h"

/// @file
/// @brief The bottom-right texture panel: what every submesh of the current model
///        is actually textured with.
///
/// A model's material bindings are the one part of a preview you cannot check by
/// looking at the render -- a wrong diffuse and a right diffuse both just look like
/// "a texture". So this panel lists them as data: one group per submesh, and inside
/// it one row per texture that submesh's material references, each with a
/// thumbnail, the dat ids, the pixel size, the source format, the channel layout
/// (R / RG / RGB / RGBA) and the role the material gives it.
///
/// It owns GDI thumbnails built from @ref ModelPreview::textures, so it needs the
/// model only for the duration of the @ref set_model call.
namespace castlemist::texpanel {

/// Registers the panel's window class. Call once before @ref create.
void register_class(HINSTANCE instance);

/// Creates the panel as a child window. Scrolls itself; draws itself.
HWND create(HWND parent, HINSTANCE instance, int control_id);

/// Rebuilds the panel from @p model (null clears it). Copies everything it needs,
/// so the model may be released afterwards.
void set_model(HWND panel, const ModelPreview* model);

/// Number of texture rows currently listed -- 0 when the model has no resolvable
/// textures, which is the host's cue to hide the panel and grey its toggle.
int row_count(HWND panel);

/// Invoked when the user double-clicks a texture row, with that texture's dat ids.
/// Wire it to "reveal this entry in the MFT list".
using ActivateCallback = std::function<void(uint32_t fileId, uint32_t baseId)>;
void set_activate_callback(HWND panel, ActivateCallback cb);

} // namespace castlemist::texpanel

#endif // GW2_TEXTURE_PANEL_H
