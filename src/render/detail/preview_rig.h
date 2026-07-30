/// @file
/// @brief GW2's Equipment-Preview / paper-doll studio light rig, lifted verbatim
///        from the client and reproduced exactly.
///
/// The in-game preview windows (Equipment Preview, hero panel, build-template and
/// change-appearance paper dolls) do NOT use map lighting. Each builds its own
/// GrScene + GrCamera + ShLight and lights the character from a fixed 8-entry
/// table compiled into the executable. See docs/research/gw2-preview-render.md.
///
/// Table: `.rdata` @ 0x141B10520 in Gw2-64 (HdPaperDoll.cpp; byte-identical copies
/// live in BtHeroPaperDoll @0x141AE3170 and EmdPreview @0x141AFC260), 28 bytes per
/// entry: {float azDeg; float elDeg; float r,g,b; float intensity; int enabled;}.
///
/// VERIFIED against dumps/captures/weaponpreview2.rdc: evaluating this table with
/// eval_preview_rig() reproduces the pixel-shader constants of the real preview
/// character draw (eid 15194) to 2e-5 across all 28 components -- see the table in
/// docs/research/gw2-preview-render.md. These are not tuned values; changing them
/// makes castlemist disagree with the game.

#pragma once

#include <cmath>

namespace castlemist::render {

/// One row of the client's paper-doll light table.
struct PreviewLight {
    float azDeg;      ///< Azimuth, degrees (field +0).
    float elDeg;      ///< Elevation, degrees (field +4).
    float r, g, b;    ///< Linear colour (fields +8/+12/+16).
    float intensity;  ///< Scalar strength (field +20).
    int   enabled;    ///< Field +24; 3 of the 8 rows ship disabled.
};

/// The rig, verbatim. Row 0 is the key; rows 3/6/7 ship disabled and are kept so
/// the table matches the binary one-for-one.
inline constexpr PreviewLight kPreviewRig[8] = {
    {  52.2581f,  40.0000f, 0.9059f, 0.9490f, 1.0000f, 1.0500f, 1 }, // key, cool white
    {-161.4194f, -40.0645f, 0.7255f, 0.8039f, 1.0000f, 1.0000f, 1 }, // blue back-fill
    { 180.0000f, -89.9999f, 1.0000f, 1.0000f, 1.0000f, 0.3226f, 1 }, // bounce
    { 180.0000f,  90.0000f, 0.7843f, 0.9961f, 0.9804f, 0.2500f, 0 },
    { 121.9355f,  20.3226f, 1.0000f, 0.9098f, 0.8667f, 0.6000f, 1 }, // warm rim
    { -47.6129f,  25.0000f, 1.0000f, 0.9098f, 0.8667f, 0.7000f, 1 }, // warm rim
    {   0.0000f, -45.0000f, 1.0000f, 1.0000f, 1.0000f, 0.1300f, 0 },
    { -90.0000f,   0.0000f, 0.5098f, 0.4824f, 1.0000f, 1.3032f, 0 },
};

/// Backlight globals the paper-doll forces every frame, overriding whatever the
/// map's env chunk put there (engine params `grblcol` / `grbldir`; materials see
/// them as BacklightColor / BacklightDirection).
///
/// The direction is the client's own thread-safe-static init: azimuth 30 deg,
/// elevation -40 deg, and -- unlike the rig lights -- it is NOT negated.
inline constexpr float kPreviewBacklightColor[4] = {1.5f, 1.5f, 1.5f, 1.5f};
inline constexpr float kPreviewBacklightAzDeg = 30.0f;
inline constexpr float kPreviewBacklightElDeg = -40.0f;

/// Everything ShLight hands the material shaders for one preview frame.
struct PreviewRigUniforms {
    float shRed[4], shGreen[4], shBlue[4];      ///< SH ambient: xyz = band 1, w = DC.
    float shSun[4], shSunColor[4], shSunData[4];///< The dedicated directional slot.
    float backlightColor[4], backlightDir[4];
};

/// The client's light-direction convention (HdPaperDoll.cpp ~line 465):
///     dir = -(cos(az)cos(el), sin(az)cos(el), sin(el))
/// GW2 world space, Z-up -- the same space castlemist already feeds the map rig's
/// sunDir in, so these go into g_game_vals unconverted.
inline void preview_light_dir(float azDeg, float elDeg, float out[3]) {
    const float kDeg2Rad = 0.017453292f;
    float A = elDeg * kDeg2Rad, B = azDeg * kDeg2Rad;
    out[0] = -(std::cos(B) * std::cos(A));
    out[1] = -(std::sin(B) * std::cos(A));
    out[2] = -std::sin(A);
}

/// Reproduce `ShLight::AddLight` (sub_140AB70F0) over the table.
///
/// The one non-obvious rule, and the thing an eyeballed reimplementation gets
/// wrong: the FIRST enabled light is not accumulated into the spherical harmonics
/// at all. The function opens with `if (!*(a1+120)) return sub_140AB6ED0(...)`,
/// which parks that light in the dedicated directional slot at +88. Only lights
/// 2..n reach the SH bands. Folding all five into SH instead gives a visibly
/// flatter, ~40% brighter ambient and no key direction.
///
/// @param scale Overall brightness multiplier; 1.0 = exactly what the game sends.
inline PreviewRigUniforms eval_preview_rig(float scale = 1.0f) {
    const float kY1 = 0.488603f;    // SH band-1 coefficient
    const float kY0 = 0.28209499f;  // SH band-0 coefficient
    PreviewRigUniforms u{};

    bool haveSun = false;
    float sh[3][4] = {};
    for (const PreviewLight& L : kPreviewRig) {
        if (!L.enabled) continue;
        float d[3];
        preview_light_dir(L.azDeg, L.elDeg, d);
        const float c[3] = {L.r, L.g, L.b};
        if (!haveSun) {  // first enabled light -> the sun slot, never the SH
            haveSun = true;
            u.shSun[0] = d[0]; u.shSun[1] = d[1]; u.shSun[2] = d[2]; u.shSun[3] = 0.0f;
            for (int k = 0; k < 3; ++k) u.shSunColor[k] = c[k] * L.intensity * scale;
            u.shSunColor[3] = L.intensity;
            // shSunData repeats the direction with .w = -1 (measured in the capture).
            u.shSunData[0] = d[0]; u.shSunData[1] = d[1]; u.shSunData[2] = d[2];
            u.shSunData[3] = -1.0f;
            continue;
        }
        for (int ch = 0; ch < 3; ++ch) {
            float w = L.intensity * c[ch] * scale;
            for (int k = 0; k < 3; ++k) sh[ch][k] += d[k] * kY1 * w;
            sh[ch][3] += kY0 * w;
        }
    }
    for (int k = 0; k < 4; ++k) {
        u.shRed[k] = sh[0][k];
        u.shGreen[k] = sh[1][k];
        u.shBlue[k] = sh[2][k];
    }

    for (int k = 0; k < 4; ++k) u.backlightColor[k] = kPreviewBacklightColor[k];
    const float kDeg2Rad = 0.017453292f;
    float a = kPreviewBacklightAzDeg * kDeg2Rad, e = kPreviewBacklightElDeg * kDeg2Rad;
    u.backlightDir[0] = std::cos(a) * std::cos(e);
    u.backlightDir[1] = std::sin(a) * std::cos(e);
    u.backlightDir[2] = std::sin(e);
    u.backlightDir[3] = 0.0f;
    return u;
}

} // namespace castlemist::render
