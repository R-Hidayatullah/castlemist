/// @file
/// @brief File commands: open, export, load template/keys, search and filters.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <commdlg.h>
#include <fstream>
#include <thread>

#include "castlemist/format/struct_template.h"
#include "castlemist/format/strs_keys.h"

namespace castlemist::ui {

bool load_dat_path(HWND hwnd, const wchar_t* path) {
    HCURSOR old_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = false;
    try {
        Gw2Dat fresh;
        load_dat_file(fresh, castlemist::core::to_ansi(path));

        // Invalidate any extraction still in flight from the previous archive.
        ++g_app->request_generation;
        show_loading(false, 0);

        stop_video();
        g_app->data_gw2 = std::move(fresh);
        g_app->current_entry = ExtractedEntry{};
        g_app->has_loaded_entry = false;
        g_app->dat_loaded = true;

        castlemist::mft::set_source(g_app->hwnd_list, g_app->data_gw2);
        castlemist::hex::set_data(g_app->hwnd_hex_before, nullptr, 0);
        castlemist::hex::set_data(g_app->hwnd_hex_after, nullptr, 0);
        castlemist::gfx::clear_texture();
        castlemist::render::clear_model();
        castlemist::texpanel::set_model(g_app->hwnd_tex_info, nullptr);
        SetWindowTextW(g_app->hwnd_text_preview, L"");
        set_export_enabled(false);
        castlemist::info::show_dat_info(g_app->hwnd_info, g_app->data_gw2);
        SetWindowTextW(g_app->hwnd_search_edit, L"");
        InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);

        wchar_t title[512];
        swprintf(title, 512, L"castlemist - %ls (%zu assets)", path, g_app->data_gw2.mft_base_id_data_list.size());
        SetWindowTextW(hwnd, title);
        ok = true;
    } catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "Failed to load .dat", MB_ICONERROR);
    }
    SetCursor(old_cursor);
    return ok;
}

void do_open_file(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Guild Wars 2 Archive (*.dat)\0*.dat\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    load_dat_path(hwnd, path);
}

void do_load_template(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Struct template (*.json)\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }

    std::string error;
    if (!castlemist::tpl::load_from_file(castlemist::core::to_ansi(path), error)) {
        MessageBoxA(hwnd, error.c_str(), "Failed to load struct JSON", MB_ICONERROR);
        return;
    }
    SetWindowTextW(g_app->hwnd_status_label, L"Struct template loaded.");

    // If a .modl entry is currently selected but wasn't parsed (no template was
    // loaded when it was extracted), re-extract it now that we have the template.
    if (g_app->dat_loaded && g_app->has_loaded_entry && g_app->current_entry.kind == PreviewKind::Model &&
        !g_app->current_entry.model) {
        on_entry_selected(g_app->current_mft_index);
    }
}

// Loads a string-key CSV (textId,key8_hex); also pulls a sibling strs_textbase.csv
// (fileId,baseTextId). With both, packed strs records decrypt in the preview.
void load_keys_from(const std::wstring& csv_path) {
    castlemist::skeys::load_keys(csv_path);
    std::wstring dir = csv_path;
    size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"" : dir.substr(0, slash + 1);
    castlemist::skeys::load_textbase(dir + L"strs_textbase.csv");
    if (g_app && g_app->hwnd_status_label) {
        wchar_t msg[192];
        swprintf(msg, 192, L"String keys loaded: %zu  (textbase: %s)",
                 castlemist::skeys::key_count(), castlemist::skeys::textbase_ready() ? L"ok" : L"MISSING strs_textbase.csv");
        SetWindowTextW(g_app->hwnd_status_label, msg);
    }
    if (g_app && g_app->dat_loaded && g_app->has_loaded_entry &&
        g_app->current_entry.kind == PreviewKind::Strs) {
        on_entry_selected(g_app->current_mft_index);
    }
}

void do_load_keys(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"String keys (*.csv)\0*.csv\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    load_keys_from(path);
}

// Best-effort: pick up textkeys.csv + strs_textbase.csv at startup so strs
// decrypt "just works" once tools/strs has produced them into dumps/strs/.
void try_autoload_keys() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    size_t s = dir.find_last_of(L"\\/");
    dir = (s == std::wstring::npos) ? L"" : dir.substr(0, s + 1);
    const wchar_t* rel[] = {L"", L"..\\", L"..\\..\\..\\dumps\\strs\\"};
    for (const wchar_t* r : rel) {
        std::wstring base = dir + r;
        if (GetFileAttributesW((base + L"textkeys.csv").c_str()) != INVALID_FILE_ATTRIBUTES) {
            castlemist::skeys::load_keys(base + L"textkeys.csv");
            castlemist::skeys::load_textbase(base + L"strs_textbase.csv");
            return;
        }
    }
}

void do_export(HWND hwnd, bool export_compressed) {
    const std::vector<uint8_t>& data =
        export_compressed ? g_app->current_entry.compressed : g_app->current_entry.decompressed;

    if (!g_app->has_loaded_entry || data.empty()) {
        MessageBoxW(hwnd, L"Select an entry first.", L"castlemist", MB_ICONINFORMATION);
        return;
    }

    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Binary file\0*.bin\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bin";
    ofn.Flags = OFN_OVERWRITEPROMPT;

    if (!GetSaveFileNameW(&ofn)) {
        return;
    }

    std::ofstream out(castlemist::core::to_ansi(path), std::ios::binary);
    if (!out) {
        MessageBoxW(hwnd, L"Failed to open the file for writing.", L"castlemist", MB_ICONERROR);
        return;
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// The selected combo item's text ("" for item 0 = "(all)").
std::string combo_sel(HWND combo) {
    int i = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (i <= 0) return {};
    wchar_t w[64] = L"";
    SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(w));
    std::string s;
    for (wchar_t c : std::wstring(w)) s.push_back(static_cast<char>(c));  // fourccs/types are ASCII
    return s;
}

// Reads the id box + Type/Container combos and refilters the list. In INDEX
// mode the filter is a fast SQL query (covers type/container); in PARSE mode
// only the id box applies (type/container need an index).
void apply_filters() {
    if (!g_app->dat_loaded && !g_app->index_loaded) return;

    wchar_t buf[32] = L"";
    GetWindowTextW(g_app->hwnd_search_edit, buf, 32);
    bool id_active = buf[0] != L'\0';
    uint32_t id_val = id_active ? static_cast<uint32_t>(wcstoul(buf, nullptr, 10)) : 0;
    bool by_file_id = SendMessageW(g_app->hwnd_search_fileid_check, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (g_app->index_loaded) {
        std::string type = combo_sel(g_app->hwnd_filter_type);
        std::string cont = combo_sel(g_app->hwnd_filter_container);

        // Content combo: index 0 is the no-op, so an out-of-range or unset
        // selection degrades to "no content filter" rather than a wrong one.
        castlemist::db::ContentFilter content;
        LRESULT sel = SendMessageW(g_app->hwnd_filter_content, CB_GETCURSEL, 0, 0);
        if (sel > 0 && sel < static_cast<LRESULT>(std::size(kContentFilters))) {
            const ContentFilterChoice& c = kContentFilters[sel];
            // magics is comma-separated so the table can stay a constexpr literal.
            for (const char* b = c.magics; *b;) {
                const char* e = b;
                while (*e && *e != ',') ++e;
                content.magics.emplace_back(b, e);
                b = *e ? e + 1 : e;
            }
            content.require_chunk = c.require_chunk;
            content.exclude_chunk = c.exclude_chunk;
        }

        if (!id_active && type.empty() && cont.empty() && content.empty()) {
            castlemist::mft::set_filter(g_app->hwnd_list, {});  // no filter -> show every asset
            SetWindowTextW(g_app->hwnd_status_label, L"Index: showing all entries");
            return;
        }
        std::vector<uint32_t> ids =
            castlemist::db::query_base_ids(type, cont, content, id_val, by_file_id, id_active, 300000);
        castlemist::mft::set_filter(g_app->hwnd_list, ids);
        wchar_t st[128];
        swprintf(st, 128, L"Index filter -> %zu entries", ids.size());
        SetWindowTextW(g_app->hwnd_status_label, st);
        return;
    }

    // Parse mode: id search only (no type/container without an index).
    if (!id_active) { castlemist::mft::set_filter(g_app->hwnd_list, {}); return; }
    std::vector<uint32_t> base_ids;
    if (by_file_id) {
        for (uint32_t file_id : search_by_file_id(g_app->data_gw2, id_val)) {
            uint32_t base_id = get_by_base_id(g_app->data_gw2, file_id);
            if (base_id != 0) base_ids.push_back(base_id);
        }
    } else {
        base_ids = search_by_base_id(g_app->data_gw2, id_val);
    }
    castlemist::mft::set_filter(g_app->hwnd_list, base_ids);
}

void do_search() { apply_filters(); }

void do_clear_search() {
    SetWindowTextW(g_app->hwnd_search_edit, L"");
    if (g_app->hwnd_filter_type) SendMessageW(g_app->hwnd_filter_type, CB_SETCURSEL, 0, 0);
    if (g_app->hwnd_filter_container) SendMessageW(g_app->hwnd_filter_container, CB_SETCURSEL, 0, 0);
    if (g_app->hwnd_filter_content) SendMessageW(g_app->hwnd_filter_content, CB_SETCURSEL, 0, 0);
    if (g_app->index_loaded) apply_filters();
    else castlemist::mft::set_filter(g_app->hwnd_list, {});
}


} // namespace castlemist::ui
