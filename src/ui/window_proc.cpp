/// @file
/// @brief The menu bar and the three window procedures.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <cstdio>
#include <thread>
#include <windowsx.h>

#include "castlemist/format/struct_template.h"
#include "castlemist/render/gw2bgfx_view.h"
#include "castlemist/ui/struct_tree.h"

namespace castlemist::ui {

HMENU build_menu() {
    g_file_menu = CreatePopupMenu();
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_OPEN, L"&Open .dat...");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_OPEN_INDEX, L"Open &Index DB... (gw2index/gw2local)");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_OPEN_LOOSE, L"Open &File... (outside a .dat)");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_BUILD_INDEX, L"&Build Index DB from .dat...");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_LOAD_TEMPLATE, L"Load Struct &JSON... (gw2_packfile.json)");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_LOAD_KEYS, L"Load String &Keys... (textkeys.csv)");
    AppendMenuW(g_file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_EXPORT_COMPRESSED, L"Export &Compressed (Before)...");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_EXPORT_DECOMPRESSED, L"Export &Decompressed (After)...");
    AppendMenuW(g_file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_EXIT, L"E&xit");
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_COMPRESSED, MF_GRAYED | MF_DISABLED);
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_DECOMPRESSED, MF_GRAYED | MF_DISABLED);

    HMENU tools_menu = CreatePopupMenu();
    AppendMenuW(tools_menu, MF_STRING, ID_TOOLS_DECODE_LINK, L"&Decode Chat Link... ([&...])");
    AppendMenuW(tools_menu, MF_STRING, ID_TOOLS_DECODE_TOKEN,
                L"Decode &Token / Filename Bytes...");

    g_theme_menu = CreatePopupMenu();
    AppendMenuW(g_theme_menu, MF_STRING, ID_VIEW_THEME_DARK, L"&Dark");
    AppendMenuW(g_theme_menu, MF_STRING, ID_VIEW_THEME_LIGHT, L"&Light");
    AppendMenuW(g_theme_menu, MF_STRING, ID_VIEW_THEME_CUSTOM, L"&Custom");
    AppendMenuW(g_theme_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_theme_menu, MF_STRING, ID_VIEW_THEME_ACCENT, L"Choose &Accent Colour...");

    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_theme_menu), L"&Theme");

    HMENU menu_bar = CreateMenu();
    AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_file_menu), L"&File");
    AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"&View");
    AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools_menu), L"&Tools");
    return menu_bar;
}

// The radio mark follows g_theme_mode rather than being toggled at the click
// site, so the menu is still right after load_theme_preference() restores a
// theme nobody clicked on.
void sync_theme_menu() {
    if (g_theme_menu == nullptr) return;
    CheckMenuRadioItem(g_theme_menu, ID_VIEW_THEME_DARK, ID_VIEW_THEME_CUSTOM,
                       g_theme_mode == ThemeMode::Light    ? ID_VIEW_THEME_LIGHT
                       : g_theme_mode == ThemeMode::Custom ? ID_VIEW_THEME_CUSTOM
                                                           : ID_VIEW_THEME_DARK,
                       MF_BYCOMMAND);
}

/// @brief Switch theme, repaint everything and remember the choice.
static void set_theme(HWND hwnd, ThemeMode mode, COLORREF accent = 0) {
    apply_theme(mode, accent);
    sync_theme_menu();
    refresh_theme(hwnd);
    save_theme_preference();
}

/// @brief The standard colour picker, seeded with the current accent.
static void do_choose_accent(HWND hwnd) {
    // Custom colours persist for the life of the dialog only; static so a second
    // visit still shows what was mixed on the first.
    static COLORREF custom[16] = {0};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hwnd;
    cc.rgbResult = g_theme_custom_accent;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        set_theme(hwnd, ThemeMode::Custom, cc.rgbResult);
    }
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        // The subtitle band: white text on a dark plate, like a video player.
        if (g_app != nullptr && reinterpret_cast<HWND>(lparam) == g_app->hwnd_video_subtitle) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkColor(dc, RGB(16, 16, 18));
            return reinterpret_cast<LRESULT>(theme_brush(RGB(16, 16, 18)));
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        castlemist::gfx::render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL: {
        if (g_app == nullptr) {
            break;
        }
        int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        zoom_by(std::pow(1.1f, static_cast<float>(notches)));
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (g_app != nullptr) {
            g_app->preview_dragging = true;
            g_app->preview_drag_last = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_app != nullptr && g_app->preview_dragging) {
            POINT cur{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT rc;
            GetClientRect(hwnd, &rc);
            float w = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
            float h = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
            g_app->preview_pan_x +=
                static_cast<float>(g_app->preview_drag_last.x - cur.x) / w / g_app->preview_zoom;
            g_app->preview_pan_y +=
                static_cast<float>(g_app->preview_drag_last.y - cur.y) / h / g_app->preview_zoom;
            g_app->preview_drag_last = cur;
            castlemist::gfx::set_view(g_app->preview_zoom, g_app->preview_pan_x, g_app->preview_pan_y);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_app != nullptr && g_app->preview_dragging) {
            g_app->preview_dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        fit_view();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/// Hands the selected entry to the "Game 1:1" surface.
///
/// The bgfx view loads straight out of the archive by MFT index rather than
/// from the ModelPreview castlemist::render consumes -- that is the point of
/// it: nothing between the .dat and the draw call has been reinterpreted, so
/// what it shows can be trusted as a reference for the other renderer.
void sync_bgfx_view() {
    if (g_app == nullptr || !castlemist::gw2bgfxview::available()) return;
    if (!g_app->bgfx_view_active || g_app->hwnd_model_bgfx == nullptr) return;
    if (g_app->bgfx_model_loaded) return;

    if (!castlemist::gw2bgfxview::initialize(g_app->hwnd_model_bgfx)) {
        SetWindowTextW(g_app->hwnd_text_preview,
                       L"Game 1:1 view: bgfx could not initialise on this machine.");
        g_app->bgfx_model_loaded = true; // do not retry every paint
        return;
    }
    if (!g_app->has_loaded_entry) return;

    std::string error;
    // mft_index is the archive row; the view wants exactly that (baseId - 1).
    if (!castlemist::gw2bgfxview::set_model(g_app->data_gw2, g_app->current_mft_index, error)) {
        castlemist::gw2bgfxview::clear_model();
    }
    g_app->bgfx_model_loaded = true;
    InvalidateRect(g_app->hwnd_model_bgfx, nullptr, FALSE);
}

/// The "Game 1:1" surface. Deliberately spare next to ModelWndProc: no gizmo,
/// no picking, no transform card -- this view exists to show the model the way
/// the client would draw it, and every extra control is another thing standing
/// between the archive and the pixels.
LRESULT CALLBACK BgfxWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        sync_bgfx_view();
        castlemist::gw2bgfxview::render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        castlemist::gw2bgfxview::on_resize(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_MOUSEWHEEL: {
        int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        castlemist::gw2bgfxview::zoom(std::pow(0.9f, static_cast<float>(notches)));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (g_app != nullptr) {
            g_app->bgfx_dragging = true;
            g_app->bgfx_drag_last = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_app != nullptr && g_app->bgfx_dragging) {
            POINT cur{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // cur - last, NOT last - cur. ModelWndProc negates both deltas
            // because the Direct3D 11 renderer's camera sits on the other side
            // of its lookAt convention; this view does not share it. Measured
            // through this view's own matrices: a positive dyaw carries the
            // face nearest the camera to the RIGHT, and a positive dpitch
            // carries it DOWN, which is exactly a cursor moving right and down.
            // Copying the negation from ModelWndProc inverted both axes and the
            // model ran away from the mouse.
            castlemist::gw2bgfxview::orbit(
                static_cast<float>(cur.x - g_app->bgfx_drag_last.x) * 0.01f,
                static_cast<float>(cur.y - g_app->bgfx_drag_last.y) * 0.01f);
            g_app->bgfx_drag_last = cur;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_app != nullptr) g_app->bgfx_dragging = false;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        // Capture can be taken away without a WM_LBUTTONUP ever arriving (an
        // alt-tab mid-drag, a modal dialog). Without this the drag flag stays
        // set and every later mouse move keeps spinning the model with no
        // button held.
        if (g_app != nullptr) g_app->bgfx_dragging = false;
        return 0;
    case WM_RBUTTONDOWN:
        // Right-click flips the model 180 deg about X. Characters' armour is
        // stored in a bind-pose space whose root rotation lives in a skeleton
        // this view does not read, so those arrive upside down; props do not.
        // Rather than guess a rule that is wrong half the time, this is the
        // one-click correction.
        if (g_app != nullptr) {
            float t[3];
            castlemist::gw2bgfxview::rotation_trim(t);
            castlemist::gw2bgfxview::set_rotation_trim(t[0] == 0.0f ? 180.0f : 0.0f, t[1], t[2]);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK ModelWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        castlemist::render::render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC: {
        // The transform readout card (child of this surface): dark-on-light card.
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, OPAQUE);
        SetTextColor(dc, kColText);
        SetBkColor(dc, kColCard);
        return reinterpret_cast<LRESULT>(theme_brush(kColCard));
    }
    case WM_MOUSEWHEEL: {
        if (g_app == nullptr) {
            break;
        }
        int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        castlemist::render::zoom(std::pow(0.9f, static_cast<float>(notches))); // wheel up -> closer
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (g_app != nullptr) {
            int mx = GET_X_LPARAM(lparam), my = GET_Y_LPARAM(lparam);
            // A gizmo handle under the cursor grabs the click (edit the object
            // transform); otherwise the drag orbits the camera as before.
            if (castlemist::render::gizmo_begin(mx, my)) {
                g_app->gizmo_dragging = true;
                update_gizmo_readout();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                g_app->model_dragging = true;
                g_app->model_drag_last = POINT{mx, my};
                g_app->model_down = POINT{mx, my};
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_app != nullptr && g_app->gizmo_dragging) {
            castlemist::render::gizmo_update(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            update_gizmo_readout();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_app != nullptr && g_app->model_dragging) {
            POINT cur{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // Negate both deltas so the model follows the cursor (drag right ->
            // spins right, drag down -> tilts down) instead of the inverted default.
            castlemist::render::orbit(static_cast<float>(g_app->model_drag_last.x - cur.x) * 0.01f,
                          static_cast<float>(g_app->model_drag_last.y - cur.y) * 0.01f);
            g_app->model_drag_last = cur;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_app != nullptr && castlemist::render::gizmo_mode() != castlemist::render::GizmoMode::None) {
            // Hover highlight of the gizmo handle under the cursor.
            int axis = castlemist::render::gizmo_pick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (axis != g_app->gizmo_hover_axis) {
                g_app->gizmo_hover_axis = axis;
                castlemist::render::set_gizmo_hover(axis);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_app != nullptr && g_app->gizmo_dragging) {
            castlemist::render::gizmo_end();
            g_app->gizmo_dragging = false;
            update_gizmo_readout();
            ReleaseCapture();
        } else if (g_app != nullptr && g_app->model_dragging) {
            g_app->model_dragging = false;
            ReleaseCapture();
            // Map mode: a click that didn't really drag picks the prop under the
            // cursor into the small inset preview (a drag is an orbit, not a pick).
            if (g_app->map_pick_enabled && castlemist::render::scene_active()) {
                int ux = GET_X_LPARAM(lparam), uy = GET_Y_LPARAM(lparam);
                if (std::abs(ux - g_app->model_down.x) < 5 && std::abs(uy - g_app->model_down.y) < 5) {
                    int m = castlemist::render::pick_scene_instance(ux, uy);
                    if (m >= 0) {
                        castlemist::render::set_scene_focus(m);
                        castlemist::render::set_show_focus(true);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
        }
        return 0;
    case WM_RBUTTONUP:
        // Right-click clears the map inset preview.
        if (g_app != nullptr && castlemist::render::scene_active() && castlemist::render::scene_focus() >= 0) {
            castlemist::render::set_scene_focus(-1);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        reset_model_view();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        g_app = new AppState();
        g_app->hwnd_main = hwnd;

        g_app->hwnd_search_edit =
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_EDIT)), g_hinstance, nullptr);
        g_app->hwnd_search_fileid_check = CreateWindowExW(
            0, L"BUTTON", L"By File ID", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_FILEID_CHECK)), g_hinstance, nullptr);
        g_app->hwnd_search_button =
            CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_SEARCH_BUTTON), g_hinstance, nullptr);
        g_app->hwnd_clear_button =
            CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_CLEAR_BUTTON), g_hinstance, nullptr);
        // Type/Container filter combos (hidden until an index DB is loaded).
        g_app->hwnd_filter_type = CreateWindowExW(
            0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FILTER_TYPE)), g_hinstance, nullptr);
        g_app->hwnd_filter_container = CreateWindowExW(
            0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FILTER_CONTAINER)), g_hinstance, nullptr);
        // "What does it actually contain" -- the container fourcc alone is too
        // coarse (see castlemist::db::ContentFilter). Own row: the labels are long.
        g_app->hwnd_filter_content = CreateWindowExW(
            0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FILTER_CONTENT)), g_hinstance, nullptr);

        g_app->hwnd_list = castlemist::mft::create(hwnd, g_hinstance, ID_LISTVIEW);
        castlemist::mft::set_selection_callback(g_app->hwnd_list, on_entry_selected);

        g_app->hwnd_split_list_middle =
            castlemist::ui::create_splitter(hwnd, g_hinstance, ID_SPLIT_LIST_MIDDLE, castlemist::ui::SplitOrientation::Vertical);
        castlemist::ui::set_drag_callback(g_app->hwnd_split_list_middle, [](int delta) {
            RECT rc;
            GetClientRect(g_app->hwnd_main, &rc);
            int client_w = rc.right - rc.left;
            if (client_w <= 0) {
                return;
            }
            g_app->list_width_ratio =
                std::clamp(g_app->list_width_ratio + static_cast<double>(delta) / client_w, 0.08, 0.6);
            relayout();
        });

        // cntc content-browser splitters (only visible in content mode): a vertical
        // one resizing the Types|Entries|Assets block vs the preview, and a
        // horizontal one resizing the Assets table vs the Types/Entries tables.
        g_app->hwnd_split_content =
            castlemist::ui::create_splitter(hwnd, g_hinstance, ID_SPLIT_CONTENT, castlemist::ui::SplitOrientation::Vertical);
        castlemist::ui::set_drag_callback(g_app->hwnd_split_content, [](int delta) {
            g_app->content_browser_w = std::clamp(g_app->content_browser_w + delta, 240, 1200);
            relayout();
        });
        g_app->hwnd_split_content_h =
            castlemist::ui::create_splitter(hwnd, g_hinstance, ID_SPLIT_CONTENT_H, castlemist::ui::SplitOrientation::Horizontal);
        castlemist::ui::set_drag_callback(g_app->hwnd_split_content_h, [](int delta) {
            RECT rc; GetClientRect(g_app->hwnd_main, &rc);
            int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
            // Drag down (delta > 0) grows the top tables, shrinks the Assets table.
            g_app->content_assets_ratio =
                std::clamp(g_app->content_assets_ratio - static_cast<double>(delta) / h, 0.12, 0.80);
            relayout();
        });

        g_app->hwnd_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TAB)), g_hinstance, nullptr);
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(L"Compressed");
            TabCtrl_InsertItem(g_app->hwnd_tab, 0, &item);
            item.pszText = const_cast<wchar_t*>(L"Decompressed");
            TabCtrl_InsertItem(g_app->hwnd_tab, 1, &item);
            item.pszText = const_cast<wchar_t*>(L"Structure");
            TabCtrl_InsertItem(g_app->hwnd_tab, 2, &item);
            item.pszText = const_cast<wchar_t*>(L"Preview");
            TabCtrl_InsertItem(g_app->hwnd_tab, 3, &item);
            TabCtrl_SetCurSel(g_app->hwnd_tab, static_cast<int>(MiddleTab::Preview));
        }

        // WS_CLIPCHILDREN so the subtitle overlay child survives the D3D present.
        g_app->hwnd_preview =
            CreateWindowExW(WS_EX_CLIENTEDGE, kPreviewClassName, L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd, nullptr,
                            g_hinstance, nullptr);
        g_app->hwnd_zoom_in =
            CreateWindowExW(0, L"BUTTON", L"Zoom In", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ZOOM_IN), g_hinstance, nullptr);
        g_app->hwnd_zoom_out =
            CreateWindowExW(0, L"BUTTON", L"Zoom Out", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ZOOM_OUT), g_hinstance, nullptr);
        g_app->hwnd_rotate =
            CreateWindowExW(0, L"BUTTON", L"Rotate", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ROTATE), g_hinstance, nullptr);
        g_app->hwnd_fit = CreateWindowExW(0, L"BUTTON", L"Fit", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(ID_FIT), g_hinstance, nullptr);
        // Alpha on/off: push-like checkbox. Checked (default) = composite over a
        // checkerboard honoring alpha; unchecked = show raw RGB, ignoring alpha.
        g_app->hwnd_alpha =
            CreateWindowExW(0, L"BUTTON", L"Alpha", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ALPHA_TOGGLE), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_alpha, BM_SETCHECK, castlemist::gfx::alpha_aware() ? BST_CHECKED : BST_UNCHECKED, 0);

        // Model preview surface (its own D3D device/swapchain) + mode buttons.
        g_app->hwnd_model = CreateWindowExW(WS_EX_CLIENTEDGE, kModelClassName, L"",
                                             WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd, nullptr,
                                             g_hinstance, nullptr);
        // The "Game 1:1" surface sits beside it, hidden until the mode is
        // picked. Separate windows because bgfx creates its own D3D11 device
        // and cannot share the one castlemist::render already owns.
        if (castlemist::gw2bgfxview::available()) {
            g_app->hwnd_model_bgfx =
                CreateWindowExW(WS_EX_CLIENTEDGE, kBgfxClassName, L"",
                                 WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd,
                                 nullptr, g_hinstance, nullptr);
            g_app->hwnd_mode_gw2bgfx =
                CreateWindowExW(0, L"BUTTON", L"Game 1:1", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                                 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_MODE_GW2BGFX),
                                 g_hinstance, nullptr);
        }
        g_app->hwnd_mode_full =
            CreateWindowExW(0, L"BUTTON", L"Full", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_FULL), g_hinstance, nullptr);
        g_app->hwnd_mode_plain =
            CreateWindowExW(0, L"BUTTON", L"Plain", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_PLAIN), g_hinstance, nullptr);
        g_app->hwnd_mode_wire =
            CreateWindowExW(0, L"BUTTON", L"Wireframe", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_WIRE), g_hinstance, nullptr);
        // Real game shaders (bgfx DXBC from each material's AMAT package).
        g_app->hwnd_mode_shader =
            CreateWindowExW(0, L"BUTTON", L"Shader", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_SHADER), g_hinstance, nullptr);
        g_app->hwnd_model_reset =
            CreateWindowExW(0, L"BUTTON", L"Reset", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODEL_RESET), g_hinstance, nullptr);
        // Skeleton on/off: a push-like checkbox so the button stays lit while on.
        g_app->hwnd_skel_toggle =
            CreateWindowExW(0, L"BUTTON", L"Skeleton", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_SKEL), g_hinstance, nullptr);
        // Animation clip selector + play/pause (populated per model).
        g_app->hwnd_anim_combo =
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ANIM_COMBO), g_hinstance, nullptr);
        g_app->hwnd_anim_play =
            CreateWindowExW(0, L"BUTTON", L"Play", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ANIM_PLAY), g_hinstance, nullptr);
        // Texture resolution: full-res (default, checked) vs the reduced half-size
        // sibling. Toggling reloads the current model with the chosen resolution.
        g_app->hwnd_tex_fullres =
            CreateWindowExW(0, L"BUTTON", L"Full-res tex", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_TEX_FULLRES), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_tex_fullres, BM_SETCHECK, BST_CHECKED, 0);
        // Light pre-pass toggle (Shader mode): real deferred directional lighting
        // vs the flat light-buffer stand-in. Defaults ON to match the renderer.
        g_app->hwnd_light_toggle =
            CreateWindowExW(0, L"BUTTON", L"Light", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LIGHT_PREPASS), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_light_toggle, BM_SETCHECK,
                     castlemist::render::lightprepass() ? BST_CHECKED : BST_UNCHECKED, 0);
        g_app->hwnd_effects_toggle =
            CreateWindowExW(0, L"BUTTON", L"Effects", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_EFFECTS_TOGGLE), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_effects_toggle, BM_SETCHECK,
                     castlemist::render::show_effects() ? BST_CHECKED : BST_UNCHECKED, 0);
        // Live cloth simulation on/off (push-like; shown only when the model can
        // be simulated). Drapes the mesh from its pinned top band under gravity.
        g_app->hwnd_cloth_toggle =
            CreateWindowExW(0, L"BUTTON", L"Cloth", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_CLOTH_TOGGLE), g_hinstance, nullptr);
        // Model light-intensity slider (Full/Plain reconstruction): 0.0 .. 3.0, so a
        // dark model can be lifted / a bright one dimmed. The multi-directional env
        // light keeps every orientation readable; this scales its overall level.
        g_app->hwnd_light_label =
            CreateWindowExW(0, L"STATIC", L"Light", WS_CHILD | SS_LEFT, 0, 0, 0, 0, hwnd, nullptr, g_hinstance,
                            nullptr);
        g_app->hwnd_light_slider =
            CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(ID_LIGHT_SLIDER), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_light_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 300));
        SendMessageW(g_app->hwnd_light_slider, TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(castlemist::render::light_intensity() * 100.0f));
        // Headlight controls: a "Follow cam" toggle (light stays behind the camera so
        // the model never darkens as you orbit) + an angle slider that swings the
        // light from directly behind the camera (bright front) to grazing (darker).
        g_app->hwnd_light_follow =
            CreateWindowExW(0, L"BUTTON", L"Follow cam", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LIGHT_FOLLOW), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_light_follow, BM_SETCHECK, castlemist::render::light_follow() ? BST_CHECKED : BST_UNCHECKED, 0);
        g_app->hwnd_light_angle =
            CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(ID_LIGHT_ANGLE), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_light_angle, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(g_app->hwnd_light_angle, TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(castlemist::render::light_angle() * 100.0f));
        // "GW2 rig": light a lone skin with the game's own Equipment-Preview light
        // rig (8-light studio table read out of the client, verified against a
        // capture) instead of our calibrated daylight stand-in. On by default.
        g_app->hwnd_light_gw2rig =
            CreateWindowExW(0, L"BUTTON", L"GW2 rig", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(ID_LIGHT_GW2RIG), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_light_gw2rig, BM_SETCHECK,
                     castlemist::render::preview_rig() ? BST_CHECKED : BST_UNCHECKED, 0);
        // LOD + texture-size controls (single model, second toolbar row): pick the
        // target submesh ("All" = overall), its LOD level, and full/reduced texture.
        g_app->hwnd_submesh_combo =
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_SUBMESH_COMBO), g_hinstance, nullptr);
        g_app->hwnd_lod_combo =
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LOD_COMBO), g_hinstance, nullptr);
        g_app->hwnd_tex_reduced =
            CreateWindowExW(0, L"BUTTON", L"Reduced tex", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_TEX_REDUCED), g_hinstance, nullptr);
        // Blender-style transform gizmo: Move / Rotate / Scale mode buttons (push-
        // like radio group), a Grid toggle, and a Reset. Move is the default mode.
        g_app->hwnd_gizmo_move =
            CreateWindowExW(0, L"BUTTON", L"Move", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_GIZMO_MOVE), g_hinstance, nullptr);
        g_app->hwnd_gizmo_rotate =
            CreateWindowExW(0, L"BUTTON", L"Rotate", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_GIZMO_ROTATE), g_hinstance, nullptr);
        g_app->hwnd_gizmo_scale =
            CreateWindowExW(0, L"BUTTON", L"Scale", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_GIZMO_SCALE), g_hinstance, nullptr);
        g_app->hwnd_gizmo_grid =
            CreateWindowExW(0, L"BUTTON", L"Grid", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_GIZMO_GRID), g_hinstance, nullptr);
        g_app->hwnd_gizmo_reset =
            CreateWindowExW(0, L"BUTTON", L"Reset XF", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_GIZMO_RESET), g_hinstance, nullptr);
        // Toggle for the per-submesh texture panel in the bottom-right column.
        g_app->hwnd_tex_panel =
            CreateWindowExW(0, L"BUTTON", L"Textures", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_TEX_PANEL), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_gizmo_move, BM_SETCHECK,
                     castlemist::render::gizmo_mode() == castlemist::render::GizmoMode::Translate ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_app->hwnd_gizmo_rotate, BM_SETCHECK,
                     castlemist::render::gizmo_mode() == castlemist::render::GizmoMode::Rotate ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_app->hwnd_gizmo_scale, BM_SETCHECK,
                     castlemist::render::gizmo_mode() == castlemist::render::GizmoMode::Scale ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_app->hwnd_gizmo_grid, BM_SETCHECK, castlemist::render::show_grid() ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_app->hwnd_tex_panel, BM_SETCHECK, g_app->show_tex_panel ? BST_CHECKED : BST_UNCHECKED, 0);
        // Transform readout overlay: a child of the 3D surface itself so the DXGI
        // present (blt, DISCARD) is clipped around it (parent has WS_CLIPCHILDREN)
        // and it composites reliably at the surface top-left. Fixed at (10,8).
        g_app->hwnd_gizmo_readout =
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 10, 8, 214, 92, g_app->hwnd_model, nullptr,
                            g_hinstance, nullptr);
        // Audio playback controls (shown only for ASND/audio entries).
        g_app->hwnd_audio_play =
            CreateWindowExW(0, L"BUTTON", L"▶ Play", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_AUDIO_PLAY), g_hinstance, nullptr);
        g_app->hwnd_audio_stop =
            CreateWindowExW(0, L"BUTTON", L"■ Stop", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_AUDIO_STOP), g_hinstance, nullptr);
        g_app->hwnd_audio_combo =
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_AUDIO_COMBO), g_hinstance, nullptr);
        // Playback position / seek trackbar + a "m:ss / m:ss" time readout.
        g_app->hwnd_audio_seek =
            CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_AUDIO_SEEK), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_audio_seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, kAudioSeekMax));
        SendMessageW(g_app->hwnd_audio_seek, TBM_SETPOS, TRUE, 0);
        g_app->hwnd_audio_time =
            CreateWindowExW(0, L"STATIC", L"0:00 / 0:00", WS_CHILD | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd,
                             nullptr, g_hinstance, nullptr);
        // Bink video transport (shown only for KB2*/BIK* cinematic entries).
        g_app->hwnd_video_play =
            CreateWindowExW(0, L"BUTTON", L"▶ Play", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_PLAY), g_hinstance, nullptr);
        g_app->hwnd_video_stop =
            CreateWindowExW(0, L"BUTTON", L"■ Stop", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_STOP), g_hinstance, nullptr);
        g_app->hwnd_video_loop =
            CreateWindowExW(0, L"BUTTON", L"Loop", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_LOOP), g_hinstance, nullptr);
        g_app->hwnd_video_mute =
            CreateWindowExW(0, L"BUTTON", L"Mute", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_MUTE), g_hinstance, nullptr);
        g_app->hwnd_video_seek =
            CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_SEEK), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_video_seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, kVideoSeekMax));
        g_app->hwnd_video_volume =
            CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_VOLUME), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_video_volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(g_app->hwnd_video_volume, TBM_SETPOS, TRUE, castlemist::vid::volume());
        g_app->hwnd_video_time =
            CreateWindowExW(0, L"STATIC", L"0:00 / 0:00", WS_CHILD | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd,
                             nullptr, g_hinstance, nullptr);
        g_app->hwnd_video_track =
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_TRACK), g_hinstance, nullptr);
        g_app->hwnd_video_subs =
            CreateWindowExW(0, L"BUTTON", L"Subs", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_SUBS), g_hinstance, nullptr);
        g_app->hwnd_video_clip =
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_VIDEO_CLIP), g_hinstance, nullptr);
        // Subtitle band: a child of the D3D surface, so the DXGI present (blt,
        // DISCARD) is clipped around it -- the same trick the gizmo readout uses
        // over the model surface.
        g_app->hwnd_video_subtitle =
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTER | SS_CENTERIMAGE, 0, 0, 0, 0,
                            g_app->hwnd_preview, nullptr, g_hinstance, nullptr);
        // Map layer toggles (shown only for map/mapc entries).
        g_app->hwnd_layer_prop =
            CreateWindowExW(0, L"BUTTON", L"Props", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LAYER_PROP), g_hinstance, nullptr);
        g_app->hwnd_layer_zone =
            CreateWindowExW(0, L"BUTTON", L"Zones", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LAYER_ZONE), g_hinstance, nullptr);
        g_app->hwnd_layer_coll =
            CreateWindowExW(0, L"BUTTON", L"Collision", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LAYER_COLL), g_hinstance, nullptr);
        // Map: toggle the small inset preview of the prop clicked in the scene.
        g_app->hwnd_map_preview =
            CreateWindowExW(0, L"BUTTON", L"Preview", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MAP_PREVIEW), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_map_preview, BM_SETCHECK, BST_CHECKED, 0);

        // Text / strs preview (read-only, both scrollbars for code/markup).
        g_app->hwnd_text_preview = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0,
            0, 0, hwnd, nullptr, g_hinstance, nullptr);
        // cntc content browser = three sortable report-view tables:
        //   Types (name | count) | Entries (id | assets)   -- side by side, top
        //   Assets (# | fileId | kind)                     -- stacked below them
        // Click a column header to sort by it; each row's lParam holds a stable
        // backing-vector index so selection/sort stay in sync (see the WM_NOTIFY
        // and content_lv_compare handlers).
        const DWORD lv_style = WS_CHILD | WS_VSCROLL | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS;
        const DWORD lv_ex = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER;
        g_app->hwnd_content_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", lv_style, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(ID_CONTENT_LIST), g_hinstance, nullptr);
        g_app->hwnd_content_child = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", lv_style, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(ID_CONTENT_CHILD), g_hinstance, nullptr);
        g_app->hwnd_content_asset_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", lv_style, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(ID_CONTENT_ASSET_LIST), g_hinstance, nullptr);
        ListView_SetExtendedListViewStyle(g_app->hwnd_content_list, lv_ex);
        ListView_SetExtendedListViewStyle(g_app->hwnd_content_child, lv_ex);
        ListView_SetExtendedListViewStyle(g_app->hwnd_content_asset_list, lv_ex);
        lv_add_col(g_app->hwnd_content_list, 0, L"Type", 112);
        lv_add_col(g_app->hwnd_content_list, 1, L"Count", 55);
        lv_add_col(g_app->hwnd_content_child, 0, L"Id", 80);
        lv_add_col(g_app->hwnd_content_child, 1, L"Assets", 50);
        lv_add_col(g_app->hwnd_content_child, 2, L"Slug (id)", 110);
        lv_add_col(g_app->hwnd_content_child, 3, L"Labels", 320);
        lv_add_col(g_app->hwnd_content_asset_list, 0, L"#", 36);
        lv_add_col(g_app->hwnd_content_asset_list, 1, L"FileId", 90);
        lv_add_col(g_app->hwnd_content_asset_list, 2, L"Kind", 110);
        // strs string table -> its own report table (one row per string record).
        g_app->hwnd_strs_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", lv_style, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(ID_STRS_LIST), g_hinstance, nullptr);
        ListView_SetExtendedListViewStyle(g_app->hwnd_strs_list, lv_ex);
        lv_add_col(g_app->hwnd_strs_list, 0, L"#", 60);
        lv_add_col(g_app->hwnd_strs_list, 1, L"textId", 80);
        lv_add_col(g_app->hwnd_strs_list, 2, L"Encoding", 130);
        lv_add_col(g_app->hwnd_strs_list, 3, L"Bytes", 60);
        lv_add_col(g_app->hwnd_strs_list, 4, L"Text", 900);
        {
            static HFONT text_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                                  FIXED_PITCH | FF_MODERN, L"Consolas");
            SendMessageW(g_app->hwnd_text_preview, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
            SendMessageW(g_app->hwnd_text_preview, EM_SETLIMITTEXT, 0, 0);
            SendMessageW(g_app->hwnd_content_list, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
            SendMessageW(g_app->hwnd_content_child, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
            SendMessageW(g_app->hwnd_content_asset_list, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
            SendMessageW(g_app->hwnd_strs_list, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
        }

        g_app->hwnd_hex_before = castlemist::hex::create(hwnd, g_hinstance, ID_HEX_BEFORE, 0, 0, 0, 0);
        g_app->hwnd_hex_after = castlemist::hex::create(hwnd, g_hinstance, ID_HEX_AFTER, 0, 0, 0, 0);

        // "Structure" tab: JSON-struct-template-driven parsed field tree, right
        // after the two hex panels -- see struct_tree.h / BinaryParser.
        // A small toolbar row above the tree lists every chunk actually found
        // inside the CURRENT entry's own packfile (id/offset + fourcc + name --
        // see populate_struct_container_combo()) so picking one jumps straight
        // to that node in the tree below, instead of scrolling/expanding by hand.
        g_app->hwnd_struct_container_label =
            CreateWindowExW(0, L"STATIC", L"Chunk:", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                             nullptr, g_hinstance, nullptr);
        g_app->hwnd_struct_container_combo = CreateWindowExW(
            0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STRUCT_CONTAINER_COMBO)), g_hinstance, nullptr);
        g_app->hwnd_struct_tree =
            castlemist::structtree::create(hwnd, g_hinstance, ID_STRUCT_TREE, 0, 0, 0, 0);

        g_app->hwnd_split_middle_info =
            castlemist::ui::create_splitter(hwnd, g_hinstance, ID_SPLIT_MIDDLE_INFO, castlemist::ui::SplitOrientation::Vertical);
        castlemist::ui::set_drag_callback(g_app->hwnd_split_middle_info, [](int delta) {
            RECT rc;
            GetClientRect(g_app->hwnd_main, &rc);
            int client_w = rc.right - rc.left;
            if (client_w <= 0) {
                return;
            }
            g_app->info_width_ratio =
                std::clamp(g_app->info_width_ratio - static_cast<double>(delta) / client_w, 0.08, 0.6);
            relayout();
        });

        g_app->hwnd_info = castlemist::info::create(hwnd, g_hinstance, ID_INFO_PANEL);

        // Bottom of the right column: the per-submesh texture panel, with a
        // horizontal splitter against the info panel. Both stay hidden unless a
        // model preview with resolvable textures is up (see layout_children).
        g_app->hwnd_split_info_tex =
            castlemist::ui::create_splitter(hwnd, g_hinstance, ID_SPLIT_INFO_TEX, castlemist::ui::SplitOrientation::Horizontal);
        castlemist::ui::set_drag_callback(g_app->hwnd_split_info_tex, [](int delta) {
            RECT rc;
            GetClientRect(g_app->hwnd_main, &rc);
            int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
            // Drag down (delta > 0) grows the info panel and shrinks the textures.
            g_app->tex_panel_ratio =
                std::clamp(g_app->tex_panel_ratio - static_cast<double>(delta) / h, 0.15, 0.85);
            relayout();
        });
        g_app->hwnd_tex_info = castlemist::texpanel::create(hwnd, g_hinstance, ID_TEX_INFO);
        // Double-clicking a texture row reveals that entry in the MFT list, so the
        // panel is a way *into* a texture, not just a readout about it.
        castlemist::texpanel::set_activate_callback(g_app->hwnd_tex_info,
                                           [](uint32_t fileId, uint32_t) { cl_open_fid(fileId); });

        g_app->hwnd_status_label =
            CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS_LABEL)), g_hinstance, nullptr);
        g_app->hwnd_progress =
            CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROGRESS)), g_hinstance, nullptr);

        if (!castlemist::gfx::initialize(g_app->hwnd_preview)) {
            MessageBoxW(hwnd, L"Failed to initialize Direct3D 11. Image preview will be unavailable.",
                        L"castlemist", MB_ICONWARNING);
        }
        if (!castlemist::render::initialize(g_app->hwnd_model)) {
            MessageBoxW(hwnd, L"Failed to initialize Direct3D 11 for the model viewer. 3D preview will be unavailable.",
                        L"castlemist", MB_ICONWARNING);
        }
        // The struct template (gw2_packfile.json) is no longer auto-loaded here:
        // it's a multi-megabyte JSON parse that used to run synchronously on
        // every startup whether or not the user ever opens the Structure tab or
        // a model/map entry. It's now loaded lazily, on the first entry/panel
        // that actually needs it, via castlemist::tpl::get_or_auto_load() --
        // see populate_struct_tree() and entry_extractor.cpp. File -> Load
        // Struct JSON... (do_load_template) still loads it eagerly on demand.
        try_autoload_keys();
        // NB: the gw2_index.db auto-load runs from WinMain after the window is shown
        // (not here) -- it loads the .dat MFT synchronously and must not run inside
        // WM_CREATE, where an exception would abort window creation.

        // Apply the modern Segoe UI shell font to every control at once.
        ensure_ui_fonts();
        EnumChildWindows(hwnd, apply_font_cb, reinterpret_cast<LPARAM>(g_ui_font));
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND ctl = reinterpret_cast<HWND>(lparam);
        SetBkMode(dc, OPAQUE);
        if (g_app && ctl == g_app->hwnd_gizmo_readout) {
            SetTextColor(dc, kColText);
            SetBkColor(dc, kColCard);
            return reinterpret_cast<LRESULT>(theme_brush(kColCard));
        }
        if (g_app && ctl == g_app->hwnd_status_label) {
            SetTextColor(dc, kColSubtle);
            SetBkColor(dc, kColBand);
            return reinterpret_cast<LRESULT>(theme_brush(kColBand));
        }
        SetTextColor(dc, kColText);
        SetBkColor(dc, kColBg);
        return reinterpret_cast<LRESULT>(theme_brush(kColBg));
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kColText);
        SetBkColor(dc, kColPanel);
        return reinterpret_cast<LRESULT>(theme_brush(kColPanel));
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kColText);
        SetBkColor(dc, kColPanel);
        return reinterpret_cast<LRESULT>(theme_brush(kColPanel));
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, kColBg);
        return reinterpret_cast<LRESULT>(theme_brush(kColBg));
    }
    case WM_SIZE:
        if (g_app != nullptr) {
            layout_children(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_HSCROLL:
        // Light-intensity trackbar: rescale the reconstruction environment light.
        if (g_app && reinterpret_cast<HWND>(lparam) == g_app->hwnd_light_slider) {
            int pos = static_cast<int>(SendMessageW(g_app->hwnd_light_slider, TBM_GETPOS, 0, 0));
            castlemist::render::set_light_intensity(pos / 100.0f);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        // Headlight angle trackbar: swing the camera-relative light behind..over.
        if (g_app && reinterpret_cast<HWND>(lparam) == g_app->hwnd_light_angle) {
            int pos = static_cast<int>(SendMessageW(g_app->hwnd_light_angle, TBM_GETPOS, 0, 0));
            castlemist::render::set_light_angle(pos / 100.0f);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        // Audio seek bar: scrub the playback head. Track thumb drag while it's held
        // (so the auto-refresh doesn't fight the user), then seek on release/jump.
        if (g_app && reinterpret_cast<HWND>(lparam) == g_app->hwnd_audio_seek) {
            int code = LOWORD(wparam);
            g_app->audio_seek_dragging = (code == TB_THUMBTRACK);
            int permille = static_cast<int>(SendMessageW(g_app->hwnd_audio_seek, TBM_GETPOS, 0, 0));
            double dur = g_app->audio_sel_info.ok ? g_app->audio_sel_info.seconds
                                                  : castlemist::snd::duration_seconds();
            double target = dur * permille / kAudioSeekMax;
            // Live time readout while dragging; commit the seek when the thumb settles.
            SetWindowTextW(g_app->hwnd_audio_time,
                           (castlemist::core::format_mmss(target) + L" / " + castlemist::core::format_mmss(dur)).c_str());
            if (code != TB_THUMBTRACK) {
                // Scrubbing before pressing Play: load the selected clip first, so the
                // bar acts as "start here" rather than doing nothing.
                if (castlemist::snd::duration_seconds() <= 0) {
                    const auto& clips = active_audio_entry().audio_clips;
                    int sel = audio_selected_index();
                    if (sel >= 0 && sel < static_cast<int>(clips.size()))
                        castlemist::snd::play(clips[sel].data.data(), clips[sel].data.size());
                }
                if (castlemist::snd::seek(target))
                    SetTimer(g_app->hwnd_main, TIMER_AUDIO, 100, nullptr);
            }
            return 0;
        }
        // Video seek bar: same pattern, but in FRAMES. The actual BinkGoto only
        // fires when the thumb settles -- seeking on every THUMBTRACK message
        // would decode a key frame per mouse pixel.
        if (g_app && reinterpret_cast<HWND>(lparam) == g_app->hwnd_video_seek) {
            int code = LOWORD(wparam);
            g_app->video_seek_dragging = (code == TB_THUMBTRACK);
            const castlemist::vid::Info& vi = castlemist::vid::info();
            int permille = static_cast<int>(SendMessageW(g_app->hwnd_video_seek, TBM_GETPOS, 0, 0));
            uint32_t target = (vi.frames > 1)
                                  ? static_cast<uint32_t>(static_cast<double>(permille) / kVideoSeekMax *
                                                              (vi.frames - 1) + 0.5)
                                  : 0;
            double dur = castlemist::vid::duration_seconds();
            double tsec = (vi.fps_num > 0) ? static_cast<double>(target) * vi.fps_den / vi.fps_num : 0.0;
            wchar_t t[96];
            swprintf(t, 96, L"%ls / %ls  (%u/%u)", castlemist::core::format_mmss(tsec).c_str(), castlemist::core::format_mmss(dur).c_str(),
                     vi.frames ? target + 1 : 0u, vi.frames);
            SetWindowTextW(g_app->hwnd_video_time, t);
            if (code != TB_THUMBTRACK && castlemist::vid::is_open()) {
                // An exact seek replays the delta chain (single-key-frame files),
                // so a long jump genuinely takes seconds -- say so instead of
                // looking hung.
                double cost = castlemist::vid::seek_cost_estimate_ms(target);
                HCURSOR prev = nullptr;
                if (cost > 250.0) {
                    prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                    wchar_t s[128];
                    swprintf(s, 128, L"Seeking to frame %u — decoding ~%.1f s…", target + 1, cost / 1000.0);
                    SetWindowTextW(g_app->hwnd_status_label, s);
                    UpdateWindow(g_app->hwnd_status_label);
                }
                castlemist::vid::seek_frame(target);
                present_video_frame();
                update_video_ui(false);
                if (prev) {
                    SetCursor(prev);
                    SetWindowTextW(g_app->hwnd_status_label, L"Ready");
                }
            }
            return 0;
        }
        if (g_app && reinterpret_cast<HWND>(lparam) == g_app->hwnd_video_volume) {
            castlemist::vid::set_volume(static_cast<int>(SendMessageW(g_app->hwnd_video_volume, TBM_GETPOS, 0, 0)));
            return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (g_app == nullptr) {
            return 0;
        }
        if (header->hwndFrom == g_app->hwnd_list) {
            return castlemist::mft::handle_notify(g_app->hwnd_list, header);
        }
        if (header->hwndFrom == g_app->hwnd_tab && header->code == TCN_SELCHANGE) {
            relayout();
        }
        if (header->hwndFrom == g_app->hwnd_struct_tree) {
            return castlemist::structtree::handle_notify(g_app->hwnd_struct_tree, header);
        }
        // cntc content-browser tables (Types / Entries / Assets): selection drives
        // the drill-down; column-header clicks sort. Each carries a backing index
        // in its row lParam, so sorting stays consistent with the click handlers.
        HWND from = header->hwndFrom;
        int listIdx = (from == g_app->hwnd_content_list)         ? 0
                      : (from == g_app->hwnd_content_child)       ? 1
                      : (from == g_app->hwnd_content_asset_list)  ? 2
                                                                  : -1;
        if (listIdx >= 0) {
            if (header->code == LVN_COLUMNCLICK) {
                auto* nlv = reinterpret_cast<NMLISTVIEW*>(header);
                content_sort_click(listIdx, from, nlv->iSubItem);
            } else if (header->code == LVN_ITEMCHANGED) {
                auto* nlv = reinterpret_cast<NMLISTVIEW*>(header);
                if ((nlv->uChanged & LVIF_STATE) && (nlv->uNewState & LVIS_SELECTED) &&
                    !(nlv->uOldState & LVIS_SELECTED)) {
                    if (listIdx == 0) on_content_master_select(static_cast<int>(nlv->lParam));
                    else if (listIdx == 1) on_content_child_select(static_cast<int>(nlv->lParam));
                    else on_content_asset_select();
                }
            }
        }
        return 0;
    }
    case WM_APP_INDEX_PROGRESS: {
        // Posted from the build worker. Keep this cheap: it arrives every 500 rows
        // and the status label is the only thing that needs to change.
        size_t done = static_cast<size_t>(wparam);
        size_t total = static_cast<size_t>(lparam);
        wchar_t st[160];
        if (total > 0)
            swprintf(st, 160, L"Building index: %zu / %zu entries (%.0f%%) - File > Build Index to cancel",
                     done, total, 100.0 * (double)done / (double)total);
        else
            swprintf(st, 160, L"Building index: %zu entries", done);
        SetWindowTextW(g_app->hwnd_status_label, st);
        return 0;
    }
    case WM_APP_INDEX_DONE:
        on_index_build_done(hwnd, wparam != 0);
        return 0;
    case WM_APP_EXTRACT_DONE: {
        std::unique_ptr<ExtractResult> result(reinterpret_cast<ExtractResult*>(lparam));
        if (g_app == nullptr || result->generation != g_app->request_generation) {
            return 0; // superseded by a newer selection (or a newly opened archive) -- discard
        }
        show_loading(false, 0);
        if (result->success) {
            apply_extracted_entry(result->mft_index, std::move(result->entry));
        } else {
            stop_video();
            g_app->current_entry = ExtractedEntry{};
            g_app->has_loaded_entry = false;
            set_export_enabled(false);
            MessageBoxA(hwnd, result->error_message.c_str(), "Extraction failed", MB_ICONWARNING);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_FILE_OPEN:
            do_open_file(hwnd);
            return 0;
        case ID_FILE_OPEN_LOOSE:
            do_open_loose_file(hwnd);
            return 0;
        case ID_FILE_BUILD_INDEX:
            do_build_index(hwnd);
            return 0;
        case ID_FILE_OPEN_INDEX:
            do_open_index(hwnd);
            return 0;
        case ID_TOOLS_DECODE_LINK:
            open_chat_link_decoder(hwnd);
            return 0;
        case ID_TOOLS_DECODE_TOKEN:
            open_token_decoder(hwnd);
            return 0;
        case ID_VIEW_THEME_DARK:
            set_theme(hwnd, ThemeMode::Dark);
            return 0;
        case ID_VIEW_THEME_LIGHT:
            set_theme(hwnd, ThemeMode::Light);
            return 0;
        case ID_VIEW_THEME_CUSTOM:
            set_theme(hwnd, ThemeMode::Custom, g_theme_custom_accent);
            return 0;
        case ID_VIEW_THEME_ACCENT:
            do_choose_accent(hwnd);
            return 0;
        case ID_FILE_LOAD_TEMPLATE:
            do_load_template(hwnd);
            return 0;
        case ID_FILE_LOAD_KEYS:
            do_load_keys(hwnd);
            return 0;
        case ID_FILE_EXPORT_COMPRESSED:
            do_export(hwnd, true);
            return 0;
        case ID_FILE_EXPORT_DECOMPRESSED:
            do_export(hwnd, false);
            return 0;
        case ID_FILE_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case ID_SEARCH_BUTTON:
            do_search();
            return 0;
        case ID_CLEAR_BUTTON:
            do_clear_search();
            return 0;
        case ID_ZOOM_IN:
            zoom_by(1.25f);
            return 0;
        case ID_ZOOM_OUT:
            zoom_by(1.0f / 1.25f);
            return 0;
        case ID_ROTATE:
            rotate_90();
            return 0;
        case ID_FIT:
            fit_view();
            return 0;
        case ID_ALPHA_TOGGLE:
            castlemist::gfx::set_alpha_aware(SendMessageW(g_app->hwnd_alpha, BM_GETCHECK, 0, 0) == BST_CHECKED);
            castlemist::gfx::render();
            return 0;
        case ID_SUBMESH_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) refresh_lod_controls();
            return 0;
        case ID_LOD_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                int lod = static_cast<int>(SendMessageW(g_app->hwnd_lod_combo, CB_GETCURSEL, 0, 0));
                castlemist::render::set_submesh_lod(lod_target_submesh(), lod);
                InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            }
            return 0;
        case ID_TEX_REDUCED: {
            bool on = SendMessageW(g_app->hwnd_tex_reduced, BM_GETCHECK, 0, 0) == BST_CHECKED;
            castlemist::render::set_submesh_tex_reduced(lod_target_submesh(), on);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_GIZMO_MOVE:
            set_gizmo_mode_ui(castlemist::render::GizmoMode::Translate);
            return 0;
        case ID_GIZMO_ROTATE:
            set_gizmo_mode_ui(castlemist::render::GizmoMode::Rotate);
            return 0;
        case ID_GIZMO_SCALE:
            set_gizmo_mode_ui(castlemist::render::GizmoMode::Scale);
            return 0;
        case ID_GIZMO_GRID:
            castlemist::render::set_show_grid(SendMessageW(g_app->hwnd_gizmo_grid, BM_GETCHECK, 0, 0) == BST_CHECKED);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_TEX_PANEL:
            g_app->show_tex_panel = SendMessageW(g_app->hwnd_tex_panel, BM_GETCHECK, 0, 0) == BST_CHECKED;
            relayout();   // the panel takes space out of the right column
            return 0;
        case ID_GIZMO_RESET:
            castlemist::render::reset_object_transform();
            update_gizmo_readout();
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_MODE_FULL:
            set_model_mode(castlemist::render::RenderMode::Full);
            return 0;
        case ID_MODE_PLAIN:
            set_model_mode(castlemist::render::RenderMode::Plain);
            return 0;
        case ID_MODE_WIRE:
            set_model_mode(castlemist::render::RenderMode::Wireframe);
            break;
        case ID_MODE_SHADER:
            set_model_mode(castlemist::render::RenderMode::GameShader);
            return 0;
        case ID_MODE_GW2BGFX: {
            // Swap which surface is on screen. Nothing about the D3D11 renderer
            // changes -- it keeps its model, its camera and its mode, so
            // toggling back lands exactly where it was left.
            g_app->bgfx_view_active =
                SendMessageW(g_app->hwnd_mode_gw2bgfx, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_app->bgfx_model_loaded = false;
            relayout();
            if (g_app->bgfx_view_active) {
                sync_bgfx_view();
                InvalidateRect(g_app->hwnd_model_bgfx, nullptr, FALSE);
            } else {
                InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            }
            return 0;
        }
        case ID_MODEL_RESET:
            reset_model_view();
            return 0;
        case ID_MAP_PREVIEW: {
            bool on = SendMessageW(g_app->hwnd_map_preview, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_app->map_pick_enabled = on;
            castlemist::render::set_show_focus(on);
            if (!on) castlemist::render::set_scene_focus(-1);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_LAYER_PROP:
            castlemist::render::set_layer_visible(castlemist::render::LAYER_PROP,
                                      SendMessageW(g_app->hwnd_layer_prop, BM_GETCHECK, 0, 0) == BST_CHECKED);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_LAYER_COLL:
            castlemist::render::set_layer_visible(castlemist::render::LAYER_COLLISION,
                                      SendMessageW(g_app->hwnd_layer_coll, BM_GETCHECK, 0, 0) == BST_CHECKED);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_LAYER_ZONE: {
            bool on = SendMessageW(g_app->hwnd_layer_zone, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // Lazily load the zone models the first time zones are enabled (keeps
            // the initial map load prop-only and fast).
            if (on && !g_app->map_zone_loaded && g_app->current_entry.map) {
                HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                auto zl = build_map_zone_layer(g_app->current_entry.decompressed,
                                               g_app->data_gw2.file_info.file_path);
                if (zl && !zl->instances.empty()) {
                    std::vector<castlemist::render::SceneInstance> insts;
                    for (const MapInstance& mi : zl->instances) {
                        castlemist::render::SceneInstance si;
                        si.model = mi.model;
                        for (int k = 0; k < 3; ++k) { si.pos[k] = mi.pos[k]; si.rot[k] = mi.rot[k]; }
                        si.scale = mi.scale; si.layer = mi.layer;
                        insts.push_back(si);
                    }
                    castlemist::render::add_scene_models(zl->models, insts);
                }
                g_app->map_zone_loaded = true;
                SetCursor(old);
            }
            castlemist::render::set_layer_visible(castlemist::render::LAYER_ZONE, on);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_MODE_SKEL:
            g_app->show_skeleton =
                SendMessageW(g_app->hwnd_skel_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
            castlemist::render::set_show_skeleton(g_app->show_skeleton);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_FILTER_TYPE:
        case ID_FILTER_CONTAINER:
        case ID_FILTER_CONTENT:
            if (HIWORD(wparam) == CBN_SELCHANGE) apply_filters();
            return 0;
        case ID_STRUCT_CONTAINER_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) on_struct_container_changed();
            return 0;
        case ID_ANIM_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(g_app->hwnd_anim_combo, CB_GETCURSEL, 0, 0));
                castlemist::render::set_animation(sel - 1); // item 0 = bind pose (-1), 1..nclips = clips
                // Selecting the rig implies the user wants to see it.
                if (castlemist::render::has_skeleton() && !g_app->show_skeleton) {
                    g_app->show_skeleton = true;
                    SendMessageW(g_app->hwnd_skel_toggle, BM_SETCHECK, BST_CHECKED, 0);
                    castlemist::render::set_show_skeleton(true);
                }
                InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            }
            return 0;
        case ID_ANIM_PLAY: {
            bool playing = SendMessageW(g_app->hwnd_anim_play, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // Play from the bind pose auto-selects the first embedded clip with real
            // keyframed motion (character/boss/weapon models); mounts/props embed only a
            // static zeropose, so there's nothing to auto-play there.
            if (playing && castlemist::render::current_animation() == -1) {
                int motion = castlemist::render::first_motion_animation();
                if (motion >= 0) {
                    SendMessageW(g_app->hwnd_anim_combo, CB_SETCURSEL, motion + 1, 0); // item0 = bind
                    castlemist::render::set_animation(motion);
                }
            }
            castlemist::render::set_playing(playing);
            if (playing) {
                SetTimer(g_app->hwnd_main, TIMER_ANIM, 16, nullptr); // ~60 fps
            } else {
                KillTimer(g_app->hwnd_main, TIMER_ANIM);
            }
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_AUDIO_PLAY: {
            const auto& clips = active_audio_entry().audio_clips;
            int sel = audio_selected_index();
            if (sel >= 0 && sel < static_cast<int>(clips.size()) &&
                castlemist::snd::play(clips[sel].data.data(), clips[sel].data.size())) {
                g_app->audio_seek_dragging = false;
                update_audio_seek_ui(false);
                SetTimer(g_app->hwnd_main, TIMER_AUDIO, 100, nullptr); // ~10 Hz seek-bar refresh
            }
            return 0;
        }
        // The cntc content tables are ListViews now -> their selection/sort events
        // arrive via WM_NOTIFY (LVN_ITEMCHANGED / LVN_COLUMNCLICK), not WM_COMMAND.
        case ID_AUDIO_STOP:
            castlemist::snd::stop();
            KillTimer(g_app->hwnd_main, TIMER_AUDIO);
            g_app->audio_seek_dragging = false;
            update_audio_seek_ui(true);
            return 0;
        case ID_VIDEO_PLAY:
            castlemist::vid::toggle_pause();
            update_video_ui(false);
            return 0;
        case ID_VIDEO_STOP:
            castlemist::vid::stop();
            present_video_frame();
            update_video_ui(true);
            return 0;
        case ID_VIDEO_LOOP:
            castlemist::vid::set_looping(SendMessageW(g_app->hwnd_video_loop, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case ID_VIDEO_MUTE:
            castlemist::vid::set_muted(SendMessageW(g_app->hwnd_video_mute, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case ID_VIDEO_SUBS:
            g_app->video_subs_on = SendMessageW(g_app->hwnd_video_subs, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // First time it's switched on for this movie, go find the script.
            if (g_app->video_subs_on && !g_app->video_subs_searched && g_app->has_loaded_entry) {
                search_video_subtitles(g_app->current_mft_index);
            }
            g_app->video_sub_shown = -1; // force the overlay to re-evaluate
            update_video_ui(false);
            return 0;
        case ID_VIDEO_CLIP:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(g_app->hwnd_video_clip, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel != g_app->cinp_video_sel) {
                    std::wstring err = start_cinp_video(sel);
                    SetWindowTextW(g_app->hwnd_text_preview,
                                   cinematic_info_text(g_app->current_entry, err).c_str());
                    relayout();
                }
            }
            return 0;
        case ID_VIDEO_TRACK:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                // Row 0 = "All tracks (mixed)"; rows 1..n = track 0..n-1. Applied
                // by per-track volume, so it takes effect mid-playback with no
                // reopen and no re-seek.
                int row = static_cast<int>(SendMessageW(g_app->hwnd_video_track, CB_GETCURSEL, 0, 0));
                castlemist::vid::set_active_track(row <= 0 ? -1 : row - 1);
            }
            return 0;
        case ID_AUDIO_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                castlemist::snd::stop();
                KillTimer(g_app->hwnd_main, TIMER_AUDIO);
                g_app->audio_seek_dragging = false;
                update_audio_info(audio_selected_index());
                update_audio_seek_ui(true);
            }
            return 0;
        case ID_TEX_FULLRES: {
            bool full = SendMessageW(g_app->hwnd_tex_fullres, BM_GETCHECK, 0, 0) == BST_CHECKED;
            set_texture_full_res(full);
            // Reload the current model so materials re-decode at the chosen resolution.
            if (g_app->dat_loaded && g_app->has_loaded_entry &&
                g_app->current_entry.kind == PreviewKind::Model) {
                on_entry_selected(g_app->current_mft_index);
            }
            return 0;
        }
        case ID_LIGHT_PREPASS: {
            bool on = SendMessageW(g_app->hwnd_light_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
            castlemist::render::set_lightprepass(on);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE); // redraw with the new lighting
            return 0;
        }
        case ID_LIGHT_FOLLOW: {
            bool on = SendMessageW(g_app->hwnd_light_follow, BM_GETCHECK, 0, 0) == BST_CHECKED;
            castlemist::render::set_light_follow(on);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_LIGHT_GW2RIG: {
            bool on = SendMessageW(g_app->hwnd_light_gw2rig, BM_GETCHECK, 0, 0) == BST_CHECKED;
            castlemist::render::set_preview_rig(on);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_EFFECTS_TOGGLE: {
            bool on = SendMessageW(g_app->hwnd_effects_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
            castlemist::render::set_show_effects(on);
            // Start/stop the repaint timer so idle effects keep (or stop) animating.
            if (on && castlemist::render::has_effects())
                SetTimer(g_app->hwnd_main, TIMER_ANIM, 16, nullptr);
            else if (!castlemist::render::is_playing())
                KillTimer(g_app->hwnd_main, TIMER_ANIM);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_CLOTH_TOGGLE: {
            bool on = SendMessageW(g_app->hwnd_cloth_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
            castlemist::render::set_cloth_enabled(on);
            on = castlemist::render::cloth_enabled();  // build may have failed -> reflect real state
            SendMessageW(g_app->hwnd_cloth_toggle, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            // Keep repainting while the sim runs so the cloth animates continuously.
            if (on)
                SetTimer(g_app->hwnd_main, TIMER_ANIM, 16, nullptr);
            else if (!castlemist::render::is_playing() && !(castlemist::render::has_effects() && castlemist::render::show_effects()))
                KillTimer(g_app->hwnd_main, TIMER_ANIM);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (wparam == TIMER_ANIM &&
            (castlemist::render::is_playing() || castlemist::render::cloth_enabled() ||
             (castlemist::render::has_effects() && castlemist::render::show_effects()))) {
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
        }
        if (wparam == TIMER_AUDIO) {
            update_audio_seek_ui(false);
            if (!castlemist::snd::is_playing()) {
                // Clip finished on its own -> park the bar at the end and stop ticking.
                KillTimer(g_app->hwnd_main, TIMER_AUDIO);
            }
        }
        if (wparam == TIMER_VIDEO && castlemist::vid::is_open()) {
            // pump() returns false until the next frame is actually due, so this
            // ticks ~120 times a second but only does real work ~30 times.
            if (castlemist::vid::pump()) {
                present_video_frame();
                update_video_ui(false);
            } else if (castlemist::vid::finished()) {
                update_video_ui(false); // flip the button back to "Play"
            }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ANIM);
        KillTimer(hwnd, TIMER_AUDIO);
        KillTimer(hwnd, TIMER_VIDEO);
        castlemist::vid::shutdown();
        castlemist::snd::shutdown();
        castlemist::gfx::shutdown();
        castlemist::render::shutdown();
        castlemist::gw2bgfxview::shutdown();
        delete g_app;
        g_app = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}


} // namespace castlemist::ui
