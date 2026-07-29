/// @file
/// @brief The bottom-right texture panel: one group per submesh, one row per
///        texture that submesh's material uses.

#include "castlemist/ui/texture_panel.h"

#include <windowsx.h>   // GET_Y_LPARAM

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace castlemist::texpanel {

namespace {

constexpr wchar_t kClassName[] = L"Gw2TexturePanel";

// Mirrors the app's light palette (app_state.h). Duplicated rather than included
// because this module is a self-contained widget like the MFT list and the info
// panel -- nothing below the shell reaches into the shell's private header.
constexpr COLORREF kColPanel  = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kColBand   = RGB(0xE9, 0xEC, 0xF1);
constexpr COLORREF kColText   = RGB(0x22, 0x27, 0x30);
constexpr COLORREF kColSubtle = RGB(0x5A, 0x63, 0x70);
constexpr COLORREF kColAccent = RGB(0x2D, 0x7F, 0xF9);
constexpr COLORREF kColCard   = RGB(0xF7, 0xF9, 0xFC);
constexpr COLORREF kColBorder = RGB(0xD3, 0xD8, 0xDF);
constexpr COLORREF kColHover  = RGB(0xEA, 0xF2, 0xFE);

constexpr int kThumbPx     = 64;   // thumbnail cell edge
constexpr int kPad         = 8;
constexpr int kTexRowH     = kThumbPx + 2 * kPad;   // 80
constexpr int kHeaderRowH  = 26;
constexpr int kChipH       = 15;

// One decoded texture, scaled once into a GDI bitmap. Shared by every row that
// references it -- a material set commonly repeats the same texture across
// submeshes, and a 2048x2048 source must not be rescaled per row.
struct Thumb {
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
};

struct Row {
    bool header = false;
    // --- header rows ---
    std::wstring title;         // "Submesh 0  -  mat 3  -  2080 tris"
    COLORREF chip = kColAccent; // material colour swatch
    // --- texture rows ---
    int thumb = -1;             // index into State::thumbs
    std::wstring role;          // "Diffuse", "Normal", "Texture 3"
    std::wstring detail;        // "2048 x 2048  -  DXT5  -  10 mips"
    std::wstring ids;           // "file 771205  -  base 771204"
    std::wstring channels;      // "RGBA" -- drawn as a chip
    bool cutout = false;
    uint32_t fileId = 0, baseId = 0;
};

struct State {
    std::vector<Thumb> thumbs;
    std::vector<Row> rows;
    int texRows = 0;         // non-header rows (what row_count reports)
    int scroll = 0;          // scroll offset in pixels
    int contentH = 0;        // total content height in pixels
    int hover = -1;          // row under the cursor
    bool tracking = false;   // WM_MOUSELEAVE requested
    HFONT font = nullptr, fontBold = nullptr, fontSmall = nullptr;
    ActivateCallback on_activate;
};

State* get_state(HWND h) { return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

// ---------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------

// Box-downsample RGBA8 to at most `maxEdge` on the long side. Averaging the whole
// source block rather than point-sampling matters here: these are 1k-2k textures
// shown at 64px, and a nearest-neighbour shrink of a detail/normal map is pure
// aliasing noise -- you cannot tell two textures apart from it.
std::vector<uint8_t> downsample_rgba(const std::vector<uint8_t>& src, int sw, int sh, int maxEdge, int& dw,
                                     int& dh) {
    dw = sw;
    dh = sh;
    if (sw <= 0 || sh <= 0 || src.size() < static_cast<size_t>(sw) * sh * 4) return {};
    int longEdge = std::max(sw, sh);
    if (longEdge > maxEdge) {
        dw = std::max(1, sw * maxEdge / longEdge);
        dh = std::max(1, sh * maxEdge / longEdge);
    }
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = &src[(static_cast<size_t>(sy) * sw + x0) * 4];
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    acc[0] += row[0]; acc[1] += row[1]; acc[2] += row[2]; acc[3] += row[3];
                    ++n;
                }
            }
            uint8_t* dst = &out[(static_cast<size_t>(y) * dw + x) * 4];
            for (int c = 0; c < 4; ++c) dst[c] = static_cast<uint8_t>(acc[c] / (n ? n : 1));
        }
    }
    return out;
}

// Scale the texture down and composite it over a checkerboard into a top-down
// 32bpp DIB. Compositing (rather than blitting the raw RGB) is what makes a
// cutout mask readable: the transparent parts of a leaf/hair atlas show the
// checker instead of whatever colour happens to sit in the transparent texels.
Thumb make_thumb(const ModelTextureCPU& tex) {
    Thumb t;
    int w = 0, h = 0;
    std::vector<uint8_t> px = downsample_rgba(tex.rgba, tex.width, tex.height, kThumbPx, w, h);
    if (px.empty()) return t;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr || bits == nullptr) {
        if (bmp != nullptr) DeleteObject(bmp);
        return t;
    }
    uint8_t* dst = static_cast<uint8_t*>(bits);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = &px[(static_cast<size_t>(y) * w + x) * 4];
            int a = s[3];
            int bg = (((x >> 3) + (y >> 3)) & 1) ? 0xC8 : 0xF0;   // 8px checker
            uint8_t* d = &dst[(static_cast<size_t>(y) * w + x) * 4];
            d[0] = static_cast<uint8_t>((s[2] * a + bg * (255 - a)) / 255);   // B
            d[1] = static_cast<uint8_t>((s[1] * a + bg * (255 - a)) / 255);   // G
            d[2] = static_cast<uint8_t>((s[0] * a + bg * (255 - a)) / 255);   // R
            d[3] = 255;
        }
    }
    t.bmp = bmp;
    t.w = w;
    t.h = h;
    return t;
}

void free_thumbs(State& st) {
    for (Thumb& t : st.thumbs)
        if (t.bmp != nullptr) DeleteObject(t.bmp);
    st.thumbs.clear();
}

// ---------------------------------------------------------------------------
// Building the row list from a model
// ---------------------------------------------------------------------------

// Same golden-ratio hue walk the 3D submesh colouring uses, so material k keeps
// the same swatch no matter how many materials the model has.
COLORREF material_colour(uint32_t mi) {
    float hue = std::fmod(mi * 0.6180339887f + 0.12f, 1.0f) * 6.0f;
    int i = static_cast<int>(hue);
    float f = hue - i, q = 1.0f - f;
    float r = 0, g = 0, b = 0;
    switch (i) {
        case 0: r = 1; g = f; b = 0; break;
        case 1: r = q; g = 1; b = 0; break;
        case 2: r = 0; g = 1; b = f; break;
        case 3: r = 0; g = q; b = 1; break;
        case 4: r = f; g = 0; b = 1; break;
        default: r = 1; g = 0; b = q; break;
    }
    // Darkened a little: these sit on white, not over a 3D scene.
    auto c = [](float v) { return static_cast<int>((0.15f + 0.60f * v) * 255.0f); };
    return RGB(c(r), c(g), c(b));
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void build_rows(State& st, const ModelPreview& model) {
    // fileId -> index into model.textures, so a material's file-id list (which is
    // what the MODL actually stores) resolves back to decoded pixels.
    std::unordered_map<uint32_t, int> byFileId;
    for (size_t i = 0; i < model.textures.size(); ++i)
        byFileId.emplace(model.textures[i].fileId, static_cast<int>(i));

    // Which pixel-shader sampler slot the material's real game shader binds each
    // texture to, when the AMAT resolved. Only these are provably bound by the
    // game itself; everything else is a MODL reference whose use we infer.
    std::unordered_map<uint32_t, std::unordered_map<int, int>> slotOf;   // matIndex -> (texIdx -> slot)
    for (const auto& gm : model.gameMaterials) {
        if (!gm.ok) continue;
        auto& m = slotOf[gm.index];
        for (const auto& s : gm.samplers)
            if (s.global == 0 && s.gameTex >= 0) m.emplace(s.gameTex, s.slot);
    }

    // Thumbnails are built lazily per texture and shared across every row.
    std::unordered_map<int, int> thumbOf;   // texture index -> index into st.thumbs
    auto thumb_for = [&](int ti) -> int {
        auto it = thumbOf.find(ti);
        if (it != thumbOf.end()) return it->second;
        Thumb t = make_thumb(model.textures[ti]);
        int idx = -1;
        if (t.bmp != nullptr) {
            idx = static_cast<int>(st.thumbs.size());
            st.thumbs.push_back(t);
        }
        thumbOf.emplace(ti, idx);
        return idx;
    };

    // One group per SUBMESH, in draw order -- not per material. Two submeshes
    // sharing a material are two separate things on screen, and "which textures is
    // *this* piece made of" is the question the panel exists to answer, so each
    // gets its own group even when the answer repeats.
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const ModelMeshCPU& mesh = model.meshes[mi];
        const ModelMaterialCPU* mat = nullptr;
        for (const auto& m : model.materials)
            if (m.index == mesh.materialIndex) { mat = &m; break; }

        Row h;
        h.header = true;
        h.chip = material_colour(mesh.materialIndex);
        wchar_t buf[160];
        swprintf(buf, 160, L"Submesh %zu    mat %u    %zu tris", mi, mesh.materialIndex,
                 mesh.indices.size() / 3);
        h.title = buf;
        st.rows.push_back(std::move(h));

        if (mat == nullptr) {
            Row r;
            r.role = L"(material not found)";
            st.rows.push_back(std::move(r));
            ++st.texRows;
            continue;
        }

        // Every texture the material references, in file order, then the
        // diffuse/normal picks in case those came from a shader binding that is not
        // in textureFileIds. Deduped: a material may list the same fileId twice.
        std::vector<int> texIdx;
        auto add = [&](int ti) {
            if (ti >= 0 && ti < static_cast<int>(model.textures.size()) &&
                std::find(texIdx.begin(), texIdx.end(), ti) == texIdx.end())
                texIdx.push_back(ti);
        };
        for (uint32_t fid : mat->textureFileIds) {
            auto it = byFileId.find(fid);
            if (it != byFileId.end()) add(it->second);
        }
        add(mat->diffuseTex);
        add(mat->normalTex);

        if (texIdx.empty()) {
            Row r;
            r.role = L"(no texture resolved)";
            st.rows.push_back(std::move(r));
            ++st.texRows;
            continue;
        }

        auto slots = slotOf.find(mesh.materialIndex);
        int ordinal = 0;
        for (int ti : texIdx) {
            const ModelTextureCPU& tex = model.textures[ti];
            Row r;
            r.thumb = thumb_for(ti);
            r.fileId = tex.fileId;
            r.baseId = tex.baseId;
            r.cutout = tex.hasCutout;
            r.channels = widen(tex.channels);

            // Role: what the renderer does with it. Diffuse/Normal are the two the
            // reconstruction path binds; anything else is named by its position in
            // the material's own list rather than guessed at.
            if (ti == mat->diffuseTex)      r.role = L"Diffuse";
            else if (ti == mat->normalTex)  r.role = L"Normal";
            else if (tex.isNormal)          r.role = L"Normal map";
            else {
                swprintf(buf, 160, L"Texture %d", ordinal);
                r.role = buf;
            }
            if (slots != slotOf.end()) {
                auto s = slots->second.find(ti);
                if (s != slots->second.end()) {
                    swprintf(buf, 160, L"%ls    t%d", r.role.c_str(), s->second);
                    r.role = buf;
                }
            }

            swprintf(buf, 160, L"%d x %d    %ls", tex.width, tex.height, widen(tex.fmt).c_str());
            r.detail = buf;
            if (tex.mipCount > 0) {
                swprintf(buf, 160, L"    %d mip%ls", tex.mipCount, tex.mipCount == 1 ? L"" : L"s");
                r.detail += buf;
            }
            swprintf(buf, 160, L"file %u    base %u", tex.fileId, tex.baseId);
            r.ids = buf;

            st.rows.push_back(std::move(r));
            ++st.texRows;
            ++ordinal;
        }
    }
}

int row_height(const Row& r) { return r.header ? kHeaderRowH : kTexRowH; }

void recompute_content(HWND panel, State& st) {
    st.contentH = 0;
    for (const Row& r : st.rows) st.contentH += row_height(r);
    RECT rc;
    GetClientRect(panel, &rc);
    int viewH = rc.bottom - rc.top;
    st.scroll = std::clamp(st.scroll, 0, std::max(0, st.contentH - viewH));

    SCROLLINFO si{};
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, st.contentH - 1);
    si.nPage = static_cast<UINT>(std::max(1, viewH));
    si.nPos = st.scroll;
    SetScrollInfo(panel, SB_VERT, &si, TRUE);
}

// Row under a client-area y coordinate, or -1.
int row_at(const State& st, int y) {
    int top = -st.scroll;
    for (size_t i = 0; i < st.rows.size(); ++i) {
        int h = row_height(st.rows[i]);
        if (y >= top && y < top + h) return static_cast<int>(i);
        top += h;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

// A small rounded label (channel layout, "cutout"). Returns the width it used.
int draw_chip(HDC dc, int x, int y, const wchar_t* text, COLORREF fg, COLORREF bg, HFONT font) {
    SIZE sz{};
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
    int w = sz.cx + 10;
    RECT rc{x, y, x + w, y + kChipH};
    HBRUSH br = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, bg);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
    SetTextColor(dc, fg);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    return w;
}

void draw_text(HDC dc, RECT rc, const std::wstring& s, COLORREF col, HFONT font) {
    if (s.empty()) return;
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

void paint(HWND panel, State& st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(panel, &ps);
    RECT client;
    GetClientRect(panel, &client);
    int W = client.right, H = client.bottom;

    // Double-buffered: rows blit bitmaps and draw several text runs each, so
    // painting straight to the window flickers badly while scrolling.
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP back = CreateCompatibleBitmap(hdc, std::max(1, W), std::max(1, H));
    HGDIOBJ oldBack = SelectObject(dc, back);

    HBRUSH bg = CreateSolidBrush(kColPanel);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    if (st.rows.empty()) {
        RECT rc = client;
        draw_text(dc, rc, L"   No model textures to show.", kColSubtle, st.font);
    }

    HDC memdc = CreateCompatibleDC(hdc);
    int top = -st.scroll;
    for (size_t i = 0; i < st.rows.size(); ++i) {
        const Row& r = st.rows[i];
        int h = row_height(r);
        if (top + h > 0 && top < H) {
            RECT rowRc{0, top, W, top + h};
            if (r.header) {
                HBRUSH b = CreateSolidBrush(kColBand);
                FillRect(dc, &rowRc, b);
                DeleteObject(b);
                RECT chip{kPad, top + 6, kPad + 4, top + h - 6};
                HBRUSH cb = CreateSolidBrush(r.chip);
                FillRect(dc, &chip, cb);
                DeleteObject(cb);
                RECT tr{kPad + 12, top, W - kPad, top + h};
                draw_text(dc, tr, r.title, kColText, st.fontBold);
            } else {
                if (static_cast<int>(i) == st.hover) {
                    HBRUSH b = CreateSolidBrush(kColHover);
                    FillRect(dc, &rowRc, b);
                    DeleteObject(b);
                }
                // Thumbnail cell (framed, so a texture whose border is white still
                // reads as an image rather than bleeding into the panel).
                int tx = kPad, ty = top + kPad;
                RECT frame{tx - 1, ty - 1, tx + kThumbPx + 1, ty + kThumbPx + 1};
                HBRUSH fb = CreateSolidBrush(kColCard);
                FillRect(dc, &frame, fb);
                DeleteObject(fb);
                HBRUSH fr = CreateSolidBrush(kColBorder);
                FrameRect(dc, &frame, fr);
                DeleteObject(fr);
                if (r.thumb >= 0 && r.thumb < static_cast<int>(st.thumbs.size())) {
                    const Thumb& t = st.thumbs[r.thumb];
                    HGDIOBJ ob = SelectObject(memdc, t.bmp);
                    // Centred in the cell: non-square textures keep their aspect.
                    int ox = tx + (kThumbPx - t.w) / 2, oy = ty + (kThumbPx - t.h) / 2;
                    BitBlt(dc, ox, oy, t.w, t.h, memdc, 0, 0, SRCCOPY);
                    SelectObject(memdc, ob);
                }

                int lx = tx + kThumbPx + kPad + 4;
                int lw = std::max(20, W - lx - kPad);
                // Channel chip is right-aligned on the first line; the role text
                // stops short of it so the two never overlap on a narrow panel.
                int chipW = 0;
                if (!r.channels.empty()) {
                    HFONT of = static_cast<HFONT>(SelectObject(dc, st.fontSmall));
                    SIZE sz{};
                    GetTextExtentPoint32W(dc, r.channels.c_str(), static_cast<int>(r.channels.size()), &sz);
                    SelectObject(dc, of);
                    chipW = sz.cx + 10;
                }
                if (chipW > 0 && lw > chipW + 40)
                    draw_chip(dc, W - kPad - chipW, ty + 1, r.channels.c_str(), RGB(0xFF, 0xFF, 0xFF),
                              kColAccent, st.fontSmall);
                else
                    chipW = 0;

                RECT l1{lx, ty, lx + lw - (chipW ? chipW + 6 : 0), ty + 18};
                RECT l2{lx, ty + 20, lx + lw, ty + 38};
                RECT l3{lx, ty + 40, lx + lw, ty + 58};
                draw_text(dc, l1, r.role, kColText, st.fontBold);
                draw_text(dc, l2, r.detail, kColSubtle, st.font);
                draw_text(dc, l3, r.ids, kColSubtle, st.font);
                if (r.cutout)
                    draw_chip(dc, lx, ty + kThumbPx - kChipH + 2, L"alpha cutout", kColText,
                              RGB(0xFF, 0xE7, 0xB8), st.fontSmall);

                RECT sep{0, top + h - 1, W, top + h};
                HBRUSH sb = CreateSolidBrush(kColBorder);
                FillRect(dc, &sep, sb);
                DeleteObject(sb);
            }
        }
        top += h;
    }
    DeleteDC(memdc);

    BitBlt(hdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBack);
    DeleteObject(back);
    DeleteDC(dc);
    EndPaint(panel, &ps);
}

// ---------------------------------------------------------------------------

void scroll_by(HWND panel, State& st, int delta) {
    RECT rc;
    GetClientRect(panel, &rc);
    int maxScroll = std::max(0, st.contentH - static_cast<int>(rc.bottom - rc.top));
    int want = std::clamp(st.scroll + delta, 0, maxScroll);
    if (want == st.scroll) return;
    st.scroll = want;
    SetScrollPos(panel, SB_VERT, st.scroll, TRUE);
    InvalidateRect(panel, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND panel, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = get_state(panel);
    switch (msg) {
    case WM_CREATE: {
        State* s = new State();
        s->font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        s->fontBold = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        s->fontSmall = CreateFontW(-10, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SetWindowLongPtrW(panel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        return 0;
    }
    case WM_DESTROY:
        if (st != nullptr) {
            free_thumbs(*st);
            if (st->font != nullptr) DeleteObject(st->font);
            if (st->fontBold != nullptr) DeleteObject(st->fontBold);
            if (st->fontSmall != nullptr) DeleteObject(st->fontSmall);
            delete st;
            SetWindowLongPtrW(panel, GWLP_USERDATA, 0);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;   // paint() fills the whole client area
    case WM_PAINT:
        if (st != nullptr) paint(panel, *st);
        return 0;
    case WM_SIZE:
        if (st != nullptr) {
            recompute_content(panel, *st);
            InvalidateRect(panel, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (st != nullptr) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            scroll_by(panel, *st, -d * kTexRowH / (WHEEL_DELTA * 2));
        }
        return 0;
    case WM_VSCROLL:
        if (st != nullptr) {
            RECT rc;
            GetClientRect(panel, &rc);
            int page = rc.bottom - rc.top;
            switch (LOWORD(wp)) {
            case SB_LINEUP:   scroll_by(panel, *st, -kTexRowH / 2); break;
            case SB_LINEDOWN: scroll_by(panel, *st, kTexRowH / 2); break;
            case SB_PAGEUP:   scroll_by(panel, *st, -page); break;
            case SB_PAGEDOWN: scroll_by(panel, *st, page); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si{};
                si.cbSize = sizeof si;
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(panel, SB_VERT, &si);
                scroll_by(panel, *st, si.nTrackPos - st->scroll);
                break;
            }
            default: break;
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (st != nullptr) {
            int hov = row_at(*st, GET_Y_LPARAM(lp));
            if (hov >= 0 && st->rows[static_cast<size_t>(hov)].header) hov = -1;
            if (hov != st->hover) {
                st->hover = hov;
                InvalidateRect(panel, nullptr, FALSE);
            }
            if (!st->tracking) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, panel, 0};
                TrackMouseEvent(&tme);
                st->tracking = true;
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (st != nullptr) {
            st->tracking = false;
            if (st->hover != -1) {
                st->hover = -1;
                InvalidateRect(panel, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (st != nullptr && st->on_activate) {
            int i = row_at(*st, GET_Y_LPARAM(lp));
            if (i >= 0) {
                const Row& r = st->rows[static_cast<size_t>(i)];
                if (!r.header && r.fileId != 0) st->on_activate(r.fileId, r.baseId);
            }
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(panel, msg, wp, lp);
}

} // namespace

void register_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // WM_ERASEBKGND is handled
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND create(HWND parent, HINSTANCE instance, int control_id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, kClassName, L"", WS_CHILD | WS_VSCROLL, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance, nullptr);
}

void set_model(HWND panel, const ModelPreview* model) {
    State* st = get_state(panel);
    if (st == nullptr) return;
    free_thumbs(*st);
    st->rows.clear();
    st->texRows = 0;
    st->scroll = 0;
    st->hover = -1;
    if (model != nullptr) build_rows(*st, *model);
    recompute_content(panel, *st);
    InvalidateRect(panel, nullptr, FALSE);
}

int row_count(HWND panel) {
    State* st = get_state(panel);
    return st != nullptr ? st->texRows : 0;
}

void set_activate_callback(HWND panel, ActivateCallback cb) {
    State* st = get_state(panel);
    if (st != nullptr) st->on_activate = std::move(cb);
}

} // namespace castlemist::texpanel
