/// @file
/// @brief Tests for the Win32 shell's non-visual invariants.
///
/// Most of the ui layer needs a real window, so it is exercised by the headless
/// `GW2_*` hooks (see docs/testing.md) rather than from here. What *can* be
/// checked without a window is the part that has silently broken before: the
/// control-id table. Ids are assigned by hand across several ranges, and a
/// duplicate does not fail to compile -- it makes two controls answer to the
/// same WM_COMMAND, so one of them quietly stops working.

#include "test_framework.h"

#include "detail/app_state.h"

#include <map>
#include <string>
#include <vector>

using namespace castlemist::ui;

namespace {

struct Id {
    const char* name;
    UINT_PTR value;
};

/// Every control and command id the window uses.
const Id kIds[] = {
    {"ID_FILE_OPEN", ID_FILE_OPEN},
    {"ID_FILE_EXPORT_COMPRESSED", ID_FILE_EXPORT_COMPRESSED},
    {"ID_FILE_EXPORT_DECOMPRESSED", ID_FILE_EXPORT_DECOMPRESSED},
    {"ID_FILE_EXIT", ID_FILE_EXIT},
    {"ID_FILE_LOAD_TEMPLATE", ID_FILE_LOAD_TEMPLATE},
    {"ID_FILE_LOAD_KEYS", ID_FILE_LOAD_KEYS},
    {"ID_FILE_OPEN_INDEX", ID_FILE_OPEN_INDEX},
    {"ID_TOOLS_DECODE_LINK", ID_TOOLS_DECODE_LINK},

    {"ID_LISTVIEW", ID_LISTVIEW},
    {"ID_HEX_BEFORE", ID_HEX_BEFORE},
    {"ID_HEX_AFTER", ID_HEX_AFTER},
    {"ID_INFO_PANEL", ID_INFO_PANEL},
    {"ID_TAB", ID_TAB},
    {"ID_SPLIT_LIST_MIDDLE", ID_SPLIT_LIST_MIDDLE},
    {"ID_SPLIT_MIDDLE_INFO", ID_SPLIT_MIDDLE_INFO},
    {"ID_SPLIT_CONTENT", ID_SPLIT_CONTENT},
    {"ID_SPLIT_CONTENT_H", ID_SPLIT_CONTENT_H},
    {"ID_SEARCH_EDIT", ID_SEARCH_EDIT},
    {"ID_SEARCH_FILEID_CHECK", ID_SEARCH_FILEID_CHECK},
    {"ID_SEARCH_BUTTON", ID_SEARCH_BUTTON},
    {"ID_CLEAR_BUTTON", ID_CLEAR_BUTTON},
    {"ID_FILTER_TYPE", ID_FILTER_TYPE},
    {"ID_FILTER_CONTAINER", ID_FILTER_CONTAINER},

    {"ID_ZOOM_IN", ID_ZOOM_IN},
    {"ID_ZOOM_OUT", ID_ZOOM_OUT},
    {"ID_ROTATE", ID_ROTATE},
    {"ID_FIT", ID_FIT},
    {"ID_MODE_FULL", ID_MODE_FULL},
    {"ID_MODE_PLAIN", ID_MODE_PLAIN},
    {"ID_MODE_WIRE", ID_MODE_WIRE},
    {"ID_MODEL_RESET", ID_MODEL_RESET},
    {"ID_MODE_SKEL", ID_MODE_SKEL},
    {"ID_MODE_SHADER", ID_MODE_SHADER},
    {"ID_ANIM_COMBO", ID_ANIM_COMBO},
    {"ID_ANIM_PLAY", ID_ANIM_PLAY},
    {"ID_LAYER_PROP", ID_LAYER_PROP},
    {"ID_LAYER_ZONE", ID_LAYER_ZONE},
    {"ID_LAYER_COLL", ID_LAYER_COLL},
    {"ID_TEX_FULLRES", ID_TEX_FULLRES},

    {"ID_AUDIO_PLAY", ID_AUDIO_PLAY},
    {"ID_AUDIO_STOP", ID_AUDIO_STOP},
    {"ID_AUDIO_COMBO", ID_AUDIO_COMBO},
    {"ID_AUDIO_SEEK", ID_AUDIO_SEEK},

    {"ID_CONTENT_LIST", ID_CONTENT_LIST},
    {"ID_CONTENT_CHILD", ID_CONTENT_CHILD},
    {"ID_CONTENT_ASSET_LIST", ID_CONTENT_ASSET_LIST},

    {"ID_LIGHT_PREPASS", ID_LIGHT_PREPASS},
    {"ID_ALPHA_TOGGLE", ID_ALPHA_TOGGLE},
    {"ID_SUBMESH_COMBO", ID_SUBMESH_COMBO},
    {"ID_LOD_COMBO", ID_LOD_COMBO},
    {"ID_TEX_REDUCED", ID_TEX_REDUCED},
    {"ID_EFFECTS_TOGGLE", ID_EFFECTS_TOGGLE},
    {"ID_LIGHT_SLIDER", ID_LIGHT_SLIDER},
    {"ID_LIGHT_ANGLE", ID_LIGHT_ANGLE},
    {"ID_LIGHT_FOLLOW", ID_LIGHT_FOLLOW},

    {"ID_GIZMO_MOVE", ID_GIZMO_MOVE},
    {"ID_GIZMO_ROTATE", ID_GIZMO_ROTATE},
    {"ID_GIZMO_SCALE", ID_GIZMO_SCALE},
    {"ID_GIZMO_GRID", ID_GIZMO_GRID},
    {"ID_GIZMO_RESET", ID_GIZMO_RESET},
    {"ID_MAP_PREVIEW", ID_MAP_PREVIEW},
    {"ID_CLOTH_TOGGLE", ID_CLOTH_TOGGLE},
    {"ID_STRS_LIST", ID_STRS_LIST},

    {"ID_VIDEO_PLAY", ID_VIDEO_PLAY},
    {"ID_VIDEO_STOP", ID_VIDEO_STOP},
    {"ID_VIDEO_LOOP", ID_VIDEO_LOOP},
    {"ID_VIDEO_MUTE", ID_VIDEO_MUTE},
    {"ID_VIDEO_SEEK", ID_VIDEO_SEEK},
    {"ID_VIDEO_VOLUME", ID_VIDEO_VOLUME},
    {"ID_VIDEO_TRACK", ID_VIDEO_TRACK},
    {"ID_VIDEO_SUBS", ID_VIDEO_SUBS},
    {"ID_VIDEO_CLIP", ID_VIDEO_CLIP},

    {"ID_CL_INPUT", ID_CL_INPUT},
    {"ID_CL_DECODE", ID_CL_DECODE},
    {"ID_CL_OUTPUT", ID_CL_OUTPUT},
    {"ID_CL_SEARCH_BASE", ID_CL_SEARCH_BASE},
    {"ID_CL_SEARCH_FILE", ID_CL_SEARCH_FILE},
    {"ID_CL_CLOSE", ID_CL_CLOSE},
    {"ID_CL_RESOLVE", ID_CL_RESOLVE},
    {"ID_CL_OPEN_ICON", ID_CL_OPEN_ICON},
    {"ID_CL_OPEN_MODEL", ID_CL_OPEN_MODEL},

    {"ID_STATUS_LABEL", ID_STATUS_LABEL},
    {"ID_PROGRESS", ID_PROGRESS},
};

} // namespace

CM_TEST(control_ids, are_unique) {
    std::map<UINT_PTR, std::string> seen;
    for (const Id& id : kIds) {
        auto it = seen.find(id.value);
        if (it != seen.end()) {
            ::castlemist::test::fail(__FILE__, __LINE__,
                            std::string("id ") + std::to_string(id.value) + " is used by both " +
                                it->second + " and " + id.name);
        }
        seen[id.value] = id.name;
    }
    CHECK_EQ(seen.size(), sizeof(kIds) / sizeof(kIds[0]));
}

// Menu commands and child-control ids share one WM_COMMAND stream, so the two
// ranges must not overlap.
CM_TEST(control_ids, menu_and_control_ranges_do_not_overlap) {
    for (const Id& id : kIds) {
        bool is_menu = std::string(id.name).rfind("ID_FILE_", 0) == 0 ||
                       std::string(id.name).rfind("ID_TOOLS_", 0) == 0;
        if (is_menu) {
            CHECK(id.value >= 1000 && id.value < 2000);
        } else {
            CHECK(id.value >= 2000);
        }
    }
}

CM_TEST(timers, have_distinct_ids) {
    CHECK_NE(TIMER_ANIM, TIMER_AUDIO);
    CHECK_NE(TIMER_ANIM, TIMER_VIDEO);
    CHECK_NE(TIMER_AUDIO, TIMER_VIDEO);
}

// The two WM_APP messages carry different payload contracts; sharing a value
// would hand a ContentMap pointer to the extract-result handler.
CM_TEST(app_messages, are_distinct_and_above_wm_app) {
    CHECK(WM_APP_EXTRACT_DONE >= WM_APP);
    CHECK(WM_APP_CMAP_DONE >= WM_APP);
    CHECK_NE(WM_APP_EXTRACT_DONE, WM_APP_CMAP_DONE);
}

CM_TEST(layout_metrics, are_positive_and_self_consistent) {
    CHECK(kSplitterThickness > 0);
    CHECK(kMinPaneSize > kSplitterThickness);
    CHECK(kSearchBarHeight > 0);
    CHECK(kTabHeight > 0);
    CHECK(kToolbarHeight > 0);
    CHECK(kStatusBarHeight > 0);
    CHECK(kAudioSeekMax > 0);
    CHECK(kVideoSeekMax > 0);
}

CM_TEST(theme, brushes_are_cached_per_colour) {
    HBRUSH a = theme_brush(kColBg);
    HBRUSH b = theme_brush(kColBg);
    HBRUSH c = theme_brush(kColPanel);
    CHECK(a != nullptr);
    CHECK(a == b);   // the cache must not leak a new brush per repaint
    CHECK(a != c);
}

CM_TEST(content_types, unknown_types_get_a_readable_placeholder) {
    CHECK_EQ(content_type_label(35), std::wstring(L"Item"));
    CHECK(content_type_label(9999).find(L"9999") != std::wstring::npos);
}
