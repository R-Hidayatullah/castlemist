/// @file
/// @brief Locks castlemist's preview lighting to GW2's own, measured off the GPU.
///
/// The expected numbers are NOT hand-picked: they are the pixel-shader constants
/// RenderDoc read back from the real Equipment Preview character draw (capture
/// dumps/captures/weaponpreview2.rdc, eid 15194), which castlemist reproduces by
/// evaluating the client's own 8-light table. See
/// docs/research/gw2-preview-render.md.
///
/// This matters because the failure mode is silent: get the rig wrong and skins
/// still render, just lit differently from the game, and nothing tells you. The
/// two rules easiest to break are (a) the first enabled light goes to the sun slot
/// instead of the SH bands, and (b) the light-direction formula's leading minus.

#include "test_framework.h"

#include "detail/preview_rig.h"

using namespace castlemist::render;

namespace {
// 2e-5 is the observed agreement; the capture readback itself was rounded to 5
// decimals, so this is as tight as the reference allows.
constexpr float kEps = 1e-4f;

void check4(const float* got, float a, float b, float c, float d) {
    CHECK_NEAR(got[0], a, kEps);
    CHECK_NEAR(got[1], b, kEps);
    CHECK_NEAR(got[2], c, kEps);
    CHECK_NEAR(got[3], d, kEps);
}
} // namespace

CM_TEST(preview_rig, matches_captured_gpu_constants) {
    PreviewRigUniforms u = eval_preview_rig();
    // cb0[2..4] of the real preview draw.
    check4(u.shRed, 0.19360f, 0.08209f, 0.13941f, 0.66238f);
    check4(u.shGreen, 0.22713f, 0.09183f, 0.18630f, 0.65143f);
    check4(u.shBlue, 0.29937f, 0.11538f, 0.25859f, 0.69092f);
    // cb0[5..7]: the dedicated directional slot.
    check4(u.shSun, -0.46890f, -0.60577f, -0.64279f, 0.0f);
    check4(u.shSunColor, 0.95118f, 0.99647f, 1.05000f, 1.05000f);
    check4(u.shSunData, -0.46890f, -0.60577f, -0.64279f, -1.0f);
    // cb0[8..9]: the backlight globals the paper-doll forces (grblcol / grbldir).
    check4(u.backlightColor, 1.5f, 1.5f, 1.5f, 1.5f);
    check4(u.backlightDir, 0.66341f, 0.38302f, -0.64279f, 0.0f);
}

CM_TEST(preview_rig, table_matches_the_client) {
    // 8 rows, 5 enabled -- the client ships 3 disabled and we keep them so the
    // table stays a one-for-one image of .rdata @0x141B10520.
    int on = 0;
    for (const PreviewLight& L : kPreviewRig) on += L.enabled ? 1 : 0;
    CHECK_EQ(on, 5);
    CHECK_NEAR(kPreviewRig[0].azDeg, 52.2581f, 1e-4f);
    CHECK_NEAR(kPreviewRig[0].intensity, 1.05f, 1e-6f);
    CHECK_EQ(kPreviewRig[3].enabled, 0);
    CHECK_EQ(kPreviewRig[6].enabled, 0);
    CHECK_EQ(kPreviewRig[7].enabled, 0);
}

CM_TEST(preview_rig, key_light_is_excluded_from_the_sh) {
    // The regression this guards: folding the key into the SH as well would raise
    // every DC term by kY0 * I * colour. Assert the DC is exactly the sum over the
    // NON-key lights, so that mistake fails loudly instead of just looking flat.
    const float kY0 = 0.28209499f;
    float dc[3] = {0, 0, 0};
    bool first = true;
    for (const PreviewLight& L : kPreviewRig) {
        if (!L.enabled) continue;
        if (first) { first = false; continue; }   // key -> sun slot, not the SH
        const float c[3] = {L.r, L.g, L.b};
        for (int k = 0; k < 3; ++k) dc[k] += kY0 * L.intensity * c[k];
    }
    PreviewRigUniforms u = eval_preview_rig();
    CHECK_NEAR(u.shRed[3], dc[0], kEps);
    CHECK_NEAR(u.shGreen[3], dc[1], kEps);
    CHECK_NEAR(u.shBlue[3], dc[2], kEps);
    // And the key really is in the sun slot, at full strength.
    CHECK_NEAR(u.shSunColor[3], kPreviewRig[0].intensity, kEps);
}

CM_TEST(preview_rig, directions_are_unit_length) {
    // dir = -(cos az cos el, sin az cos el, sin el) is unit by construction; a typo
    // that drops a cos would silently rescale the light instead of rotating it.
    for (const PreviewLight& L : kPreviewRig) {
        float d[3];
        preview_light_dir(L.azDeg, L.elDeg, d);
        CHECK_NEAR(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1e-5f);
    }
}

CM_TEST(preview_rig, scale_is_linear_and_leaves_geometry_alone) {
    PreviewRigUniforms a = eval_preview_rig(1.0f);
    PreviewRigUniforms b = eval_preview_rig(2.0f);
    for (int k = 0; k < 4; ++k) CHECK_NEAR(b.shRed[k], a.shRed[k] * 2.0f, kEps);
    for (int k = 0; k < 3; ++k) CHECK_NEAR(b.shSunColor[k], a.shSunColor[k] * 2.0f, kEps);
    // Directions must NOT scale -- they are normals, not radiance.
    for (int k = 0; k < 3; ++k) CHECK_NEAR(b.shSun[k], a.shSun[k], kEps);
    for (int k = 0; k < 4; ++k) CHECK_NEAR(b.backlightDir[k], a.backlightDir[k], kEps);
}
