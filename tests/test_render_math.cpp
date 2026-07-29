/// @file
/// @brief Tests for the renderer's row-major maths and Granny pose maths.
///
/// These matrices are handed straight to both castlemist's own shaders and the
/// game's DXBC shaders, so a convention slip here (row-major vs column-major,
/// row-vector vs column-vector) does not crash -- it silently renders a model
/// inside-out or off screen. Hence the explicit convention tests.

#include "test_framework.h"

#include "detail/math.h"

using namespace castlemist::render;

namespace {
constexpr float kEps = 1e-5f;

void check_identity(const Mat4& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) CHECK_NEAR(m.m[r * 4 + c], r == c ? 1.0f : 0.0f, kEps);
}
} // namespace

CM_TEST(vec3, basic_operations) {
    Vec3 a{1, 2, 3}, b{4, 5, 6};
    Vec3 d = sub(a, b);
    CHECK_NEAR(d.x, -3.0f, kEps);
    CHECK_NEAR(dot(a, b), 32.0f, kEps);

    Vec3 c = cross(Vec3{1, 0, 0}, Vec3{0, 1, 0});
    CHECK_NEAR(c.x, 0.0f, kEps);
    CHECK_NEAR(c.y, 0.0f, kEps);
    CHECK_NEAR(c.z, 1.0f, kEps);
}

CM_TEST(vec3, norm_of_a_zero_vector_does_not_divide_by_zero) {
    Vec3 z = norm(Vec3{0, 0, 0});
    CHECK(z.x == z.x); // not NaN
    CHECK(z.y == z.y);
    CHECK(z.z == z.z);
}

CM_TEST(mat4, identity_is_the_multiplicative_unit) {
    check_identity(identity());
    Mat4 t = translate(Vec3{3, -4, 5});
    check_identity(identity());
    Mat4 ti = mul(t, identity());
    for (int i = 0; i < 16; ++i) CHECK_NEAR(ti.m[i], t.m[i], kEps);
}

// Row-vector convention: the translation lives in the LAST ROW (m12..m14).
// If it ever moves to the last column, every model draws at the origin.
CM_TEST(mat4, translation_lives_in_the_last_row) {
    Mat4 t = translate(Vec3{7, 8, 9});
    CHECK_NEAR(t.m[12], 7.0f, kEps);
    CHECK_NEAR(t.m[13], 8.0f, kEps);
    CHECK_NEAR(t.m[14], 9.0f, kEps);
    CHECK_NEAR(t.m[3], 0.0f, kEps);
    CHECK_NEAR(t.m[7], 0.0f, kEps);
    CHECK_NEAR(t.m[11], 0.0f, kEps);
}

CM_TEST(mat4, transform_point_applies_the_translation) {
    Mat4 t = translate(Vec3{1, 2, 3});
    Vec3 p = transformPoint(Vec3{10, 20, 30}, t);
    CHECK_NEAR(p.x, 11.0f, kEps);
    CHECK_NEAR(p.y, 22.0f, kEps);
    CHECK_NEAR(p.z, 33.0f, kEps);
}

CM_TEST(mat4, rotations_are_right_handed) {
    // 90 degrees about Y takes +X to -Z under the row-vector convention.
    Vec3 p = transformPoint(Vec3{1, 0, 0}, rotY(3.14159265f / 2));
    CHECK_NEAR(p.x, 0.0f, 1e-4);
    CHECK_NEAR(p.z, -1.0f, 1e-4);

    // 90 degrees about X takes +Y to +Z.
    Vec3 q = transformPoint(Vec3{0, 1, 0}, rotX(3.14159265f / 2));
    CHECK_NEAR(q.y, 0.0f, 1e-4);
    CHECK_NEAR(q.z, 1.0f, 1e-4);
}

// Composition order: mul(A, B) must mean "A then B" for a row vector.
CM_TEST(mat4, multiplication_composes_left_to_right) {
    Mat4 first = translate(Vec3{1, 0, 0});
    Mat4 then = rotY(3.14159265f / 2);
    Vec3 p = transformPoint(Vec3{0, 0, 0}, mul(first, then));
    // translate to (1,0,0), then rotate 90 deg about Y -> (0,0,-1)
    CHECK_NEAR(p.x, 0.0f, 1e-4);
    CHECK_NEAR(p.z, -1.0f, 1e-4);
}

CM_TEST(mat4, look_at_puts_the_eye_at_the_origin) {
    Mat4 v = lookAt(Vec3{0, 0, -5}, Vec3{0, 0, 0}, Vec3{0, 1, 0});
    Vec3 eye = transformPoint(Vec3{0, 0, -5}, v);
    CHECK_NEAR(eye.x, 0.0f, 1e-4);
    CHECK_NEAR(eye.y, 0.0f, 1e-4);
    CHECK_NEAR(eye.z, 0.0f, 1e-4);
}

CM_TEST(mat4, perspective_maps_the_near_plane_to_zero_depth) {
    const float zn = 0.1f, zf = 100.0f;
    Mat4 p = perspective(1.0f, 16.0f / 9.0f, zn, zf);
    // Row-vector: z_clip = z*m[10] + m[14], w_clip = z*m[11].
    float z_near = zn * p.m[10] + p.m[14];
    float w_near = zn * p.m[11];
    CHECK_NEAR(z_near / w_near, 0.0f, 1e-4);

    float z_far = zf * p.m[10] + p.m[14];
    float w_far = zf * p.m[11];
    CHECK_NEAR(z_far / w_far, 1.0f, 1e-4);
}

CM_TEST(pose, identity_quaternion_gives_the_identity_matrix) {
    const float q[4] = {0, 0, 0, 1};
    float m[9] = {};
    quatToM3(q, m);
    const float expect[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) CHECK_NEAR(m[i], expect[i], kEps);
}

CM_TEST(pose, quaternion_rotation_matches_the_matrix_it_builds) {
    // 90 degrees about Z.
    const float s = 0.70710678f;
    const float q[4] = {0, 0, s, s};
    float m[9] = {};
    quatToM3(q, m);
    float v[3] = {1, 0, 0}, o[3] = {};
    m3vec(m, v, o);
    CHECK_NEAR(o[0], 0.0f, 1e-4);
    CHECK_NEAR(o[1], 1.0f, 1e-4);
    CHECK_NEAR(o[2], 0.0f, 1e-4);
}

CM_TEST(pose, unnormalized_quaternions_are_renormalized) {
    const float q[4] = {0, 0, 2 * 0.70710678f, 2 * 0.70710678f}; // same rotation, length 2
    float m[9] = {};
    quatToM3(q, m);
    float v[3] = {1, 0, 0}, o[3] = {};
    m3vec(m, v, o);
    CHECK_NEAR(o[0], 0.0f, 1e-4);
    CHECK_NEAR(o[1], 1.0f, 1e-4); // not scaled by 4
}

CM_TEST(pose, quaternion_product_is_hamilton_order) {
    const float s = 0.70710678f;
    const float rz90[4] = {0, 0, s, s};
    float o[4] = {};
    quatMul(rz90, rz90, o); // 180 degrees about Z
    CHECK_NEAR(o[2], 1.0f, 1e-4);
    CHECK_NEAR(o[3], 0.0f, 1e-4);
}

CM_TEST(pose, m3mul_composes_two_rotations) {
    const float s = 0.70710678f;
    const float q[4] = {0, 0, s, s};
    float a[9] = {}, o[9] = {};
    quatToM3(q, a);
    m3mul(a, a, o); // 180 degrees about Z
    float v[3] = {1, 0, 0}, r[3] = {};
    m3vec(o, v, r);
    CHECK_NEAR(r[0], -1.0f, 1e-4);
    CHECK_NEAR(r[1], 0.0f, 1e-4);
}

// GW2 is Z-up, so a map prop's yaw is a rotation about Z, not Y.
CM_TEST(scene, world_matrix_treats_z_as_up) {
    const float pos[3] = {10, 20, 30};
    const float rot[3] = {0, 0, 3.14159265f / 2}; // 90 degrees of yaw
    Mat4 w = sceneWorld(pos, rot, 2.0f);
    Vec3 p = transformPoint(Vec3{1, 0, 0}, w);
    CHECK_NEAR(p.x, 10.0f, 1e-3); // scaled 2x, yawed to +Y, then translated
    CHECK_NEAR(p.y, 22.0f, 1e-3);
    CHECK_NEAR(p.z, 30.0f, 1e-3);
}

CM_TEST(scene, non_uniform_scale_is_per_axis) {
    Mat4 s = scaleMatXYZ(2, 3, 4);
    Vec3 p = transformPoint(Vec3{1, 1, 1}, s);
    CHECK_NEAR(p.x, 2.0f, kEps);
    CHECK_NEAR(p.y, 3.0f, kEps);
    CHECK_NEAR(p.z, 4.0f, kEps);
}
