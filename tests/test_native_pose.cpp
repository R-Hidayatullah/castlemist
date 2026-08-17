/// @file
/// @brief Granny pose composition and the GPU bone-palette conventions.
///
/// These pin the properties the renderers actually depend on when they hand a
/// bone palette to a shader. They are pure functions over hand-built rigs, so
/// unlike the end-to-end model tests they need no Gw2.dat and never skip.
///
/// The complementary check lives in `gw2dat_cli skel`, which measures the same
/// invariants against real archive data (composing a clip's zeropose reproduces
/// the bind pose to RMS 6.9e-5 on a 302-bone rig, and its skin matrices come out
/// identity to RMS 2.5e-5). That one proves the decoder agrees with the
/// archive; these prove the conventions are what the shaders are told they are.

#include "test_framework.h"

#include "castlemist/native/granny_pose.hpp"

#include <cmath>
#include <string>
#include <vector>

using castlemist::granny::Anim;
using castlemist::granny::composePose;
using castlemist::granny::Curve;
using castlemist::granny::PoseBone;
using castlemist::granny::PoseXform;
using castlemist::granny::skinMatrix;
using castlemist::granny::skinMatrixColumnMajor;
using castlemist::granny::Track;

namespace {

// One bind bone's storage. PoseBone is only a view, so the arrays have to
// outlive it.
struct TestBone {
    std::string name;
    int parent = -1;
    float localPos[3] = {0, 0, 0};
    float localQuat[4] = {0, 0, 0, 1};
    float localScale[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float invWorld[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

std::vector<PoseBone> viewOf(const std::vector<TestBone>& bones) {
    std::vector<PoseBone> pb(bones.size());
    for (size_t i = 0; i < bones.size(); ++i)
        pb[i] = {bones[i].name.c_str(), bones[i].parent, bones[i].localPos,
                 bones[i].localQuat, bones[i].localScale, bones[i].invWorld};
    return pb;
}

/// Quaternion (xyzw) for a rotation of `deg` about a unit axis.
void axisAngle(float ax, float ay, float az, float deg, float q[4]) {
    const float r = deg * 3.14159265358979323846f / 180.0f;
    const float s = std::sin(r * 0.5f);
    q[0] = ax * s; q[1] = ay * s; q[2] = az * s; q[3] = std::cos(r * 0.5f);
}

/// The bind InverseWorld the archive stores, derived from a *rigid* bind world.
///
/// For a column-vector transform `y = W x + p`, the row-vector row-major matrix
/// is rows 0..2 = Wᵀ and row 3 = the translation. The inverse of a rigid
/// (W orthonormal) transform is `x = Wᵀ y - Wᵀ p`, so the inverse's row-vector
/// form has rows 0..2 = (Wᵀ)ᵀ = W and row 3 = -Wᵀ p.
void rigidInvWorld(const PoseXform& world, float out[16]) {
    const float* W = world.lin;
    for (int r = 0; r < 3; ++r) {
        out[r*4+0] = W[r*3+0]; out[r*4+1] = W[r*3+1]; out[r*4+2] = W[r*3+2];
        out[r*4+3] = 0.0f;
    }
    for (int c = 0; c < 3; ++c) {
        float acc = 0.0f;
        for (int k = 0; k < 3; ++k) acc += W[k*3+c] * world.pos[k];
        out[12+c] = -acc;
    }
    out[15] = 1.0f;
}

/// A three-bone chain with rotation at every joint, so a convention slip in the
/// linear part cannot hide behind pure translation.
std::vector<TestBone> makeChain() {
    std::vector<TestBone> b(3);
    b[0].name = "root"; b[0].parent = -1;
    b[0].localPos[0] = 1.0f; b[0].localPos[1] = 2.0f; b[0].localPos[2] = 3.0f;
    axisAngle(0, 0, 1, 30.0f, b[0].localQuat);

    b[1].name = "mid"; b[1].parent = 0;
    b[1].localPos[1] = 5.0f;
    axisAngle(1, 0, 0, 45.0f, b[1].localQuat);

    b[2].name = "tip"; b[2].parent = 1;
    b[2].localPos[0] = 2.0f;
    axisAngle(0, 1, 0, -60.0f, b[2].localQuat);
    return b;
}

/// Fills every bone's invWorld from its own bind pose, which is what the archive
/// ships: InverseWorld is by definition the inverse of the bind world.
void bakeBindInverses(std::vector<TestBone>& bones) {
    std::vector<PoseXform> world;
    composePose(viewOf(bones), nullptr, 0.0f, world);
    for (size_t i = 0; i < bones.size(); ++i) rigidInvWorld(world[i], bones[i].invWorld);
}

Curve constantCurve(int dim, std::vector<float> values) {
    Curve c;
    c.fmt = castlemist::granny::F_K32fC32f; // anything non-identity
    c.dim = dim;
    c.knots.push_back(0.0f);               // one knot == constant
    c.controls = std::move(values);
    return c;
}

} // namespace

// The invariant every un-animated model rests on: at the bind pose the skin
// matrix is identity, so a vertex is left exactly where the archive put it.
// Get the transpose or the multiply order wrong and this is what breaks -- the
// model collapses or explodes the instant a palette is bound, even with no clip
// playing.
CM_TEST(pose, bind_pose_skin_matrices_are_identity) {
    std::vector<TestBone> bones = makeChain();
    bakeBindInverses(bones);

    std::vector<PoseBone> pb = viewOf(bones);
    std::vector<PoseXform> world;
    composePose(pb, nullptr, 0.0f, world);

    CHECK_EQ(world.size(), bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        float m[16];
        skinMatrix(pb[i], world[i], m);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                const float want = (r == c) ? 1.0f : 0.0f;
                CHECK(std::fabs(m[r*4+c] - want) < 1e-4f);
            }
    }
}

// GW2's HLSL multiplies mul(M, v), the opposite of the row-vector form the
// palette is composed in -- the same reason World/ViewProjection are uploaded
// transposed. This pins that the column-major helper really is the transpose,
// and in particular that the translation moves from the last ROW to the last
// COLUMN, which is the half that silently produces garbage geometry.
CM_TEST(pose, column_major_skin_matrix_is_the_transpose) {
    std::vector<TestBone> bones = makeChain();
    bakeBindInverses(bones);
    std::vector<PoseBone> pb = viewOf(bones);

    // Pose away from bind so the matrices are not symmetric by accident.
    Anim clip;
    clip.valid = true;
    clip.duration = 1.0f;
    Track tr;
    tr.name = "mid";
    tr.pos = constantCurve(3, {7.0f, -3.0f, 2.0f});
    clip.tracks.push_back(tr);

    std::vector<PoseXform> world;
    composePose(pb, &clip, 0.0f, world);

    for (size_t i = 0; i < bones.size(); ++i) {
        float rowMajor[16], colMajor[16];
        skinMatrix(pb[i], world[i], rowMajor);
        skinMatrixColumnMajor(pb[i], world[i], colMajor);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK(std::fabs(colMajor[r*4+c] - rowMajor[c*4+r]) < 1e-6f);
    }

    // The moved bone must actually have a non-identity skin matrix, or the check
    // above would be comparing two identity matrices and proving nothing.
    float moved[16];
    skinMatrix(pb[1], world[1], moved);
    bool differs = false;
    for (int r = 0; r < 4 && !differs; ++r)
        for (int c = 0; c < 4 && !differs; ++c)
            if (std::fabs(moved[r*4+c] - ((r == c) ? 1.0f : 0.0f)) > 1e-3f) differs = true;
    CHECK(differs);
}

// A clip drives part of a rig: bones it names move, bones it does not keep their
// bind local transform. If an absent track were treated as "identity transform"
// instead, every undriven bone would snap to its parent's origin.
CM_TEST(pose, bones_without_a_track_keep_their_bind_transform) {
    std::vector<TestBone> bones = makeChain();
    bakeBindInverses(bones);
    std::vector<PoseBone> pb = viewOf(bones);

    std::vector<PoseXform> bind;
    composePose(pb, nullptr, 0.0f, bind);

    Anim clip;
    clip.valid = true;
    clip.duration = 1.0f;
    Track tr;
    tr.name = "nosuchbone";           // matches nothing in the rig
    tr.pos = constantCurve(3, {99.0f, 99.0f, 99.0f});
    clip.tracks.push_back(tr);

    std::vector<PoseXform> posed;
    composePose(pb, &clip, 0.0f, posed);

    for (size_t i = 0; i < bones.size(); ++i)
        for (int k = 0; k < 3; ++k)
            CHECK(std::fabs(posed[i].pos[k] - bind[i].pos[k]) < 1e-5f);
}

// A track that animates only rotation must leave the bind TRANSLATION alone.
// granny_anim.hpp's sampler achieves this by returning without writing for an
// identity curve, which only works because composePose pre-fills the bind value
// first -- a detail worth pinning, since "clear then sample" would collapse the
// bone to its parent's origin and is the natural way to write it wrong.
CM_TEST(pose, an_orientation_only_track_preserves_the_bind_translation) {
    std::vector<TestBone> bones = makeChain();
    bakeBindInverses(bones);
    std::vector<PoseBone> pb = viewOf(bones);

    std::vector<PoseXform> bind;
    composePose(pb, nullptr, 0.0f, bind);

    Anim clip;
    clip.valid = true;
    clip.duration = 1.0f;
    Track tr;
    tr.name = "tip";
    float q[4];
    axisAngle(0, 0, 1, 90.0f, q);
    tr.ori = constantCurve(4, {q[0], q[1], q[2], q[3]});
    // pos and sca stay F_Identity -> sample() must not touch the pre-filled bind.
    clip.tracks.push_back(tr);

    std::vector<PoseXform> posed;
    composePose(pb, &clip, 0.0f, posed);

    // "tip" is a leaf: rotating it cannot move its own origin.
    for (int k = 0; k < 3; ++k)
        CHECK(std::fabs(posed[2].pos[k] - bind[2].pos[k]) < 1e-5f);
    // ...but it must have changed orientation.
    bool linDiffers = false;
    for (int k = 0; k < 9; ++k)
        if (std::fabs(posed[2].lin[k] - bind[2].lin[k]) > 1e-3f) linDiffers = true;
    CHECK(linDiffers);
}

// Parents are stored before children, which is what lets one forward pass be
// correct. Moving a parent must carry its children with it.
CM_TEST(pose, a_parent_transform_propagates_to_its_children) {
    std::vector<TestBone> bones = makeChain();
    bakeBindInverses(bones);
    std::vector<PoseBone> pb = viewOf(bones);

    std::vector<PoseXform> bind;
    composePose(pb, nullptr, 0.0f, bind);

    Anim clip;
    clip.valid = true;
    clip.duration = 1.0f;
    Track tr;
    tr.name = "root";
    tr.pos = constantCurve(3, {bones[0].localPos[0] + 10.0f,
                               bones[0].localPos[1],
                               bones[0].localPos[2]});
    clip.tracks.push_back(tr);

    std::vector<PoseXform> posed;
    composePose(pb, &clip, 0.0f, posed);

    // Every bone shifts by the same +10 on x, and nothing else changes.
    for (size_t i = 0; i < bones.size(); ++i) {
        CHECK(std::fabs((posed[i].pos[0] - bind[i].pos[0]) - 10.0f) < 1e-4f);
        CHECK(std::fabs(posed[i].pos[1] - bind[i].pos[1]) < 1e-4f);
        CHECK(std::fabs(posed[i].pos[2] - bind[i].pos[2]) < 1e-4f);
    }
}

// An empty rig must not be a special case for the caller: composePose on no
// bones yields no transforms rather than reading past the end.
CM_TEST(pose, an_empty_rig_composes_to_an_empty_pose) {
    std::vector<PoseBone> none;
    std::vector<PoseXform> out(4); // deliberately non-empty going in
    composePose(none, nullptr, 0.0f, out);
    CHECK(out.empty());
}
