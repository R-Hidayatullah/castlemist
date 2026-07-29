/// @file
/// @brief Tests for the classifiers that name what a .dat entry contains.

#include "test_framework.h"

#include "castlemist/extract/content_types.h"
#include "castlemist/extract/entry_extractor.h"

#include <string>

CM_TEST(content_type, names_the_ida_verified_types) {
    // These four are asserted by name in the client (CONTENT_TYPE_* constants).
    CHECK_EQ(std::string(content_type_name(35)), std::string("Item"));
    CHECK_EQ(std::string(content_type_name(48)), std::string("Minipet"));
    CHECK_EQ(std::string(content_type_name(51)), std::string("Outfit"));
    CHECK_EQ(std::string(content_type_name(66)), std::string("Skin"));
}

CM_TEST(content_type, unknown_types_return_null_for_the_caller_to_format) {
    CHECK(content_type_name(9999) == nullptr);
    CHECK(content_type_name(2) == nullptr);
}

// The slug is an ArenaNet internal identifier: two short base64-ish groups
// joined by a dot. It is what makes an object addressable; the object's other
// strings are shared human labels.
CM_TEST(content_slug, accepts_real_identifier_slugs) {
    CHECK(is_content_slug("vl8Av.4gynM"));
    CHECK(is_content_slug("sEhjD.9j1Ju"));
    CHECK(is_content_slug("FOyVJ.AL3Ye"));
    CHECK(is_content_slug("abc.def"));           // shortest allowed on both sides
    CHECK(is_content_slug("a+b/c-d_.12345678")); // full base64-ish alphabet, 8 + 8
}

CM_TEST(content_slug, rejects_readable_labels) {
    CHECK_FALSE(is_content_slug("Damage Multiplier")); // spaces
    CHECK_FALSE(is_content_slug("AggroRadius"));       // no dot
    CHECK_FALSE(is_content_slug("Within Range"));
    CHECK_FALSE(is_content_slug("/Roles[1]/LiveGestureCharrMale"));
}

CM_TEST(content_slug, rejects_groups_outside_the_3_to_8_character_range) {
    CHECK_FALSE(is_content_slug("ab.cdef"));            // left too short
    CHECK_FALSE(is_content_slug("abcdefghi.jkl"));      // left too long
    CHECK_FALSE(is_content_slug("abcd.ef"));            // right too short
    CHECK_FALSE(is_content_slug("abcd.efghijklm"));     // right too long
}

CM_TEST(content_slug, rejects_degenerate_input) {
    CHECK_FALSE(is_content_slug(""));
    CHECK_FALSE(is_content_slug("."));
    CHECK_FALSE(is_content_slug("nodothere"));
}

// The texture-resolution preference is global state read by background threads;
// it must round-trip and default to full resolution.
CM_TEST(texture_resolution, defaults_to_full_and_round_trips) {
    CHECK(texture_full_res());
    set_texture_full_res(false);
    CHECK_FALSE(texture_full_res());
    set_texture_full_res(true);
    CHECK(texture_full_res());
}

CM_TEST(extract_api, missing_dat_paths_fail_without_throwing) {
    std::vector<SubtitleLine> subs;
    CHECK_EQ(find_video_subtitles("", {1, 2, 3}, 42, subs), 0u);
    CHECK(subs.empty());

    // An empty candidate list or a zero fileId short-circuits before any I/O.
    CHECK_EQ(find_video_subtitles("no_such.dat", {}, 42, subs), 0u);
    CHECK_EQ(find_video_subtitles("no_such.dat", {1}, 0, subs), 0u);

    CHECK(load_video_by_fileid("", 42).empty());
    CHECK(load_video_by_fileid("no_such.dat", 0).empty());
    CHECK(load_video_by_fileid("no_such.dat", 42).empty());
}

CM_TEST(extract_api, map_helpers_reject_input_they_cannot_use) {
    std::vector<uint8_t> not_a_map{'n', 'o', 'p', 'e'};
    CHECK(build_map_zone_layer(not_a_map, "") == nullptr);

    MapScene empty;
    CHECK_FALSE(augment_map_scene_game(empty, ""));
}
