/// @file
/// @brief The audio transport (clip selection, info, seek bar) and the strs table.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <cstdio>

#include "castlemist/format/strs_view.h"
#include "castlemist/format/strs_keys.h"

namespace castlemist::ui {

ExtractedEntry& active_audio_entry() {
    if (g_app->has_loaded_entry && g_app->current_entry.kind == PreviewKind::Content && g_app->content_sub_loaded)
        return g_app->content_sub;
    return g_app->current_entry;
}

int audio_selected_index() {
    if (active_audio_entry().audio_clips.size() <= 1) return 0;
    int s = static_cast<int>(SendMessageW(g_app->hwnd_audio_combo, CB_GETCURSEL, 0, 0));
    return s < 0 ? 0 : s;
}

// Fill the strs table with one row per string record. `base` is the file's
// baseTextId (from the txtm-derived textbase map, -1 when unknown) so each row can
// show its real global textId; packed records are RC4-decrypted when their key is
// loaded, otherwise flagged. Returns the row count.
size_t populate_strs_table(const std::vector<uint8_t>& bytes, long long base) {
    HWND lv = g_app->hwnd_strs_list;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    // castlemist::skeys::records also decrypts; it falls back to plain framing without keys.
    std::vector<castlemist::strs::Record> recs = castlemist::skeys::records(bytes.data(), bytes.size(), base);
    for (const auto& r : recs) {
        wchar_t num[16], tid[24], enc[64], len[16];
        swprintf(num, 16, L"%u", r.index);
        if (r.textId >= 0) swprintf(tid, 24, L"%lld", r.textId); else wcscpy(tid, L"—");
        if (!r.packed) {
            wcscpy(enc, L"raw UTF-16");
        } else if (r.locked) {
            swprintf(enc, 64, L"packed rb=%u \x1F513 no key", r.rangeBits);
        } else {
            swprintf(enc, 64, L"packed rb=%u decrypted", r.rangeBits);
        }
        swprintf(len, 16, L"%u", r.recLen);
        // Collapse newlines so multi-line strings stay on one row.
        std::wstring text = r.text;
        for (auto& ch : text) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (r.locked) text = L"(encrypted — RC4 key not captured)";
        lv_add_row(lv, static_cast<LPARAM>(r.index), num, tid, enc, len, text.c_str());
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
    return recs.size();
}

// "m:ss" for a duration in seconds (negative/NaN -> 0:00).

void update_audio_info(int sel) {
    const auto& clips = active_audio_entry().audio_clips;
    if (sel < 0 || sel >= static_cast<int>(clips.size())) {
        g_app->audio_sel_info = castlemist::snd::ClipInfo{};
        return;
    }
    const auto& c = clips[sel];
    castlemist::snd::ClipInfo info = castlemist::snd::probe(c.data.data(), c.data.size());
    g_app->audio_sel_info = info;   // drives the seek bar's total time

    wchar_t buf[900];
    if (info.ok) {
        // Average bitrate of the encoded stream (file bytes over decoded duration).
        double kbps = (info.seconds > 0) ? (double)c.data.size() * 8.0 / info.seconds / 1000.0 : 0.0;
        unsigned long long frames =
            (unsigned long long)(info.seconds * info.sampleRate + 0.5);
        swprintf(buf, 900,
                 L"Audio — %hs%ls\r\n"
                 L"────────────────────────────────\r\n"
                 L"Sound        %d of %zu\r\n"
                 L"Duration     %.2f s   (%ls)\r\n"
                 L"Sample rate  %u Hz\r\n"
                 L"Channels     %u  (%ls)\r\n"
                 L"Bit depth    16-bit PCM (decoded)\r\n"
                 L"Samples      %llu frames\r\n"
                 L"Bitrate      ~%.0f kbps\r\n"
                 L"Encoded size %zu bytes\r\n"
                 L"────────────────────────────────\r\n"
                 L"Press  ▶ Play  to listen; drag the bar to seek.",
                 c.codec.c_str(), clips.size() > 1 ? L"  (bank)" : L"",
                 sel + 1, clips.size(),
                 info.seconds, castlemist::core::format_mmss(info.seconds).c_str(),
                 info.sampleRate,
                 info.channels, castlemist::core::channel_layout_name(info.channels),
                 frames, kbps, c.data.size());
    } else {
        swprintf(buf, 900,
                 L"Audio — %hs\r\n────────────────────────────────\r\n"
                 L"Sound        %d of %zu\r\n"
                 L"Encoded size %zu bytes\r\n\r\n"
                 L"Could not decode this sound for playback.",
                 c.codec.c_str(), sel + 1, clips.size(), c.data.size());
    }
    SetWindowTextW(g_app->hwnd_text_preview, buf);
}

// Reflect the live playback head onto the seek trackbar + "pos / dur" label.
// When `reset`, force the bar back to 0 (a fresh sound / stop).
void update_audio_seek_ui(bool reset) {
    // Total time comes from the SELECTED clip's probe (always current), falling back
    // to the loaded player's clip. That way picking another sound in a bank shows its
    // duration right away, without having to press Play first.
    double dur = g_app->audio_sel_info.ok ? g_app->audio_sel_info.seconds
                                          : castlemist::snd::duration_seconds();
    double pos = reset ? 0.0 : castlemist::snd::position_seconds();
    if (!g_app->audio_seek_dragging) {
        int permille = (dur > 0) ? static_cast<int>(pos / dur * kAudioSeekMax + 0.5) : 0;
        if (permille < 0) permille = 0; else if (permille > kAudioSeekMax) permille = kAudioSeekMax;
        SendMessageW(g_app->hwnd_audio_seek, TBM_SETPOS, TRUE, permille);
    }
    std::wstring t = castlemist::core::format_mmss(pos) + L" / " + castlemist::core::format_mmss(dur);
    SetWindowTextW(g_app->hwnd_audio_time, t.c_str());
}


} // namespace castlemist::ui
