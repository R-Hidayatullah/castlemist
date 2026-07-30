/// @file
/// @brief The UI palette and the theme switch.
///
/// This used to live in `src/ui/detail/app_state.h`, private to the shell, and
/// the self-contained widgets (the texture panel, the info panel) each kept
/// their own copy of the colours. That was fine while there was one palette and
/// it never changed. It stopped being fine the moment a theme could be switched
/// at runtime: a duplicated constant does not follow the theme, so the texture
/// strip stayed light inside a dark window.
///
/// The palette is therefore shared vocabulary, in a public header, and the
/// widgets read the same variables the shell does.

#ifndef CASTLEMIST_UI_THEME_H
#define CASTLEMIST_UI_THEME_H

#include <windows.h>

namespace castlemist::ui {

/// @brief Which palette is in force.
enum class ThemeMode {
    Dark,   ///< the default
    Light,
    Custom, ///< a base palette re-tinted around a user-chosen accent
};

// ---------------------------------------------------------------------------
// The palette.
//
// Variables, not constants: a theme switch is exactly a change to these. Every
// drawing site reads them by name, which is why switching themes needed no
// change at any call site -- it is one assignment per colour and a repaint.
//
// Assign only through apply_theme().
// ---------------------------------------------------------------------------

inline COLORREF kColBg     = RGB(0x1B, 0x1F, 0x26); ///< window background
inline COLORREF kColPanel  = RGB(0x24, 0x29, 0x33); ///< editors, lists
inline COLORREF kColBand   = RGB(0x15, 0x18, 0x1E); ///< toolbar, status band
inline COLORREF kColText   = RGB(0xE6, 0xEA, 0xF0); ///< primary text
inline COLORREF kColSubtle = RGB(0x98, 0xA3, 0xB3); ///< secondary text
inline COLORREF kColAccent = RGB(0x4C, 0x93, 0xFF); ///< accent
inline COLORREF kColCard   = RGB(0x2B, 0x31, 0x3C); ///< overlay readout card
inline COLORREF kColBorder = RGB(0x39, 0x40, 0x4C); ///< hairline borders
inline COLORREF kColHover  = RGB(0x2E, 0x3A, 0x4E); ///< hovered row or chip

/// @brief The mode currently applied. Read to place the menu's radio mark.
inline ThemeMode g_theme_mode = ThemeMode::Dark;
/// @brief The accent chosen for ThemeMode::Custom.
inline COLORREF g_theme_custom_accent = RGB(0x4C, 0x93, 0xFF);
/// @brief True while a dark palette is in force. Drives the DWM title bar and
///        the `DarkMode_Explorer` control theme.
inline bool g_theme_is_dark = true;

/// @brief A cached solid brush for @p c. Owned by the cache; do not delete.
HBRUSH theme_brush(COLORREF c);

/// @brief Install a palette. @p accent applies only to ThemeMode::Custom.
void apply_theme(ThemeMode mode, COLORREF accent = 0);

/// @brief Tell uxtheme this process wants dark common controls.
///        Called by apply_theme(); the DarkMode_* window themes are ignored
///        without it. No-op where the (undocumented, ordinal-only) export is
///        missing -- controls just stay light.
void enable_dark_mode_support();

/// @brief Re-theme @p hwnd and every descendant, then repaint.
///        Call after apply_theme(); list and tree views cache their colours and
///        will not pick up a new palette on their own.
void refresh_theme(HWND hwnd);

/// @brief Ask DWM to draw @p hwnd's title bar dark. No-op before Windows 10 1809.
void apply_titlebar_theme(HWND hwnd);

/// @brief Read the saved theme from HKCU and apply it. Call once, before the
///        first window class is registered, so the first paint is already themed.
void load_theme_preference();

/// @brief Persist the current theme to HKCU.
void save_theme_preference();

} // namespace castlemist::ui

#endif // CASTLEMIST_UI_THEME_H
