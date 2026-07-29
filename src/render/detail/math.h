/// @file
/// @brief Row-major 3D maths and Granny pose maths for the model renderer.
///
/// Deliberately tiny and header-only: it matches `gw2viewer.cpp` exactly, so the
/// reconstruction shaders and the game's own DXBC shaders see byte-identical
/// matrices. Row-vector convention throughout (`v * M`), which is why the
/// translation lives in the matrix's last row and why matrices are transposed
/// before being written into a bgfx constant buffer.
///
/// @warning Not a public header -- internal to `src/render/`.

#pragma once

#include <cmath>

namespace castlemist::render {

struct Vec3 {
    float x, y, z;
};
inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 norm(Vec3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : a;
}
struct Mat4 {
    float m[16];
};
inline Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}
inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
inline Mat4 perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / std::tan(fovy * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    return r;
}
inline Mat4 lookAt(Vec3 eye, Vec3 at, Vec3 up) {
    Vec3 z = norm(sub(at, eye));
    Vec3 x = norm(cross(up, z));
    Vec3 y = cross(z, x);
    Mat4 r = identity();
    r.m[0] = x.x; r.m[1] = y.x; r.m[2] = z.x;
    r.m[4] = x.y; r.m[5] = y.y; r.m[6] = z.y;
    r.m[8] = x.z; r.m[9] = y.z; r.m[10] = z.z;
    r.m[12] = -dot(x, eye); r.m[13] = -dot(y, eye); r.m[14] = -dot(z, eye);
    return r;
}
// Row-major (row-vector) translate/rotate helpers for the free-trackball model
// rotation -- translation lives in the last row (m[12..14]), matching lookAt.
inline Mat4 translate(Vec3 t) {
    Mat4 r = identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
inline Mat4 rotY(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}
inline Mat4 rotX(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
    return r;
}
// Row-vector point transform (p is treated as [x y z 1], translation in the
// matrix's last row) -- used to get a submesh's world/view-space depth for
// back-to-front transparent sorting.
inline Vec3 transformPoint(Vec3 p, const Mat4& m) {
    return {
        p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12],
        p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13],
        p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14],
    };
}

// Same HLSL as gw2viewer.cpp (albedo * lambert + hemispheric ambient, optional

// ---- pose maths (matches native/main.cpp posemath, verified against the bind pose)
// Granny transform: v' = pos + R(quat) * (ScaleShear * v).
inline void quatToM3(const float q[4], float m[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = x*x + y*y + z*z + w*w; float s = n > 1e-12f ? 2.0f/n : 0.0f;
    float xs=x*s,ys=y*s,zs=z*s, wx=w*xs,wy=w*ys,wz=w*zs, xx=x*xs,xy=x*ys,xz=x*zs, yy=y*ys,yz=y*zs,zz=z*zs;
    m[0]=1-(yy+zz); m[1]=xy-wz;    m[2]=xz+wy;
    m[3]=xy+wz;     m[4]=1-(xx+zz);m[5]=yz-wx;
    m[6]=xz-wy;     m[7]=yz+wx;    m[8]=1-(xx+yy);
}
inline void m3mul(const float a[9], const float b[9], float o[9]) {
    for (int r=0;r<3;++r) for (int c=0;c<3;++c)
        o[r*3+c]=a[r*3+0]*b[0*3+c]+a[r*3+1]*b[1*3+c]+a[r*3+2]*b[2*3+c];
}
inline void m3vec(const float m[9], const float v[3], float o[3]) {
    for (int r=0;r<3;++r) o[r]=m[r*3+0]*v[0]+m[r*3+1]*v[1]+m[r*3+2]*v[2];
}
// Hamilton quaternion product (x,y,z,w).
inline void quatMul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

// ---- scene placement helpers (GW2 is Z-up, so yaw is a rotation about Z) ----
inline Mat4 rotZ(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
    return r;
}
inline Mat4 scaleMat(float s) {
    Mat4 r = identity();
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}
// world (row-vector: p_world = p_model * world) = Scale * Rot(XYZ) * Translate.
inline Mat4 sceneWorld(const float pos[3], const float rot[3], float scale) {
    Mat4 R = mul(mul(rotX(rot[0]), rotY(rot[1])), rotZ(rot[2]));
    return mul(mul(scaleMat(scale), R), translate({pos[0], pos[1], pos[2]}));
}


inline Mat4 scaleMatXYZ(float sx, float sy, float sz) {
    Mat4 r = identity();
    r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
    return r;
}

// Object transform (TRS about the model center, then world-translate by pos).

} // namespace castlemist::render
