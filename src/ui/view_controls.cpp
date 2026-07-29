/// @file
/// @brief Toolbar commands: zoom, fit, render mode, gizmo mode, LOD selectors.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>

namespace castlemist::ui {

void set_export_enabled(bool enabled) {
    if (g_file_menu == nullptr) {
        return;
    }
    UINT flags = enabled ? MF_ENABLED : (MF_GRAYED | MF_DISABLED);
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_COMPRESSED, flags);
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_DECOMPRESSED, flags);
}

void show_loading(bool loading, uint32_t mft_index) {
    if (g_app == nullptr) {
        return;
    }
    if (loading) {
        wchar_t buf[128];
        swprintf(buf, 128, L"Loading entry #%u...", mft_index);
        SetWindowTextW(g_app->hwnd_status_label, buf);
        ShowWindow(g_app->hwnd_progress, SW_SHOW);
        SendMessageW(g_app->hwnd_progress, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(g_app->hwnd_progress, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(g_app->hwnd_progress, SW_HIDE);
        SetWindowTextW(g_app->hwnd_status_label, L"Ready");
    }
}

void zoom_by(float factor) {
    if (g_app == nullptr) {
        return;
    }
    g_app->preview_zoom = std::clamp(g_app->preview_zoom * factor, 0.05f, 40.0f);
    castlemist::gfx::set_view(g_app->preview_zoom, g_app->preview_pan_x, g_app->preview_pan_y);
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
}

void rotate_90() {
    if (g_app == nullptr) {
        return;
    }
    g_app->preview_rotation_quarters = (g_app->preview_rotation_quarters + 1) % 4;
    g_app->preview_pan_x = 0.0f;
    g_app->preview_pan_y = 0.0f;
    castlemist::gfx::set_rotation(g_app->preview_rotation_quarters);
    castlemist::gfx::set_view(g_app->preview_zoom, g_app->preview_pan_x, g_app->preview_pan_y);
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
}

void fit_view() {
    if (g_app == nullptr) {
        return;
    }
    g_app->preview_zoom = 1.0f;
    g_app->preview_pan_x = 0.0f;
    g_app->preview_pan_y = 0.0f;
    castlemist::gfx::reset_view();
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
}

// Lazily extract + build the real game (bgfx DXBC) shaders for the current map
// scene the first time the map "Shader" mode is picked. Kept out of the initial
// map load (which stays prop-only fast); done here on demand with a wait cursor.
void ensure_map_game_materials() {
    if (g_app == nullptr || !castlemist::render::scene_active()) return;
    if (castlemist::render::scene_game_ready()) return;
    if (!g_app->current_entry.map) return;
    MapScene& ms = *g_app->current_entry.map;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    if (augment_map_scene_game(ms, g_app->data_gw2.file_info.file_path))
        castlemist::render::build_scene_game_materials(ms.models);
    SetCursor(old);
}

void set_model_mode(castlemist::render::RenderMode mode) {
    if (g_app == nullptr) {
        return;
    }
    g_app->model_mode = mode;
    // The map scene needs its game materials built before GameShader can draw.
    if (mode == castlemist::render::RenderMode::GameShader && castlemist::render::scene_active())
        ensure_map_game_materials();
    castlemist::render::set_mode(mode);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

void reset_model_view() {
    castlemist::render::reset_view();
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

// Refresh the transform-readout overlay with the current Loc/Rot/Scale values.
void update_gizmo_readout() {
    if (g_app == nullptr || g_app->hwnd_gizmo_readout == nullptr) return;
    float p[3], r[3], s[3];
    castlemist::render::get_object_transform(p, r, s);
    const wchar_t* mode = L"Move";
    switch (castlemist::render::gizmo_mode()) {
        case castlemist::render::GizmoMode::Rotate: mode = L"Rotate"; break;
        case castlemist::render::GizmoMode::Scale: mode = L"Scale"; break;
        case castlemist::render::GizmoMode::None: mode = L"View"; break;
        default: break;
    }
    wchar_t buf[256];
    swprintf(buf, 256,
             L"%ls\nLoc   X %.2f  Y %.2f  Z %.2f\nRot   X %.1f  Y %.1f  Z %.1f\nScale X %.2f  Y %.2f  Z %.2f",
             mode, p[0], p[1], p[2], r[0], r[1], r[2], s[0], s[1], s[2]);
    SetWindowTextW(g_app->hwnd_gizmo_readout, buf);
}

// Switch gizmo mode and keep the three push-buttons in a radio state.
void set_gizmo_mode_ui(castlemist::render::GizmoMode m) {
    castlemist::render::set_gizmo_mode(m);
    SendMessageW(g_app->hwnd_gizmo_move, BM_SETCHECK,
                 m == castlemist::render::GizmoMode::Translate ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_app->hwnd_gizmo_rotate, BM_SETCHECK,
                 m == castlemist::render::GizmoMode::Rotate ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_app->hwnd_gizmo_scale, BM_SETCHECK,
                 m == castlemist::render::GizmoMode::Scale ? BST_CHECKED : BST_UNCHECKED, 0);
    update_gizmo_readout();
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

// The LOD/texture controls target this submesh: combo item 0 = "All submeshes"
// (-1, the overall control); item k = submesh k-1.
int lod_target_submesh() {
    int c = static_cast<int>(SendMessageW(g_app->hwnd_submesh_combo, CB_GETCURSEL, 0, 0));
    return c <= 0 ? -1 : c - 1;
}

// Repopulate the LOD combo + reduced-tex checkbox to reflect the current submesh
// selection ("All" shows the max LOD range and an unchecked box as a neutral start).
void refresh_lod_controls() {
    if (g_app == nullptr) return;
    int sub = lod_target_submesh();
    int nlod = (sub < 0) ? castlemist::render::max_lod_count() : castlemist::render::submesh_lod_count(sub);
    if (nlod < 1) nlod = 1;
    SendMessageW(g_app->hwnd_lod_combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < nlod; ++i) {
        wchar_t b[24];
        swprintf(b, 24, L"LOD %d", i);
        SendMessageW(g_app->hwnd_lod_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b));
    }
    int curLod = (sub < 0) ? 0 : castlemist::render::submesh_lod(sub);
    SendMessageW(g_app->hwnd_lod_combo, CB_SETCURSEL, std::min(curLod, nlod - 1), 0);
    bool reduced = (sub >= 0) && castlemist::render::submesh_tex_reduced(sub);
    SendMessageW(g_app->hwnd_tex_reduced, BM_SETCHECK, reduced ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Rebuild the submesh selector for a freshly loaded model.
void populate_lod_controls() {
    if (g_app == nullptr) return;
    SendMessageW(g_app->hwnd_submesh_combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_app->hwnd_submesh_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All submeshes"));
    int n = castlemist::render::submesh_count();
    for (int i = 0; i < n; ++i) {
        wchar_t w[160];
        MultiByteToWideChar(CP_UTF8, 0, castlemist::render::submesh_label(i), -1, w, 160);
        SendMessageW(g_app->hwnd_submesh_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w));
    }
    SendMessageW(g_app->hwnd_submesh_combo, CB_SETCURSEL, 0, 0);
    refresh_lod_controls();
}

// Index of the audio sound currently selected (0 for single-clip entries).
// The entry whose audio is active: the clicked content sub-asset in cntc browser mode,
// otherwise the main selected entry.

} // namespace castlemist::ui
