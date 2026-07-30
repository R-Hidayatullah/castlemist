/// @file
/// @brief The Win32 shell's shared state: window handles, control ids and the app model.
///
/// castlemist's UI is one window with one selected entry, so its state is one
/// struct. It is split across several translation units by *area of the window*
/// -- layout, preview, audio transport, video transport, content browser, file
/// commands, dialogs, window procedures -- and this header is the seam between
/// them.
///
/// @warning Not a public header -- internal to `src/ui/`.

#pragma once

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "castlemist/native/gw2dat.h"
#include "castlemist/ui/theme.h"

#include "castlemist/db/index_db.h"
#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/chat_link.h"
#include "castlemist/format/content_map.h"
#include "castlemist/media/audio_player.h"
#include "castlemist/media/video_player.h"
#include "castlemist/render/d3d_renderer.h"
#include "castlemist/render/model_renderer.h"
#include "castlemist/ui/hexview.h"
#include "castlemist/ui/info_panel.h"
#include "castlemist/ui/mft_listview.h"
#include "castlemist/ui/splitter.h"
#include "castlemist/ui/texture_panel.h"

namespace castlemist::ui {

constexpr wchar_t kMainClassName[] = L"Gw2BrowserMain";
constexpr wchar_t kPreviewClassName[] = L"Gw2PreviewSurface";
constexpr wchar_t kModelClassName[] = L"Gw2ModelSurface";

constexpr int kSplitterThickness = 5;
constexpr int kSearchBarHeight = 118;  // id row + Type/Container row + Content row
constexpr int kMinPaneSize = 80;
constexpr int kTabHeight = 26;
constexpr int kToolbarHeight = 28;
constexpr int kStatusBarHeight = 26;

constexpr UINT_PTR ID_FILE_OPEN = 1001;
constexpr UINT_PTR ID_FILE_EXPORT_COMPRESSED = 1002;
constexpr UINT_PTR ID_FILE_EXPORT_DECOMPRESSED = 1003;
constexpr UINT_PTR ID_FILE_EXIT = 1004;
constexpr UINT_PTR ID_FILE_LOAD_TEMPLATE = 1005;
constexpr UINT_PTR ID_FILE_LOAD_KEYS = 1006;
constexpr UINT_PTR ID_FILE_OPEN_INDEX = 1007;
constexpr UINT_PTR ID_TOOLS_DECODE_LINK = 1008;
// View > Theme.
constexpr UINT_PTR ID_VIEW_THEME_DARK   = 1010;
constexpr UINT_PTR ID_VIEW_THEME_LIGHT  = 1011;
constexpr UINT_PTR ID_VIEW_THEME_CUSTOM = 1012;
constexpr UINT_PTR ID_VIEW_THEME_ACCENT = 1013;
// Chat-link decoder popup controls.
constexpr int ID_CL_INPUT = 2070;
constexpr UINT_PTR ID_CL_DECODE = 2071;
constexpr int ID_CL_OUTPUT = 2072;
constexpr UINT_PTR ID_CL_SEARCH_BASE = 2073;
constexpr UINT_PTR ID_CL_SEARCH_FILE = 2074;
constexpr UINT_PTR ID_CL_CLOSE = 2075;
constexpr UINT_PTR ID_CL_RESOLVE = 2076;
constexpr UINT_PTR ID_CL_OPEN_ICON = 2077;
constexpr UINT_PTR ID_CL_OPEN_MODEL = 2078;
constexpr int ID_LISTVIEW = 2001;
constexpr int ID_HEX_BEFORE = 2002;
constexpr int ID_HEX_AFTER = 2003;
constexpr int ID_INFO_PANEL = 2004;
constexpr int ID_TAB = 2005;
constexpr int ID_SPLIT_LIST_MIDDLE = 2010;
constexpr int ID_SPLIT_MIDDLE_INFO = 2011;
constexpr int ID_SPLIT_CONTENT = 2012;    // cntc: browser block | preview
constexpr int ID_SPLIT_CONTENT_H = 2013;  // cntc: top tables | assets table
constexpr int ID_SEARCH_EDIT = 2020;
constexpr int ID_SEARCH_FILEID_CHECK = 2021;
constexpr UINT_PTR ID_SEARCH_BUTTON = 2022;
constexpr UINT_PTR ID_CLEAR_BUTTON = 2023;
constexpr int ID_FILTER_TYPE = 2024;
constexpr int ID_FILTER_CONTAINER = 2025;
constexpr int ID_FILTER_CONTENT = 2026;

/// @brief One entry in the index-mode "Content" filter combo.
///
/// Maps a human label to a ::castlemist::db::ContentFilter. Kept here so the
/// combo that displays them and the query that applies them cannot drift: the
/// combo is populated by walking this table, and the selected index reads back
/// out of it.
struct ContentFilterChoice {
    const wchar_t* label;
    const char* magics;         ///< comma-separated entries.magic values, "" = any
    const char* require_chunk;  ///< chunk fourcc the entry must carry
    const char* exclude_chunk;  ///< chunk fourcc the entry must NOT carry
};

/// Index 0 must stay the no-op so "(any content)" is the default selection.
///
/// The MODL pair is the reason this exists: 198330 entries declare the MODL
/// container but only 183166 carry a GEOM chunk, so "container = MODL" alone
/// mixes 15164 animation-only files in with the actual meshes. Note "DDS "
/// carries a trailing pad byte in the index -- that is the stored magic, not a
/// typo.
/// The ArenaNet texture family. `gw2_atex.hpp` accepts ATEX/ATTX/ATEC/ATEP/ATEU/
/// ATET; the archive additionally carries CTEX, and the C-prefixed variants pair
/// with the A-prefixed ones. Listed in full rather than trimmed to what one
/// archive happens to hold, so the filter still works on a different .dat.
#define CM_TEXTURE_MAGICS "ATEX,ATTX,ATEC,ATEP,ATEU,ATET,CTEX,CTTX,CTEC,CTEP,CTEU,CTET"

inline constexpr ContentFilterChoice kContentFilters[] = {
    {L"(any content)",                  "",                  "",     ""},
    // Counts below are from the shipped archive, as a sanity check on the predicate.
    {L"3D model - has GEOM mesh",       "",                  "GEOM", ""},        // 183166
    // NOT just "no GEOM" -- that matches every texture and sound too (624989).
    // Anim-only models are the ones that carry ANIM but no mesh.
    {L"Anim only - ANIM, no mesh",      "",                  "ANIM", "GEOM"},    // 15164
    {L"Rigged - has SKEL",              "",                  "SKEL", ""},        // 183166
    {L"Has collision - COLL",           "",                  "COLL", ""},        // 111626
    {L"Has properties - PRPS",          "",                  "PRPS", ""},
    {L"Texture - any ArenaNet",         CM_TEXTURE_MAGICS,   "",     ""},        // 425437
    {L"Texture - ATEX",                 "ATEX",              "",     ""},        // 334887
    {L"Texture - ATEU",                 "ATEU",              "",     ""},        // 46439
    {L"Texture - ATEP",                 "ATEP",              "",     ""},        // 44045
    {L"Texture - ATEC",                 "ATEC",              "",     ""},
    {L"Texture - ATTX",                 "ATTX",              "",     ""},
    {L"Texture - ATET",                 "ATET",              "",     ""},
    {L"Texture - CTEX",                 "CTEX",              "",     ""},        // 66
    {L"Texture - DDS",                  "DDS ",              "",     ""},        // 17727 (padded magic)
    {L"Cinematic scene - CSCN",         "",                  "CSCN", ""},        // 1446
    {L"Material shaders - BGFX",        "",                  "BGFX", ""},        // 2817
    {L"Map props - prp2",               "",                  "prp2", ""},        // 284
    // The fourcc really is "trn." with a trailing dot, like "env." and "msn.".
    {L"Map terrain - trn.",             "",                  "trn.", ""},        // 284
};
constexpr UINT_PTR ID_ZOOM_IN = 2030;
constexpr UINT_PTR ID_ZOOM_OUT = 2031;
constexpr UINT_PTR ID_ROTATE = 2032;
constexpr UINT_PTR ID_FIT = 2033;
constexpr UINT_PTR ID_MODE_FULL = 2034;
constexpr UINT_PTR ID_MODE_PLAIN = 2035;
constexpr UINT_PTR ID_MODE_WIRE = 2036;
constexpr UINT_PTR ID_MODEL_RESET = 2037;
constexpr UINT_PTR ID_MODE_SKEL = 2038;
constexpr UINT_PTR ID_MODE_SHADER = 2039;
constexpr UINT_PTR ID_ANIM_COMBO = 2050;
constexpr UINT_PTR ID_ANIM_PLAY = 2051;
constexpr UINT_PTR ID_LAYER_PROP = 2052;
constexpr UINT_PTR ID_LAYER_ZONE = 2053;
constexpr UINT_PTR ID_LAYER_COLL = 2054;
constexpr UINT_PTR ID_TEX_FULLRES = 2055;
constexpr UINT_PTR ID_AUDIO_PLAY = 2056;
constexpr UINT_PTR ID_AUDIO_STOP = 2057;
constexpr UINT_PTR ID_AUDIO_COMBO = 2058;
constexpr UINT_PTR ID_CONTENT_LIST = 2059;   // master: content types
constexpr UINT_PTR ID_CONTENT_CHILD = 2079;  // child: entries of the selected type
constexpr UINT_PTR ID_LIGHT_PREPASS = 2060;
constexpr UINT_PTR ID_ALPHA_TOGGLE = 2061;
constexpr UINT_PTR ID_SUBMESH_COMBO = 2062;
constexpr UINT_PTR ID_LOD_COMBO = 2063;
constexpr UINT_PTR ID_TEX_REDUCED = 2064;
constexpr UINT_PTR ID_EFFECTS_TOGGLE = 2065;
constexpr UINT_PTR ID_LIGHT_SLIDER = 2081;   // model light intensity trackbar
constexpr UINT_PTR ID_LIGHT_ANGLE = 2090;    // model headlight angle trackbar
constexpr UINT_PTR ID_LIGHT_FOLLOW = 2091;   // "follow camera" (headlight) toggle
constexpr UINT_PTR ID_CONTENT_ASSET_LIST = 2082; // cntc entry asset selector (sortable table)
// Blender-style transform gizmo controls (single-model surface).
constexpr UINT_PTR ID_GIZMO_MOVE = 2083;
constexpr UINT_PTR ID_GIZMO_ROTATE = 2084;
constexpr UINT_PTR ID_GIZMO_SCALE = 2085;
constexpr UINT_PTR ID_GIZMO_GRID = 2086;
constexpr UINT_PTR ID_GIZMO_RESET = 2087;
constexpr UINT_PTR ID_TEX_PANEL = 2103;     // single-model: "Textures" toggle for the panel below
constexpr int ID_TEX_INFO = 2104;           // the per-submesh texture panel itself (bottom right)
constexpr int ID_SPLIT_INFO_TEX = 2105;     // horizontal: info panel | texture panel
constexpr UINT_PTR ID_MAP_PREVIEW = 2088;   // map: toggle the picked-prop inset preview
constexpr UINT_PTR ID_CLOTH_TOGGLE = 2089;  // single-model: live cloth simulation on/off
constexpr UINT_PTR ID_AUDIO_SEEK = 2092;    // audio playback position / seek trackbar
constexpr UINT_PTR ID_STRS_LIST = 2093;     // strs string-table report table
// Bink video player controls (Preview surface for KB2*/BIK* cinematics).
constexpr UINT_PTR ID_VIDEO_PLAY = 2094;    // Play / Pause toggle
constexpr UINT_PTR ID_VIDEO_STOP = 2095;    // stop + rewind to frame 1
constexpr UINT_PTR ID_VIDEO_LOOP = 2096;    // loop toggle
constexpr UINT_PTR ID_VIDEO_MUTE = 2097;    // mute toggle
constexpr UINT_PTR ID_VIDEO_SEEK = 2098;    // position / seek trackbar (permille)
constexpr UINT_PTR ID_VIDEO_VOLUME = 2099;  // volume trackbar (0..100)
constexpr UINT_PTR ID_VIDEO_TRACK = 2100;   // audio-track selector (multi-track Binks)
constexpr UINT_PTR ID_VIDEO_SUBS = 2101;    // subtitle toggle (searches the CINP scripts)
constexpr UINT_PTR ID_VIDEO_CLIP = 2102;    // which movie of a CINP cinematic to play
constexpr UINT_PTR TIMER_ANIM = 1;
constexpr UINT_PTR TIMER_AUDIO = 2;         // ~10 Hz refresh of the audio seek bar / time
constexpr UINT_PTR TIMER_VIDEO = 3;         // video frame pump (see on_video_tick)
constexpr int kAudioSeekMax = 1000;         // seek trackbar range (permille of duration)
constexpr int kVideoSeekMax = 1000;         // video seek trackbar range (permille of frames)
constexpr int ID_STATUS_LABEL = 2040;
constexpr int ID_PROGRESS = 2041;

constexpr UINT WM_APP_EXTRACT_DONE = WM_APP + 1;
constexpr UINT WM_APP_CMAP_DONE = WM_APP + 2;

enum class MiddleTab { Compressed = 0, Decompressed = 1, Preview = 2 };

// Result of a background extract_entry() call, handed back to the UI thread
// via PostMessageW (WPARAM = generation, LPARAM = heap pointer to this,
// ownership transferred to whoever handles WM_APP_EXTRACT_DONE).
struct ExtractResult {
    uint64_t generation = 0;
    uint32_t mft_index = 0;
    bool success = false;
    std::string error_message;
    ExtractedEntry entry;
};

inline HINSTANCE g_hinstance = nullptr;
inline HMENU g_file_menu = nullptr;
inline HMENU g_theme_menu = nullptr;  // View > Theme, for the radio mark

struct AppState {
    Gw2Dat data_gw2;
    ExtractedEntry current_entry;
    uint32_t current_mft_index = 0;
    bool dat_loaded = false;

    // Index-DB navigation (Stage 2). When an index is loaded, the list gains
    // Type/Container columns + filters. index_meta maps base_id -> interned
    // (typeIdx<<16 | contIdx) so 800k rows cost ~3MB, not per-cell SQL.
    bool index_loaded = false;
    std::vector<std::string> idx_type_names, idx_cont_names;
    std::unordered_map<uint32_t, uint32_t> index_meta;
    // Real decompressed size per base_id, straight from the index. base_ids are
    // dense (1..N with density 1.000 on a retail dat), so a flat array indexed by
    // base_id is both the smallest and fastest choice (~6MB for 808k entries) --
    // the list asks for this on every visible-row repaint.
    std::vector<uint64_t> index_usize;
    bool has_loaded_entry = false; // current_entry reflects a *completed* extraction, safe to export

    // Bumped on every new selection (and on opening a new archive); a
    // background result is only applied if its snapshot still matches this
    // when it comes back -- anything older is silently discarded, which is
    // what makes "select something else before the old one finishes" work.
    uint64_t request_generation = 0;

    HWND hwnd_main = nullptr;
    HWND hwnd_status_label = nullptr;
    HWND hwnd_progress = nullptr;
    HWND hwnd_search_edit = nullptr;
    HWND hwnd_filter_type = nullptr;       // index-mode type filter combo
    HWND hwnd_filter_container = nullptr;  // index-mode container filter combo
    HWND hwnd_filter_content = nullptr;    // index-mode "what does it contain" filter combo
    HWND hwnd_search_fileid_check = nullptr;
    HWND hwnd_search_button = nullptr;
    HWND hwnd_clear_button = nullptr;
    HWND hwnd_list = nullptr;
    HWND hwnd_tab = nullptr;
    HWND hwnd_preview = nullptr;      // image D3D surface (gw2gfx)
    HWND hwnd_model = nullptr;        // model D3D surface (gw2m3d)
    HWND hwnd_text_preview = nullptr; // text read-only edit
    HWND hwnd_strs_list = nullptr;    // strs string table as a sortable report table
    HWND hwnd_content_list = nullptr;  // master: cntc content TYPES (kind == Content)
    HWND hwnd_content_child = nullptr; // child: the entries (objects) of the selected type
    int content_type_sel = -1;         // selected master type row (into content_types)
    int content_obj_sel = -1;          // selected entry -> index into content_objects
    // Distinct content types present in current_entry.content_objects, each with
    // its object count. Master list row i <-> content_types[i]. Rebuilt per entry.
    std::vector<std::pair<uint32_t, uint32_t>> content_types;
    // Child list row -> index into content_objects (the entries of the selected type).
    std::vector<int> content_child_objidx;
    HWND hwnd_content_asset_list = nullptr; // asset selector table for the selected entry
    // Per-table sort state (0 = types, 1 = entries, 2 = assets); col < 0 = unsorted.
    int content_sort_col[3] = {-1, -1, -1};
    bool content_sort_asc[3] = {true, true, true};
    ExtractedEntry content_sub;       // the asset currently selected in the content list
    bool content_sub_loaded = false;  // content_sub holds a valid loaded asset
    HWND hwnd_info = nullptr;
    // Bottom of the right-hand column: the per-submesh texture panel, with its own
    // horizontal splitter against the info panel above it. Only claims space while
    // a model preview is up and the "Textures" toggle is on.
    HWND hwnd_tex_info = nullptr;
    HWND hwnd_split_info_tex = nullptr;
    double tex_panel_ratio = 0.42;   // fraction of the right column the panel gets
    HWND hwnd_hex_before = nullptr;
    HWND hwnd_hex_after = nullptr;
    HWND hwnd_split_list_middle = nullptr;
    HWND hwnd_split_middle_info = nullptr;
    HWND hwnd_zoom_in = nullptr;
    HWND hwnd_zoom_out = nullptr;
    HWND hwnd_rotate = nullptr;
    HWND hwnd_fit = nullptr;
    HWND hwnd_alpha = nullptr;
    HWND hwnd_mode_full = nullptr;
    HWND hwnd_mode_plain = nullptr;
    HWND hwnd_mode_wire = nullptr;
    HWND hwnd_mode_shader = nullptr;
    HWND hwnd_model_reset = nullptr;
    HWND hwnd_skel_toggle = nullptr;
    HWND hwnd_anim_combo = nullptr;
    HWND hwnd_anim_play = nullptr;
    HWND hwnd_tex_fullres = nullptr;
    HWND hwnd_light_toggle = nullptr;
    HWND hwnd_effects_toggle = nullptr;
    HWND hwnd_cloth_toggle = nullptr;
    HWND hwnd_light_label = nullptr;   // "Light" caption for the intensity slider
    HWND hwnd_light_slider = nullptr;  // model light-intensity trackbar (Full/Plain)
    HWND hwnd_light_angle = nullptr;   // headlight angle trackbar (behind-cam .. grazing)
    HWND hwnd_light_follow = nullptr;  // "Follow cam" headlight toggle
    HWND hwnd_submesh_combo = nullptr; // LOD/texture target: "All submeshes" + each submesh
    HWND hwnd_lod_combo = nullptr;     // LOD level selector
    HWND hwnd_tex_reduced = nullptr;   // reduced (half-res) texture toggle
    HWND hwnd_audio_play = nullptr;
    HWND hwnd_audio_stop = nullptr;
    HWND hwnd_audio_combo = nullptr; // sound selector for multi-sound banks
    HWND hwnd_audio_seek = nullptr;  // playback position / seek trackbar (0..1000 permille)
    HWND hwnd_audio_time = nullptr;  // "m:ss / m:ss" position/duration label
    bool audio_seek_dragging = false; // user is scrubbing the seek bar (pause auto-updates)
    // Format/duration of the clip currently SELECTED (probed on every selection
    // change, before any playback). The seek bar's total time comes from here, so
    // switching sounds in a bank immediately shows that sound's real duration
    // instead of whatever was last played. Cached because probe() fully decodes.
    castlemist::snd::ClipInfo audio_sel_info;
    // --- Bink video player (Preview surface) ---
    HWND hwnd_video_play = nullptr;   // Play / Pause
    HWND hwnd_video_stop = nullptr;
    HWND hwnd_video_loop = nullptr;
    HWND hwnd_video_mute = nullptr;
    HWND hwnd_video_seek = nullptr;   // position trackbar (0..1000 permille of frames)
    HWND hwnd_video_volume = nullptr; // volume trackbar (0..100)
    HWND hwnd_video_time = nullptr;   // "m:ss / m:ss  (frame N/M)" readout
    HWND hwnd_video_track = nullptr;  // audio-track combo (only for multi-track Binks)
    HWND hwnd_video_subs = nullptr;   // "Subs" toggle
    HWND hwnd_video_subtitle = nullptr; // overlay label across the bottom of the video
    // Dialogue pulled from the CINP cinematic that drives the current movie.
    std::vector<SubtitleLine> video_subs;
    bool     video_subs_searched = false; // the CINP scan already ran for this entry
    uint32_t video_subs_cinp = 0;         // CINP baseId the lines came from (0 = none)
    // Off until asked for: turning it on triggers the CINP scan, which costs a
    // couple of seconds, so it must be an explicit choice rather than a tax on
    // every video preview.
    bool     video_subs_on = false;
    int      video_sub_shown = -1;        // index of the line currently on screen
    HWND     hwnd_video_clip = nullptr;   // CINP: which referenced movie is playing
    // A CINP entry's own bytes are the script; the movie it plays is a separate
    // dat entry, loaded here and kept alive for as long as gw2vid reads it.
    std::vector<uint8_t> cinp_video_bytes;
    int      cinp_video_sel = -1;
    bool video_seek_dragging = false; // scrubbing: suspend automatic seek-bar updates
    HWND hwnd_layer_prop = nullptr;
    HWND hwnd_layer_zone = nullptr;
    HWND hwnd_layer_coll = nullptr;
    HWND hwnd_map_preview = nullptr; // map: toggle the picked-prop inset preview
    bool map_zone_loaded = false; // whether the zone layer has been lazily loaded

    // Blender-style gizmo controls + transform readout (single-model surface).
    HWND hwnd_gizmo_move = nullptr;
    HWND hwnd_gizmo_rotate = nullptr;
    HWND hwnd_gizmo_scale = nullptr;
    HWND hwnd_gizmo_grid = nullptr;
    HWND hwnd_gizmo_reset = nullptr;
    HWND hwnd_tex_panel = nullptr;     // "Textures" toggle for the bottom-right texture panel
    HWND hwnd_gizmo_readout = nullptr; // overlay label: Loc/Rot/Scale values
    bool gizmo_dragging = false;       // a gizmo handle is being dragged
    int  gizmo_hover_axis = -1;        // last hovered handle (for highlight)

    // Layout ratios (0..1) of available space; scale sanely on window resize.
    double list_width_ratio = 0.22;
    double info_width_ratio = 0.18;
    // cntc content-browser sizing (resizable via its own splitters): total width of
    // the Types|Entries|Assets block, and the fraction of that block's height the
    // Assets table gets (Types/Entries share the rest).
    int content_browser_w = 416;
    double content_assets_ratio = 0.35;
    HWND hwnd_split_content = nullptr;   // vertical: browser block | preview surface
    HWND hwnd_split_content_h = nullptr; // horizontal: Types/Entries | Assets

    // Preview zoom/pan/rotation (mirrors gw2gfx's internal state so drag math
    // has something to read back without adding renderer getters).
    bool preview_dragging = false;
    POINT preview_drag_last{};
    float preview_zoom = 1.0f;
    float preview_pan_x = 0.0f;
    float preview_pan_y = 0.0f;
    int preview_rotation_quarters = 0;

    // Model orbit-drag state (mirrors the image drag state above).
    bool model_dragging = false;
    POINT model_drag_last{};
    POINT model_down{};        // where the left button went down (click-vs-drag test)
    bool map_pick_enabled = true; // map: a click (no drag) picks a prop into the inset preview
    castlemist::render::RenderMode model_mode = castlemist::render::RenderMode::Full;
    bool show_skeleton = false; // bind-pose skeleton overlay toggle (persists across models)
    bool show_tex_panel = false; // per-submesh texture strip toggle (persists across models)
};

inline AppState* g_app = nullptr;

inline HFONT g_ui_font = nullptr;   // Segoe UI, applied to every control
inline HFONT g_ui_font_bold = nullptr;

// ---------------------------------------------------------------------------
// Cross-file entry points, grouped by the file that defines them.
// ---------------------------------------------------------------------------

// ---- window_proc.cpp
/// @brief Put the radio mark on the entry matching g_theme_mode.
void sync_theme_menu();
void ensure_ui_fonts();
BOOL CALLBACK apply_font_cb(HWND child, LPARAM font);

// ---- listview_util.cpp -- report-view ListView helpers
void lv_add_col(HWND lv, int i, const wchar_t* text, int width);
int lv_add_row(HWND lv, LPARAM param, const wchar_t* col0,
               const wchar_t* col1 = nullptr, const wchar_t* col2 = nullptr,
               const wchar_t* col3 = nullptr, const wchar_t* col4 = nullptr);
LPARAM lv_selected_param(HWND lv);

// ---- layout.cpp -- where every child window goes
void layout_children(int client_w, int client_h);
void relayout();

// ---- view_controls.cpp -- toolbar commands and the model-view controls
void set_export_enabled(bool enabled);
void show_loading(bool loading, uint32_t mft_index);
void zoom_by(float factor);
void rotate_90();
void fit_view();
void ensure_map_game_materials();
void set_model_mode(castlemist::render::RenderMode mode);
void reset_model_view();
void update_gizmo_readout();
void set_gizmo_mode_ui(castlemist::render::GizmoMode m);
int lod_target_submesh();
void refresh_lod_controls();
void populate_lod_controls();

// ---- audio_ui.cpp -- the audio transport and the strs table
ExtractedEntry& active_audio_entry();
int audio_selected_index();
size_t populate_strs_table(const std::vector<uint8_t>& bytes, long long base);
void update_audio_info(int sel);
void update_audio_seek_ui(bool reset);

// ---- video_ui.cpp -- the Bink transport, subtitles and cinematic clips
void update_video_ui(bool reset);
void layout_video_subtitle();
void present_video_frame();
void stop_video_stream();
void stop_video();
void populate_video_tracks();
void populate_video_clips();
void search_video_subtitles(uint32_t mft_index);
std::wstring start_video(const ExtractedEntry& e);
std::wstring cinematic_info_text(const ExtractedEntry& e, const std::wstring& error);
std::wstring start_cinp_video(int index);
std::wstring video_info_text(const VideoMeta& v, const std::wstring& error);

// ---- content_browser.cpp -- the cntc types/entries/assets drill-down
void render_content_sub();
std::wstring content_asset_kind(uint32_t fid);
void load_content_asset_fid(uint32_t fid);
void populate_content_master();
void on_content_master_select(int idx);
const std::vector<uint32_t>& selected_entry_assets();
void on_content_child_select(int oi);
void on_content_asset_select();
std::wstring content_type_label(uint32_t type);
int CALLBACK content_lv_compare(LPARAM l1, LPARAM l2, LPARAM lpctx);
void content_sort_click(int list, HWND lv, int col);

// ---- preview.cpp -- turning an ExtractedEntry into the visible preview surface
void update_preview_texture();
void apply_extracted_entry(uint32_t mft_index, ExtractedEntry&& entry);
void on_entry_selected(uint32_t mft_index);

// ---- file_ops.cpp -- open, export, load template/keys, search and filters
bool load_dat_path(HWND hwnd, const wchar_t* path);
void do_open_file(HWND hwnd);
void do_load_template(HWND hwnd);
void load_keys_from(const std::wstring& csv_path);
void do_load_keys(HWND hwnd);
void try_autoload_keys();
void do_export(HWND hwnd, bool export_compressed);
std::string combo_sel(HWND combo);
void apply_filters();
void do_search();
void do_clear_search();

// ---- chat_link_dialog.cpp -- the &[base64] chat-link decoder popup
void cl_do_decode();
std::wstring cmap_cache_path();
uint32_t cl_content_id();
void cl_show_resolved();
void cl_open_fid(uint32_t fid);
void cl_resolve_asset();
void cl_search(bool by_file_id);
LRESULT CALLBACK ChatLinkWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
void open_chat_link_decoder(HWND owner);

// ---- index_ui.cpp -- opening a gw2index SQLite and wiring its filters
void finish_open_index(HWND hwnd, bool silent);
void do_open_index(HWND hwnd);
void try_autoload_index(HWND hwnd);

// ---- window_proc.cpp -- the menu and the three window procedures
HMENU build_menu();
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK ModelWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

} // namespace castlemist::ui
