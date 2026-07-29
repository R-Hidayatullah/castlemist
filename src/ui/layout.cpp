/// @file
/// @brief Positioning every child window for the current client size.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>

namespace castlemist::ui {

void layout_children(int client_w, int client_h) {
    if (g_app == nullptr) {
        return;
    }

    // Reserve the bottom status strip (loading label + progress bar) out of
    // the height every other pane gets to use.
    const int usable_h = std::max(0, client_h - kStatusBarHeight);

    int list_w = std::clamp(static_cast<int>(client_w * g_app->list_width_ratio), kMinPaneSize,
                             std::max(kMinPaneSize, client_w - 2 * kMinPaneSize - 2 * kSplitterThickness));
    int info_w = std::clamp(static_cast<int>(client_w * g_app->info_width_ratio), kMinPaneSize,
                             std::max(kMinPaneSize, client_w - list_w - kMinPaneSize - 2 * kSplitterThickness));
    int middle_w = std::max(kMinPaneSize, client_w - list_w - info_w - 2 * kSplitterThickness);

    // Left column: search bar + list.
    MoveWindow(g_app->hwnd_search_edit, 6, 6, std::max(0, list_w - 12), 22, TRUE);
    MoveWindow(g_app->hwnd_search_fileid_check, 6, 32, std::max(0, list_w - 12 - 130), 22, TRUE);
    MoveWindow(g_app->hwnd_search_button, std::max(6, list_w - 122), 32, 55, 22, TRUE);
    MoveWindow(g_app->hwnd_clear_button, std::max(6, list_w - 63), 32, 55, 22, TRUE);
    // Filter combos row (index mode): Type | Container, split across the width.
    {
        int cw = std::max(40, (list_w - 12 - 6) / 2);
        MoveWindow(g_app->hwnd_filter_type, 6, 60, cw, 200, TRUE);
        MoveWindow(g_app->hwnd_filter_container, 6 + cw + 6, 60, cw, 200, TRUE);
    }
    MoveWindow(g_app->hwnd_list, 0, kSearchBarHeight, list_w, std::max(0, usable_h - kSearchBarHeight), TRUE);

    int x = list_w;
    MoveWindow(g_app->hwnd_split_list_middle, x, 0, kSplitterThickness, usable_h, TRUE);
    x += kSplitterThickness;

    const int middle_x = x;
    MoveWindow(g_app->hwnd_tab, middle_x, 0, middle_w, kTabHeight, TRUE);

    const int content_y = kTabHeight;
    const int content_h = std::max(0, usable_h - content_y);
    int sel = TabCtrl_GetCurSel(g_app->hwnd_tab);
    MiddleTab tab = (sel == 0) ? MiddleTab::Compressed : (sel == 1) ? MiddleTab::Decompressed : MiddleTab::Preview;

    ShowWindow(g_app->hwnd_hex_before, tab == MiddleTab::Compressed ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_hex_after, tab == MiddleTab::Decompressed ? SW_SHOW : SW_HIDE);

    // The single Preview tab hosts one of three surfaces depending on the
    // selected entry's detected type: image (gw2gfx), model (gw2m3d), or a
    // read-only text control (text/strs, and the fallback for anything else).
    bool show_preview = tab == MiddleTab::Preview;
    PreviewKind pk = g_app->has_loaded_entry ? g_app->current_entry.kind : PreviewKind::None;
    // cntc content browser: types list | entries list; the right pane shows the
    // clicked entry's asset, so its type ("ek") drives which surface is shown.
    bool content_mode = show_preview && pk == PreviewKind::Content;
    PreviewKind ek = content_mode ? (g_app->content_sub_loaded ? g_app->content_sub.kind : PreviewKind::None) : pk;
    const ExtractedEntry& se = content_mode ? g_app->content_sub : g_app->current_entry;
    // A Bink cinematic streams into the very same gw2gfx surface as a still
    // image (its frames are just a texture that changes 30 times a second), so
    // "show_image" covers the D3D pane for both; only the toolbar differs.
    // ...but only once the stream is actually open; if the Bink runtime is
    // missing or refused the file we fall through to the text surface, which
    // carries the header details plus the reason.
    bool show_video =
        show_preview && (ek == PreviewKind::Video || ek == PreviewKind::Cinematic) && castlemist::vid::is_open();
    bool show_image = show_preview && (ek == PreviewKind::Image || show_video);
    bool show_map = show_preview && !content_mode && ek == PreviewKind::Map && g_app->current_entry.map != nullptr &&
                    !g_app->current_entry.map->instances.empty();
    bool show_model = show_preview && ((ek == PreviewKind::Model && se.model != nullptr) || show_map);
    bool show_audio = show_preview && ek == PreviewKind::Audio;
    // strs gets its own table surface instead of the plain-text control.
    bool show_strs = show_preview && ek == PreviewKind::Strs;
    bool show_text = show_preview && !show_image && !show_model && !show_audio && !show_strs;
    // cntc browser tables: Types | Entries (side by side, top) + Assets (below).
    // All three are always visible in content mode -- the asset table is a real
    // pane in the left column now, not a dropdown floating over the preview toolbar.
    ShowWindow(g_app->hwnd_content_list, content_mode ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_content_child, content_mode ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_content_asset_list, content_mode ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_split_content, content_mode ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_split_content_h, content_mode ? SW_SHOW : SW_HIDE);

    ShowWindow(g_app->hwnd_preview, show_image ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_model, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_text_preview, show_text ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_strs_list, show_strs ? SW_SHOW : SW_HIDE);
    bool show_still = show_image && !show_video;
    ShowWindow(g_app->hwnd_zoom_in, show_still ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_zoom_out, show_still ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_rotate, show_still ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_fit, show_still ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_alpha, show_still ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_play, show_video ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_stop, show_video ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_loop, show_video ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_mute, (show_video && castlemist::vid::has_audio()) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_volume, (show_video && castlemist::vid::has_audio()) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_seek, show_video ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_time, show_video ? SW_SHOW : SW_HIDE);
    // Track selector only earns its space when the movie really has >1 track.
    bool video_multi_track = show_video && castlemist::vid::track_count() > 1;
    ShowWindow(g_app->hwnd_video_track, video_multi_track ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_video_subs, show_video ? SW_SHOW : SW_HIDE);
    bool video_multi_clip = show_video && g_app->current_entry.cinp_video_ids.size() > 1;
    ShowWindow(g_app->hwnd_video_clip, video_multi_clip ? SW_SHOW : SW_HIDE);
    if (!show_video) ShowWindow(g_app->hwnd_video_subtitle, SW_HIDE);
    ShowWindow(g_app->hwnd_mode_full, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_mode_plain, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_mode_wire, show_model ? SW_SHOW : SW_HIDE);
    // "Shader" (real DXBC) mode now applies to the map scene too (lazily builds
    // the game materials on first use), so show it for any 3D model surface.
    ShowWindow(g_app->hwnd_mode_shader, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_model_reset, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_skel_toggle, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    bool show_anim = show_model && !show_map && castlemist::render::has_skeleton();
    ShowWindow(g_app->hwnd_anim_combo, show_anim ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_anim_play, show_anim ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_tex_fullres, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_light_toggle, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_effects_toggle, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_cloth_toggle, (show_model && !show_map && castlemist::render::has_cloth()) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_light_label, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_light_slider, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_light_follow, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_light_angle, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_submesh_combo, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_lod_combo, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_tex_reduced, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    bool gizmo_ui = show_model && !show_map;
    ShowWindow(g_app->hwnd_gizmo_move, gizmo_ui ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_gizmo_rotate, gizmo_ui ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_gizmo_scale, gizmo_ui ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_gizmo_grid, gizmo_ui ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_gizmo_reset, gizmo_ui ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_gizmo_readout, gizmo_ui ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_tex_panel,
               (gizmo_ui && castlemist::texpanel::row_count(g_app->hwnd_tex_info) > 0) ? SW_SHOW : SW_HIDE);
    bool audio_multi = show_audio && se.audio_clips.size() > 1;
    ShowWindow(g_app->hwnd_audio_play, show_audio ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_audio_stop, show_audio ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_audio_combo, audio_multi ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_audio_seek, show_audio ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_audio_time, show_audio ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_layer_prop, show_map ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_layer_zone, show_map ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_layer_coll, show_map ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_map_preview, show_map ? SW_SHOW : SW_HIDE);

    if (tab == MiddleTab::Compressed) {
        MoveWindow(g_app->hwnd_hex_before, middle_x, content_y, middle_w, content_h, TRUE);
    } else if (tab == MiddleTab::Decompressed) {
        MoveWindow(g_app->hwnd_hex_after, middle_x, content_y, middle_w, content_h, TRUE);
    } else {
        constexpr int kButtonW = 70;
        constexpr int kButtonH = 22;
        constexpr int kGap = 6;
        // In cntc content-browser mode the left portion is a 3-table browser:
        //   Types | Entries   (side by side, top band)
        //   Assets            (full-width table, stacked below them)
        // The preview surface takes whatever is left to the right. The asset table
        // living here (rather than a dropdown over the toolbar) is what keeps the
        // per-surface buttons from ever being covered, at any window size.
        // Content-browser block is now resizable: content_browser_w (total width,
        // dragged by the vertical splitter) and content_assets_ratio (Assets height
        // fraction, dragged by the horizontal splitter). Types/Entries share the
        // block width ~43/57. All clamped so both the block and the preview keep a
        // usable minimum at any window size.
        int clist_col_w = 0;
        if (content_mode) {
            clist_col_w = std::clamp(g_app->content_browser_w, 240, std::max(240, middle_w - 260));
            int master_w = std::max(80, static_cast<int>((clist_col_w - kGap) * 0.43));
            int child_w = std::max(80, clist_col_w - kGap - master_w);
            int avail = std::max(0, content_h - kGap - kSplitterThickness);
            int assets_h = std::clamp(static_cast<int>(avail * g_app->content_assets_ratio),
                                      std::min(avail, 60), std::min(avail, std::max(60, avail - 60)));
            int top_h = std::max(0, avail - assets_h);
            MoveWindow(g_app->hwnd_content_list, middle_x, content_y, master_w, top_h, TRUE);
            MoveWindow(g_app->hwnd_content_child, middle_x + master_w + kGap, content_y, child_w, top_h, TRUE);
            // Horizontal splitter between the top tables and the assets table.
            MoveWindow(g_app->hwnd_split_content_h, middle_x, content_y + top_h, clist_col_w, kSplitterThickness, TRUE);
            MoveWindow(g_app->hwnd_content_asset_list, middle_x,
                       content_y + top_h + kSplitterThickness, clist_col_w, assets_h, TRUE);
            // Vertical splitter between the browser block and the preview surface.
            MoveWindow(g_app->hwnd_split_content, middle_x + clist_col_w, content_y, kSplitterThickness, content_h, TRUE);
        }
        int clist_w = content_mode ? (clist_col_w + kSplitterThickness) : 0;
        int pane_x = middle_x + (content_mode ? clist_w + kGap : 0);
        int pane_w = middle_w - (content_mode ? clist_w + kGap : 0);
        auto place_button = [&](HWND h, int slot) {
            MoveWindow(h, pane_x + kGap + (kButtonW + kGap) * slot, content_y + 3, kButtonW, kButtonH, TRUE);
        };
        int surface_y = content_y + kToolbarHeight;
        int surface_h = std::max(0, content_h - kToolbarHeight);
        if (show_image) {
            if (show_video) {
                // Transport row: Play/Pause  Stop  Loop  Mute | volume | seek | m:ss
                place_button(g_app->hwnd_video_play, 0);
                place_button(g_app->hwnd_video_stop, 1);
                int vx = pane_x + kGap + (kButtonW + kGap) * 2;
                MoveWindow(g_app->hwnd_video_loop, vx, content_y + 3, 52, kButtonH, TRUE);
                vx += 52 + kGap;
                if (castlemist::vid::has_audio()) {
                    MoveWindow(g_app->hwnd_video_mute, vx, content_y + 3, 52, kButtonH, TRUE);
                    vx += 52 + kGap;
                    MoveWindow(g_app->hwnd_video_volume, vx, content_y + 3, 80, kButtonH, TRUE);
                    vx += 80 + kGap;
                    if (castlemist::vid::track_count() > 1) {
                        MoveWindow(g_app->hwnd_video_track, vx, content_y + 3, 190, 300, TRUE);
                        vx += 190 + kGap;
                    }
                }
                MoveWindow(g_app->hwnd_video_subs, vx, content_y + 3, 52, kButtonH, TRUE);
                vx += 52 + kGap;
                if (g_app->current_entry.cinp_video_ids.size() > 1) {
                    MoveWindow(g_app->hwnd_video_clip, vx, content_y + 3, 150, 300, TRUE);
                    vx += 150 + kGap;
                }
                const int vtime_w = 150; // "m:ss / m:ss  (1234/5678)"
                int vseek_w = std::max(80, (pane_x + pane_w - kGap) - vx - vtime_w - kGap);
                MoveWindow(g_app->hwnd_video_seek, vx, content_y + 3, vseek_w, kButtonH, TRUE);
                MoveWindow(g_app->hwnd_video_time, vx + vseek_w + kGap, content_y + 3, vtime_w, kButtonH, TRUE);
            } else {
                place_button(g_app->hwnd_zoom_in, 0);
                place_button(g_app->hwnd_zoom_out, 1);
                place_button(g_app->hwnd_rotate, 2);
                place_button(g_app->hwnd_fit, 3);
                place_button(g_app->hwnd_alpha, 4);
            }
            MoveWindow(g_app->hwnd_preview, pane_x, surface_y, pane_w, surface_h, TRUE);
            layout_video_subtitle();
            castlemist::gfx::on_resize(pane_w, surface_h);
            // A DXGI_SWAP_EFFECT_DISCARD swapchain otherwise keeps showing its
            // last-Presented frame stretched into the new size until the next
            // real Present() -- force one immediately so resizes never look stale.
            castlemist::gfx::render();
        } else if (show_model) {
            // Wrapping toolbar: lay every visible control out left-to-right and wrap
            // to a new row when it would overflow the pane, so on a narrow window
            // ALL buttons stay visible (no more controls clipped off the right edge).
            // The 3D surface then starts below however many rows the toolbar used.
            struct TB { HWND h; int w; int ch; int yPad; };
            const int comboH = 240; // combobox height param drives its dropdown list
            std::vector<TB> tb;
            tb.push_back({g_app->hwnd_mode_full, kButtonW, kButtonH, 0});
            tb.push_back({g_app->hwnd_mode_plain, kButtonW, kButtonH, 0});
            tb.push_back({g_app->hwnd_mode_wire, kButtonW, kButtonH, 0});
            tb.push_back({g_app->hwnd_mode_shader, kButtonW, kButtonH, 0});
            tb.push_back({g_app->hwnd_model_reset, kButtonW, kButtonH, 0});
            if (show_map) {
                tb.push_back({g_app->hwnd_layer_prop, kButtonW, kButtonH, 0});
                tb.push_back({g_app->hwnd_layer_zone, kButtonW, kButtonH, 0});
                tb.push_back({g_app->hwnd_layer_coll, kButtonW, kButtonH, 0});
                tb.push_back({g_app->hwnd_map_preview, kButtonW, kButtonH, 0});
            } else {
                tb.push_back({g_app->hwnd_skel_toggle, kButtonW, kButtonH, 0});
                if (show_anim) {
                    tb.push_back({g_app->hwnd_anim_combo, 150, comboH, 0});
                    tb.push_back({g_app->hwnd_anim_play, 56, kButtonH, 0});
                }
                tb.push_back({g_app->hwnd_tex_fullres, 90, kButtonH, 0});
                tb.push_back({g_app->hwnd_light_toggle, 70, kButtonH, 0});
                tb.push_back({g_app->hwnd_submesh_combo, 190, comboH, 0});
                tb.push_back({g_app->hwnd_lod_combo, 90, comboH, 0});
                tb.push_back({g_app->hwnd_tex_reduced, 100, kButtonH, 0});
                if (castlemist::texpanel::row_count(g_app->hwnd_tex_info) > 0)
                    tb.push_back({g_app->hwnd_tex_panel, 74, kButtonH, 0});
                tb.push_back({g_app->hwnd_effects_toggle, 76, kButtonH, 0});
                if (castlemist::render::has_cloth()) tb.push_back({g_app->hwnd_cloth_toggle, 60, kButtonH, 0});
                tb.push_back({g_app->hwnd_light_label, 34, kButtonH, 4});
                tb.push_back({g_app->hwnd_light_slider, 130, kButtonH, 0});
                tb.push_back({g_app->hwnd_light_follow, 78, kButtonH, 0});
                tb.push_back({g_app->hwnd_light_angle, 110, kButtonH, 0});
                tb.push_back({g_app->hwnd_gizmo_move, 52, kButtonH, 0});
                tb.push_back({g_app->hwnd_gizmo_rotate, 52, kButtonH, 0});
                tb.push_back({g_app->hwnd_gizmo_scale, 52, kButtonH, 0});
                tb.push_back({g_app->hwnd_gizmo_grid, 44, kButtonH, 0});
                tb.push_back({g_app->hwnd_gizmo_reset, 68, kButtonH, 0});
            }
            int rowLeft = pane_x + kGap;
            int right = pane_x + pane_w - kGap;
            int fx = rowLeft, fy = content_y + 3, rows = 1;
            for (const auto& it : tb) {
                if (fx > rowLeft && fx + it.w > right) { fx = rowLeft; fy += kToolbarHeight; rows++; }
                MoveWindow(it.h, fx, fy + it.yPad, it.w, it.ch, TRUE);
                fx += it.w + kGap;
            }
            int toolbar_h = rows * kToolbarHeight;
            int surf_y = content_y + toolbar_h;
            int surf_h = std::max(0, content_h - toolbar_h);
            MoveWindow(g_app->hwnd_model, pane_x, surf_y, pane_w, surf_h, TRUE);
            castlemist::render::on_resize(pane_w, surf_h);
            castlemist::render::render();
        } else if (show_audio) {
            // Audio: a Play/Stop row + a seek bar + "m:ss / m:ss" time readout above
            // the info text (with a sound selector for multi-sound banks).
            place_button(g_app->hwnd_audio_play, 0);
            place_button(g_app->hwnd_audio_stop, 1);
            int cx = pane_x + kGap + (kButtonW + kGap) * 2;
            if (se.audio_clips.size() > 1) {
                MoveWindow(g_app->hwnd_audio_combo, cx, content_y + 3, 160, 300, TRUE);
                cx += 160 + kGap;
            }
            const int time_w = 96;
            int seek_w = std::max(80, (pane_x + pane_w - kGap) - cx - time_w - kGap);
            MoveWindow(g_app->hwnd_audio_seek, cx, content_y + 3, seek_w, kButtonH, TRUE);
            MoveWindow(g_app->hwnd_audio_time, cx + seek_w + kGap, content_y + 3, time_w, kButtonH, TRUE);
            MoveWindow(g_app->hwnd_text_preview, pane_x, surface_y, pane_w, surface_h, TRUE);
        } else if (show_strs) {
            // strs: the record table fills the whole pane (no toolbar of its own).
            MoveWindow(g_app->hwnd_strs_list, pane_x, content_y, pane_w, content_h, TRUE);
        } else {
            // text / unrecognized / content summary: full-height text control.
            // Nothing floats over the toolbar band anymore, so it can fill the pane.
            MoveWindow(g_app->hwnd_text_preview, pane_x, content_y, pane_w, content_h, TRUE);
        }
    }

    x = middle_x + middle_w;
    MoveWindow(g_app->hwnd_split_middle_info, x, 0, kSplitterThickness, usable_h, TRUE);
    x += kSplitterThickness;
    int right_w = std::max(0, client_w - x);

    // Right column: info panel on top, the per-submesh texture panel below it.
    // The texture panel only claims space when there is actually something to
    // show -- a single-model preview, the toggle on, and at least one resolved
    // texture -- so every other entry type keeps the full-height info panel it had
    // before. (Map scenes are excluded: they are many models, not one submesh set.)
    bool tex_visible = gizmo_ui && g_app->show_tex_panel && castlemist::texpanel::row_count(g_app->hwnd_tex_info) > 0;
    ShowWindow(g_app->hwnd_tex_info, tex_visible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_split_info_tex, tex_visible ? SW_SHOW : SW_HIDE);
    if (tex_visible) {
        int avail = std::max(0, usable_h - kSplitterThickness);
        int tex_h = std::clamp(static_cast<int>(avail * g_app->tex_panel_ratio), std::min(avail, 80),
                               std::max(0, avail - 60));
        int info_h = std::max(0, avail - tex_h);
        MoveWindow(g_app->hwnd_info, x, 0, right_w, info_h, TRUE);
        MoveWindow(g_app->hwnd_split_info_tex, x, info_h, right_w, kSplitterThickness, TRUE);
        MoveWindow(g_app->hwnd_tex_info, x, info_h + kSplitterThickness, right_w, tex_h, TRUE);
    } else {
        MoveWindow(g_app->hwnd_info, x, 0, right_w, usable_h, TRUE);
    }

    // Bottom status strip: label on the left, progress bar on the right.
    const int status_y = usable_h;
    const int progress_w = 200;
    const int inner_h = kStatusBarHeight - 6;
    MoveWindow(g_app->hwnd_progress, std::max(0, client_w - progress_w - 8), status_y + 3, progress_w, inner_h, TRUE);
    MoveWindow(g_app->hwnd_status_label, 8, status_y + 3, std::max(0, client_w - progress_w - 24), inner_h, TRUE);
}

void relayout() {
    if (g_app == nullptr) {
        return;
    }
    RECT rc;
    GetClientRect(g_app->hwnd_main, &rc);
    layout_children(rc.right - rc.left, rc.bottom - rc.top);
}


} // namespace castlemist::ui
