/// @file
/// @brief The &[base64] chat-link decoder popup.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <atomic>
#include <cstdio>
#include <thread>

#include "castlemist/format/content_map.h"

namespace castlemist::ui {

// ---- Chat-link decoder popup ----------------------------------------------
// A modeless tool window: paste a "[&...]" link, see the typed breakdown, and
// push the extracted id into the main base-id / file-id search.
HWND g_cl_wnd = nullptr;
HWND g_cl_input = nullptr;
HWND g_cl_output = nullptr;
HWND g_cl_status = nullptr;
castlemist::chat::Decoded g_cl_last;


void cl_do_decode() {
    wchar_t wbuf[1024] = L"";
    GetWindowTextW(g_cl_input, wbuf, 1024);
    std::string s;
    for (wchar_t* p = wbuf; *p; ++p)
        if (*p < 128) s.push_back(static_cast<char>(*p));
    g_cl_last = castlemist::chat::decode(s);
    SetWindowTextW(g_cl_output, castlemist::core::from_ascii(castlemist::chat::to_report(g_cl_last)).c_str());
    if (g_cl_last.ok && g_cl_last.primary_id) {
        wchar_t st[160];
        swprintf(st, 160, L"Search-ready: %s = %u", castlemist::core::from_ascii(g_cl_last.primary_label).c_str(),
                 g_cl_last.primary_id);
        SetWindowTextW(g_cl_status, st);
    } else {
        SetWindowTextW(g_cl_status, g_cl_last.ok ? L"Decoded (no single id to search)"
                                                 : L"Not a valid chat link");
    }
}

std::atomic<bool> g_cmap_building{false};

std::wstring cmap_cache_path() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p + L"content_map.bin";
}

uint32_t g_cl_icon_fid = 0;   // resolved icon (texture) fileId for the current link
uint32_t g_cl_model_fid = 0;  // resolved model (MODL) fileId for the current link

// The numeric content id carried by the decoded link (item/skin/outfit).
uint32_t cl_content_id() {
    const auto& r = g_cl_last.raw;
    if (g_cl_last.header == 0x02 && r.size() >= 5) return r[2] | (r[3] << 8) | (r[4] << 16);
    if ((g_cl_last.header == 0x0A || g_cl_last.header == 0x0B) && r.size() >= 4)
        return r[1] | (r[2] << 8) | (r[3] << 16);
    return 0;
}

// After the content map is available, resolve the decoded link's id to its dat
// assets (icon/model/...), classify each via the index, list them in the readout
// and arm the Open icon / Open model buttons.
void cl_show_resolved() {
    g_cl_icon_fid = g_cl_model_fid = 0;
    uint32_t ctype = castlemist::cmap::content_type_for_header(g_cl_last.header);
    uint32_t id = cl_content_id();
    if (!ctype || !id) {
        SetWindowTextW(g_cl_status, L"This link type has no cntc asset mapping (item/skin/outfit only).");
        return;
    }
    const std::vector<uint32_t>& fids = castlemist::cmap::resolve_all(ctype, id);
    std::wstring rep = castlemist::core::from_ascii(castlemist::chat::to_report(g_cl_last));
    if (fids.empty()) {
        wchar_t st[200];
        swprintf(st, 200, L"id %u: no asset in content map (%zu entries)", id, castlemist::cmap::size());
        SetWindowTextW(g_cl_status, st);
        rep += L"\r\nAssets (cntc): none found\r\n";
        SetWindowTextW(g_cl_output, rep.c_str());
        return;
    }
    rep += L"\r\nAssets (cntc):\r\n";
    for (uint32_t fid : fids) {
        uint32_t base = get_by_base_id(g_app->data_gw2, fid);
        std::string kind = "?";
        if (g_app->index_loaded && base) {
            castlemist::db::EntryInfo e = castlemist::db::lookup(base);
            kind = !e.container.empty() ? e.container : e.type;
        }
        bool tex = (kind == "texture" || kind == "dds" || kind.rfind("ATE", 0) == 0);
        bool mdl = (kind == "MODL");
        if (tex && !g_cl_icon_fid) g_cl_icon_fid = fid;
        if (mdl && !g_cl_model_fid) g_cl_model_fid = fid;
        wchar_t line[96];
        swprintf(line, 96, L"  fileId %u   (%hs)%ls\r\n", fid, kind.c_str(),
                 (fid == g_cl_icon_fid ? L"  <- texture" : fid == g_cl_model_fid ? L"  <- model" : L""));
        rep += line;
    }
    // NOTE: cntc content references the appearance MODEL + its material/render
    // textures + sounds -- NOT the 2D inventory icon (that fileId comes from the
    // server render service and is not stored in cntc).
    rep += L"\r\n(2D inventory icon is not in cntc; texture above = a model material)\r\n";
    SetWindowTextW(g_cl_output, rep.c_str());
    wchar_t st[200];
    swprintf(st, 200, L"id %u -> %zu asset(s). model=%u  texture=%u", id, fids.size(), g_cl_model_fid,
             g_cl_icon_fid);
    SetWindowTextW(g_cl_status, st);
}

// Navigate the main browser to a resolved asset fileId (reuses the file-id search).
void cl_open_fid(uint32_t fid) {
    if (!fid) { MessageBeep(MB_ICONWARNING); return; }
    if (!g_app->dat_loaded && !g_app->index_loaded) return;
    wchar_t num[16];
    swprintf(num, 16, L"%u", fid);
    SetWindowTextW(g_app->hwnd_search_edit, num);
    SendMessageW(g_app->hwnd_search_fileid_check, BM_SETCHECK, BST_CHECKED, 0);
    apply_filters();
    if (g_app->hwnd_main) { SetForegroundWindow(g_app->hwnd_main); SetFocus(g_app->hwnd_list); }
}

// Resolve the decoded link to dat assets via the cntc content map. Builds the map
// on first use (from a disk cache if present, else by parsing every cntc pack on a
// background thread -- needs the main Gw2.dat index loaded for the cntc list).
void cl_resolve_asset() {
    if (!g_cl_last.ok || castlemist::cmap::content_type_for_header(g_cl_last.header) == 0) {
        SetWindowTextW(g_cl_status, L"Asset resolve supports Item / Skin / Outfit links.");
        return;
    }
    if (castlemist::cmap::built()) { cl_show_resolved(); return; }
    if (g_cmap_building) { SetWindowTextW(g_cl_status, L"Still building content map..."); return; }
    if (!g_app->dat_loaded) {
        MessageBoxW(g_cl_wnd, L"Open the .dat first (previews + asset resolve read from it).",
                    L"castlemist", MB_ICONINFORMATION);
        return;
    }
    if (castlemist::cmap::load(cmap_cache_path())) { cl_show_resolved(); return; }

    std::vector<uint32_t> base_ids;
    if (g_app->index_loaded)
        base_ids = castlemist::db::query_base_ids("", "cntc", 0, false, false, 100000);
    if (base_ids.empty()) {
        MessageBoxW(g_cl_wnd,
                    L"No cntc entries available.\nLoad the main Gw2.dat index DB "
                    L"(File > Open Index DB) so the content packs can be enumerated.",
                    L"castlemist", MB_ICONINFORMATION);
        return;
    }
    // Copy the MftData for each cntc (baseId -> physical index baseId-1) so the
    // worker touches no shared mutable state.
    std::vector<MftData> entries;
    entries.reserve(base_ids.size());
    for (uint32_t b : base_ids) {
        uint32_t idx = b - 1;
        if (idx < g_app->data_gw2.mft_data_list.size()) entries.push_back(g_app->data_gw2.mft_data_list[idx]);
    }
    std::string dat_path = g_app->data_gw2.file_info.file_path;
    std::wstring cache = cmap_cache_path();
    g_cmap_building = true;
    wchar_t st[128];
    swprintf(st, 128, L"Building content map from %zu cntc packs... (one-time)", entries.size());
    SetWindowTextW(g_cl_status, st);
    std::thread([dat_path, entries, cache]() {
        castlemist::cmap::build(dat_path, entries, nullptr);
        castlemist::cmap::save(cache);
        g_cmap_building = false;
        if (g_cl_wnd) PostMessageW(g_cl_wnd, WM_APP_CMAP_DONE, 0, 0);
    }).detach();
}

// Feed the decoded primary id into the main search box + file-id checkbox, run
// the existing filter, and surface the main window.
void cl_search(bool by_file_id) {
    if (!g_cl_last.ok || g_cl_last.primary_id == 0) { MessageBeep(MB_ICONWARNING); return; }
    if (!g_app->dat_loaded && !g_app->index_loaded) {
        MessageBoxW(g_cl_wnd, L"Open a .dat or an index DB first, then search.", L"castlemist",
                    MB_ICONINFORMATION);
        return;
    }
    wchar_t num[16];
    swprintf(num, 16, L"%u", g_cl_last.primary_id);
    SetWindowTextW(g_app->hwnd_search_edit, num);
    SendMessageW(g_app->hwnd_search_fileid_check, BM_SETCHECK,
                 by_file_id ? BST_CHECKED : BST_UNCHECKED, 0);
    apply_filters();
    if (g_app->hwnd_main) {
        SetForegroundWindow(g_app->hwnd_main);
        SetFocus(g_app->hwnd_list);
    }
}

LRESULT CALLBACK ChatLinkWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_CL_DECODE: cl_do_decode(); return 0;
        case ID_CL_RESOLVE: cl_resolve_asset(); return 0;
        case ID_CL_OPEN_ICON: cl_open_fid(g_cl_icon_fid); return 0;
        case ID_CL_OPEN_MODEL: cl_open_fid(g_cl_model_fid); return 0;
        case ID_CL_SEARCH_BASE: cl_search(false); return 0;
        case ID_CL_SEARCH_FILE: cl_search(true); return 0;
        case ID_CL_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_APP_CMAP_DONE: cl_show_resolved(); return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        g_cl_wnd = g_cl_input = g_cl_output = g_cl_status = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void open_chat_link_decoder(HWND owner) {
    if (g_cl_wnd) {  // already open -> just bring it forward
        SetForegroundWindow(g_cl_wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ChatLinkWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2ChatLinkWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    const int W = 560, H = 470;
    g_cl_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2ChatLinkWnd", L"Decode Chat Link",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                               W, H, owner, nullptr, g_hinstance, nullptr);
    if (!g_cl_wnd) return;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0,
                             FIXED_PITCH | FF_MODERN, L"Consolas");

    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h,
                  UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_cl_wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };

    mk(L"STATIC", L"Paste a chat link ( [&AgH1WQAA] ):", SS_LEFT, 10, 10, 400, 18, 0);
    g_cl_input = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 10, 30, W - 130, 24, ID_CL_INPUT);
    mk(L"BUTTON", L"Decode", BS_DEFPUSHBUTTON, W - 110, 29, 90, 26, ID_CL_DECODE);

    g_cl_output = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                     10, 64, W - 24, H - 200, ID_CL_OUTPUT);
    SendMessageW(g_cl_output, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);

    mk(L"BUTTON", L"Resolve assets via cntc (Item/Skin/Outfit)", BS_PUSHBUTTON, 10, H - 128, 290, 26,
       ID_CL_RESOLVE);
    g_cl_status = mk(L"STATIC", L"", SS_LEFT, 10, H - 98, W - 24, 18, 0);
    mk(L"BUTTON", L"Open model", BS_PUSHBUTTON, 10, H - 76, 100, 28, ID_CL_OPEN_MODEL);
    mk(L"BUTTON", L"Open texture", BS_PUSHBUTTON, 115, H - 76, 100, 28, ID_CL_OPEN_ICON);
    mk(L"BUTTON", L"id->base", BS_PUSHBUTTON, 220, H - 76, 75, 28, ID_CL_SEARCH_BASE);
    mk(L"BUTTON", L"id->file", BS_PUSHBUTTON, 300, H - 76, 75, 28, ID_CL_SEARCH_FILE);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, W - 90, H - 76, 80, 28, ID_CL_CLOSE);

    ShowWindow(g_cl_wnd, SW_SHOW);
    SetFocus(g_cl_input);
}

// Shared post-open logic: an index DB has already been opened via castlemist::db::open().
// Auto-opens the .dat it references (for previews), builds the Type/Container
// column map + filter combos. `silent` suppresses the missing-.dat popup (used by
// the startup auto-loader, where an absent archive is fine).

} // namespace castlemist::ui
