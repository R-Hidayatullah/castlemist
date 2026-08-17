/// @file
/// @brief Applying an extracted entry to the preview surfaces.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <cstdio>
#include <thread>

#include "castlemist/format/strs_keys.h"
#include "castlemist/format/strs_view.h"
#include "castlemist/render/gw2bgfx_view.h"

namespace castlemist::ui {

void update_preview_texture() {
    if (!g_app->current_entry.is_image || g_app->current_entry.preview_pixels.empty()) {
        castlemist::gfx::clear_texture();
        return;
    }

    castlemist::gfx::set_texture(g_app->current_entry.preview_dxgi_format, g_app->current_entry.preview_width,
                         g_app->current_entry.preview_height, g_app->current_entry.preview_pitch,
                         g_app->current_entry.preview_pixels.data());
    g_app->preview_zoom = 1.0f;
    g_app->preview_pan_x = 0.0f;
    g_app->preview_pan_y = 0.0f;
    g_app->preview_rotation_quarters = 0;
}

// Fills the "Chunk:" combo from the CURRENT entry's own already-parsed tree
// (g_app->struct_root, set at the end of populate_struct_tree() below) --
// i.e. exactly the chunks this specific packfile actually contains, not the
// full universe of container fourccs the loaded template knows about. Each
// row is one of root's immediate children as BinaryParser::parseGw2Packfile
// built them: the "Header" node, then one "FOURCC  (vNN)" node per real
// chunk, in file order, with a combo-box index -> ParsedNode::offset mapping
// stashed via CB_SETITEMDATA so on_struct_container_changed() can hand that
// offset straight to struct_tree::select_chunk_by_offset() with no re-parse
// and no string round-trip. Item 0 is always "(select a chunk)" and carries
// no node -- it just means "nothing to jump to yet".
void populate_struct_container_combo() {
    if (g_app == nullptr || g_app->hwnd_struct_container_combo == nullptr) {
        return;
    }
    HWND combo = g_app->hwnd_struct_container_combo;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(select a chunk)"));
    SendMessageW(combo, CB_SETITEMDATA, 0, static_cast<LPARAM>(-1));

    if (!g_app->struct_root) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        return;
    }

    // Each row shows "0xOFFSET  FOURCC (vNN)  (ResolvedTypeName)" so the raw
    // chunk id (its byte offset in the entry, matching what the hex view and
    // the tree's own "@0x..." suffix show), its fourcc+version, and its
    // resolved schema name are all visible without opening the tree first.
    // ("Header" is the one row with a placeholder typeName of "PF" rather
    // than a chunk schema -- it's the PF header entry, not a real chunk.)
    for (const auto& child : g_app->struct_root->children) {
        if (!child) continue;
        wchar_t buf[512];
        // name/typeName are ArenaNet fourcc/type identifiers -- always ASCII
        // (see the same assumption in struct_tree.cpp's format_label).
        std::wstring name = castlemist::core::from_ascii(child->name);
        std::wstring type = castlemist::core::from_ascii(child->typeName);
        swprintf(buf, 512, L"0x%zX  %ls  (%ls)", child->offset, name.c_str(), type.c_str());
        int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf)));
        if (idx >= 0) {
            SendMessageW(combo, CB_SETITEMDATA, idx, static_cast<LPARAM>(child->offset));
        }
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

// The combo's CBN_SELCHANGE handler: jump the Structure tree straight to the
// chosen chunk. No re-parse -- the tree already holds every chunk this entry
// has, this just scrolls to and selects the matching node (see
// struct_tree::select_chunk_by_offset).
void on_struct_container_changed() {
    if (g_app == nullptr || g_app->hwnd_struct_container_combo == nullptr) {
        return;
    }
    int sel = static_cast<int>(SendMessageW(g_app->hwnd_struct_container_combo, CB_GETCURSEL, 0, 0));
    if (sel <= 0) {
        return;
    }
    LRESULT data = SendMessageW(g_app->hwnd_struct_container_combo, CB_GETITEMDATA, sel, 0);
    if (data == CB_ERR || data < 0) {
        return;
    }
    castlemist::structtree::select_chunk_by_offset(g_app->hwnd_struct_tree, static_cast<size_t>(data));
}

// Walks current_entry.decompressed against the loaded JSON struct template
// and feeds the resulting ParsedNode tree to the "Structure" tab. Mirrors the
// same tpl-or-nothing pattern build_model_preview and build_map_scene already
// use: the app works with no template loaded, the tree just explains why it's
// empty instead of showing one.
//
// This does real work -- the struct template can be a multi-megabyte JSON
// file, and the binary walk itself isn't free either -- so it is deliberately
// NOT called unconditionally from apply_extracted_entry() any more. Only
// populate_struct_tree_if_visible() calls it, and only when the Structure tab
// is actually the one on screen; see that function and its callers
// (relayout()'s TCN_SELCHANGE path, and here once per fresh selection).
void populate_struct_tree() {
    if (g_app == nullptr) {
        return;
    }
    g_app->struct_tree_dirty = false;
    if (g_app->current_entry.decompressed.empty()) {
        castlemist::structtree::clear(g_app->hwnd_struct_tree);
        g_app->struct_root.reset();
        populate_struct_container_combo();
        return;
    }

    // Lazy: parses gw2_packfile.json on this first real need, not at startup.
    auto tpl = castlemist::tpl::get_or_auto_load();
    if (!tpl) {
        castlemist::structtree::set_message(g_app->hwnd_struct_tree,
            L"No struct template is loaded.\r\n"
            L"Use File -> Load Struct JSON... (gw2_packfile.json) to enable the parsed structure tree.");
        g_app->struct_root.reset();
        populate_struct_container_combo();
        return;
    }

    BinaryParser parser;
    ParsedNodePtr root;
    std::string error;
    bool ok = parser.parse(g_app->current_entry.decompressed, *tpl, root, error);
    if (!ok || !root) {
        std::wstring werror(error.begin(), error.end());
        castlemist::structtree::set_message(g_app->hwnd_struct_tree,
            L"Could not parse this entry against the loaded template:\r\n" + werror);
        g_app->struct_root.reset();
        populate_struct_container_combo();
        return;
    }

    castlemist::structtree::set_tree(g_app->hwnd_struct_tree, root);

    // The "Chunk:" combo is a navigator over THIS entry's own parsed tree, so
    // it's rebuilt on every fresh parse (new entry selected, or the template
    // got reloaded and changed what's here) -- not just once like the old
    // template-wide fileTypes list used to be.
    g_app->struct_root = root;
    populate_struct_container_combo();
}

void populate_struct_tree_if_visible() {
    if (g_app == nullptr || g_app->hwnd_tab == nullptr || g_app->hwnd_struct_tree == nullptr) {
        return;
    }
    if (!g_app->struct_tree_dirty) {
        return; // already up to date for the current entry -- nothing to do
    }
    int sel = TabCtrl_GetCurSel(g_app->hwnd_tab);
    if (sel != static_cast<int>(MiddleTab::Structure)) {
        return; // Structure tab isn't showing -- defer the parse until it is
    }
    populate_struct_tree();
}

// Applies a (freshly extracted or failed) entry to the UI. Called only from
// the main thread: either synchronously right after a same-thread extract
// (there isn't one anymore, but kept generic) or from the WM_APP_EXTRACT_DONE
// handler once a background result's generation is confirmed current.
void apply_extracted_entry(uint32_t mft_index, ExtractedEntry&& entry) {
    if (g_app == nullptr) {
        return;
    }

    castlemist::snd::stop(); // stop any audio from the previously selected entry
    KillTimer(g_app->hwnd_main, TIMER_AUDIO);
    g_app->audio_seek_dragging = false;
    g_app->audio_sel_info = castlemist::snd::ClipInfo{}; // drop the previous clip's duration
    // MUST happen before current_entry is replaced: the Bink player reads the
    // movie in place out of the old entry's `decompressed` buffer.
    stop_video();

    g_app->current_entry = std::move(entry);
    g_app->has_loaded_entry = true;

    castlemist::hex::set_data(g_app->hwnd_hex_before, g_app->current_entry.compressed.data(),
                      g_app->current_entry.compressed.size());
    castlemist::hex::set_data(g_app->hwnd_hex_after, g_app->current_entry.decompressed.data(),
                      g_app->current_entry.decompressed.size());
    // Lazy: mark the struct tree stale for this entry, but only actually parse
    // it (template load + binary walk) if the Structure tab happens to be the
    // one showing right now. If the user is looking at another tab, the parse
    // is deferred until they click into Structure -- see TCN_SELCHANGE in
    // window_proc.cpp / relayout().
    g_app->struct_tree_dirty = true;
    populate_struct_tree_if_visible();

    // One choke point for the texture panel: every branch below either leaves it
    // empty or refills it, and clearing here also releases the previous model's
    // thumbnail bitmaps at the same moment its pixels go away.
    castlemist::texpanel::set_model(g_app->hwnd_tex_info, nullptr);

    // Route the decoded payload to whichever preview surface fits its type.
    switch (g_app->current_entry.kind) {
    case PreviewKind::Image:
        castlemist::render::clear_model();
        update_preview_texture();
        break;
    case PreviewKind::Model:
        castlemist::gfx::clear_texture();
        // A different entry is on screen now, so the "Game 1:1" surface has to
        // reload. Deferred to its next paint rather than done here: the view is
        // usually hidden, and loading a model it is not showing costs a full
        // archive read and a pile of GPU uploads for nothing.
        g_app->bgfx_model_loaded = false;
        if (g_app->current_entry.model) {
            castlemist::render::set_model(*g_app->current_entry.model);
            // A standalone skin/model has no env of its own -> light it with the
            // baked GW2-daylight rig (clears any rig left over from a map view).
            castlemist::render::clear_environment_rig();
            castlemist::render::set_mode(g_app->model_mode);
            // Restore the skeleton toggle; disable + uncheck it for rig-less models.
            bool has_skel = castlemist::render::has_skeleton();
            EnableWindow(g_app->hwnd_skel_toggle, has_skel);
            bool on = has_skel && g_app->show_skeleton;
            SendMessageW(g_app->hwnd_skel_toggle, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            castlemist::render::set_show_skeleton(on);
            // Per-submesh texture panel (bottom right). Built from the ModelPreview
            // rather than the renderer because the decoded RGBA, the material
            // bindings and the submesh list all live here -- the GPU side only ever
            // sees the two textures it binds. The toggle persists across models, but
            // a model whose textures didn't resolve has nothing to show.
            castlemist::texpanel::set_model(g_app->hwnd_tex_info, g_app->current_entry.model.get());
            bool texOn = castlemist::texpanel::row_count(g_app->hwnd_tex_info) > 0 && g_app->show_tex_panel;
            SendMessageW(g_app->hwnd_tex_panel, BM_SETCHECK, texOn ? BST_CHECKED : BST_UNCHECKED, 0);

            // Populate the animation clip selector (item 0 = bind pose).
            KillTimer(g_app->hwnd_main, TIMER_ANIM);
            SendMessageW(g_app->hwnd_anim_play, BM_SETCHECK, BST_UNCHECKED, 0);
            castlemist::render::set_playing(false);
            // Keep the "Game 1:1" surface in step: a new entry must not inherit
            // the previous model's playback state on either view.
            castlemist::gw2bgfxview::set_playing(false);
            castlemist::gw2bgfxview::set_animation(-1);
            SendMessageW(g_app->hwnd_anim_combo, CB_RESETCONTENT, 0, 0);
            int nclips = castlemist::render::animation_count();
            if (castlemist::render::has_skeleton()) {
                // Item 0 = bind pose, 1..nclips = embedded clips.
                SendMessageW(g_app->hwnd_anim_combo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(L"Bind pose (rest)"));
                for (int i = 0; i < nclips; ++i) {
                    wchar_t item[160];
                    const char* nm = castlemist::render::animation_name(i);
                    wchar_t wnm[96];
                    MultiByteToWideChar(CP_UTF8, 0, (nm && *nm) ? nm : "clip", -1, wnm, 96);
                    swprintf(item, 160, L"%ls  (%.2fs)", wnm, castlemist::render::animation_duration(i));
                    SendMessageW(g_app->hwnd_anim_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
                }
                SendMessageW(g_app->hwnd_anim_combo, CB_SETCURSEL, 0, 0);
                castlemist::render::set_animation(-1);
            }
            populate_lod_controls();
            update_gizmo_readout();
            // Baked particle/light effects run on their own spawn timers, so keep
            // repainting the model surface even when no animation is playing --
            // that's what makes an idle weapon's fire keep blazing.
            if (castlemist::render::has_effects() && castlemist::render::show_effects())
                SetTimer(g_app->hwnd_main, TIMER_ANIM, 16, nullptr);
        } else {
            castlemist::render::clear_model();
            SetWindowTextW(g_app->hwnd_text_preview,
                           L"This is a .modl model, but no struct template is loaded.\r\n"
                           L"Use File -> Load Struct JSON... (gw2_packfile.json) to enable model preview.");
        }
        break;
    case PreviewKind::Map:
        castlemist::gfx::clear_texture();
        if (g_app->current_entry.map && !g_app->current_entry.map->instances.empty()) {
            const MapScene& ms = *g_app->current_entry.map;
            std::vector<castlemist::render::SceneInstance> insts;
            insts.reserve(ms.instances.size());
            for (const MapInstance& mi : ms.instances) {
                castlemist::render::SceneInstance si;
                si.model = mi.model;
                for (int k = 0; k < 3; ++k) { si.pos[k] = mi.pos[k]; si.rot[k] = mi.rot[k]; }
                si.scale = mi.scale;
                si.layer = mi.layer;
                insts.push_back(si);
            }
            castlemist::render::set_scene(ms.models, insts);
            // Light the scene with the map's own env chunk (real per-map sun + SH
            // ambient) when present; otherwise fall back to the baked daylight rig.
            castlemist::render::set_environment_rig(ms.env);
            castlemist::render::set_mode(g_app->model_mode);
            // Default layers: props + terrain on, zones + collision off.
            g_app->map_zone_loaded = false;
            castlemist::render::set_layer_visible(castlemist::render::LAYER_PROP, true);
            castlemist::render::set_layer_visible(castlemist::render::LAYER_TERRAIN, true);
            castlemist::render::set_layer_visible(castlemist::render::LAYER_ZONE, false);
            castlemist::render::set_layer_visible(castlemist::render::LAYER_COLLISION, false);
            SendMessageW(g_app->hwnd_layer_prop, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(g_app->hwnd_layer_zone, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(g_app->hwnd_layer_coll, BM_SETCHECK, BST_UNCHECKED, 0);
        } else {
            castlemist::render::clear_model();
            SetWindowTextW(g_app->hwnd_text_preview,
                           L"This is a map (mapc/area), but its prop scene could not be built.\r\n"
                           L"Load the struct template (gw2_packfile.json) to enable map preview.");
        }
        break;
    case PreviewKind::Text:
        castlemist::gfx::clear_texture();
        castlemist::render::clear_model();
        SetWindowTextW(g_app->hwnd_text_preview, g_app->current_entry.text_preview.c_str());
        break;
    case PreviewKind::Strs: {
        castlemist::gfx::clear_texture();
        castlemist::render::clear_model();
        // Tabular preview: one row per string record (#, textId, encoding, bytes,
        // text). The file's baseTextId comes from the txtm-derived textbase map, so
        // rows carry real global textIds and packed records can be RC4-decrypted.
        std::vector<uint32_t> fids = get_by_file_id(g_app->data_gw2, mft_index + 1);
        long long base = castlemist::skeys::base_for_fileids(fids);
        populate_strs_table(g_app->current_entry.decompressed, base);
        break;
    }
    case PreviewKind::Audio: {
        castlemist::gfx::clear_texture();
        castlemist::render::clear_model();
        const auto& clips = g_app->current_entry.audio_clips;
        SendMessageW(g_app->hwnd_audio_combo, CB_RESETCONTENT, 0, 0);
        if (clips.size() > 1) {
            for (size_t i = 0; i < clips.size(); ++i) {
                wchar_t item[64];
                swprintf(item, 64, L"Sound %zu / %zu", i + 1, clips.size());
                SendMessageW(g_app->hwnd_audio_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            }
            SendMessageW(g_app->hwnd_audio_combo, CB_SETCURSEL, 0, 0);
        }
        update_audio_info(0);
        update_audio_seek_ui(true);
        break;
    }
    case PreviewKind::Video: {
        castlemist::render::clear_model();
        castlemist::gfx::clear_texture();
        // The stream plays directly out of current_entry.decompressed -- no copy,
        // no temp file, which matters when a cinematic is ~100 MB.
        std::wstring err = start_video(g_app->current_entry);
        SetWindowTextW(g_app->hwnd_text_preview, video_info_text(g_app->current_entry.video, err).c_str());
        break;
    }
    case PreviewKind::Cinematic: {
        castlemist::render::clear_model();
        castlemist::gfx::clear_texture();
        // The CINP already handed us the dialogue; only the movie has to be
        // fetched. Subtitles are therefore ON from the start here -- unlike the
        // bare-.bik path, nothing has to be searched for.
        g_app->video_subs = g_app->current_entry.subtitles;
        g_app->video_subs_searched = true;
        g_app->video_subs_cinp = mft_index + 1;
        g_app->video_subs_on = !g_app->video_subs.empty();
        SendMessageW(g_app->hwnd_video_subs, BM_SETCHECK,
                     g_app->video_subs_on ? BST_CHECKED : BST_UNCHECKED, 0);
        populate_video_clips();
        std::wstring err = g_app->current_entry.cinp_video_ids.empty()
                               ? L"this cinematic references no movie"
                               : start_cinp_video(0);
        SetWindowTextW(g_app->hwnd_text_preview,
                       cinematic_info_text(g_app->current_entry, err).c_str());
        break;
    }
    case PreviewKind::Content: {
        castlemist::gfx::clear_texture();
        castlemist::render::clear_model();
        g_app->content_sub = ExtractedEntry{};
        g_app->content_sub_loaded = false;
        // Master = content types; child (entries) + detail (preview) fill on click.
        populate_content_master();
        SetWindowTextW(g_app->hwnd_text_preview, g_app->current_entry.text_preview.c_str());
        break;
    }
    default:
        castlemist::gfx::clear_texture();
        castlemist::render::clear_model();
        SetWindowTextW(g_app->hwnd_text_preview, L"(no preview available for this entry)");
        break;
    }

    castlemist::info::show_entry_info(g_app->hwnd_info, g_app->data_gw2, mft_index, g_app->current_entry);
    set_export_enabled(true);
    relayout(); // swap in the surface that matches this entry's type
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

void on_entry_selected(uint32_t mft_index) {
    if (g_app == nullptr || mft_index >= g_app->data_gw2.mft_data_list.size()) {
        return;
    }

    // A fresh mouse click on a new row emits both LVN_ITEMCHANGED and NM_CLICK
    // for it; skip the duplicate while that exact row's extraction is still in
    // flight (has_loaded_entry flips true only once it completes). Re-clicking a
    // row that has already finished loading still reloads it, which is the point
    // of also listening to NM_CLICK/NM_DBLCLK.
    if (g_app->request_generation != 0 && mft_index == g_app->current_mft_index && !g_app->has_loaded_entry) {
        return;
    }

    uint64_t generation = ++g_app->request_generation;
    g_app->current_mft_index = mft_index;

    // Immediately wipe the previous entry's data from all three content panels
    // so nothing stale lingers while the new entry decodes in the background;
    // the WM_APP_EXTRACT_DONE handler refills them once it's ready.
    stop_video(); // the Bink stream reads out of the buffer we're about to free
    g_app->current_entry = ExtractedEntry{};
    g_app->has_loaded_entry = false;
    set_export_enabled(false);
    castlemist::hex::set_data(g_app->hwnd_hex_before, nullptr, 0);
    castlemist::hex::set_data(g_app->hwnd_hex_after, nullptr, 0);
    castlemist::structtree::clear(g_app->hwnd_struct_tree);
    g_app->struct_tree_dirty = false; // just cleared it -- nothing stale to lazily repopulate yet
    castlemist::gfx::clear_texture();
    castlemist::render::clear_model();
    castlemist::texpanel::set_model(g_app->hwnd_tex_info, nullptr);
    SetWindowTextW(g_app->hwnd_text_preview, L"");
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
    show_loading(true, mft_index);

    std::string file_path = g_app->data_gw2.file_info.file_path;
    MftData entry_copy = g_app->data_gw2.mft_data_list[mft_index];
    HWND hwnd_main = g_app->hwnd_main;

    // Fully self-contained: touches no shared state (own file handle inside
    // extract_entry, copies of everything else), so it's safe to just detach
    // and let stale results be discarded by generation number when they
    // arrive -- no thread ever needs to be waited on or interrupted.
    std::thread([hwnd_main, generation, mft_index, file_path = std::move(file_path), entry_copy]() {
        auto result = std::make_unique<ExtractResult>();
        result->generation = generation;
        result->mft_index = mft_index;
        try {
            // Indexed overload: lets decompress_raw_entry() consult the
            // gw2index DB (base_id = mft_index + 1) for this entry's real
            // type/container/chunks before falling back to byte-sniffing.
            result->entry = extract_entry_indexed(file_path, entry_copy, mft_index);
            result->success = true;
        } catch (const std::exception& e) {
            result->success = false;
            result->error_message = e.what();
        }
        PostMessageW(hwnd_main, WM_APP_EXTRACT_DONE, static_cast<WPARAM>(generation),
                     reinterpret_cast<LPARAM>(result.release()));
    }).detach();
}

// Loads a .dat by path and rebinds the whole UI to it. Reusable by File->Open
// and by the index-DB opener (which auto-loads the archive the index references).

} // namespace castlemist::ui
