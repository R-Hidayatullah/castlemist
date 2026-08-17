// granny_pose.hpp -- pose a bind-pose skeleton with a decoded Granny clip, and
// build the skinning matrices a GPU bone palette wants.
//
// The math here is not new: it is the pose composition that
// `src/render/skeleton.cpp` has been running (and `tools/gw2dat_cli` has been
// validating) all along, lifted into one place so the D3D11 renderer and the
// bgfx "Game 1:1" surface cannot drift apart. `gw2dat_cli`'s own copy is
// deliberately NOT routed through here: it is the independent cross-check that
// composing a clip's zeropose reproduces the InverseWorld bind positions, and a
// check that shares an implementation with the thing it checks proves nothing.
//
// Granny transform, per bone:  v' = pos + R(quat) * (ScaleShear * v)
// so the local linear part is L = R * SS, and world transforms compose down the
// hierarchy. GW2 stores parents before children, which is what lets a single
// forward pass over bones[] be correct.
//
// Verified end-to-end against the archive via `gw2dat_cli skel`: composing the
// embedded "zeropose" clip reproduces the bind pose to RMS 6.9e-5 on a 302-bone
// rig (fileId 1634661) and 2.0e-5 on a 294-bone one (fileId 904350).
#ifndef GRANNY_POSE_HPP
#define GRANNY_POSE_HPP

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "granny_anim.hpp"

namespace castlemist::granny {

/// One bone's animated transform in model space.
struct PoseXform {
    float lin[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  ///< Column-vector 3x3 (`m3vec` applies it).
    float pos[3] = {0, 0, 0};
};

/// A read-only view of one bind-pose bone.
///
/// Exists so `castlemist::model::Bone` (the archive type) and
/// `castlemist::render::ModelJoint` (the renderer's) can both feed this math
/// without either of them moving or gaining a dependency on the other. Field
/// names differ between the two (`scaleShear` vs `localScale`, `worldPos` vs
/// `pos`); the pointers below are what actually matters.
struct PoseBone {
    const char* name = "";
    int parent = -1;                  ///< Index into the same array; <0 or >= own index = root.
    const float* localPos = nullptr;   ///< 3
    const float* localQuat = nullptr;  ///< 4, xyzw
    const float* localScale = nullptr; ///< 9, row-major 3x3
    const float* invWorld = nullptr;   ///< 16, row-major row-vector, model -> bone (bind)
};

namespace detail {

/// Granny quaternion -> column-vector 3x3. Same expansion as the renderer's
/// `quatToM3`; kept here so this header stands alone.
inline void poseQuatToM3(const float q[4], float m[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float n = x*x + y*y + z*z + w*w;
    const float s = n > 1e-12f ? 2.0f / n : 0.0f;
    const float xs = x*s, ys = y*s, zs = z*s;
    const float wx = w*xs, wy = w*ys, wz = w*zs;
    const float xx = x*xs, xy = x*ys, xz = x*zs;
    const float yy = y*ys, yz = y*zs, zz = z*zs;
    m[0] = 1-(yy+zz); m[1] = xy-wz;     m[2] = xz+wy;
    m[3] = xy+wz;     m[4] = 1-(xx+zz); m[5] = yz-wx;
    m[6] = xz-wy;     m[7] = yz+wx;     m[8] = 1-(xx+yy);
}

inline void poseM3Mul(const float a[9], const float b[9], float o[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
}

inline void poseM3Vec(const float m[9], const float v[3], float o[3]) {
    for (int r = 0; r < 3; ++r) o[r] = m[r*3+0]*v[0] + m[r*3+1]*v[1] + m[r*3+2]*v[2];
}

} // namespace detail

/// Maps a clip's transform tracks to bone indices by name. Built once per
/// (skeleton, clip) pair rather than per frame.
inline std::unordered_map<std::string, int> trackIndexByName(const Anim& clip) {
    std::unordered_map<std::string, int> byName;
    byName.reserve(clip.tracks.size() * 2);
    for (size_t i = 0; i < clip.tracks.size(); ++i) byName[clip.tracks[i].name] = (int)i;
    return byName;
}

/// Composes every bone's model-space transform at time @p t.
///
/// @param clip  The clip to sample, or nullptr for the pure bind pose.
/// @param byName Track lookup from ::trackIndexByName, or nullptr to build one
///               internally. A bone with no matching track keeps its bind local
///               transform, which is what lets a clip animate part of a rig.
inline void composePose(const std::vector<PoseBone>& bones,
                        const Anim* clip,
                        float t,
                        std::vector<PoseXform>& out,
                        const std::unordered_map<std::string, int>* byName = nullptr) {
    using namespace detail;
    out.assign(bones.size(), PoseXform{});

    std::unordered_map<std::string, int> owned;
    if (clip && !byName) { owned = trackIndexByName(*clip); byName = &owned; }

    for (size_t i = 0; i < bones.size(); ++i) {
        const PoseBone& b = bones[i];
        float pos[3] = {0, 0, 0};
        float quat[4] = {0, 0, 0, 1};
        float ss[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (b.localPos)   std::memcpy(pos, b.localPos, sizeof pos);
        if (b.localQuat)  std::memcpy(quat, b.localQuat, sizeof quat);
        if (b.localScale) std::memcpy(ss, b.localScale, sizeof ss);

        if (clip && byName) {
            auto it = byName->find(b.name ? b.name : "");
            if (it != byName->end() && it->second >= 0 && it->second < (int)clip->tracks.size()) {
                const Track& tr = clip->tracks[it->second];
                // sample() leaves the pre-filled bind value alone for an
                // identity curve, so a track that animates only rotation keeps
                // the bind translation instead of collapsing to the origin.
                sample(tr.pos, t, pos, 3);
                sample(tr.ori, t, quat, 4);
                sample(tr.sca, t, ss, 9);
            }
        }

        float R[9]; poseQuatToM3(quat, R);
        float L[9]; poseM3Mul(R, ss, L);

        const int p = b.parent;
        if (p >= 0 && p < (int)i) {
            poseM3Mul(out[p].lin, L, out[i].lin);
            float rp[3]; poseM3Vec(out[p].lin, pos, rp);
            for (int k = 0; k < 3; ++k) out[i].pos[k] = out[p].pos[k] + rp[k];
        } else {
            std::memcpy(out[i].lin, L, sizeof L);
            for (int k = 0; k < 3; ++k) out[i].pos[k] = pos[k];
        }
    }
}

/// The skin matrix for one bone: `InverseWorld(bind) * AnimatedWorld`.
///
/// Row-major, row-vector (`mul(v, M)` in HLSL terms) -- the translation is the
/// last ROW. A bone sitting exactly at its bind transform yields identity here,
/// which is why an un-animated model is left precisely where it started.
/// Transpose it for a shader that multiplies `mul(M, v)`; see
/// ::skinMatrixColumnMajor.
inline void skinMatrix(const PoseBone& bone, const PoseXform& world, float out[16]) {
    // AnimatedWorld as a row-vector matrix: the linear block is `lin`
    // transposed, the translation is row 3.
    const float Wr[16] = {
        world.lin[0], world.lin[3], world.lin[6], 0,
        world.lin[1], world.lin[4], world.lin[7], 0,
        world.lin[2], world.lin[5], world.lin[8], 0,
        world.pos[0], world.pos[1], world.pos[2], 1,
    };
    static const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float* IB = bone.invWorld ? bone.invWorld : kIdentity;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r*4+c] = IB[r*4+0]*Wr[0*4+c] + IB[r*4+1]*Wr[1*4+c]
                       + IB[r*4+2]*Wr[2*4+c] + IB[r*4+3]*Wr[3*4+c];
}

/// ::skinMatrix transposed, for a shader whose convention is `mul(M, v)` --
/// which is GW2's own HLSL. The translation lands in the last COLUMN.
inline void skinMatrixColumnMajor(const PoseBone& bone, const PoseXform& world, float out[16]) {
    float rowMajor[16];
    skinMatrix(bone, world, rowMajor);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r*4+c] = rowMajor[c*4+r];
}

} // namespace castlemist::granny
#endif // GRANNY_POSE_HPP
