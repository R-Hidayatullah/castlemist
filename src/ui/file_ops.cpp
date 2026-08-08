/// @file
/// @brief File commands: open, export, load template/keys, search and filters.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <commdlg.h>
#include <fstream>
#include <thread>

#include "castlemist/db/index_builder.h"
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
        castlemist::structtree::clear(g_app->hwnd_struct_tree);
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

// Open an asset that is not inside a .dat -- something exported from the archive,
// or a file that was never in one. Runs the same detection as an archive entry, so
// ATEX-family textures, DDS, Bink video, audio, packfiles and text all preview
// exactly as they do when browsing Gw2.dat.
void do_open_loose_file(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    // "All Files" is not last by accident: an export carries whatever extension
    // the user typed, and raw MFT dumps usually have none at all.
    ofn.lpstrFilter =
        L"Textures (ATEX family, DDS)\0*.atex;*.attx;*.atec;*.atep;*.ateu;*.atet;*.ctex;*.dds\0"
        L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.webp\0"
        L"Video (Bink)\0*.bik;*.bk2\0"
        L"All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.nFilterIndex = 4;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        MessageBoxW(hwnd, L"Could not open that file.", L"castlemist", MB_ICONWARNING | MB_OK);
        return;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        MessageBoxW(hwnd, L"That file is empty.", L"castlemist", MB_ICONWARNING | MB_OK);
        return;
    }

    std::string source;
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) { source.resize(static_cast<size_t>(len) - 1);
                       WideCharToMultiByte(CP_UTF8, 0, path, -1, source.data(), len, nullptr, nullptr); }
    }

    ExtractedEntry entry;
    try {
        entry = extract_loose_file(std::move(bytes), source);
    } catch (const std::exception&) {
        MessageBoxW(hwnd, L"Could not read that file.", L"castlemist", MB_ICONWARNING | MB_OK);
        return;
    }

    // There is no MFT index for a loose file. UINT32_MAX makes the two id lookups
    // inside apply_extracted_entry resolve to baseId 0, which never matches.
    PreviewKind kind = entry.kind;
    size_t shown = entry.decompressed.size();
    apply_extracted_entry(UINT32_MAX, std::move(entry));

    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    wchar_t st[MAX_PATH + 96];
    if (kind == PreviewKind::None)
        swprintf(st, MAX_PATH + 96, L"%ls - unrecognised format, showing %zu bytes as hex", name, shown);
    else
        swprintf(st, MAX_PATH + 96, L"%ls - %zu bytes", name, shown);
    SetWindowTextW(g_app->hwnd_status_label, st);
}

// ---- index building -------------------------------------------------------
//
// A cold index is ~800k entries, every one decompressed far enough to learn its
// real type, so this runs for minutes and cannot go anywhere near the UI thread.
// The worker owns the build; it reports by posting messages, and stops when the
// cancel flag flips. The thread is detached and the flag lives in a namespace
// scope so the app can ask it to stop and then exit without joining.
std::atomic<bool> g_index_cancel{false};
std::atomic<bool> g_index_running{false};
std::wstring g_index_out_path;
std::string g_index_error;

void do_build_index(HWND hwnd) {
    if (g_index_running.load()) {
        // Second invocation while a build is live means "stop", which is the only
        // sensible reading -- there is nothing else the menu item could do now.
        g_index_cancel.store(true);
        SetWindowTextW(g_app->hwnd_status_label, L"Index build: cancelling...");
        return;
    }

    // Source archive: reuse the open one so the common case is two clicks, but
    // still allow indexing a .dat that is not currently loaded.
    std::wstring dat_w;
    if (g_app->dat_loaded && !g_app->data_gw2.file_info.file_path.empty()) {
        const std::string& p = g_app->data_gw2.file_info.file_path;
        dat_w.assign(p.begin(), p.end());
    } else {
        wchar_t path[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Guild Wars 2 Archive (*.dat)\0*.dat\0All Files\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        dat_w = path;
    }

    wchar_t out[MAX_PATH] = L"gw2_index.db";
    OPENFILENAMEW sfn{};
    sfn.lStructSize = sizeof(sfn);
    sfn.hwndOwner = hwnd;
    sfn.lpstrFilter = L"Index database (*.db)\0*.db\0All Files\0*.*\0";
    sfn.lpstrFile = out;
    sfn.nMaxFile = MAX_PATH;
    sfn.lpstrTitle = L"Save index database as";
    sfn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&sfn)) return;

    auto narrow = [](const std::wstring& w) {
        std::string s;
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) { s.resize(static_cast<size_t>(len) - 1);
                       WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr); }
        return s;
    };

    castlemist::db::BuildOptions opt;
    opt.dat_path = narrow(dat_w);
    opt.db_path = narrow(out);
    // Reuse the struct template if one is loaded. Without it the index still
    // builds, it just cannot name each chunk's struct variant -- worth saying so
    // rather than silently producing a thinner index than the CLI would.
    opt.template_path = castlemist::tpl::source_path();

    g_index_out_path = out;
    g_index_error.clear();
    g_index_cancel.store(false);
    g_index_running.store(true);

    if (opt.template_path.empty())
        SetWindowTextW(g_app->hwnd_status_label,
                       L"Index build started (no struct template - chunk variants will be blank)...");
    else
        SetWindowTextW(g_app->hwnd_status_label, L"Index build started...");

    std::thread([hwnd, opt]() {
        auto on_progress = [hwnd](const castlemist::db::BuildProgress& p) {
            PostMessageW(hwnd, WM_APP_INDEX_PROGRESS,
                         static_cast<WPARAM>(p.processed + p.skipped), static_cast<LPARAM>(p.total));
        };
        std::string err;
        bool ok = castlemist::db::build_index(opt, on_progress, &g_index_cancel, err);
        g_index_error = ok ? std::string() : err;
        g_index_running.store(false);
        PostMessageW(hwnd, WM_APP_INDEX_DONE, ok ? 1 : 0, 0);
    }).detach();
}

void on_index_build_done(HWND hwnd, bool ok) {
    if (!ok) {
        std::wstring msg = L"Index build failed.";
        if (!g_index_error.empty()) {
            msg += L"\n\n";
            msg.append(g_index_error.begin(), g_index_error.end());
        }
        SetWindowTextW(g_app->hwnd_status_label, L"Index build failed.");
        MessageBoxW(hwnd, msg.c_str(), L"castlemist", MB_ICONERROR | MB_OK);
        return;
    }
    if (g_index_cancel.load()) {
        SetWindowTextW(g_app->hwnd_status_label,
                       L"Index build cancelled - partial index saved, re-run to resume.");
        return;
    }
    SetWindowTextW(g_app->hwnd_status_label, L"Index build finished.");
    if (MessageBoxW(hwnd, L"Index built. Open it now?", L"castlemist", MB_ICONQUESTION | MB_YESNO) == IDYES)
        load_index_path(hwnd, g_index_out_path.c_str());
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

    // The "Structure" tab is a pure function of (current bytes, current
    // template) -- unlike the model/map surfaces it needs no re-extraction,
    // just a re-walk against the newly loaded template.
    if (g_app->dat_loaded && g_app->has_loaded_entry) {
        populate_struct_tree();
    }

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
