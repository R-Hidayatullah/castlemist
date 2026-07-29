/// @file
/// @brief The cntc content browser: types -> entries -> assets -> preview.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <cstdio>

namespace castlemist::ui {

void render_content_sub() {
    ExtractedEntry& e = g_app->content_sub;
    castlemist::gfx::clear_texture();
    castlemist::render::clear_model();
    castlemist::texpanel::set_model(g_app->hwnd_tex_info, nullptr);
    if (e.is_image && !e.preview_pixels.empty()) {
        castlemist::gfx::set_texture(e.preview_dxgi_format, e.preview_width, e.preview_height,
                            e.preview_pitch, e.preview_pixels.data());
    } else if (e.kind == PreviewKind::Model && e.model) {
        castlemist::render::set_model(*e.model);
        castlemist::render::set_mode(castlemist::render::RenderMode::Full);
        castlemist::render::set_show_skeleton(false);
        castlemist::texpanel::set_model(g_app->hwnd_tex_info, e.model.get());
        SendMessageW(g_app->hwnd_tex_panel, BM_SETCHECK,
                     (g_app->show_tex_panel && castlemist::texpanel::row_count(g_app->hwnd_tex_info) > 0)
                         ? BST_CHECKED : BST_UNCHECKED, 0);
        populate_lod_controls();
        update_gizmo_readout();
    } else if (e.kind == PreviewKind::Audio && !e.audio_clips.empty()) {
        SendMessageW(g_app->hwnd_audio_combo, CB_RESETCONTENT, 0, 0);
        if (e.audio_clips.size() > 1) {
            for (size_t i = 0; i < e.audio_clips.size(); ++i) {
                wchar_t it[48]; swprintf(it, 48, L"Sound %zu / %zu", i + 1, e.audio_clips.size());
                SendMessageW(g_app->hwnd_audio_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it));
            }
            SendMessageW(g_app->hwnd_audio_combo, CB_SETCURSEL, 0, 0);
        }
        update_audio_info(0);
        update_audio_seek_ui(true);
    } else {
        SetWindowTextW(g_app->hwnd_text_preview,
                       e.text_preview.empty() ? L"(no visual/audio preview for this asset type)"
                                              : e.text_preview.c_str());
    }
}

// Short human kind for a cntc asset fileId (MODL / texture / ASND / ...), via the
// loaded index when available (cheap; only called for the few assets of one object).
std::wstring content_asset_kind(uint32_t fid) {
    uint32_t base = get_by_base_id(g_app->data_gw2, fid);
    if (!base) return L"(missing)";
    if (g_app->index_loaded) {
        castlemist::db::EntryInfo e = castlemist::db::lookup(base);
        std::string k = !e.container.empty() ? e.container : e.type;
        if (!k.empty()) return std::wstring(k.begin(), k.end());
    }
    return L"asset";
}

// DETAIL: load one asset fileId and show it in the right-hand preview surface.
void load_content_asset_fid(uint32_t fid) {
    castlemist::snd::stop();
    KillTimer(g_app->hwnd_main, TIMER_AUDIO);
    g_app->audio_seek_dragging = false;
    g_app->audio_sel_info = castlemist::snd::ClipInfo{};
    g_app->content_sub = ExtractedEntry{};
    g_app->content_sub_loaded = false;
    uint32_t base = get_by_base_id(g_app->data_gw2, fid);
    if (base == 0 || base - 1 >= g_app->data_gw2.mft_data_list.size()) {
        SetWindowTextW(g_app->hwnd_text_preview, L"(referenced asset is not present in this dat build)");
    } else {
        try {
            g_app->content_sub = extract_entry(g_app->data_gw2, base - 1);
            g_app->content_sub_loaded = true;
            render_content_sub();
        } catch (const std::exception&) {
            SetWindowTextW(g_app->hwnd_text_preview, L"(failed to load this asset)");
        }
    }
    relayout();
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

// MASTER: fill the type list -- one row per distinct content type in the cntc,
// with the number of entries (objects) of that type. content_objects is already
// sorted by (type, id), so equal types are contiguous. Row i <-> content_types[i].
void populate_content_master() {
    const auto& objs = g_app->current_entry.content_objects;
    g_app->content_types.clear();
    for (const auto& o : objs) {
        if (!g_app->content_types.empty() && g_app->content_types.back().first == o.type)
            g_app->content_types.back().second += 1;
        else
            g_app->content_types.push_back({o.type, 1});
    }
    HWND lb = g_app->hwnd_content_list;
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lb);
    for (int i = 0; i < static_cast<int>(g_app->content_types.size()); ++i) {
        const auto& t = g_app->content_types[i];
        const char* tn = content_type_name(t.first);
        wchar_t name[72], cnt[24];
        if (tn)
            swprintf(name, 72, L"%hs", tn);
        else
            swprintf(name, 72, L"Type %u", t.first);
        swprintf(cnt, 24, L"%u", t.second);
        lv_add_row(lb, i, name, cnt); // lParam = index into content_types
    }
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    ListView_DeleteAllItems(g_app->hwnd_content_child);
    ListView_DeleteAllItems(g_app->hwnd_content_asset_list);
    g_app->content_child_objidx.clear();
    g_app->content_type_sel = -1;
    g_app->content_obj_sel = -1;
    g_app->content_sort_col[0] = g_app->content_sort_col[1] = g_app->content_sort_col[2] = -1;
}

// CHILD: a type was picked in the master list -> fill the entries table with the
// entries (objects) of that type. Clears the preview until an entry is clicked.
// idx = index into content_types (carried in the clicked row's lParam).
void on_content_master_select(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_app->content_types.size())) return;
    g_app->content_type_sel = idx;
    const uint32_t type = g_app->content_types[idx].first;
    const auto& objs = g_app->current_entry.content_objects;
    HWND cb = g_app->hwnd_content_child;
    SendMessageW(cb, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(cb);
    g_app->content_child_objidx.clear();
    for (int i = 0; i < static_cast<int>(objs.size()); ++i) {
        if (objs[i].type != type) continue;
        g_app->content_child_objidx.push_back(i);
        wchar_t idw[24], naw[24];
        swprintf(idw, 24, L"%u", objs[i].id);
        swprintf(naw, 24, L"%zu", objs[i].assets.size());
        // Id slug + the readable labels, so the row says what the object IS instead
        // of being a bare number. (The slug is the identity; labels are the context.)
        std::wstring cn(objs[i].name.begin(), objs[i].name.end());
        std::string joined;
        for (const auto& l : objs[i].labels) {
            if (!joined.empty()) joined += ", ";
            joined += l;
            if (joined.size() > 120) break;
        }
        std::wstring lw(joined.begin(), joined.end());
        lv_add_row(cb, i, idw, naw, cn.c_str(), lw.c_str()); // lParam = content_objects idx
    }
    SendMessageW(cb, WM_SETREDRAW, TRUE, 0);
    ListView_DeleteAllItems(g_app->hwnd_content_asset_list);
    g_app->content_sort_col[1] = g_app->content_sort_col[2] = -1;
    castlemist::snd::stop();
    castlemist::gfx::clear_texture();
    castlemist::render::clear_model();
    g_app->content_sub = ExtractedEntry{};
    g_app->content_sub_loaded = false;
    g_app->content_obj_sel = -1;
    const char* tn = content_type_name(type);
    const size_t ne = g_app->content_child_objidx.size();
    wchar_t hdr[128];
    if (tn)
        swprintf(hdr, 128, L"%hs — %zu entr%ls\r\n\r\nClick an entry to preview its asset.", tn, ne,
                 ne == 1 ? L"y" : L"ies");
    else
        swprintf(hdr, 128, L"Type %u — %zu entr%ls\r\n\r\nClick an entry to preview its asset.", type, ne,
                 ne == 1 ? L"y" : L"ies");
    SetWindowTextW(g_app->hwnd_text_preview, hdr);
    relayout();
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

// The asset fileIds of the entry currently selected in the entries table, or empty.
const std::vector<uint32_t>& selected_entry_assets() {
    static const std::vector<uint32_t> kEmpty;
    const auto& objs = g_app->current_entry.content_objects;
    int oi = g_app->content_obj_sel;
    if (oi < 0 || oi >= static_cast<int>(objs.size())) return kEmpty;
    return objs[oi].assets;
}

// DETAIL trigger: an entry (object) was picked -> fill the asset table and preview
// its first asset. oi = index into content_objects (the entry row's lParam).
// Selecting a different asset row re-renders that asset (see on_content_asset_select).
void on_content_child_select(int oi) {
    const auto& objs = g_app->current_entry.content_objects;
    if (oi < 0 || oi >= static_cast<int>(objs.size())) return;
    g_app->content_obj_sel = oi;
    const auto& assets = objs[oi].assets;
    // Fill the asset table: one row per asset (index + fileId + classified kind).
    HWND ac = g_app->hwnd_content_asset_list;
    SendMessageW(ac, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(ac);
    for (int i = 0; i < static_cast<int>(assets.size()); ++i) {
        wchar_t num[16], fid[24];
        swprintf(num, 16, L"%d", i + 1);
        swprintf(fid, 24, L"%u", assets[i]);
        lv_add_row(ac, i, num, fid, content_asset_kind(assets[i]).c_str()); // lParam = asset index
    }
    SendMessageW(ac, WM_SETREDRAW, TRUE, 0);
    g_app->content_sort_col[2] = -1;
    if (assets.empty()) {
        castlemist::snd::stop();
        castlemist::gfx::clear_texture();
        castlemist::render::clear_model();
        g_app->content_sub = ExtractedEntry{};
        g_app->content_sub_loaded = false;
        // No previewable asset, but the object is still identifiable -- show what it is.
        const char* tn = content_type_name(objs[oi].type);
        std::wstring cn(objs[oi].name.begin(), objs[oi].name.end());
        wchar_t buf[1024];
        swprintf(buf, 1024,
                 L"Content object\r\n────────────────────────────────\r\n"
                 L"Type      %hs (%u)\r\n"
                 L"Id        %u\r\n"
                 L"Slug      %ls\r\n",
                 tn ? tn : "Type", objs[oi].type, objs[oi].id,
                 cn.empty() ? L"(none)" : cn.c_str());
        std::wstring info(buf);
        if (!objs[oi].labels.empty()) {
            info += L"\r\nString fields (shared parameter / condition names):\r\n";
            for (const auto& l : objs[oi].labels) {
                info += L"  \x2022 ";
                info += std::wstring(l.begin(), l.end());
                info += L"\r\n";
            }
        }
        info += L"────────────────────────────────\r\n"
                L"No dat assets referenced, so there is nothing to render.\r\n"
                L"The slug is ArenaNet's internal identifier from the cntc string table\r\n"
                L"(not a localized display name); the labels above are the readable\r\n"
                L"strings this object shares with others of its kind.";
        SetWindowTextW(g_app->hwnd_text_preview, info.c_str());
        relayout();
        return;
    }
    // Select the first asset row; its LVN_ITEMCHANGED drives the actual load
    // (single code path, same as the user clicking a different asset).
    ListView_SetItemState(ac, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

// The selected asset row changed -> render the chosen asset of the current entry.
void on_content_asset_select() {
    const auto& assets = selected_entry_assets();
    int sel = static_cast<int>(lv_selected_param(g_app->hwnd_content_asset_list));
    if (sel < 0 || sel >= static_cast<int>(assets.size())) return;
    load_content_asset_fid(assets[sel]);
}

// Sort context handed to the ListView comparator: which of the 3 backing tables,
// which column, and direction. Each content row carries its backing-vector index
// in lParam, so sorting the visible rows never loses the type/entry/asset mapping.
struct ContentSortCtx { int list; int col; bool asc; };

std::wstring content_type_label(uint32_t type) {
    const char* tn = content_type_name(type);
    wchar_t b[72];
    if (tn) swprintf(b, 72, L"%hs", tn); else swprintf(b, 72, L"Type %u", type);
    return b;
}

int CALLBACK content_lv_compare(LPARAM l1, LPARAM l2, LPARAM lpctx) {
    auto* c = reinterpret_cast<ContentSortCtx*>(lpctx);
    long long r = 0;
    if (c->list == 0) { // Types: col0 = name, col1 = count
        const auto& A = g_app->content_types[l1];
        const auto& B = g_app->content_types[l2];
        if (c->col == 1) r = static_cast<long long>(A.second) - static_cast<long long>(B.second);
        else r = _wcsicmp(content_type_label(A.first).c_str(), content_type_label(B.first).c_str());
    } else if (c->list == 1) { // Entries: col0 = id, col1 = asset count
        const auto& objs = g_app->current_entry.content_objects;
        if (l1 >= static_cast<LPARAM>(objs.size()) || l2 >= static_cast<LPARAM>(objs.size())) return 0;
        const auto& A = objs[l1];
        const auto& B = objs[l2];
        if (c->col == 1) r = static_cast<long long>(A.assets.size()) - static_cast<long long>(B.assets.size());
        else r = static_cast<long long>(A.id) - static_cast<long long>(B.id);
    } else { // Assets: col0 = index, col1 = fileId, col2 = kind
        const auto& assets = selected_entry_assets();
        if (l1 >= static_cast<LPARAM>(assets.size()) || l2 >= static_cast<LPARAM>(assets.size())) return 0;
        uint32_t fa = assets[l1], fb = assets[l2];
        if (c->col == 1) r = static_cast<long long>(fa) - static_cast<long long>(fb);
        else if (c->col == 2) r = _wcsicmp(content_asset_kind(fa).c_str(), content_asset_kind(fb).c_str());
        else r = static_cast<long long>(l1) - static_cast<long long>(l2);
    }
    if (r == 0) r = static_cast<long long>(l1) - static_cast<long long>(l2);
    int s = (r < 0) ? -1 : (r > 0) ? 1 : 0;
    return c->asc ? s : -s;
}

// A column header was clicked -> (re)sort that table, toggling direction when the
// same column is clicked again.
void content_sort_click(int list, HWND lv, int col) {
    if (list < 0 || list > 2 || col < 0) return;
    if (g_app->content_sort_col[list] == col)
        g_app->content_sort_asc[list] = !g_app->content_sort_asc[list];
    else {
        g_app->content_sort_col[list] = col;
        g_app->content_sort_asc[list] = true;
    }
    ContentSortCtx ctx{list, col, g_app->content_sort_asc[list]};
    ListView_SortItems(lv, content_lv_compare, reinterpret_cast<LPARAM>(&ctx));
}


} // namespace castlemist::ui
