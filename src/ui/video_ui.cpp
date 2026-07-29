/// @file
/// @brief The Bink video transport, its subtitles and CINP cinematic clip list.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <cstdio>

namespace castlemist::ui {

// ---------------------------------------------------------------- Bink video

// Refreshes the transport row: Play/Pause caption, seek position, "m:ss / m:ss
// (frame N/M)" readout, and the toggle check states.
void update_video_ui(bool reset) {
    if (g_app == nullptr || g_app->hwnd_video_seek == nullptr) {
        return;
    }
    const castlemist::vid::Info& vi = castlemist::vid::info();
    double dur = castlemist::vid::duration_seconds();
    double pos = reset ? 0.0 : castlemist::vid::position_seconds();
    uint32_t frame = reset ? 0 : castlemist::vid::current_frame();

    if (!g_app->video_seek_dragging) {
        int permille = (vi.frames > 1) ? static_cast<int>(static_cast<double>(frame) /
                                                          (vi.frames - 1) * kVideoSeekMax + 0.5)
                                       : 0;
        SendMessageW(g_app->hwnd_video_seek, TBM_SETPOS, TRUE, std::clamp(permille, 0, kVideoSeekMax));
    }

    wchar_t t[96];
    swprintf(t, 96, L"%ls / %ls  (%u/%u)", castlemist::core::format_mmss(pos).c_str(), castlemist::core::format_mmss(dur).c_str(),
             vi.frames ? frame + 1 : 0u, vi.frames);
    SetWindowTextW(g_app->hwnd_video_time, t);

    SetWindowTextW(g_app->hwnd_video_play, castlemist::vid::playing() ? L"❚❚ Pause" : L"▶ Play");
    SendMessageW(g_app->hwnd_video_loop, BM_SETCHECK, castlemist::vid::looping() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_app->hwnd_video_mute, BM_SETCHECK, castlemist::vid::muted() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_app->hwnd_video_subs, BM_SETCHECK, g_app->video_subs_on ? BST_CHECKED : BST_UNCHECKED, 0);

    // Subtitle overlay: the last line whose trigger time has passed. Lines have a
    // start time only (the CINP fires them as triggers), so each stays up until
    // the next one -- capped so a trailing line doesn't linger forever.
    // Each cue carries a real start AND end (from the CSCN visibility curve), so
    // this is a plain .srt-style lookup: show the cue whose window contains the
    // playback head, nothing otherwise.
    int want = -1;
    if (g_app->video_subs_on) {
        for (size_t i = 0; i < g_app->video_subs.size(); ++i) {
            const SubtitleLine& l = g_app->video_subs[i];
            if (pos + 1e-3 >= l.time && pos < l.end_time) { want = static_cast<int>(i); break; }
        }
    }
    if (want != g_app->video_sub_shown) {
        g_app->video_sub_shown = want;
        SetWindowTextW(g_app->hwnd_video_subtitle, want < 0 ? L"" : g_app->video_subs[want].text.c_str());
        InvalidateRect(g_app->hwnd_video_subtitle, nullptr, TRUE);
    }
    ShowWindow(g_app->hwnd_video_subtitle, (want >= 0) ? SW_SHOW : SW_HIDE);
}

// Lays the subtitle band across the bottom of the video surface. Called whenever
// the preview pane is (re)sized.
void layout_video_subtitle() {
    if (g_app == nullptr || g_app->hwnd_video_subtitle == nullptr) return;
    RECT rc;
    GetClientRect(g_app->hwnd_preview, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int band_w = std::max(120, w - 40);
    int band_h = 46;
    MoveWindow(g_app->hwnd_video_subtitle, (w - band_w) / 2, std::max(0, h - band_h - 14), band_w, band_h, TRUE);
}

// Pushes the freshly decoded Bink frame into the D3D preview surface and paints it.
void present_video_frame() {
    const uint8_t* px = castlemist::vid::frame_bgra();
    if (px == nullptr) {
        return;
    }
    castlemist::gfx::update_video(px, castlemist::vid::frame_pitch());
    castlemist::gfx::render();
}

// Tears down any video the previous entry left playing. Called on every entry
// switch (and on shutdown) -- crucial, because the player decodes straight out of
// ExtractedEntry::decompressed, which is about to be replaced.
// Closes just the stream (keeps the entry's subtitles/clip list), for switching
// between the movies of one cinematic.
void stop_video_stream() {
    if (g_app == nullptr) return;
    KillTimer(g_app->hwnd_main, TIMER_VIDEO);
    g_app->video_seek_dragging = false;
    g_app->video_sub_shown = -1;
    if (g_app->hwnd_video_subtitle) {
        SetWindowTextW(g_app->hwnd_video_subtitle, L"");
        ShowWindow(g_app->hwnd_video_subtitle, SW_HIDE);
    }
    castlemist::vid::close();
}

void stop_video() {
    if (g_app == nullptr) {
        return;
    }
    KillTimer(g_app->hwnd_main, TIMER_VIDEO);
    g_app->video_seek_dragging = false;
    g_app->cinp_video_bytes.clear();
    g_app->cinp_video_bytes.shrink_to_fit();
    g_app->cinp_video_sel = -1;
    g_app->video_subs.clear();
    g_app->video_subs_searched = false;
    g_app->video_subs_cinp = 0;
    g_app->video_sub_shown = -1;
    if (g_app->hwnd_video_subtitle) {
        SetWindowTextW(g_app->hwnd_video_subtitle, L"");
        ShowWindow(g_app->hwnd_video_subtitle, SW_HIDE);
    }
    castlemist::vid::close();
}

// Fills the audio-track combo ("All tracks" + one row per track). Only ever
// visible for a movie with more than one track.
void populate_video_tracks() {
    HWND cb = g_app->hwnd_video_track;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    uint32_t n = castlemist::vid::track_count();
    if (n <= 1) {
        return;
    }
    // Row 0 is the "hear everything" debug option -- on a cinematic that really
    // means every dubbed language talking over each other, so it is not the
    // default; open() starts on the first voice track instead.
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All tracks (all languages at once)"));
    for (uint32_t i = 0; i < n; ++i) {
        std::string lbl = castlemist::vid::track_label(i);
        std::wstring w(lbl.begin(), lbl.end());
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
    int sel = castlemist::vid::active_track(); // -1 = mixed -> row 0
    SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(sel < 0 ? 0 : sel + 1), 0);
}

// Fills the CINP movie selector ("Movie 1/3 — fileId N").
void populate_video_clips() {
    HWND cb = g_app->hwnd_video_clip;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    const auto& ids = g_app->current_entry.cinp_video_ids;
    for (size_t i = 0; i < ids.size(); ++i) {
        wchar_t it[80];
        swprintf(it, 80, L"Movie %zu/%zu — fileId %u", i + 1, ids.size(), ids[i]);
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it));
    }
    if (!ids.empty()) {
        SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(std::max(0, g_app->cinp_video_sel)), 0);
    }
}

// Locates the CINP cinematic that drives the current movie and loads its timed
// dialogue. Runs once per entry, on demand (the Subs button), because it reads
// and decompresses every CINP pack -- a couple of seconds. Needs the index DB
// open, which is where the CINP list comes from.
void search_video_subtitles(uint32_t mft_index) {
    g_app->video_subs.clear();
    g_app->video_subs_cinp = 0;
    g_app->video_sub_shown = -1;
    g_app->video_subs_searched = true;

    if (!castlemist::db::is_open()) {
        SetWindowTextW(g_app->hwnd_status_label,
                       L"Subtitles need the index DB (File → Open Index DB…) to list the CINP scripts.");
        return;
    }
    std::vector<uint32_t> fids = get_by_file_id(g_app->data_gw2, mft_index + 1);
    if (fids.empty()) {
        SetWindowTextW(g_app->hwnd_status_label, L"No fileId for this entry — cannot match a cinematic.");
        return;
    }
    std::vector<uint32_t> cinps = castlemist::db::query_base_ids("", "CINP", 0, false, false, 100000);
    if (cinps.empty()) {
        SetWindowTextW(g_app->hwnd_status_label, L"No CINP cinematics in the index.");
        return;
    }

    HCURSOR prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    wchar_t msg[160];
    swprintf(msg, 160, L"Searching %zu cinematic scripts for this movie…", cinps.size());
    SetWindowTextW(g_app->hwnd_status_label, msg);
    UpdateWindow(g_app->hwnd_status_label);

    uint32_t found = 0;
    for (uint32_t fid : fids) {
        found = find_video_subtitles(g_app->data_gw2.file_info.file_path, cinps, fid, g_app->video_subs);
        if (found) break;
    }
    SetCursor(prev);

    g_app->video_subs_cinp = found;
    if (found) {
        swprintf(msg, 160, L"%zu subtitle lines from CINP %u", g_app->video_subs.size(), found);
    } else {
        swprintf(msg, 160, L"No cinematic script references this movie — no subtitles available.");
    }
    SetWindowTextW(g_app->hwnd_status_label, msg);
}

// Opens the entry's Bink stream in place and shows its first frame (paused).
// Returns an empty string on success, or a human-readable reason on failure.
std::wstring start_video(const ExtractedEntry& e) {
    if (!castlemist::vid::open(e.decompressed.data(), e.decompressed.size())) {
        std::string err = castlemist::vid::last_error();
        std::wstring w(err.begin(), err.end());
        return w.empty() ? L"the Bink runtime could not open this stream" : w;
    }
    const castlemist::vid::Info& vi = castlemist::vid::info();
    castlemist::gfx::begin_video(vi.width, vi.height);
    // Video frames are opaque; the checkerboard alpha composite is for textures.
    castlemist::gfx::set_alpha_aware(false);
    populate_video_tracks();
    layout_video_subtitle();
    present_video_frame();
    update_video_ui(true);
    // ~120 Hz poll. pump() is a cheap BinkWait check until a frame is actually
    // due, so this costs almost nothing between frames and keeps A/V sync tight.
    SetTimer(g_app->hwnd_main, TIMER_VIDEO, 8, nullptr);
    return {};
}

// What a CINP ties together, for the text surface / info panel.
std::wstring cinematic_info_text(const ExtractedEntry& e, const std::wstring& error) {
    wchar_t b[400];
    swprintf(b, 400, L"Cinematic (CINP / CSCN v%d scene script)\r\n"
                     L"────────────────────────────────\r\n"
                     L"Sequences    %u\r\nMovies       %zu\r\nOther assets %zu\r\n"
                     L"Dialogue     %u slot(s), %zu with timed text\r\n",
             e.cinp_version, e.cinp_sequences, e.cinp_video_ids.size(), e.cinp_other_ids.size(),
             e.cinp_text_resources, e.subtitles.size());
    std::wstring s = b;
    for (size_t i = 0; i < e.cinp_video_ids.size(); ++i) {
        swprintf(b, 256, L"  movie %zu: fileId %u\r\n", i + 1, e.cinp_video_ids[i]);
        s += b;
    }
    for (uint32_t fid : e.cinp_other_ids) {
        swprintf(b, 256, L"  asset  : fileId %u\r\n", fid);
        s += b;
    }
    if (e.cinp_video_ids.empty()) {
        s += L"\r\nThis is an IN-ENGINE cinematic: it directs actors, cameras and\r\n"
             L"sound on a timeline rather than playing a pre-rendered movie, so\r\n"
             L"there is nothing to play back here.\r\n";
        if (e.cinp_text_resources > 0 && e.subtitles.empty()) {
            s += L"Its dialogue slots carry no inline text -- the lines live in the\r\n"
                 L"localized string packs, reached by a token this browser cannot\r\n"
                 L"resolve yet.\r\n";
        }
    } else if (!error.empty()) {
        s += L"\r\nPlayback unavailable: " + error + L"\r\n";
    }
    if (!e.subtitles.empty()) {
        s += L"\r\nSubtitles\r\n";
        for (const SubtitleLine& l : e.subtitles) {
            swprintf(b, 256, L"  %6.2fs  ", l.time);
            s += b + l.text + L"\r\n";
        }
    }
    return s;
}

// Loads the Nth movie a CINP references and starts it. The CINP already gave us
// the subtitles, so nothing has to be searched for. Returns "" or a reason.
std::wstring start_cinp_video(int index) {
    const auto& ids = g_app->current_entry.cinp_video_ids;
    if (index < 0 || index >= static_cast<int>(ids.size())) return L"this cinematic references no movie";

    HCURSOR prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    wchar_t s[128];
    swprintf(s, 128, L"Loading movie fileId %u…", ids[index]);
    SetWindowTextW(g_app->hwnd_status_label, s);
    UpdateWindow(g_app->hwnd_status_label);

    stop_video_stream();
    g_app->cinp_video_bytes = load_video_by_fileid(g_app->data_gw2.file_info.file_path, ids[index]);
    SetCursor(prev);
    SetWindowTextW(g_app->hwnd_status_label, L"Ready");
    if (g_app->cinp_video_bytes.empty()) return L"the referenced movie could not be read from the archive";

    g_app->cinp_video_sel = index;
    // Each movie is its own CSCN sequence with its own timeline, so only that
    // sequence's cues belong to it.
    int seq = (index < static_cast<int>(g_app->current_entry.cinp_video_seq.size()))
                  ? g_app->current_entry.cinp_video_seq[index]
                  : -1;
    g_app->video_subs.clear();
    for (const SubtitleLine& l : g_app->current_entry.subtitles) {
        if (seq < 0 || l.sequence == seq) g_app->video_subs.push_back(l);
    }
    g_app->video_sub_shown = -1;
    if (!castlemist::vid::open(g_app->cinp_video_bytes.data(), g_app->cinp_video_bytes.size())) {
        std::string err = castlemist::vid::last_error();
        return std::wstring(err.begin(), err.end());
    }
    const castlemist::vid::Info& vi = castlemist::vid::info();
    castlemist::gfx::begin_video(vi.width, vi.height);
    castlemist::gfx::set_alpha_aware(false);
    populate_video_tracks();
    layout_video_subtitle();
    present_video_frame();
    update_video_ui(true);
    SetTimer(g_app->hwnd_main, TIMER_VIDEO, 8, nullptr);
    return {};
}

// Info text shown under/behind the video surface (and when playback is impossible).
std::wstring video_info_text(const VideoMeta& v, const std::wstring& error) {
    wchar_t buf[1200];
    std::wstring runtime = L"(not loaded)";
    if (castlemist::vid::runtime_available()) {
        const std::string& ver = castlemist::vid::runtime_version();
        std::wstring wver(ver.begin(), ver.end());
        runtime = castlemist::vid::runtime_path();
        if (!wver.empty()) {
            runtime += L"   [" + wver + L"]";
        }
    }
    swprintf(buf, 1200,
             L"Video — %hs\r\n"
             L"────────────────────────────────\r\n"
             L"Resolution   %u x %u\r\n"
             L"Frames       %u\r\n"
             L"Frame rate   %.3f fps  (%u/%u)\r\n"
             L"Duration     %.2f s   (%ls)\r\n"
             L"Audio tracks %u\r\n"
             L"Alpha plane  %ls\r\n"
             L"────────────────────────────────\r\n"
             L"Runtime      %ls\r\n%ls",
             v.codec.c_str(), v.width, v.height, v.frames,
             v.fps_den ? static_cast<double>(v.fps_num) / v.fps_den : 0.0, v.fps_num, v.fps_den,
             v.seconds, castlemist::core::format_mmss(v.seconds).c_str(), v.tracks, v.has_alpha ? L"yes" : L"no",
             runtime.c_str(),
             error.empty() ? L"" : (L"\r\nPlayback unavailable: " + error + L"\r\n").c_str());
    std::wstring out = buf;
    // Per-track detail, once the stream is open and the tracks were probed.
    for (uint32_t i = 0; i < castlemist::vid::track_count(); ++i) {
        std::string lbl = castlemist::vid::track_label(i);
        out += L"  " + std::wstring(lbl.begin(), lbl.end()) + L"\r\n";
    }
    if (castlemist::vid::track_count() > 1) {
        out += L"\r\nThis movie carries one voice track per dubbed language "
               L"(the game plays track id 0 + language+1).\r\n";
    }
    if (!g_app->video_subs.empty()) {
        wchar_t hdr[96];
        swprintf(hdr, 96, L"\r\nSubtitles — %zu lines (CINP %u)\r\n",
                 g_app->video_subs.size(), g_app->video_subs_cinp);
        out += hdr;
        for (const SubtitleLine& l : g_app->video_subs) {
            wchar_t t[32];
            swprintf(t, 32, L"  %6.2fs  ", l.time);
            out += t + l.text + L"\r\n";
        }
    }
    return out;
}

// Render the clicked cntc sub-asset into the right-pane surfaces (texture / model /
// audio info / text), reusing the existing image/model/text controls.

} // namespace castlemist::ui
