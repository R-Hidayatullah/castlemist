/// @file
/// @brief Typed access to the `GW2_*` environment variables used as debug hooks.
/// @ingroup core

#pragma once

#include <cstdint>
#include <string>

namespace castlemist::core {

/// @addtogroup core
/// @{

/// @brief Debug/automation hooks, all read from the environment.
///
/// castlemist has no scripting layer, so headless verification runs are driven
/// by environment variables: set one, launch the exe, read the dump it writes.
/// They are grouped here so that `docs/testing.md` and the code cannot drift.
///
/// | Variable | Effect |
/// |----------|--------|
/// | `GW2_AUTOLOAD=<mftIdx>`  | select that entry at startup and screenshot it |
/// | `GW2_SCENE=<baseId>`     | load a map/scene instead of a single model |
/// | `GW2_SCENEGAME=1`        | force game-shader materials on the loaded scene |
/// | `GW2_AUDIOTEST=<mftIdx>` | probe/play an audio entry, log `audiotest.txt` |
/// | `GW2_VIDEOTEST=<mftIdx>` | open a Bink entry and dump frame info |
/// | `GW2_PARSETEST=<ids>`    | run the summary parsers, log `parsetest.txt` |
/// | `GW2_PIMGTEST=<mftIdx>`  | composite a PIMG atlas to `pimg_preview.bmp` |
/// | `GW2_GIZMOTEST=1`        | exercise gizmo hit-testing and dragging |
/// | `GW2_LODTEST=1`          | dump each LOD/texture-size combination |
/// | `GW2_FXTEST=1`           | dump the particle/effect pass |
/// | `GW2_CLOTHTEST=1`        | dump rest/draped/wind cloth states |
/// | `GW2_LIGHT`, `GW2_LIGHTINT`, `GW2_GLOBALLIT` | lighting scale knobs |
/// | `GW2_POSTEXP`, `GW2_POSTAMB`, `GW2_POSTGAMMA`, `GW2_AUTOEXP` | tone mapping |
/// | `GW2_EFFECTS`, `GW2_FXINTENSITY`, `GW2_WIND`, `GW2_WINDDIR` | effect rig |
/// | `GW2_LIGHTPREPASS`       | toggle the deferred light pre-pass |
/// | `GW2_MATDBG`, `GW2_CONSTDBG`, `GW2_EXPDBG`, `GW2_TEXDBG` | verbose logs |
namespace env {

/// @brief True when @p name is set to anything at all (presence-only flag).
bool is_set(const char* name);

/// @brief Value of @p name as a bool: absent or "0" is false, anything else true.
bool flag(const char* name, bool fallback = false);

/// @brief Value of @p name parsed as a signed integer.
int32_t integer(const char* name, int32_t fallback = 0);

/// @brief Value of @p name parsed as an unsigned integer.
uint32_t unsigned_integer(const char* name, uint32_t fallback = 0);

/// @brief Value of @p name parsed as a float, ignoring values <= @p minimum.
/// @param minimum Guards against `GW2_LIGHT=0` silently blacking out the scene.
float real(const char* name, float fallback, float minimum = 0.0f);

/// @brief Raw value of @p name, or an empty string when unset.
std::string text(const char* name);

} // namespace env

/// @}

} // namespace castlemist::core
