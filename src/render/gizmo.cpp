/// @file
/// @brief The Blender-style interactive transform gizmo and the ground grid.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace castlemist::render {

Mat4 object_matrix() {
    const float d2r = 3.14159265358979f / 180.0f;
    Mat4 R = mul(rotX(g_obj_rot[0] * d2r), mul(rotY(g_obj_rot[1] * d2r), rotZ(g_obj_rot[2] * d2r)));
    Mat4 S = scaleMatXYZ(g_obj_scale[0], g_obj_scale[1], g_obj_scale[2]);
    Mat4 SR = mul(S, R);
    Vec3 back{g_center.x + g_obj_pos[0], g_center.y + g_obj_pos[1], g_center.z + g_obj_pos[2]};
    return mul(translate({-g_center.x, -g_center.y, -g_center.z}), mul(SR, translate(back)));
}

bool gizmo_identity() {
    return g_obj_pos[0] == 0 && g_obj_pos[1] == 0 && g_obj_pos[2] == 0 &&
           g_obj_rot[0] == 0 && g_obj_rot[1] == 0 && g_obj_rot[2] == 0 &&
           g_obj_scale[0] == 1 && g_obj_scale[1] == 1 && g_obj_scale[2] == 1;
}

// The "view transform" applied to gizmo/grid geometry: centering + orbit, WITHOUT
// the object transform (so the gizmo sits at the object's transformed pivot).
Mat4 giz_view_transform() {
    return mul(translate({-g_center.x, -g_center.y, -g_center.z}), mul(g_rot, translate(g_center)));
}
Mat4 compute_giz_mvp() {
    Vec3 eye; Mat4 view, proj;
    main_camera(eye, view, proj);   // same matrices as the model draw -- see main_camera
    return mul(giz_view_transform(), mul(view, proj));
}

// World -> model-surface pixel. Returns false if behind the camera.
bool project_pt(const Mat4& mvp, Vec3 p, float& sx, float& sy) {
    float x = p.x * mvp.m[0] + p.y * mvp.m[4] + p.z * mvp.m[8] + mvp.m[12];
    float y = p.x * mvp.m[1] + p.y * mvp.m[5] + p.z * mvp.m[9] + mvp.m[13];
    float w = p.x * mvp.m[3] + p.y * mvp.m[7] + p.z * mvp.m[11] + mvp.m[15];
    if (w <= 1e-5f) return false;
    sx = (x / w * 0.5f + 0.5f) * g_w;
    sy = (1.0f - (y / w * 0.5f + 0.5f)) * g_h;
    return true;
}

Vec3 giz_pivot() { return {g_center.x + g_obj_pos[0], g_center.y + g_obj_pos[1], g_center.z + g_obj_pos[2]}; }
float giz_len() { return g_radius * 0.6f; }
Vec3 axis_dir(int k) { return k == 0 ? Vec3{1, 0, 0} : (k == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}); }

// world-space directions that map to screen right / up (transpose of g_rot).
Vec3 world_screen_right() { return {g_rot.m[0], g_rot.m[4], g_rot.m[8]}; }
Vec3 world_screen_up() { return {g_rot.m[1], g_rot.m[5], g_rot.m[9]}; }

float pt_seg_dist2(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float l2 = dx * dx + dy * dy;
    float t = l2 > 1e-6f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    float cx = ax + t * dx, cy = ay + t * dy;
    float ex = px - cx, ey = py - cy;
    return ex * ex + ey * ey;
}

// Hit-test the gizmo handles. Returns axis 0/1/2, 3 (center), or -1.
int gizmo_hit(int mx, int my) {
    if (g_gizmo_mode == GizmoMode::None || !g_has_model || g_scene_mode) return -1;
    Mat4 mvp = compute_giz_mvp();
    Vec3 P = giz_pivot();
    float L = giz_len();
    float ps_x, ps_y;
    bool pivotOk = project_pt(mvp, P, ps_x, ps_y);
    const float thr2 = 11.0f * 11.0f;
    int best = -1;
    float bestD = thr2;

    if (g_gizmo_mode == GizmoMode::Rotate) {
        // Nearest of the three rings (planes perpendicular to each axis).
        for (int k = 0; k < 3; ++k) {
            Vec3 u = axis_dir((k + 1) % 3), v = axis_dir((k + 2) % 3);
            float prevx = 0, prevy = 0; bool have = false;
            for (int i = 0; i <= 48; ++i) {
                float a = i / 48.0f * 6.2831853f;
                Vec3 pt{P.x + (u.x * std::cos(a) + v.x * std::sin(a)) * L,
                        P.y + (u.y * std::cos(a) + v.y * std::sin(a)) * L,
                        P.z + (u.z * std::cos(a) + v.z * std::sin(a)) * L};
                float sx, sy;
                if (project_pt(mvp, pt, sx, sy)) {
                    if (have) {
                        float d = pt_seg_dist2((float)mx, (float)my, prevx, prevy, sx, sy);
                        if (d < bestD) { bestD = d; best = k; }
                    }
                    prevx = sx; prevy = sy; have = true;
                } else have = false;
            }
        }
        return best;
    }

    // Translate / Scale: three straight axis handles + a center handle.
    for (int k = 0; k < 3; ++k) {
        Vec3 d = axis_dir(k);
        Vec3 tip{P.x + d.x * L, P.y + d.y * L, P.z + d.z * L};
        float s0x, s0y, s1x, s1y;
        if (project_pt(mvp, P, s0x, s0y) && project_pt(mvp, tip, s1x, s1y)) {
            float dd = pt_seg_dist2((float)mx, (float)my, s0x, s0y, s1x, s1y);
            if (dd < bestD) { bestD = dd; best = k; }
        }
    }
    if (pivotOk) {
        float cd = (mx - ps_x) * (mx - ps_x) + (my - ps_y) * (my - ps_y);
        if (cd < 9.0f * 9.0f && cd < bestD) best = 3;
    }
    return best;
}

// ---- gizmo geometry (rebuilt each frame) ----
void push_line(std::vector<ColVtx>& v, Vec3 a, Vec3 b, float r, float g, float bl, float al) {
    v.push_back({a.x, a.y, a.z, r, g, bl, al});
    v.push_back({b.x, b.y, b.z, r, g, bl, al});
}
void axis_color(int k, int hi, float& r, float& g, float& b) {
    if (k == hi) { r = 1.0f; g = 0.85f; b = 0.15f; return; } // highlighted (yellow)
    if (k == 0) { r = 0.92f; g = 0.24f; b = 0.30f; }        // X red
    else if (k == 1) { r = 0.44f; g = 0.80f; b = 0.24f; }   // Y green
    else { r = 0.26f; g = 0.55f; b = 0.96f; }               // Z blue
}

// Builds the overlay line list. The GROUND GRID is emitted FIRST and its vertex
// count reported through `gridVerts`, because the two halves are drawn with
// different depth state: the grid is scene geometry and must be hidden behind the
// model, the gizmo handles are a UI overlay and must stay on top of it. See
// draw_gizmo.
void build_gizmo_verts(std::vector<ColVtx>& v, size_t* gridVerts) {
    // Ground grid on the XZ plane, under the model.
    if (g_show_grid) {
        float gy = g_center.y - g_radius;
        float ext = g_radius * 3.0f;
        int n = 12;
        float step = (ext * 2.0f) / n;
        for (int i = 0; i <= n; ++i) {
            float o = -ext + step * i;
            bool axisX = std::fabs(o) < step * 0.25f; // line running along Z through x=0
            float a = axisX ? 0.35f : 0.16f;
            float cr = axisX ? 0.30f : 0.55f, cg = axisX ? 0.45f : 0.55f, cb = axisX ? 0.95f : 0.55f;
            push_line(v, {g_center.x + o, gy, g_center.z - ext}, {g_center.x + o, gy, g_center.z + ext}, cr, cg, cb, a);
            bool axisZ = std::fabs(o) < step * 0.25f;
            float ar = axisZ ? 0.92f : 0.55f, ag = axisZ ? 0.24f : 0.55f, ab = axisZ ? 0.30f : 0.55f;
            float aa = axisZ ? 0.35f : 0.16f;
            push_line(v, {g_center.x - ext, gy, g_center.z + o}, {g_center.x + ext, gy, g_center.z + o}, ar, ag, ab, aa);
        }
    }
    if (gridVerts) *gridVerts = v.size();

    if (g_gizmo_mode == GizmoMode::None) return;
    Vec3 P = giz_pivot();
    float L = giz_len();
    int hi = g_gizmo_dragging ? g_gizmo_axis : g_gizmo_hover;

    if (g_gizmo_mode == GizmoMode::Rotate) {
        for (int k = 0; k < 3; ++k) {
            float r, g, b; axis_color(k, hi, r, g, b);
            Vec3 u = axis_dir((k + 1) % 3), w = axis_dir((k + 2) % 3);
            Vec3 prev{}; bool have = false;
            for (int i = 0; i <= 64; ++i) {
                float a = i / 64.0f * 6.2831853f;
                Vec3 pt{P.x + (u.x * std::cos(a) + w.x * std::sin(a)) * L,
                        P.y + (u.y * std::cos(a) + w.y * std::sin(a)) * L,
                        P.z + (u.z * std::cos(a) + w.z * std::sin(a)) * L};
                if (have) push_line(v, prev, pt, r, g, b, 1.0f);
                prev = pt; have = true;
            }
        }
        return;
    }

    // Translate / Scale: colored axis lines with tip decoration.
    for (int k = 0; k < 3; ++k) {
        float r, g, b; axis_color(k, hi, r, g, b);
        Vec3 d = axis_dir(k);
        Vec3 tip{P.x + d.x * L, P.y + d.y * L, P.z + d.z * L};
        push_line(v, P, tip, r, g, b, 1.0f);
        Vec3 u = axis_dir((k + 1) % 3), w = axis_dir((k + 2) % 3);
        float hs = L * 0.10f;
        if (g_gizmo_mode == GizmoMode::Scale) {
            // small box outline at the tip
            Vec3 c[4] = {
                {tip.x + (u.x + w.x) * hs, tip.y + (u.y + w.y) * hs, tip.z + (u.z + w.z) * hs},
                {tip.x + (u.x - w.x) * hs, tip.y + (u.y - w.y) * hs, tip.z + (u.z - w.z) * hs},
                {tip.x - (u.x + w.x) * hs, tip.y - (u.y + w.y) * hs, tip.z - (u.z + w.z) * hs},
                {tip.x - (u.x - w.x) * hs, tip.y - (u.y - w.y) * hs, tip.z - (u.z - w.z) * hs},
            };
            for (int i = 0; i < 4; ++i) push_line(v, c[i], c[(i + 1) & 3], r, g, b, 1.0f);
        } else {
            // arrowhead: four lines from tip back toward the shaft
            Vec3 base{tip.x - d.x * hs * 2.0f, tip.y - d.y * hs * 2.0f, tip.z - d.z * hs * 2.0f};
            push_line(v, tip, {base.x + u.x * hs, base.y + u.y * hs, base.z + u.z * hs}, r, g, b, 1.0f);
            push_line(v, tip, {base.x - u.x * hs, base.y - u.y * hs, base.z - u.z * hs}, r, g, b, 1.0f);
            push_line(v, tip, {base.x + w.x * hs, base.y + w.y * hs, base.z + w.z * hs}, r, g, b, 1.0f);
            push_line(v, tip, {base.x - w.x * hs, base.y - w.y * hs, base.z - w.z * hs}, r, g, b, 1.0f);
        }
    }
    // center handle marker
    {
        float cr = (hi == 3) ? 1.0f : 0.85f, cg = (hi == 3) ? 0.85f : 0.85f, cb = (hi == 3) ? 0.15f : 0.85f;
        float s = L * 0.06f;
        Vec3 r = world_screen_right(), u = world_screen_up();
        Vec3 a{P.x + (r.x + u.x) * s, P.y + (r.y + u.y) * s, P.z + (r.z + u.z) * s};
        Vec3 bb{P.x + (r.x - u.x) * s, P.y + (r.y - u.y) * s, P.z + (r.z - u.z) * s};
        Vec3 c{P.x - (r.x + u.x) * s, P.y - (r.y + u.y) * s, P.z - (r.z + u.z) * s};
        Vec3 dd{P.x - (r.x - u.x) * s, P.y - (r.y - u.y) * s, P.z - (r.z - u.z) * s};
        push_line(v, a, bb, cr, cg, cb, 1.0f);
        push_line(v, bb, c, cr, cg, cb, 1.0f);
        push_line(v, c, dd, cr, cg, cb, 1.0f);
        push_line(v, dd, a, cr, cg, cb, 1.0f);
    }
}

void draw_gizmo(const Mat4& gizMVP) {
    if (!g_gizVs || !g_gizPs || !g_gizIl) return;
    if (g_gizmo_mode == GizmoMode::None && !g_show_grid) return;
    std::vector<ColVtx> verts;
    size_t gridVerts = 0;
    build_gizmo_verts(verts, &gridVerts);
    if (verts.empty()) return;

    if (verts.size() > g_giz_vert_cap || !g_gizVb) {
        g_giz_vert_cap = static_cast<UINT>(verts.size() + 256);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = g_giz_vert_cap * sizeof(ColVtx);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_gizVb.Reset();
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_gizVb))) return;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_gizVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, verts.data(), verts.size() * sizeof(ColVtx));
        g_ctx->Unmap(g_gizVb.Get(), 0);
    }
    // Update the shared CB's leading uMVP to the gizmo view transform.
    D3D11_MAPPED_SUBRESOURCE cm;
    if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        CB cb{}; cb.mvp = gizMVP; cb.exposure = g_light_intensity;
        std::memcpy(cm.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_cb.Get(), 0);
    }
    const float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendAlpha.Get(), bf, 0xffffffff);
    g_ctx->RSSetState(g_rsSolid.Get());
    UINT stride = sizeof(ColVtx), off = 0;
    ID3D11Buffer* vbs[] = {g_gizVb.Get()};
    g_ctx->IASetInputLayout(g_gizIl.Get());
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g_ctx->VSSetShader(g_gizVs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_gizPs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_cb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);

    // The GROUND GRID is world geometry: DEPTH-TESTED, so the model hides the part
    // of the floor behind it. Drawing it with the depth test off (as the whole
    // overlay used to be) paints faint grid lines straight across solid geometry,
    // which reads as the model being transparent -- the reported bug. Depth WRITES
    // stay off so the translucent lines never occlude anything themselves.
    //
    // This only works because the grid now projects with the same matrices as the
    // model (main_camera); with the old separate 0.02r..40r projection its depth was
    // not comparable to the geometry's at all.
    if (gridVerts > 0) {
        g_ctx->OMSetDepthStencilState(g_dssNoWrite.Get(), 0);
        g_ctx->Draw(static_cast<UINT>(gridVerts), 0);
    }
    // The gizmo HANDLES are UI, not geometry: always visible, Blender-style.
    if (verts.size() > gridVerts) {
        g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
        g_ctx->Draw(static_cast<UINT>(verts.size() - gridVerts), static_cast<UINT>(gridVerts));
    }
}


void set_gizmo_mode(GizmoMode m) { g_gizmo_mode = m; }
GizmoMode gizmo_mode() { return g_gizmo_mode; }
void set_show_grid(bool on) { g_show_grid = on; }
bool show_grid() { return g_show_grid; }
void set_gizmo_hover(int axis) { g_gizmo_hover = axis; }
bool gizmo_active() { return g_gizmo_dragging; }
int gizmo_pick(int sx, int sy) { return gizmo_hit(sx, sy); }

void get_object_transform(float pos[3], float rot_deg[3], float scale[3]) {
    for (int i = 0; i < 3; ++i) { pos[i] = g_obj_pos[i]; rot_deg[i] = g_obj_rot[i]; scale[i] = g_obj_scale[i]; }
}
void set_object_transform(const float pos[3], const float rot_deg[3], const float scale[3]) {
    for (int i = 0; i < 3; ++i) { g_obj_pos[i] = pos[i]; g_obj_rot[i] = rot_deg[i]; g_obj_scale[i] = scale[i]; }
}
void reset_object_transform() {
    for (int i = 0; i < 3; ++i) { g_obj_pos[i] = 0; g_obj_rot[i] = 0; g_obj_scale[i] = 1; }
    g_gizmo_dragging = false; g_gizmo_axis = -1;
}

bool gizmo_begin(int sx, int sy) {
    int hit = gizmo_hit(sx, sy);
    if (hit < 0) { g_gizmo_dragging = false; g_gizmo_axis = -1; return false; }
    g_gizmo_axis = hit;
    g_gizmo_dragging = true;
    g_giz_down = POINT{sx, sy};
    for (int i = 0; i < 3; ++i) {
        g_giz_start_pos[i] = g_obj_pos[i];
        g_giz_start_rot[i] = g_obj_rot[i];
        g_giz_start_scale[i] = g_obj_scale[i];
    }
    Mat4 mvp = compute_giz_mvp();
    float px, py;
    if (project_pt(mvp, giz_pivot(), px, py)) {
        g_giz_start_angle = std::atan2((float)sy - py, (float)sx - px);
        float dx = sx - px, dy = sy - py;
        g_giz_start_dist = std::sqrt(dx * dx + dy * dy);
        if (g_giz_start_dist < 4.0f) g_giz_start_dist = 4.0f;
    }
    return true;
}

void gizmo_update(int sx, int sy) {
    if (!g_gizmo_dragging) return;
    Mat4 mvp = compute_giz_mvp();
    Vec3 P = giz_pivot();
    float L = giz_len();
    float px, py;
    if (!project_pt(mvp, P, px, py)) return;
    int k = g_gizmo_axis;

    if (g_gizmo_mode == GizmoMode::Translate) {
        if (k >= 0 && k < 3) {
            Vec3 d = axis_dir(k);
            Vec3 tip{P.x + d.x * L, P.y + d.y * L, P.z + d.z * L};
            float s0x, s0y, s1x, s1y;
            if (project_pt(mvp, P, s0x, s0y) && project_pt(mvp, tip, s1x, s1y)) {
                float ax = s1x - s0x, ay = s1y - s0y;
                float alen = std::sqrt(ax * ax + ay * ay);
                if (alen < 1e-3f) return;
                float nx = ax / alen, ny = ay / alen;
                float mdx = (float)sx - g_giz_down.x, mdy = (float)sy - g_giz_down.y;
                float amtPx = mdx * nx + mdy * ny;
                g_obj_pos[k] = g_giz_start_pos[k] + amtPx * (L / alen);
            }
        } else {
            Vec3 wr = world_screen_right(), wu = world_screen_up();
            Vec3 refp{P.x + wr.x * L, P.y + wr.y * L, P.z + wr.z * L};
            float sbx, sby;
            float rl = 1.0f;
            if (project_pt(mvp, refp, sbx, sby)) {
                rl = std::sqrt((sbx - px) * (sbx - px) + (sby - py) * (sby - py));
                if (rl < 1e-3f) rl = 1.0f;
            }
            float wpp = L / rl;
            float mdx = (float)sx - g_giz_down.x, mdy = (float)sy - g_giz_down.y;
            float wrc[3] = {wr.x, wr.y, wr.z}, wuc[3] = {wu.x, wu.y, wu.z};
            for (int i = 0; i < 3; ++i)
                g_obj_pos[i] = g_giz_start_pos[i] + (wrc[i] * mdx - wuc[i] * mdy) * wpp;
        }
    } else if (g_gizmo_mode == GizmoMode::Scale) {
        float dx = (float)sx - px, dy = (float)sy - py;
        float dist = std::sqrt(dx * dx + dy * dy);
        float f = dist / g_giz_start_dist;
        if (f < 0.01f) f = 0.01f;
        if (k >= 0 && k < 3) g_obj_scale[k] = g_giz_start_scale[k] * f;
        else for (int i = 0; i < 3; ++i) g_obj_scale[i] = g_giz_start_scale[i] * f;
    } else if (g_gizmo_mode == GizmoMode::Rotate) {
        if (k < 0 || k > 2) return;
        float ang = std::atan2((float)sy - py, (float)sx - px);
        float delta = ang - g_giz_start_angle;
        float zc = (k == 0) ? g_rot.m[2] : (k == 1 ? g_rot.m[6] : g_rot.m[10]);
        float sign = (zc > 0.0f) ? 1.0f : -1.0f;
        g_obj_rot[k] = g_giz_start_rot[k] + delta * (180.0f / 3.14159265358979f) * sign;
    }
}

void gizmo_end() { g_gizmo_dragging = false; g_gizmo_axis = -1; }


} // namespace castlemist::render
