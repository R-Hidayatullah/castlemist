/// @file
/// @brief Opening a gw2index SQLite and wiring its type/container filters.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <commdlg.h>

namespace castlemist::ui {

void finish_open_index(HWND hwnd, bool silent) {
    // Previews still come from the .dat -> auto-open the one this index was built
    // from, unless an archive is already loaded.
    if (!g_app->dat_loaded) {
        std::wstring datp = castlemist::db::dat_path();
        if (datp.empty() || GetFileAttributesW(datp.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!silent)
                MessageBoxW(hwnd,
                            L"Index opened, but its .dat was not found on disk.\n"
                            L"Open the archive via File > Open .dat for previews; filtering still works.",
                            L"castlemist", MB_ICONWARNING);
        } else {
            load_dat_path(hwnd, datp.c_str());
        }
    }

    HCURSOR oc = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    // Intern base_id -> (typeIdx<<16 | contIdx) so 800k rows cost ~3MB.
    g_app->idx_type_names.clear();
    g_app->idx_cont_names.clear();
    g_app->index_meta.clear();
    g_app->index_meta.reserve(castlemist::db::entry_count() + 16);
    std::unordered_map<std::string, int> tmap, cmap;
    auto intern = [](std::unordered_map<std::string, int>& m, std::vector<std::string>& names, const char* s) -> int {
        if (!s || !s[0]) return 0xFFFF;
        auto it = m.find(s);
        if (it != m.end()) return it->second;
        int id = static_cast<int>(names.size());
        names.emplace_back(s);
        m[s] = id;
        return id;
    };
    castlemist::db::load_meta_map([&](uint32_t base_id, const char* type, const char* cont) {
        int ti = intern(tmap, g_app->idx_type_names, type);
        int ci = intern(cmap, g_app->idx_cont_names, cont);
        g_app->index_meta[base_id] = (static_cast<uint32_t>(ti & 0xFFFF) << 16) | static_cast<uint32_t>(ci & 0xFFFF);
    });

    // Real "Uncompressed" column: the MFT field is 0 for every compressed entry
    // (99.4% of a retail dat), so feed the list the index's decompressed sizes.
    g_app->index_usize.assign(castlemist::db::entry_count() + 2, 0);
    castlemist::db::load_size_map([](uint32_t base_id, long long size_final) {
        if (size_final <= 0) return;
        // Grow if this DB's base_ids aren't dense (Local.dat etc.); amortized.
        if (base_id >= g_app->index_usize.size()) g_app->index_usize.resize(base_id + 4096, 0);
        g_app->index_usize[base_id] = static_cast<uint64_t>(size_final);
    });
    castlemist::mft::set_size_provider(g_app->hwnd_list, [](uint32_t base_id, uint64_t& usize) -> bool {
        if (base_id >= g_app->index_usize.size()) return false;
        uint64_t v = g_app->index_usize[base_id];
        if (v == 0) return false;   // unknown -> let the MFT field show through
        usize = v;
        return true;
    });

    castlemist::mft::set_metadata_provider(g_app->hwnd_list,
        [](uint32_t base_id, std::wstring& type, std::wstring& cont) -> bool {
            auto it = g_app->index_meta.find(base_id);
            if (it == g_app->index_meta.end()) return false;
            int ti = (it->second >> 16) & 0xFFFF, ci = it->second & 0xFFFF;
            auto tow = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
            if (ti != 0xFFFF && ti < static_cast<int>(g_app->idx_type_names.size())) type = tow(g_app->idx_type_names[ti]);
            if (ci != 0xFFFF && ci < static_cast<int>(g_app->idx_cont_names.size())) cont = tow(g_app->idx_cont_names[ci]);
            return true;
        });

    // Populate the filter combos (sorted distinct values; item 0 = "(all)").
    auto fill = [](HWND combo, const std::vector<std::string>& vals) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(all)"));
        for (const std::string& v : vals) {
            std::wstring w(v.begin(), v.end());
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
        }
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    };
    fill(g_app->hwnd_filter_type, castlemist::db::types());
    fill(g_app->hwnd_filter_container, castlemist::db::containers());
    // Content choices are a fixed table, not distinct values off the DB -- they
    // are predicates over the chunk list, which has no single column to enumerate.
    SendMessageW(g_app->hwnd_filter_content, CB_RESETCONTENT, 0, 0);
    for (const ContentFilterChoice& c : kContentFilters)
        SendMessageW(g_app->hwnd_filter_content, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label));
    SendMessageW(g_app->hwnd_filter_content, CB_SETCURSEL, 0, 0);
    ShowWindow(g_app->hwnd_filter_type, SW_SHOW);
    ShowWindow(g_app->hwnd_filter_container, SW_SHOW);
    ShowWindow(g_app->hwnd_filter_content, SW_SHOW);

    g_app->index_loaded = true;
    InvalidateRect(g_app->hwnd_list, nullptr, TRUE);

    wchar_t st[128];
    swprintf(st, 128, L"Index loaded: %zu entries, %zu types, %zu containers",
             castlemist::db::entry_count(), g_app->idx_type_names.size(), g_app->idx_cont_names.size());
    SetWindowTextW(g_app->hwnd_status_label, st);
    SetCursor(oc);
}

// File -> Open Index DB: pick a gw2index/gw2local SQLite, then finish_open_index.
void do_open_index(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"gw2index database (*.db)\0*.db\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string err;
    if (!castlemist::db::open(path, err)) {
        MessageBoxA(hwnd, err.c_str(), "Failed to open index DB", MB_ICONERROR);
        return;
    }
    finish_open_index(hwnd, /*silent=*/false);
}

// Startup: best-effort auto-open the index that tools/gw2index writes, so the
// browser comes up already indexed. The exe runs from build/<preset>/bin/, so
// the search climbs towards the repository root and dumps/index/. Silent on
// absence -- File > Open Index DB still works.
void try_autoload_index(HWND hwnd) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash + 1);
    const std::wstring candidates[] = {
        dir + L"gw2_index.db",
        dir + L"..\\..\\..\\dumps\\index\\gw2_index.db",
        dir + L"..\\..\\dumps\\index\\gw2_index.db",
        dir + L"..\\dumps\\index\\gw2_index.db",
        L"dumps\\index\\gw2_index.db",
    };
    std::string err;
    for (const std::wstring& p : candidates) {
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (castlemist::db::open(p, err)) { finish_open_index(hwnd, /*silent=*/true); return; }
    }
}


} // namespace castlemist::ui
