/// @file
/// @brief Tests for the extract layer's signature sniffing and packfile summaries.
///
/// Uses the layer's internal header: these are the seams between the split
/// format files, and they are exactly the parts where a wrong answer is silent
/// (an unrecognized texture just shows as hex, a mis-parsed PIMG shows an empty
/// atlas) rather than loud.

#include "test_framework.h"

#include "internal.h"

#include <string>
#include <vector>

using namespace castlemist::extract;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<int> v) {
    std::vector<uint8_t> b;
    for (int x : v) b.push_back(uint8_t(x));
    return b;
}

/// @brief A packfile with one chunk, for the summary parsers to reject or accept.
std::vector<uint8_t> packfile(const char* container, const char* chunk, size_t payload = 32) {
    std::vector<uint8_t> f{'P', 'F', 0x0A, 0x00, 0, 0, 12, 0};
    for (int i = 0; i < 4; ++i) f.push_back(uint8_t(container[i]));
    for (int i = 0; i < 4; ++i) f.push_back(uint8_t(chunk[i]));
    uint32_t size = uint32_t(8 + payload);
    for (int i = 0; i < 4; ++i) f.push_back(uint8_t(size >> (8 * i)));
    f.push_back(1); f.push_back(0);   // chunk version
    f.push_back(16); f.push_back(0);  // chunk header size
    f.push_back(0); f.push_back(0); f.push_back(0); f.push_back(0);
    f.resize(f.size() + payload, 0);
    return f;
}

} // namespace

CM_TEST(sniff, recognises_the_whole_atex_family) {
    for (const char* magic : {"ATEX", "ATTX", "ATEC", "ATEP", "ATEU", "ATET"}) {
        std::vector<uint8_t> a(magic, magic + 4);
        CHECK(is_atex_family(a));
        // The C-prefixed twins are the same format under a different magic.
        a[0] = 'C';
        CHECK(is_atex_family(a));
    }
}

CM_TEST(sniff, rejects_near_misses_and_short_buffers) {
    CHECK_FALSE(is_atex_family(bytes({'A', 'T', 'E'})));         // too short
    CHECK_FALSE(is_atex_family(bytes({'A', 'B', 'C', 'D'})));
    CHECK_FALSE(is_atex_family(bytes({'B', 'T', 'E', 'X'})));    // wrong first letter
    CHECK_FALSE(is_atex_family({}));
}

CM_TEST(sniff, recognises_the_still_image_signatures) {
    CHECK(is_png(bytes({0x89, 'P', 'N', 'G'})));
    CHECK_FALSE(is_png(bytes({0x88, 'P', 'N', 'G'})));
    CHECK_FALSE(is_png(bytes({0x89, 'P'})));

    CHECK(is_jpeg(bytes({0xFF, 0xD8, 0xFF})));
    CHECK_FALSE(is_jpeg(bytes({0xFF, 0xD8})));

    CHECK(is_bmp(bytes({'B', 'M'})));
    CHECK_FALSE(is_bmp(bytes({'M', 'B'})));
    CHECK_FALSE(is_bmp(bytes({'B'})));
}

CM_TEST(sniff, names_the_common_dxgi_formats) {
    CHECK_EQ(std::string(dxgi_short_name(71)), std::string("BC1"));   // BC1_UNORM
    CHECK_EQ(std::string(dxgi_short_name(77)), std::string("BC3"));   // BC3_UNORM
    CHECK_EQ(std::string(dxgi_short_name(98)), std::string("BC7"));   // BC7_UNORM
    CHECK_EQ(std::string(dxgi_short_name(999)), std::string("DDS"));  // fallback
}

// 3DC / BC5 / ATI2 are all names for the same two-channel normal-map encoding;
// getting this wrong makes a normal map render as a diffuse texture.
CM_TEST(sniff, classifies_normal_map_formats) {
    CHECK(is_normal_format("3DCX/BC5"));
    CHECK(is_normal_format("BC5"));
    CHECK(is_normal_format("ATI2"));
    CHECK_FALSE(is_normal_format("DXT5"));
    CHECK_FALSE(is_normal_format("BC1"));
    CHECK_FALSE(is_normal_format(""));
}

CM_TEST(sniff, effect_detection_needs_both_greyscale_and_dark_regions) {
    // A colourful texture is never an effect mask.
    std::vector<uint8_t> colourful(4 * 4000);
    for (size_t i = 0; i + 3 < colourful.size(); i += 4) {
        colourful[i] = 200; colourful[i + 1] = 40; colourful[i + 2] = 90; colourful[i + 3] = 255;
    }
    CHECK_FALSE(looks_like_effect(colourful));

    // A uniformly bright greyscale texture is not one either -- no mask regions.
    std::vector<uint8_t> bright(4 * 4000, 220);
    CHECK_FALSE(looks_like_effect(bright));

    // Greyscale with large near-black areas is the glow/alpha-mask signature.
    std::vector<uint8_t> masky(4 * 4000, 0);
    for (size_t i = 0; i + 3 < masky.size() / 2; i += 4) {
        masky[i] = masky[i + 1] = masky[i + 2] = 240;
    }
    CHECK(looks_like_effect(masky));

    CHECK_FALSE(looks_like_effect({}));
}

CM_TEST(pimg, rejects_anything_that_is_not_a_pimg_packfile) {
    PimgAtlas atlas;
    CHECK_FALSE(parse_pimg({}, atlas));
    CHECK_FALSE(parse_pimg(bytes({'P', 'F'}), atlas));
    CHECK_FALSE(parse_pimg(packfile("MODL", "GEOM"), atlas));
    CHECK(atlas.pages.empty());
}

CM_TEST(amsp, rejects_anything_that_is_not_an_amsp_packfile) {
    CHECK(parse_amsp_sound_ids({}).empty());
    CHECK(parse_amsp_sound_ids(packfile("ASND", "ASND")).empty());
}

CM_TEST(cntc, rejects_anything_that_is_not_a_cntc_packfile) {
    CHECK(parse_cntc_summary({}).empty());
    CHECK(parse_cntc_summary(packfile("MODL", "GEOM")).empty());
    CHECK(cntc_referenced_asset_ids(packfile("MODL", "GEOM"), 100).empty());
    CHECK(parse_cntc_objects(packfile("MODL", "GEOM"), 100).empty());
}

// Each summary parser is keyed to its own container: handing one another's
// packfile must produce nothing rather than a plausible-looking mis-parse.
CM_TEST(summaries, only_accept_their_own_container) {
    auto eula = packfile("eula", "eula");
    auto abix = packfile("ABIX", "BIDX");
    auto cscn = packfile("CSCN", "CSCN");

    CHECK(parse_eula_summary(abix).empty());
    CHECK(parse_eula_summary(cscn).empty());
    CHECK(parse_abix_summary(eula).empty());
    CHECK(parse_abix_summary(cscn).empty());
    CHECK(parse_cscn_summary(eula).empty());
    CHECK(parse_cscn_summary(abix).empty());
}

CM_TEST(summaries, describe_their_own_container) {
    CHECK_FALSE(parse_eula_summary(packfile("eula", "eula")).empty());
    CHECK_FALSE(parse_abix_summary(packfile("ABIX", "BIDX")).empty());
    CHECK_FALSE(parse_cscn_summary(packfile("CSCN", "CSCN")).empty());
}

CM_TEST(cscn, subtitle_extraction_declines_non_cinematics) {
    std::vector<SubtitleLine> out;
    CHECK_FALSE(parse_cscn_subtitles({}, out));
    CHECK_FALSE(parse_cscn_subtitles(packfile("MODL", "GEOM"), out));
    CHECK(out.empty());

    int version = -1;
    uint32_t sequences = 99, text = 99;
    cscn_counts(packfile("MODL", "GEOM"), version, sequences, text);
    CHECK_EQ(version, 0);
    CHECK_EQ(sequences, 0u);
    CHECK_EQ(text, 0u);
}

CM_TEST(parallel_for, visits_every_index_exactly_once) {
    constexpr size_t kN = 1000;
    std::vector<int> hits(kN, 0);
    parallel_for(kN, [&](size_t i) { hits[i] += 1; });
    for (size_t i = 0; i < kN; ++i) CHECK_EQ(hits[i], 1);
}

CM_TEST(parallel_for, handles_the_degenerate_sizes) {
    int calls = 0;
    parallel_for(0, [&](size_t) { ++calls; });
    CHECK_EQ(calls, 0);
    parallel_for(1, [&](size_t i) { calls += int(i) + 1; });
    CHECK_EQ(calls, 1);
}
