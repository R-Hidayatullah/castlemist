/// @file
/// @brief The token / filename-bytes decoder popup ("Tools > Decode Token / Filename Bytes...").
///
/// A modeless, resizable tool window bundling the small, self-contained
/// byte<->id decoders that already exist elsewhere in the codebase as private
/// helpers, so they can be reached without hunting through a hex view:
///   * shader/material token <-> name       (base-23 pack, `game_shader.cpp`)
///   * bone name -> token                   (5-bit pack, `gw2model.hpp`)
///   * an on-disk "filename" record's 8 bytes -> fileId/subId (`BinaryParser.cpp`)
/// Each routine below is a duplicate of the verified one cited in its comment,
/// kept local (rather than exported+shared) so this popup stays a thin,
/// low-risk UI shell over logic that is tested where it already lives.
///
/// Both decoders below accept and report both 32- and 64-bit forms, because
/// real fields in this codebase carry a token in either width (a MODL
/// ModelConstantData.name is a token32, but the same base-23 scheme is also
/// the low dword of a materialToken64; bone tokens are natively 64-bit, but
/// some contexts only compare the low dword) -- see each function's comment
/// for exactly which direction is a verified engine rule versus a labelled,
/// honest convenience truncation/extension.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace castlemist::ui {

// ---- Token / filename-bytes decoder popup ---------------------------------
HWND g_td_wnd = nullptr;
HWND g_td_shader_in = nullptr;
HWND g_td_bone_in = nullptr;
HWND g_td_fid_in = nullptr;
HWND g_td_output = nullptr;
HWND g_td_shader_decode_btn = nullptr;
HWND g_td_shader_encode_btn = nullptr;
HWND g_td_bone_encode_btn = nullptr;
HWND g_td_fid_decode_btn = nullptr;
HWND g_td_close_btn = nullptr;
HWND g_td_label_shader = nullptr;
HWND g_td_label_bone = nullptr;
HWND g_td_label_fid = nullptr;
HWND g_td_label_log = nullptr;
HFONT g_td_font = nullptr;

namespace {

std::wstring g_td_log;  // appended to, newest entry on top, shown read-only

void td_log(const std::wstring& line) {
    g_td_log = line + L"\r\n\r\n" + g_td_log;
    SetWindowTextW(g_td_output, g_td_log.c_str());
}

std::string td_get_text(HWND edit) {
    wchar_t wbuf[512] = L"";
    GetWindowTextW(edit, wbuf, 512);
    std::string s;
    for (wchar_t* p = wbuf; *p; ++p)
        if (*p < 128) s.push_back(static_cast<char>(*p));
    return s;
}

// GW2 "Token" decode/encode (Arena/Services/Token/Token.cpp; client sub_140E3B360),
// duplicated from castlemist::extract's decode_token (game_shader.cpp): a base-23
// packed lowercase string. v = (token - 0x30000000) mod 2^32; peel/pack base-23
// digits. Verified against the client and the dat (0xBEF8FD7F -> "gloover", ...).
// The rule is inherently 32-bit (the subtraction wraps mod 2^32, matching a
// 32-bit engine register) -- so a 64-bit input (e.g. pasted from a
// materialToken64-typed field) is decoded from its LOW 32 bits, exactly as the
// engine's own 32-bit Token::Decode would if handed that truncated dword. This
// is not a separate/guessed 64-bit scheme, just correct handling of a wider
// paste.
constexpr char kShaderAlpha[] = "abcdefghiklmnopvrstuwxy";  // 23 chars, no j/q/z

std::string decode_shader_token(uint32_t token) {
    uint32_t v = token - 0x30000000u;  // unsigned wrap, matches engine
    std::string out;
    while (v) {
        out.push_back(kShaderAlpha[v % 23]);
        v /= 23;
    }
    return out.empty() ? "(empty -- token == 0x30000000)" : out;
}

// Mathematical inverse of decode_shader_token: not itself lifted from the
// client, but a direct reversal of the verified rule above (each kShaderAlpha
// letter is a base-23 digit, least-significant first). Only lowercase a-y
// minus j/q/z are valid; names beyond ~7 letters silently wrap past
// uint32_t range, same as the engine's own token space.
std::optional<uint32_t> encode_shader_token(const std::string& name) {
    if (name.empty()) return std::nullopt;
    uint32_t v = 0, mult = 1;
    for (char c : name) {
        const char* pos = std::strchr(kShaderAlpha, static_cast<unsigned char>(c));
        if (!pos || *pos == '\0') return std::nullopt;
        v += static_cast<uint32_t>(pos - kShaderAlpha) * mult;
        mult *= 23;
    }
    return v + 0x30000000u;
}

// GW2 bone-name -> token64, duplicated from castlemist::model::tokenizeBoneName
// (include/castlemist/native/gw2model.hpp; reverse of engine sub_140E3B5E0). Packs
// the leaf name (after the last ':' namespace separator) 5 bits/letter (a-z/A-Z ->
// 1..26, LSB-first, <=12 chars); a trailing two-digit suffix goes in the top
// nibble instead of being packed as letters. Verified against real models' mesh
// boneBindings entries. Natively 64-bit -- callers wanting a 32-bit comparison
// value (e.g. against a dword-sized field) can use the low 32 bits, which
// td_do_bone_encode reports too, clearly labelled as a truncation rather than
// an independently verified 32-bit bone-token format.
uint64_t encode_bone_token64(const std::string& name) {
    const char* s = name.c_str();
    size_t n = name.size();
    if (size_t p = name.rfind(':'); p != std::string::npos) {
        s += p + 1;
        n -= p + 1;
    }
    size_t end = n;
    uint64_t top = 0;
    if (n >= 2) {
        unsigned char last = static_cast<unsigned char>(s[n - 1]);
        unsigned char sec = static_cast<unsigned char>(s[n - 2]);
        if (last >= '0' && last <= '9' && sec >= '0' && sec <= '9') {
            end = n - 2;
            top = (static_cast<uint64_t>(last) + 10ull * static_cast<uint64_t>(sec)) & 0xFull;
        }
    }
    uint64_t tok = top << 60;
    int i = 0;
    for (size_t k = 0; k < end && i < 60; ++k, i += 5) {
        unsigned char c = static_cast<unsigned char>(s[k]);
        uint64_t v5 = 0;
        if (c >= 'a' && c <= 'z') v5 = static_cast<uint64_t>(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') v5 = static_cast<uint64_t>(c - 'A' + 1);
        tok |= (v5 & 0x1Full) << i;
    }
    return tok;
}

// Parse a run of hex digits (spaces, commas and an optional "0x" per group are
// ignored) into raw bytes, in the order they'd appear in a hex-view selection.
std::vector<uint8_t> parse_hex_bytes(const std::string& text) {
    std::string digits;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        } else if ((c == 'x' || c == 'X') && !digits.empty() && digits.back() == '0') {
            digits.pop_back();  // drop the "0" from a "0x" prefix already collected
        }
    }
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(digits.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Byte-exact port of the game's fileName->fileId decoder, duplicated from
// BinaryParser::gwString's "filename" case (sub_140DAB4A0, Arena/Services/File3/
// FileArchive.cpp:702): the on-disk record is four little-endian uint16 words --
// low, high, third (subId), fourth -- and fileId = 0xFF00*high + low - 0xFF00FF,
// gated by the same validity check (low/high must be >= 0x100; a non-zero third
// must itself be >= 0x100 and fourth must be 0).
struct FilenameDecode {
    bool have_bytes = false;
    bool valid = false;
    long file_id = 0;
    uint16_t sub_id = 0;
};

FilenameDecode decode_filename_bytes(const std::vector<uint8_t>& b) {
    FilenameDecode r;
    if (b.size() < 8) return r;
    r.have_bytes = true;
    auto rd16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(b[off] | (static_cast<uint16_t>(b[off + 1]) << 8));
    };
    uint16_t lo = rd16(0), hi = rd16(2), third = rd16(4), fourth = rd16(6);
    bool invalid = lo < 0x100 || hi < 0x100 || (third != 0 && (third < 0x100 || fourth != 0));
    if (invalid) return r;
    r.valid = true;
    r.file_id = 0xFF00L * static_cast<long>(hi) + static_cast<long>(lo) - 0xFF00FF;
    r.sub_id = third;
    return r;
}

// Measures `text` in the dialog's current font and returns a comfortably
// padded button width, so a button is never narrower than its own label
// regardless of font/DPI -- the earlier fixed-pixel widths clipped "Encode ->
// token64" at some system font sizes.
int td_button_width(HWND owner, const wchar_t* text, int min_w) {
    HDC dc = GetDC(owner);
    HFONT old = g_td_font ? static_cast<HFONT>(SelectObject(dc, g_td_font)) : nullptr;
    SIZE sz{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
    if (old) SelectObject(dc, old);
    ReleaseDC(owner, dc);
    return std::max(min_w, static_cast<int>(sz.cx) + 28);
}

}  // namespace

void td_do_shader_decode() {
    std::string s = td_get_text(g_td_shader_in);
    if (s.empty()) { MessageBeep(MB_ICONWARNING); return; }
    uint64_t raw = std::strtoull(s.c_str(), nullptr, 16);
    uint32_t token = static_cast<uint32_t>(raw);  // well-defined mod 2^32 narrowing
    std::string name = decode_shader_token(token);
    wchar_t buf[240];
    if (raw > 0xFFFFFFFFull) {
        swprintf(buf, 240,
                 L"token64 0x%016llX -> low32 0x%08X -> \"%hs\"  (base-23 decode "
                 L"always uses the low 32 bits -- see the field's comment)",
                 static_cast<unsigned long long>(raw), token, name.c_str());
    } else {
        swprintf(buf, 240, L"token32 0x%08X -> \"%hs\"", token, name.c_str());
    }
    td_log(buf);
}

void td_do_shader_encode() {
    std::string s = td_get_text(g_td_shader_in);
    if (s.empty()) { MessageBeep(MB_ICONWARNING); return; }
    auto tok = encode_shader_token(s);
    wchar_t buf[260];
    if (tok) {
        swprintf(buf, 260,
                 L"\"%hs\" -> token32 0x%08X  (zero-extended token64: 0x%016llX) -- "
                 L"letters must be lowercase a-y, no j/q/z",
                 s.c_str(), *tok, static_cast<unsigned long long>(*tok));
    } else {
        swprintf(buf, 260,
                 L"\"%hs\" -> not encodable (only lowercase a-y, excluding j/q/z, are valid)",
                 s.c_str());
    }
    td_log(buf);
}

void td_do_bone_encode() {
    std::string s = td_get_text(g_td_bone_in);
    if (s.empty()) { MessageBeep(MB_ICONWARNING); return; }
    uint64_t tok = encode_bone_token64(s);
    uint32_t low32 = static_cast<uint32_t>(tok);
    wchar_t buf[240];
    swprintf(buf, 240,
             L"bone \"%hs\" -> token64 0x%016llX  (low32 0x%08X -- convenience "
             L"truncation, not an independently verified 32-bit format)",
             s.c_str(), static_cast<unsigned long long>(tok), low32);
    td_log(buf);
}

void td_do_fid_decode() {
    std::string s = td_get_text(g_td_fid_in);
    if (s.empty()) { MessageBeep(MB_ICONWARNING); return; }
    std::vector<uint8_t> bytes = parse_hex_bytes(s);
    FilenameDecode r = decode_filename_bytes(bytes);
    wchar_t buf[240];
    if (!r.have_bytes) {
        swprintf(buf, 240, L"filename bytes: need at least 8 hex bytes (got %zu) -- "
                            L"paste lo,hi,third,fourth as 4 little-endian uint16 words",
                 bytes.size());
    } else if (!r.valid) {
        swprintf(buf, 240, L"filename bytes: fileId=0 (invalid reference)");
    } else if (r.sub_id) {
        swprintf(buf, 240, L"filename bytes -> fileId=%ld  subId=%u", r.file_id, r.sub_id);
    } else {
        swprintf(buf, 240, L"filename bytes -> fileId=%ld", r.file_id);
    }
    td_log(buf);
}

// Repositions every child for the window's current client size: edit boxes
// stretch to fill the width available after their row's button(s), which
// stay pinned to the right edge and are individually re-measured so none of
// them can clip their own label. Called after creation and on every WM_SIZE.
void td_relayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    constexpr int kMargin = 10;
    constexpr int kGap = 4;
    constexpr int kRowH = 24;
    constexpr int kLabelH = 16;

    int shader_decode_w = td_button_width(hwnd, L"Decode -> name", 70);
    int shader_encode_w = td_button_width(hwnd, L"Encode -> hex", 70);
    int bone_encode_w = td_button_width(hwnd, L"Encode -> token64", 70);
    int fid_decode_w = td_button_width(hwnd, L"Decode -> fileId", 70);
    int close_w = td_button_width(hwnd, L"Close", 70);

    // Row 1: shader token, two buttons.
    int y = kMargin;
    MoveWindow(g_td_label_shader, kMargin, y, std::max(0, w - 2 * kMargin), kLabelH, TRUE);
    y += kLabelH + 2;
    int shader_btns_w = shader_decode_w + kGap + shader_encode_w;
    int shader_edit_w = std::max(60, w - 2 * kMargin - kGap - shader_btns_w);
    MoveWindow(g_td_shader_in, kMargin, y, shader_edit_w, kRowH, TRUE);
    MoveWindow(g_td_shader_decode_btn, kMargin + shader_edit_w + kGap, y - 1, shader_decode_w, kRowH + 2, TRUE);
    MoveWindow(g_td_shader_encode_btn, kMargin + shader_edit_w + kGap + shader_decode_w + kGap, y - 1,
               shader_encode_w, kRowH + 2, TRUE);
    y += kRowH + 10;

    // Row 2: bone name, one button.
    MoveWindow(g_td_label_bone, kMargin, y, std::max(0, w - 2 * kMargin), kLabelH, TRUE);
    y += kLabelH + 2;
    int bone_edit_w = std::max(60, w - 2 * kMargin - kGap - bone_encode_w);
    MoveWindow(g_td_bone_in, kMargin, y, bone_edit_w, kRowH, TRUE);
    MoveWindow(g_td_bone_encode_btn, kMargin + bone_edit_w + kGap, y - 1, bone_encode_w, kRowH + 2, TRUE);
    y += kRowH + 10;

    // Row 3: filename-record bytes, one button.
    MoveWindow(g_td_label_fid, kMargin, y, std::max(0, w - 2 * kMargin), kLabelH, TRUE);
    y += kLabelH + 2;
    int fid_edit_w = std::max(60, w - 2 * kMargin - kGap - fid_decode_w);
    MoveWindow(g_td_fid_in, kMargin, y, fid_edit_w, kRowH, TRUE);
    MoveWindow(g_td_fid_decode_btn, kMargin + fid_edit_w + kGap, y - 1, fid_decode_w, kRowH + 2, TRUE);
    y += kRowH + 12;

    // Log: fills whatever height/width remains above the Close button.
    MoveWindow(g_td_label_log, kMargin, y, 300, kLabelH, TRUE);
    y += kLabelH + 2;
    int close_y = std::max(y + 40, h - kMargin - 28);
    int log_h = std::max(40, close_y - 8 - y);
    MoveWindow(g_td_output, kMargin, y, std::max(60, w - 2 * kMargin), log_h, TRUE);

    MoveWindow(g_td_close_btn, std::max(kMargin, w - kMargin - close_w), close_y, close_w, 28, TRUE);
}

LRESULT CALLBACK TokenDecoderWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_TD_SHADER_DECODE: td_do_shader_decode(); return 0;
        case ID_TD_SHADER_ENCODE: td_do_shader_encode(); return 0;
        case ID_TD_BONE_ENCODE: td_do_bone_encode(); return 0;
        case ID_TD_FID_DECODE: td_do_fid_decode(); return 0;
        case ID_TD_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_SIZE:
        td_relayout(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        // Keep the window from being squeezed to the point buttons overlap
        // their own edit boxes -- measured once against the widest row
        // (shader: two buttons) plus a usable minimum edit width.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        int shader_btns_w = td_button_width(hwnd, L"Decode -> name", 70) + 4 +
                             td_button_width(hwnd, L"Encode -> hex", 70);
        mmi->ptMinTrackSize.x = 20 + 150 + 4 + shader_btns_w;
        mmi->ptMinTrackSize.y = 340;
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        g_td_wnd = g_td_shader_in = g_td_bone_in = g_td_fid_in = g_td_output = nullptr;
        g_td_shader_decode_btn = g_td_shader_encode_btn = g_td_bone_encode_btn = nullptr;
        g_td_fid_decode_btn = g_td_close_btn = nullptr;
        g_td_label_shader = g_td_label_bone = g_td_label_fid = g_td_label_log = nullptr;
        g_td_log.clear();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void open_token_decoder(HWND owner) {
    if (g_td_wnd) {  // already open -> just bring it forward
        SetForegroundWindow(g_td_wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TokenDecoderWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2TokenDecoderWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    // Resizable (WS_THICKFRAME + min/max boxes) so a user with a bigger label
    // font, a longer bone name, or just a preference for a taller log can
    // drag it -- the earlier fixed-size window clipped its own button text
    // and had no way to grow.
    const int W = 640, H = 520;
    g_td_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2TokenDecoderWnd", L"Decode Token / Filename Bytes",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner, nullptr,
                               g_hinstance, nullptr);
    if (!g_td_wnd) return;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    g_td_font = font;

    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_td_wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };

    g_td_label_shader =
        mk(L"STATIC", L"Shader/material token (hex, 32- or 64-bit, e.g. BEF8FD7F) -- or a name to encode:",
           SS_LEFT, 0);
    g_td_shader_in = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_TD_SHADER_IN);
    g_td_shader_decode_btn = mk(L"BUTTON", L"Decode -> name", BS_PUSHBUTTON, ID_TD_SHADER_DECODE);
    g_td_shader_encode_btn = mk(L"BUTTON", L"Encode -> hex", BS_PUSHBUTTON, ID_TD_SHADER_ENCODE);

    g_td_label_bone =
        mk(L"STATIC", L"Bone name (e.g. bone:COG, Rig1:bone:BSpine01) -- namespace stripped automatically:",
           SS_LEFT, 0);
    g_td_bone_in = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_TD_BONE_IN);
    g_td_bone_encode_btn = mk(L"BUTTON", L"Encode -> token64", BS_PUSHBUTTON, ID_TD_BONE_ENCODE);

    g_td_label_fid =
        mk(L"STATIC", L"Filename record, 8 hex bytes (lo,hi,third,fourth as 4 little-endian words):", SS_LEFT,
           0);
    g_td_fid_in = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_TD_FID_IN);
    g_td_fid_decode_btn = mk(L"BUTTON", L"Decode -> fileId", BS_PUSHBUTTON, ID_TD_FID_DECODE);

    g_td_label_log = mk(L"STATIC", L"Log (most recent on top):", SS_LEFT, 0);
    g_td_output = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                     ID_TD_OUTPUT);
    SendMessageW(g_td_output, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);

    g_td_close_btn = mk(L"BUTTON", L"Close", BS_PUSHBUTTON, ID_TD_CLOSE);

    td_relayout(g_td_wnd);
    ShowWindow(g_td_wnd, SW_SHOW);
    SetFocus(g_td_shader_in);
}

}  // namespace castlemist::ui
