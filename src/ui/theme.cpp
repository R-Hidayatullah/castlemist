/// @file
/// @brief Theme palettes, Segoe UI fonts and the shared brush cache.

#include "castlemist/ui/theme.h"

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <unordered_map>

#include <commctrl.h>
#include <uxtheme.h>

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

// Windows' narrow file APIs (and MinGW's std::ifstream, which has no wide
// overload) expect the current ANSI code page, not UTF-8 -- convert here
// once rather than truncating the wide path byte-by-byte.

// ---------------------------------------------------------------------------
// Palettes
//
// Dark is the default. The two palettes are written out in full rather than
// derived from each other: a "dark mode" produced by inverting a light one gets
// the greys right and everything else wrong, and these values were picked
// against real texture and model previews.
// ---------------------------------------------------------------------------

namespace {

struct Palette {
    COLORREF bg, panel, band, text, subtle, accent, card, border, hover;
    bool dark;
};

constexpr Palette kDark{
    RGB(0x1B, 0x1F, 0x26), // bg
    RGB(0x24, 0x29, 0x33), // panel
    RGB(0x15, 0x18, 0x1E), // band
    RGB(0xE6, 0xEA, 0xF0), // text
    RGB(0x98, 0xA3, 0xB3), // subtle
    RGB(0x4C, 0x93, 0xFF), // accent
    RGB(0x2B, 0x31, 0x3C), // card
    RGB(0x39, 0x40, 0x4C), // border
    RGB(0x2E, 0x3A, 0x4E), // hover
    true,
};

constexpr Palette kLight{
    RGB(0xF3, 0xF4, 0xF6),
    RGB(0xFF, 0xFF, 0xFF),
    RGB(0xE9, 0xEC, 0xF1),
    RGB(0x22, 0x27, 0x30),
    RGB(0x5A, 0x63, 0x70),
    RGB(0x2D, 0x7F, 0xF9),
    RGB(0xF7, 0xF9, 0xFC),
    RGB(0xD3, 0xD8, 0xDF),
    RGB(0xEA, 0xF2, 0xFE),
    false,
};

/// @brief Perceived lightness, 0..1. The coefficients are the sRGB luma weights;
///        judging "is this dark" from the raw channels gets blue badly wrong.
double luminance(COLORREF c) {
    return (0.2126 * GetRValue(c) + 0.7152 * GetGValue(c) + 0.0722 * GetBValue(c)) / 255.0;
}

COLORREF mix(COLORREF a, COLORREF b, double t) {
    auto ch = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)),
               ch(GetBValue(a), GetBValue(b)));
}

/// @brief A custom palette: whichever base reads better against @p accent, with
///        the accent mixed into the surfaces so the tint shows without losing
///        the text contrast the base guarantees.
Palette tinted(COLORREF accent) {
    // A pale accent on a pale background is unreadable, so the base is chosen
    // for the user rather than left as a second decision.
    Palette p = luminance(accent) < 0.5 ? kLight : kDark;
    p.accent = accent;
    p.bg     = mix(p.bg,     accent, 0.06);
    p.panel  = mix(p.panel,  accent, 0.04);
    p.band   = mix(p.band,   accent, 0.09);
    p.card   = mix(p.card,   accent, 0.05);
    p.border = mix(p.border, accent, 0.18);
    p.hover  = mix(p.hover,  accent, 0.22);
    return p;
}

void install(const Palette& p) {
    kColBg     = p.bg;
    kColPanel  = p.panel;
    kColBand   = p.band;
    kColText   = p.text;
    kColSubtle = p.subtle;
    kColAccent = p.accent;
    kColCard   = p.card;
    kColBorder = p.border;
    kColHover  = p.hover;
    g_theme_is_dark = p.dark;
}

const wchar_t* const kRegKey = L"Software\\castlemist";

} // namespace

void apply_theme(ThemeMode mode, COLORREF accent) {
    switch (mode) {
    case ThemeMode::Light:
        install(kLight);
        break;
    case ThemeMode::Custom:
        g_theme_custom_accent = accent;
        install(tinted(accent));
        break;
    case ThemeMode::Dark:
    default:
        install(kDark);
        break;
    }
    g_theme_mode = mode;
    enable_dark_mode_support(); // the opt-in follows the palette, not the process
}

// ---------------------------------------------------------------------------
// Opting the process into dark common controls.
//
// The DarkMode_Explorer / DarkMode_CFD theme names do nothing on their own:
// uxtheme ignores them unless the process has declared that it wants dark mode,
// and the call that declares it is exported by ordinal only, with no header and
// no documentation. Without this, SetWindowTheme succeeds, returns S_OK, and
// changes nothing -- which is what left the search box white inside a dark
// window.
//
// Everything here is resolved by ordinal and null-checked. On a Windows without
// these exports the controls simply stay light; nothing fails.
// ---------------------------------------------------------------------------

namespace {

enum class PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };

using PfnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using PfnFlushMenuThemes = void(WINAPI*)();

PfnSetPreferredAppMode g_set_app_mode = nullptr;
PfnFlushMenuThemes     g_flush_menu_themes = nullptr;

void bind_dark_mode_api() {
    static bool tried = false;
    if (tried) return;
    tried = true;

    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux == nullptr) return;

    // Ordinal 135 is SetPreferredAppMode from Windows 10 1903 on. On 1809 the
    // same ordinal is AllowDarkModeForApp(BOOL), and the argument we pass
    // (AllowDark == 1 == TRUE) means the same thing to both, which is why this
    // needs no version check.
    g_set_app_mode =
        reinterpret_cast<PfnSetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    g_flush_menu_themes =
        reinterpret_cast<PfnFlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
}

} // namespace

void enable_dark_mode_support() {
    bind_dark_mode_api();
    if (g_set_app_mode == nullptr) return;

    // AllowDark, not ForceDark: the light theme has to keep working, and Force
    // would darken controls even while the light palette is installed.
    g_set_app_mode(g_theme_is_dark ? PreferredAppMode::AllowDark : PreferredAppMode::Default);
    if (g_flush_menu_themes != nullptr) g_flush_menu_themes();
}

// Windows exposes the dark title bar through a DWM attribute whose number
// changed: 19 before Windows 10 20H1, 20 from 20H1 on. Try the modern one and
// fall back, because passing the wrong one is a silent no-op and a light title
// bar over a dark window reads as a bug rather than as an unsupported OS.
void apply_titlebar_theme(HWND hwnd) {
    using PfnSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static PfnSetAttr set_attr = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<PfnSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"))
                   : nullptr;
    }();
    if (set_attr == nullptr) return;

    BOOL dark = g_theme_is_dark ? TRUE : FALSE;
    if (FAILED(set_attr(hwnd, 20, &dark, sizeof(dark)))) {
        set_attr(hwnd, 19, &dark, sizeof(dark));
    }
}

namespace {

/// @brief Push the palette into the controls that cache their own colours.
///        Static text and edits are recoloured per-paint through WM_CTLCOLOR*;
///        list and tree views are not -- they keep whatever they were last told.
BOOL CALLBACK retheme_cb(HWND child, LPARAM) {
    wchar_t cls[64] = {0};
    GetClassNameW(child, cls, 64);

    if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
        ListView_SetBkColor(child, kColPanel);
        ListView_SetTextBkColor(child, kColPanel);
        ListView_SetTextColor(child, kColText);
    } else if (lstrcmpiW(cls, WC_TREEVIEWW) == 0) {
        TreeView_SetBkColor(child, kColPanel);
        TreeView_SetTextColor(child, kColText);
    }

    if (lstrcmpiW(cls, WC_EDITW) == 0) {
        // Edits are the awkward case. A themed edit paints its own white client
        // area and ignores the brush returned from WM_CTLCOLOREDIT, and
        // DarkMode_CFD -- which is supposed to fix that -- is unreliable: it
        // depends on an undocumented opt-in and does nothing on some builds.
        // Removing the visual style outright is not elegant, but it is the one
        // thing that works everywhere: an unthemed edit honours the brush.
        SetWindowTheme(child, g_theme_is_dark ? L"" : L"Explorer",
                       g_theme_is_dark ? L"" : nullptr);
    } else {
        // DarkMode_Explorer is what makes a common control's scroll bars and
        // header dark; plain Explorer puts the light ones back.
        SetWindowTheme(child, g_theme_is_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    InvalidateRect(child, nullptr, TRUE);
    return TRUE;
}

} // namespace

void refresh_theme(HWND hwnd) {
    apply_titlebar_theme(hwnd);
    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(theme_brush(kColBg)));
    EnumChildWindows(hwnd, retheme_cb, 0);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
}

// ---- persistence -----------------------------------------------------------
//
// HKCU rather than a file beside the exe: the exe usually lives in a build tree
// that gets deleted, and a theme is a per-user preference, not a per-checkout one.

void save_theme_preference() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD mode = static_cast<DWORD>(g_theme_mode);
    DWORD accent = static_cast<DWORD>(g_theme_custom_accent);
    RegSetValueExW(key, L"ThemeMode", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&mode), sizeof(mode));
    RegSetValueExW(key, L"ThemeAccent", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&accent), sizeof(accent));
    RegCloseKey(key);
}

void load_theme_preference() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        apply_theme(ThemeMode::Dark);
        return;
    }
    DWORD mode = static_cast<DWORD>(ThemeMode::Dark);
    DWORD accent = static_cast<DWORD>(g_theme_custom_accent);
    DWORD size = sizeof(DWORD);
    DWORD type = 0;
    if (RegQueryValueExW(key, L"ThemeMode", nullptr, &type, reinterpret_cast<BYTE*>(&mode),
                         &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        mode = static_cast<DWORD>(ThemeMode::Dark);
    }
    size = sizeof(DWORD);
    type = 0;
    if (RegQueryValueExW(key, L"ThemeAccent", nullptr, &type, reinterpret_cast<BYTE*>(&accent),
                         &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        accent = static_cast<DWORD>(g_theme_custom_accent);
    }
    RegCloseKey(key);

    // A value written by a newer build must not leave the palette uninstalled.
    if (mode > static_cast<DWORD>(ThemeMode::Custom)) {
        mode = static_cast<DWORD>(ThemeMode::Dark);
    }
    apply_theme(static_cast<ThemeMode>(mode), static_cast<COLORREF>(accent));
}

} // namespace castlemist::ui
