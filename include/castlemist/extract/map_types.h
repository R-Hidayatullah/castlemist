/// @file
/// @brief Map scene payload: placed props, layers and the environment light rig.
/// @ingroup extract

#pragma once

#include <cstdint>
#include <vector>

#include "castlemist/extract/model_types.h"

/// @addtogroup extract
/// @{

/// @brief One placed prop in a map: which loaded model, and its world transform.
///
/// @note GW2 is Z-up, so `rot[2]` is the yaw and `pos[2]` is the height.
struct MapInstance {
    int model = 0;            ///< Index into MapScene::models.
    float pos[3] = {0, 0, 0}; ///< World position.
    float rot[3] = {0, 0, 0}; ///< Euler radians; `rot[2]` is yaw.
    float scale = 1.0f;       ///< Uniform scale.
    int layer = 0;            ///< 0 prop, 1 terrain, 2 collision, 3 zone (matches `castlemist::render::SceneLayer`).
};

/// @brief A map's real environment light rig, from the `env` chunk's day presets.
///
/// The chunk stores `PackMapEnvDataLightV*` records (byte3 colour, float
/// intensity, float3 direction). The highest-intensity light is taken as the
/// sun; the rest are collapsed into one intensity-weighted fill/sky ambient.
/// The renderer feeds this into the game lighting shaders so a model is lit by
/// the map's own light instead of a hardcoded stand-in.
///
/// When @ref present is false the renderer uses its default rig.
struct MapEnvRig {
    bool  present = false;                      ///< A usable rig was parsed.
    float sunDir[3] = {0.45f, 0.80f, 0.40f};    ///< Normalized, points *toward* the sun.
    float sunColor[3] = {1.00f, 0.91f, 0.75f};  ///< Linear RGB, 0..1.
    float sunIntensity = 1.0f;                  ///< Sun strength.
    float fillColor[3] = {0.49f, 0.68f, 0.97f}; ///< Intensity-weighted mean of the non-sun lights.
    float fillIntensity = 0.3f;                 ///< Summed fill intensity (drives ambient strength).
    int   lightCount = 0;                       ///< Lights found in the chunk.
};

/// @brief A whole map's coordinated model set: unique models plus every placement.
struct MapScene {
    std::vector<ModelPreview> models;     ///< Unique prop models, deduped by fileId.
    std::vector<uint32_t> modelFileIds;   ///< fileId of each `models[i]`, for lazy re-extraction.
    std::vector<MapInstance> instances;   ///< One per placed prop.
    uint32_t totalProps = 0;              ///< Props the map declared (may exceed instances).
    uint32_t loadedModels = 0;            ///< Models actually loaded.
    bool gameMaterialsLoaded = false;     ///< True once `models[]` carry game (DXBC) shaders.
    MapEnvRig env;                        ///< The map's real environment lighting.
};

/// @}
