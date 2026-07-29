/// @file
/// @brief Light theme palette, Segoe UI fonts and the Common-Controls v6 activation context.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <unordered_map>

namespace castlemist::ui {

HBRUSH theme_brush(COLORREF c) {
    // Small cache of solid brushes keyed by color (freed at process exit by the OS).
    static std::unordered_map<COLORREF, HBRUSH> cache;
    auto it = cache.find(c);
    if (it != cache.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    cache[c] = b;
    return b;
}

void ensure_ui_fonts() {
    if (g_ui_font) return;
    g_ui_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_ui_font_bold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

BOOL CALLBACK apply_font_cb(HWND child, LPARAM font) {
    // Skip the monospace text/hex preview + content tables (they set their own
    // Consolas font); everything else gets the Segoe UI shell font.
    if (g_app && (child == g_app->hwnd_text_preview || child == g_app->hwnd_content_list ||
                  child == g_app->hwnd_content_child || child == g_app->hwnd_content_asset_list))
        return TRUE;
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    return TRUE;
}

// Enable Common Controls v6 (modern themed buttons/combos/lists) at runtime via a
// tiny manifest activation context -- no build/link changes required.
void enable_visual_styles() {
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%scastlemist_v6.manifest", dir);
    static const char* manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
        "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" "
        "name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" processorArchitecture=\"*\" "
        "publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/></dependentAssembly></dependency>\r\n"
        "</assembly>\r\n";
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, manifest, static_cast<DWORD>(std::strlen(manifest)), &wr, nullptr);
    CloseHandle(f);
    ACTCTXW ac{};
    ac.cbSize = sizeof(ac);
    ac.lpSource = path;
    HANDLE h = CreateActCtxW(&ac);
    if (h != INVALID_HANDLE_VALUE) {
        ULONG_PTR cookie = 0;
        ActivateActCtx(h, &cookie); // kept active for the process lifetime
    }
}

// Windows' narrow file APIs (and MinGW's std::ifstream, which has no wide
// overload) expect the current ANSI code page, not UTF-8 -- convert here
// once rather than truncating the wide path byte-by-byte.

} // namespace castlemist::ui
