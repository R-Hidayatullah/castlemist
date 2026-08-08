/// @file
/// @brief Application startup: window classes, the main window, and the message loop.
///
/// The startup order here is load-bearing:
///  1. The Common-Controls v6 activation context must be in force *before*
///     InitCommonControlsEx, or every control falls back to the Windows 95 look.
///  2. Direct3D initialisation happens per surface, from each surface's WM_CREATE.
///  3. The auto-loaders (struct template, string keys, index DB) run after the
///     window exists and immediately before the message loop -- never from
///     WM_CREATE, where a thrown exception aborts window creation and the
///     process exits silently after ~2 seconds with no window and no WER event.

#include "detail/app_state.h"

#include "castlemist/core/env.h"
#include "castlemist/core/text.h"
#include "castlemist/ui/application.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <thread>

#include <map>

namespace castlemist::ui {

namespace {

/// @brief Where the GW2_* debug hooks write their dumps.
///
/// These paths used to be a hard-coded absolute directory on one developer's
/// machine, which meant every dump silently went nowhere for everybody else.
/// GW2_DUMP_DIR overrides; otherwise it is the user's temp directory, which
/// always exists and is always writable.
///
/// Returns a path with a trailing separator.
const std::string& dump_dir() {
    static const std::string dir = [] {
        if (const char* env = std::getenv("GW2_DUMP_DIR"); env != nullptr && *env != '\0') {
            std::string d = env;
            if (d.back() != '/' && d.back() != '\\') d.push_back('\\');
            return d;
        }
        char buf[MAX_PATH] = {0};
        DWORD n = GetTempPathA(MAX_PATH, buf);
        return n > 0 ? std::string(buf, n) : std::string(".\\");
    }();
    return dir;
}

/// @brief dump_dir() + @p name, for the one-off dump filenames below.
std::string dump_path(const char* name) { return dump_dir() + name; }

/// @brief `castlemist.exe [<Gw2.dat>] [<baseId>]`
///
/// Opening a 87 GB archive through a file dialog every time gets old quickly,
/// and a baseId on the command line means a link to an entry can be shared as
/// a command. Both arguments are optional and a bad one is ignored rather than
/// fatal: the window is already up, so the user can still open a dat by hand.
void apply_command_line(HWND hwnd) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return;

    if (argc > 1 && GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES) {
        if (load_dat_path(hwnd, argv[1]) && argc > 2) {
            // baseId is the MFT index plus one -- the ids in
            // docs/research/curated-test-ids.txt are baseIds.
            wchar_t* end = nullptr;
            unsigned long base = wcstoul(argv[2], &end, 10);
            if (end != argv[2] && base > 0) {
                // baseId is 1-based over the MFT, so the entry is base - 1.
                //
                // The list is NOT told to select that row. Its rows are not MFT
                // indices -- entries without a baseId are skipped, so row 2870
                // is some other entry entirely -- and setting the selection
                // fires the list's own callback, which then races this one and
                // wins. Loading the entry directly is unambiguous; the list
                // simply stays where it is.
                on_entry_selected(static_cast<uint32_t>(base - 1));
            }
        }
    }
    LocalFree(argv);
}

} // namespace

int run(HINSTANCE hInstance, int cmd_show) {
    g_hinstance = hInstance;

    // Themed common controls and DPI awareness now come from the manifest
    // embedded by src/app/castlemist.rc.in, which applies before this runs.

    // Before any window class is registered: the class background brush is
    // taken from the palette, so the very first paint is already themed and
    // the window never flashes light before switching to dark.
    load_theme_preference();
    ensure_ui_fonts();

    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX),
                             ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES |
                                 ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    castlemist::hex::register_class(hInstance);
    castlemist::ui::register_splitter_class(hInstance);
    castlemist::texpanel::register_class(hInstance);
    castlemist::structtree::register_class(hInstance);

    WNDCLASSEXW preview_class{};
    preview_class.cbSize = sizeof(preview_class);
    preview_class.style = CS_DBLCLKS;
    preview_class.lpfnWndProc = PreviewWndProc;
    preview_class.hInstance = hInstance;
    preview_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    preview_class.hbrBackground = nullptr;
    preview_class.lpszClassName = kPreviewClassName;
    RegisterClassExW(&preview_class);

    WNDCLASSEXW model_class{};
    model_class.cbSize = sizeof(model_class);
    model_class.style = CS_DBLCLKS;
    model_class.lpfnWndProc = ModelWndProc;
    model_class.hInstance = hInstance;
    model_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    model_class.hbrBackground = nullptr;
    model_class.lpszClassName = kModelClassName;
    RegisterClassExW(&model_class);

    // The icon is resource 1 in castlemist.exe (see src/app/resource.h).
    // LoadIconW picks the size Windows asks for out of the multi-size .ico, so
    // the title bar gets the 16px artwork and Alt-Tab the 32px one -- each
    // drawn for that size rather than scaled down from 256.
    HICON app_icon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    HICON app_icon_small = static_cast<HICON>(
        LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));

    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.style = CS_HREDRAW | CS_VREDRAW;
    main_class.lpfnWndProc = MainWndProc;
    main_class.hInstance = hInstance;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hIcon = app_icon;
    main_class.hIconSm = app_icon_small ? app_icon_small : app_icon;
    main_class.hbrBackground = theme_brush(kColBg);
    main_class.lpszClassName = kMainClassName;
    RegisterClassExW(&main_class);

    HWND hwnd = CreateWindowExW(0, kMainClassName, L"castlemist", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1500, 950, nullptr, build_menu(), hInstance, nullptr);
    if (hwnd == nullptr) {
        return 0;
    }

    ACCEL accel_entries[] = {{FVIRTKEY, VK_RETURN, static_cast<WORD>(ID_SEARCH_BUTTON)}};
    HACCEL accel_table = CreateAcceleratorTable(accel_entries, 1);

    sync_theme_menu();
    apply_titlebar_theme(hwnd);

    ShowWindow(hwnd, cmd_show);
    UpdateWindow(hwnd);
    refresh_theme(hwnd); // children exist by now; push the palette into them

    apply_command_line(hwnd);

    // Debug: GW2_SCENE=<mftIndex> builds a synthetic coordinated scene (a grid of
    // that model at varied positions/rotations) to verify the map scene renderer.
    if (const char* sc = std::getenv("GW2_SCENE")) {
        try {
            load_dat_file(g_app->data_gw2,
                          "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            g_app->dat_loaded = true;
            uint32_t idx = static_cast<uint32_t>(atoi(sc));
            ExtractedEntry e = extract_entry(g_app->data_gw2, idx);
            if (e.model) {
                std::vector<ModelPreview> models{*e.model};
                std::vector<castlemist::render::SceneInstance> insts;
                float step = e.model->radius * 2.6f;
                for (int gz = 0; gz < 3; ++gz)
                    for (int gx = 0; gx < 3; ++gx) {
                        castlemist::render::SceneInstance in;
                        in.model = 0;
                        in.pos[0] = (gx - 1) * step; in.pos[1] = 0; in.pos[2] = (gz - 1) * step;
                        in.rot[1] = (gx + gz) * 0.6f; // vary yaw per cell
                        in.scale = 1.0f;
                        insts.push_back(in);
                    }
                g_app->current_entry = std::move(e);
                castlemist::render::on_resize(1000, 800); // size the offscreen swapchain for the headless shot
                castlemist::render::set_scene(models, insts);
                castlemist::render::save_screenshot(dump_path("shot_scene.bmp").c_str());
                // GW2_SCENEGAME=1: also exercise the map GAME-shader path + inset
                // preview (the single-model extract carried gameMaterials).
                if (std::getenv("GW2_SCENEGAME")) {
                    castlemist::render::build_scene_game_materials(models);
                    castlemist::render::set_mode(castlemist::render::RenderMode::GameShader);
                    castlemist::render::save_screenshot(dump_path("shot_scene_game.bmp").c_str());
                    castlemist::render::set_scene_focus(0); // show the inset preview of model 0
                    castlemist::render::save_screenshot(dump_path("shot_scene_inset.bmp").c_str());
                }
            }
        } catch (const std::exception& ex) { MessageBoxA(hwnd, ex.what(), "scene failed", MB_OK); }
    }

    // Debug: GW2_AUDIOTEST=<mftIndex> exercises the audio detect+decode+play path
    // and logs the result (playback is best-effort in a headless environment).
    if (const char* at = std::getenv("GW2_AUDIOTEST")) {
        FILE* lf = std::fopen(dump_path("audiotest.txt").c_str(), "w");
        try {
            load_dat_file(g_app->data_gw2,
                          "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            uint32_t idx = static_cast<uint32_t>(atoi(at));
            ExtractedEntry e = extract_entry(g_app->data_gw2, idx);
            bool isAudio = (e.kind == PreviewKind::Audio) && !e.audio_clips.empty();
            const std::vector<uint8_t> empty;
            const auto& ad = isAudio ? e.audio_clips.front().data : empty;
            castlemist::snd::ClipInfo info = isAudio ? castlemist::snd::probe(ad.data(), ad.size()) : castlemist::snd::ClipInfo{};
            bool played = isAudio && castlemist::snd::play(ad.data(), ad.size());
            if (lf) std::fprintf(lf, "index=%u kind=%d isAudio=%d clips=%zu codec=%s bytes=%zu probe.ok=%d %uHz %uch %.2fs play=%d\n",
                    idx, (int)e.kind, (int)isAudio, e.audio_clips.size(),
                    isAudio ? e.audio_clips.front().codec.c_str() : "", ad.size(),
                    (int)info.ok, info.sampleRate, info.channels, info.seconds, (int)played);
            if (played) {
                double dur = castlemist::snd::duration_seconds();
                Sleep(400); double p1 = castlemist::snd::position_seconds();
                bool sk = castlemist::snd::seek(dur * 0.5); double p2 = castlemist::snd::position_seconds();
                if (lf) std::fprintf(lf, "duration=%.2f pos@400ms=%.2f seek(50%%)=%d pos_after_seek=%.2f\n",
                                     dur, p1, (int)sk, p2);
                Sleep(400); castlemist::snd::stop();
            }
        } catch (const std::exception& ex) { if (lf) std::fprintf(lf, "exception: %s\n", ex.what()); }
        if (lf) std::fclose(lf);
        return 0;
    }

    // Debug: GW2_PARSETEST=<baseId,baseId,...> extracts each entry and dumps its
    // detected kind + the head of its text preview -- validates the eula / ABIX /
    // txt* / CSCN summary parsers against the live dat.
    if (const char* pt = std::getenv("GW2_PARSETEST")) {
        FILE* lf = std::fopen(dump_path("parsetest.txt").c_str(), "w");
        try {
            load_dat_file(g_app->data_gw2,
                          "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            std::string ids(pt);
            size_t start = 0;
            while (start < ids.size()) {
                size_t comma = ids.find(',', start);
                std::string tok = ids.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                start = (comma == std::string::npos) ? ids.size() : comma + 1;
                if (tok.empty()) continue;
                uint32_t base = (uint32_t)strtoul(tok.c_str(), nullptr, 10);
                if (base == 0) continue;
                try {
                    ExtractedEntry e = extract_entry(g_app->data_gw2, base - 1);
                    std::string head;
                    for (size_t i = 0; i < e.text_preview.size() && head.size() < 700; ++i) {
                        wchar_t c = e.text_preview[i];
                        head += (c >= 32 && c < 127) ? (char)c : (c == L'\n' ? '\n' : (c == L'\r' ? ' ' : '.'));
                    }
                    if (lf) {
                        std::fprintf(lf, "=== base %u : kind=%d ===\n", base, (int)e.kind);
                        if (e.is_image) {
                            unsigned long long sum = 0;
                            for (uint8_t px : e.preview_pixels) sum += px;
                            std::fprintf(lf, "image %ux%u pitch=%u  label=\"%s\"  pixelsum=%llu\n",
                                         e.preview_width, e.preview_height, e.preview_pitch,
                                         e.preview_format_label.c_str(), sum);
                        }
                        std::fprintf(lf, "%s\n\n", head.c_str());
                    }
                } catch (const std::exception& ex) {
                    if (lf) std::fprintf(lf, "=== base %u : EXCEPTION %s ===\n\n", base, ex.what());
                }
            }
        } catch (const std::exception& ex) { if (lf) std::fprintf(lf, "load exception: %s\n", ex.what()); }
        if (lf) std::fclose(lf);
        return 0;
    }

    // Debug: GW2_VIDEOTEST=<baseId,baseId,...> extracts each entry, checks it is
    // detected as a Bink cinematic, opens it through dumps/binaries/bink2w64.dll,
    // decodes a run of frames, seeks, and dumps a BMP of the seeked frame.
    if (const char* vt = std::getenv("GW2_VIDEOTEST")) {
        FILE* lf = std::fopen(dump_path("videotest.txt").c_str(), "w");
        auto log = [&](const char* fmt, auto... a) { if (lf) { std::fprintf(lf, fmt, a...); std::fflush(lf); } };
        auto dump_bmp = [](const char* path, const uint8_t* bgra, int w, int h) {
            BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1;
            ih.biBitCount = 32; ih.biCompression = BI_RGB; ih.biSizeImage = w * h * 4;
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + ih.biSizeImage;
            FILE* f = std::fopen(path, "wb");
            if (!f) return;
            std::fwrite(&fh, sizeof(fh), 1, f); std::fwrite(&ih, sizeof(ih), 1, f);
            std::fwrite(bgra, 1, (size_t)w * h * 4, f); std::fclose(f);
        };
        log("runtime_available=%d\n", (int)castlemist::vid::runtime_available());
        // The normal index auto-load runs after this hook returns, so open it here
        // -- the subtitle search needs the CINP list.
        {
            std::string ierr;
            bool iok = castlemist::db::open(L"dumps/index/gw2_index.db", ierr);
            log("index db open=%d (%s)\n", (int)iok, ierr.c_str());
        }
        {
            const std::wstring& p = castlemist::vid::runtime_path();
            std::string ap(p.begin(), p.end());
            log("runtime_path=%s  version=%s  err=%s\n", ap.c_str(), castlemist::vid::runtime_version().c_str(),
                castlemist::vid::last_error().c_str());
        }
        try {
            load_dat_file(g_app->data_gw2,
                          "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            std::string ids(vt);
            size_t start = 0;
            int which = 0;
            while (start < ids.size()) {
                size_t comma = ids.find(',', start);
                std::string tok = ids.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                start = (comma == std::string::npos) ? ids.size() : comma + 1;
                if (tok.empty()) continue;
                uint32_t base = (uint32_t)strtoul(tok.c_str(), nullptr, 10);
                if (base == 0) continue;
                try {
                    ExtractedEntry e = extract_entry(g_app->data_gw2, base - 1);
                    const VideoMeta& v = e.video;
                    log("=== base %u : kind=%d  decompressed=%zu bytes ===\n", base, (int)e.kind,
                        e.decompressed.size());
                    if (e.kind == PreviewKind::Cinematic) {
                        log("  CINEMATIC: %zu movie(s), %zu other asset(s), %zu subtitle lines\n",
                            e.cinp_video_ids.size(), e.cinp_other_ids.size(), e.subtitles.size());
                        for (uint32_t f : e.cinp_video_ids) log("    movie fileId %u\n", f);
                        for (uint32_t f : e.cinp_other_ids) log("    asset fileId %u\n", f);
                        for (const SubtitleLine& l : e.subtitles) {
                            std::string t(l.text.begin(), l.text.end());
                            log("    [seq %d] %7.2f - %7.2fs  %s\n", l.sequence, l.time, l.end_time,
                                t.c_str());
                        }
                        // Load + open the first movie exactly as the UI would.
                        if (!e.cinp_video_ids.empty()) {
                            std::vector<uint8_t> mv = load_video_by_fileid(
                                g_app->data_gw2.file_info.file_path, e.cinp_video_ids[0]);
                            log("    loaded movie 1: %zu bytes\n", mv.size());
                            if (!mv.empty() && castlemist::vid::open(mv.data(), mv.size())) {
                                log("    opened %ux%u %u frames, %u track(s)\n", castlemist::vid::info().width,
                                    castlemist::vid::info().height, castlemist::vid::info().frames, castlemist::vid::track_count());
                                for (uint32_t ti = 0; ti < castlemist::vid::track_count(); ++ti)
                                    log("      %s\n", castlemist::vid::track_label(ti).c_str());
                                castlemist::vid::close();
                            } else {
                                log("    open FAILED: %s\n", castlemist::vid::last_error().c_str());
                            }
                        }
                        log("\n");
                        continue;
                    }
                    if (e.kind != PreviewKind::Video) { log("  NOT detected as video\n\n"); continue; }
                    log("  %s  %ux%u  %u frames  %u/%u fps  %.2fs  tracks=%u alpha=%d\n",
                        v.codec.c_str(), v.width, v.height, v.frames, v.fps_num, v.fps_den, v.seconds,
                        v.tracks, (int)v.has_alpha);
                    if (!castlemist::vid::open(e.decompressed.data(), e.decompressed.size())) {
                        log("  OPEN FAILED: %s\n\n", castlemist::vid::last_error().c_str());
                        continue;
                    }
                    log("  opened; has_audio=%d  tracks=%u\n", (int)castlemist::vid::has_audio(),
                        castlemist::vid::track_count());
                    for (uint32_t ti = 0; ti < castlemist::vid::track_count(); ++ti) {
                        const castlemist::vid::TrackInfo* tk = castlemist::vid::track(ti);
                        log("    %s  [type=0x%08X maxFrame=%u]\n", castlemist::vid::track_label(ti).c_str(),
                            tk ? tk->type : 0, tk ? tk->max_size : 0);
                    }
                    // Exercise track selection: mixed -> each track -> back to mixed.
                    for (int sel = -1; sel < (int)castlemist::vid::track_count(); ++sel) {
                        castlemist::vid::set_active_track(sel);
                        log("    set_active_track(%d) -> %d\n", sel, castlemist::vid::active_track());
                    }
                    castlemist::vid::set_active_track(-1);
                    // Subtitles: find the CINP that drives this movie and dump
                    // its timed dialogue. Needs the index DB for the CINP list.
                    if (castlemist::db::is_open()) {
                        std::vector<uint32_t> cinps =
                            castlemist::db::query_base_ids("", "CINP", 0, false, false, 100000);
                        std::vector<uint32_t> fids = get_by_file_id(g_app->data_gw2, base - 1 + 1);
                        std::vector<SubtitleLine> subs;
                        uint32_t cinp = 0;
                        LARGE_INTEGER q0, q1, qf; QueryPerformanceFrequency(&qf);
                        QueryPerformanceCounter(&q0);
                        for (uint32_t fid : fids) {
                            cinp = find_video_subtitles(g_app->data_gw2.file_info.file_path, cinps, fid, subs);
                            if (cinp) break;
                        }
                        QueryPerformanceCounter(&q1);
                        log("  subtitle search: %zu CINP packs in %.2fs -> CINP %u, %zu lines\n",
                            cinps.size(), (double)(q1.QuadPart - q0.QuadPart) / qf.QuadPart, cinp, subs.size());
                        for (const SubtitleLine& l : subs) {
                            std::string t(l.text.begin(), l.text.end());
                            log("      %7.2fs  %s\n", l.time, t.c_str());
                        }
                    } else {
                        log("  subtitle search skipped: index DB not open\n");
                    }
                    int w = (int)castlemist::vid::info().width, h = (int)castlemist::vid::info().height;
                    // Decode a run of frames as the timer would, then seek.
                    castlemist::vid::play();
                    int decoded = 0;
                    LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
                    while (decoded < 60) { if (castlemist::vid::pump()) ++decoded; else Sleep(1); }
                    QueryPerformanceCounter(&t1);
                    double wall = (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart;
                    log("  played %d frames in %.2fs (expected %.2fs), now at frame %u (%.2fs)\n",
                        decoded, wall, decoded * (double)castlemist::vid::info().fps_den / castlemist::vid::info().fps_num,
                        castlemist::vid::current_frame(), castlemist::vid::position_seconds());
                    castlemist::vid::pause();
                    uint32_t mid = castlemist::vid::info().frames / 2;
                    LARGE_INTEGER s0, s1; QueryPerformanceCounter(&s0);
                    castlemist::vid::seek_frame(mid);
                    QueryPerformanceCounter(&s1);
                    unsigned long long sum = 0;
                    const uint8_t* px = castlemist::vid::frame_bgra();
                    for (size_t k = 0; px && k < (size_t)w * h * 4; k += 4) sum += px[k] + px[k+1] + px[k+2];
                    log("  seek->%u took %.0f ms, frame=%u rgbsum=%llu\n", mid,
                        (double)(s1.QuadPart - s0.QuadPart) * 1000.0 / fq.QuadPart,
                        castlemist::vid::current_frame(), sum);
                    char path[256];
                    std::snprintf(path, sizeof(path),
                                  dump_path("videotest_%d_%u.bmp").c_str(), which, base);
                    if (px) dump_bmp(path, px, w, h);
                    log("  wrote %s\n\n", path);
                    castlemist::vid::close();
                    ++which;
                } catch (const std::exception& ex) {
                    log("=== base %u : EXCEPTION %s ===\n\n", base, ex.what());
                }
            }
        } catch (const std::exception& ex) { log("load exception: %s\n", ex.what()); }
        castlemist::vid::shutdown();
        if (lf) std::fclose(lf);
        return 0;
    }

    // Debug: GW2_PIMGTEST=<mftIndex> composites a PIMG atlas and writes the RGBA
    // preview to a BMP for visual verification.
    if (const char* pt = std::getenv("GW2_PIMGTEST")) {
        FILE* lf = std::fopen(dump_path("pimgtest.txt").c_str(), "w");
        try {
            load_dat_file(g_app->data_gw2, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            uint32_t idx = static_cast<uint32_t>(atoi(pt));
            ExtractedEntry e = extract_entry(g_app->data_gw2, idx);
            if (lf) std::fprintf(lf, "index=%u kind=%d is_image=%d %ux%u fmt=%s pxbytes=%zu textlen=%zu\n",
                    idx, (int)e.kind, (int)e.is_image, e.preview_width, e.preview_height,
                    e.preview_format_label.c_str(), e.preview_pixels.size(), e.text_preview.size());
            if (!e.text_preview.empty()) {
                int len = WideCharToMultiByte(CP_UTF8, 0, e.text_preview.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string u8(len > 0 ? len : 0, '\0');
                if (len > 0) WideCharToMultiByte(CP_UTF8, 0, e.text_preview.c_str(), -1, u8.data(), len, nullptr, nullptr);
                FILE* tf = std::fopen(dump_path("cntc_text.txt").c_str(), "w");
                if (tf) { std::fwrite(u8.data(), 1, u8.size(), tf); std::fclose(tf); }
            }
            // Content browser: verify a few referenced assets load into real sub-entries.
            if (e.kind == PreviewKind::Content && lf) {
                std::fprintf(lf, "content_asset_ids=%zu objects=%zu\n",
                             e.content_asset_ids.size(), e.content_objects.size());
                // Type histogram + codename samples: verifies the indexEntry-derived
                // contentType and the cntc string-table codename resolution.
                std::map<uint32_t, int> hist;
                for (const auto& o : e.content_objects) hist[o.type]++;
                int t = 0;
                for (const auto& kv : hist) {
                    const char* nm = content_type_name(kv.first);
                    std::fprintf(lf, "  type %-4u %-22s n=%d\n", kv.first, nm ? nm : "(unnamed)", kv.second);
                    if (++t >= 12) { std::fprintf(lf, "  ... %zu types total\n", hist.size()); break; }
                }
                int named = 0, labelled = 0;
                for (const auto& o : e.content_objects) {
                    if (!o.name.empty()) ++named;
                    if (!o.labels.empty()) ++labelled;
                }
                std::fprintf(lf, "  with slug id: %d / %zu   with readable labels: %d\n",
                             named, e.content_objects.size(), labelled);
                // Show objects that HAVE labels -- these are the ones the old
                // "first string = name" rule got wrong.
                int s = 0;
                for (const auto& o : e.content_objects) {
                    if (o.labels.empty()) continue;
                    const char* nm = content_type_name(o.type);
                    std::fprintf(lf, "  [%s] id=%-7u slug=%-13s labels=", nm ? nm : "?", o.id,
                                 o.name.empty() ? "-" : o.name.c_str());
                    for (size_t i = 0; i < o.labels.size() && i < 4; ++i)
                        std::fprintf(lf, "%s\"%s\"", i ? ", " : "", o.labels[i].c_str());
                    std::fprintf(lf, "\n");
                    if (++s >= 8) break;
                }
                int shown = 0;
                for (size_t i = 0; i < e.content_asset_ids.size() && shown < 8; i += 1 + e.content_asset_ids.size() / 8) {
                    uint32_t fid = e.content_asset_ids[i];
                    uint32_t base = get_by_base_id(g_app->data_gw2, fid);
                    if (!base) { std::fprintf(lf, "  fileId %u -> (not in dat)\n", fid); continue; }
                    try {
                        ExtractedEntry sub = extract_entry(g_app->data_gw2, base - 1);
                        std::fprintf(lf, "  fileId %u -> kind=%d is_image=%d %ux%u model=%d\n",
                                     fid, (int)sub.kind, (int)sub.is_image, sub.preview_width, sub.preview_height,
                                     (int)(sub.model != nullptr));
                        ++shown;
                    } catch (...) { std::fprintf(lf, "  fileId %u -> (extract failed)\n", fid); }
                }
            }
            if (e.is_image && !e.preview_pixels.empty()) {
                int w = (int)e.preview_width, h = (int)e.preview_height;
                int rowB = w * 3, pad = (4 - (rowB & 3)) & 3, stride = rowB + pad;
                uint32_t dataSize = stride * h, fileSize = 54 + dataSize;
                std::vector<uint8_t> bmp(fileSize, 0);
                uint8_t hdr[54] = {'B','M'};
                std::memcpy(hdr + 2, &fileSize, 4); uint32_t off = 54; std::memcpy(hdr + 10, &off, 4);
                uint32_t ih = 40; std::memcpy(hdr + 14, &ih, 4); std::memcpy(hdr + 18, &w, 4); std::memcpy(hdr + 22, &h, 4);
                uint16_t planes = 1, bpp = 24; std::memcpy(hdr + 26, &planes, 2); std::memcpy(hdr + 28, &bpp, 2);
                std::memcpy(bmp.data(), hdr, 54);
                for (int y = 0; y < h; ++y) {
                    const uint8_t* row = e.preview_pixels.data() + (size_t)(h - 1 - y) * w * 4;
                    uint8_t* d = bmp.data() + 54 + (size_t)y * stride;
                    for (int x = 0; x < w; ++x) { d[x*3+0] = row[x*4+2]; d[x*3+1] = row[x*4+1]; d[x*3+2] = row[x*4+0]; }
                }
                FILE* f = std::fopen(dump_path("pimg_preview.bmp").c_str(), "wb");
                if (f) { std::fwrite(bmp.data(), 1, bmp.size(), f); std::fclose(f); }
            }
        } catch (const std::exception& ex) { if (lf) std::fprintf(lf, "exception: %s\n", ex.what()); }
        if (lf) std::fclose(lf);
        return 0;
    }


    // Debug: GW2_AUTOLOAD=<mftIndex> loads the Steam dat and applies that entry
    // synchronously (drives the real model pipeline for headless skinning checks).
    if (const char* al = std::getenv("GW2_AUTOLOAD")) {
        try {
            load_dat_file(g_app->data_gw2,
                          "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            g_app->dat_loaded = true;
            uint32_t idx = static_cast<uint32_t>(atoi(al));
            ExtractedEntry e = extract_entry(g_app->data_gw2, idx);
            bool isMap = (e.kind == PreviewKind::Map);
            apply_extracted_entry(idx, std::move(e));
            castlemist::render::on_resize(1000, 800);
            if (const char* li = std::getenv("GW2_LIGHTINT")) castlemist::render::set_light_intensity((float)atof(li));
            // Debug: GW2_ORBIT="yaw,pitch" (radians) + GW2_ZOOM=factor to frame a
            // specific detail headlessly (e.g. view the roof caps from below).
            if (const char* ob = std::getenv("GW2_ORBIT")) {
                float y = 0, p = 0; sscanf(ob, "%f,%f", &y, &p); castlemist::render::orbit(y, p);
            }
            if (const char* zm = std::getenv("GW2_ZOOM")) {
                float z = (float)atof(zm); if (z > 0.01f) castlemist::render::zoom(z);
            }
            if (isMap) {
                castlemist::render::save_screenshot(dump_path("shot_map.bmp").c_str());
                castlemist::render::set_layer_visible(castlemist::render::LAYER_COLLISION, true);
                castlemist::render::save_screenshot(dump_path("shot_map_coll.bmp").c_str());
                castlemist::render::set_layer_visible(castlemist::render::LAYER_COLLISION, false);
                auto zl = build_map_zone_layer(g_app->current_entry.decompressed, g_app->data_gw2.file_info.file_path);
                if (zl && !zl->instances.empty()) {
                    std::vector<castlemist::render::SceneInstance> zi;
                    for (const MapInstance& mi : zl->instances) {
                        castlemist::render::SceneInstance si; si.model = mi.model;
                        for (int k = 0; k < 3; ++k) { si.pos[k] = mi.pos[k]; si.rot[k] = mi.rot[k]; }
                        si.scale = mi.scale; si.layer = mi.layer; zi.push_back(si);
                    }
                    castlemist::render::add_scene_models(zl->models, zi);
                }
                castlemist::render::set_layer_visible(castlemist::render::LAYER_ZONE, true);
                castlemist::render::save_screenshot(dump_path("shot_map_zone.bmp").c_str());
            } else {
                castlemist::render::set_show_skeleton(false); // isolate the MESH
                castlemist::render::set_animation(-1);
                castlemist::render::save_screenshot(dump_path("shot_bind.bmp").c_str());
                // Debug: GW2_GIZMOTEST exercises the Blender-style gizmo headlessly --
                // draws each mode, applies a programmatic transform, and checks that
                // hit-testing an axis tip returns that axis.
                if (std::getenv("GW2_GIZMOTEST")) {
                    const std::string base = dump_dir();
                    castlemist::render::set_show_grid(true);
                    castlemist::render::set_gizmo_mode(castlemist::render::GizmoMode::Translate);
                    castlemist::render::reset_object_transform();
                    castlemist::render::save_screenshot((base + "shot_gizmo_move.bmp").c_str());
                    float p[3] = {0.4f, 0.2f, 0.0f}, r[3] = {0, 35, 0}, s[3] = {1.2f, 1.0f, 0.8f};
                    castlemist::render::set_object_transform(p, r, s);
                    castlemist::render::save_screenshot((base + "shot_gizmo_xform.bmp").c_str());
                    castlemist::render::set_gizmo_mode(castlemist::render::GizmoMode::Rotate);
                    castlemist::render::save_screenshot((base + "shot_gizmo_rot.bmp").c_str());
                    castlemist::render::set_gizmo_mode(castlemist::render::GizmoMode::Scale);
                    castlemist::render::save_screenshot((base + "shot_gizmo_scale.bmp").c_str());
                    castlemist::render::reset_object_transform();
                    castlemist::render::set_gizmo_mode(castlemist::render::GizmoMode::Translate);
                    // Simulate a drag along +Y: begin near center, move up on screen.
                    int cx = 500, cy = 400;
                    int hit = castlemist::render::gizmo_pick(cx, cy);
                    bool grabbed = castlemist::render::gizmo_begin(cx, cy - 60);
                    if (grabbed) { castlemist::render::gizmo_update(cx, cy - 160); castlemist::render::gizmo_end(); }
                    float op[3], orr[3], os[3];
                    castlemist::render::get_object_transform(op, orr, os);
                    FILE* gf = std::fopen((base + "gizmotest.txt").c_str(), "w");
                    if (gf) {
                        std::fprintf(gf, "pick@center=%d grabbed=%d after-drag pos=(%.3f,%.3f,%.3f)\n", hit,
                                     grabbed ? 1 : 0, op[0], op[1], op[2]);
                        std::fclose(gf);
                    }
                    castlemist::render::save_screenshot((base + "shot_gizmo_drag.bmp").c_str());
                }
                // Debug: GW2_LODTEST dumps wireframe at LOD0 vs the highest LOD (to see
                // triangle reduction) + a reduced-texture shot, then restores.
                if (std::getenv("GW2_LODTEST")) {
                    int nsub = castlemist::render::submesh_count(), maxlod = castlemist::render::max_lod_count();
                    std::printf("LODTEST: submeshes=%d maxLODs=%d\n", nsub, maxlod);
                    castlemist::render::set_mode(castlemist::render::RenderMode::Wireframe);
                    castlemist::render::set_submesh_lod(-1, 0);
                    castlemist::render::save_screenshot(dump_path("shot_lod0.bmp").c_str());
                    castlemist::render::set_submesh_lod(-1, maxlod - 1);
                    castlemist::render::save_screenshot(dump_path("shot_lodmax.bmp").c_str());
                    castlemist::render::set_submesh_lod(-1, 0);
                    castlemist::render::set_mode(castlemist::render::RenderMode::Full);
                    castlemist::render::set_submesh_tex_reduced(-1, true);
                    castlemist::render::save_screenshot(dump_path("shot_texreduced.bmp").c_str());
                    castlemist::render::set_submesh_tex_reduced(-1, false);
                }
                // Baked particle/light effects test: warm the sim over real time
                // (each save_screenshot renders one frame with wall-clock dt), then
                // capture two frames to prove the effects animate on an idle model.
                if (std::getenv("GW2_FXTEST")) {
                    const std::string base = dump_dir();
                    FILE* lg = fopen(dump_path("fxtest.txt").c_str(), "w");
                    int c=0,e=0,l=0,live=0;
                    castlemist::render::effect_stats(c,e,l,live);
                    if (lg) fprintf(lg, "clouds=%d emitters=%d lights=%d\n", c,e,l);
                    castlemist::render::set_mode(castlemist::render::RenderMode::Full);
                    for (int i = 0; i < 50; ++i) { castlemist::render::render(); Sleep(20); }
                    castlemist::render::effect_stats(c,e,l,live);
                    if (lg) fprintf(lg, "after warmup: live=%d\n", live);
                    castlemist::render::save_screenshot((std::string(base)+"shot_fx_a.bmp").c_str());
                    for (int i = 0; i < 20; ++i) { castlemist::render::render(); Sleep(20); }
                    castlemist::render::effect_stats(c,e,l,live);
                    if (lg) fprintf(lg, "frame B: live=%d\n", live);
                    castlemist::render::save_screenshot((std::string(base)+"shot_fx_b.bmp").c_str());
                    // Also in Shader mode (real game materials on the particles).
                    castlemist::render::set_mode(castlemist::render::RenderMode::GameShader);
                    for (int i = 0; i < 20; ++i) { castlemist::render::render(); Sleep(20); }
                    castlemist::render::save_screenshot((std::string(base)+"shot_fx_shader.bmp").c_str());
                    if (lg) fclose(lg);
                }
                // Live cloth simulation test: capture the rest mesh, enable cloth,
                // warm the solver over real frames, then capture the draped mesh +
                // log displacement stats. PROOF it works: cloth_enabled() true (the
                // sim built), locked>0 (top band pinned), maxDisp grows > 0 while the
                // draped BMP differs from the rest BMP.
                if (std::getenv("GW2_CLOTHTEST")) {
                    const std::string base = dump_dir();
                    FILE* lg = fopen(dump_path("clothtest.txt").c_str(), "w");
                    castlemist::render::set_mode(castlemist::render::RenderMode::Full);
                    castlemist::render::render();
                    castlemist::render::save_screenshot((base + "shot_cloth_rest.bmp").c_str());
                    if (lg) fprintf(lg, "has_cloth=%d\n", castlemist::render::has_cloth() ? 1 : 0);
                    castlemist::render::set_cloth_enabled(true);
                    if (lg) fprintf(lg, "cloth_enabled=%d\n", castlemist::render::cloth_enabled() ? 1 : 0);
                    int v = 0, lk = 0; float mx = 0, mn = 0;
                    for (int i = 0; i < 120; ++i) { castlemist::render::render(); Sleep(16); }  // ~2 s of draping
                    castlemist::render::cloth_stats(v, lk, mx, mn);
                    if (lg) fprintf(lg, "verts=%d locked=%d maxDisp=%.3f meanDisp=%.3f\n", v, lk, mx, mn);
                    castlemist::render::save_screenshot((base + "shot_cloth_draped.bmp").c_str());
                    // Wind test: blow +X and let it settle, prove the sheet reacts.
                    castlemist::render::set_cloth_wind(600.0f, 0.0f, 0.0f);
                    for (int i = 0; i < 90; ++i) { castlemist::render::render(); Sleep(16); }
                    castlemist::render::cloth_stats(v, lk, mx, mn);
                    if (lg) fprintf(lg, "after wind: maxDisp=%.3f meanDisp=%.3f\n", mx, mn);
                    castlemist::render::save_screenshot((base + "shot_cloth_wind.bmp").c_str());
                    castlemist::render::set_cloth_wind(0, 0, 0);
                    castlemist::render::set_cloth_enabled(false);
                    if (lg) fclose(lg);
                }
                // Real game (bgfx DXBC) shaders: bind pose first.
                castlemist::render::set_mode(castlemist::render::RenderMode::GameShader);
                castlemist::render::save_screenshot(dump_path("shot_shader.bmp").c_str());
                // Accurate Granny animation + original DXBC shaders together: play the
                // first real motion clip and capture two distinct frames in Shader mode.
                {
                    int motion = castlemist::render::first_motion_animation();
                    if (motion >= 0) {
                        float du = castlemist::render::animation_duration(motion);
                        castlemist::render::set_animation(motion);
                        castlemist::render::set_anim_time(du * 0.30f);
                        castlemist::render::save_screenshot(dump_path("shot_shader_anim1.bmp").c_str());
                        castlemist::render::set_anim_time(du * 0.70f);
                        castlemist::render::save_screenshot(dump_path("shot_shader_anim2.bmp").c_str());
                    }
                }
                castlemist::render::set_animation(-1);
                castlemist::render::set_mode(castlemist::render::RenderMode::Full);
                // Pose the longest real embedded clip (to check whether the sword
                // assembles into the hand vs the separated zeropose).
                {
                    int best = -1; float bestDur = 0.05f;
                    for (int ci = 0; ci < castlemist::render::animation_count(); ++ci) {
                        float du = castlemist::render::animation_duration(ci);
                        if (du > bestDur) { bestDur = du; best = ci; }
                    }
                    if (best >= 0) {
                        castlemist::render::set_animation(best);
                        castlemist::render::set_anim_time(bestDur * 0.5f);
                        castlemist::render::save_screenshot(dump_path("shot_clip.bmp").c_str());
                    }
                    castlemist::render::set_animation(-1);
                }
            }
        } catch (const std::exception& ex) {
            MessageBoxA(hwnd, ex.what(), "autoload failed", MB_OK);
        }
    }

    // Best-effort: open the index under dumps/index/ so the browser comes up
    // already indexed against the latest Gw2.dat build (File > Open Index DB
    // overrides). Runs here -- after the window is shown and past the headless
    // GW2_* self-test hooks (which return early) -- rather than in WM_CREATE, so
    // its synchronous .dat MFT load can't abort window creation and doesn't
    // interfere with the debug hooks. Exceptions are contained.
    try {
        try_autoload_index(hwnd);
    } catch (const std::exception&) {
        // Fall back to manual File > Open Index DB; the app still runs.
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (accel_table == nullptr || !TranslateAcceleratorW(hwnd, accel_table, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

} // namespace castlemist::ui
