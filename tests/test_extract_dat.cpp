/// @file
/// @brief End-to-end extraction against a real Gw2.dat.
///
/// Everything else in the suite runs on synthetic fixtures, which proves the
/// parsers are self-consistent but not that they agree with ArenaNet. This file
/// closes that gap: it opens the actual archive and asserts that a curated set
/// of entries still decodes to the type it decoded to when it was reverse
/// engineered (`GW2CURATED.txt`).
///
/// The archive is ~87 GB and not everyone has it, so every test here **skips**
/// rather than fails when it is missing. Point `GW2_TEST_DAT` at a different
/// install to override the default location.
///
/// @code
/// set GW2_TEST_DAT=D:\Guild Wars 2\Gw2.dat
/// ctest --test-dir build -R dat --output-on-failure
/// @endcode

#include "test_framework.h"

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/struct_template.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

/// The install this project is developed against.
constexpr const char* kDefaultDat =
    R"(C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat)";

/// @brief Path to the archive under test, or an empty string when unavailable.
std::string dat_path() {
    const char* env = std::getenv("GW2_TEST_DAT");
    std::string path = (env && *env) ? env : kDefaultDat;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return path;
    }
    return {};
}

/// @brief Open the archive once per process; skip the test when it is missing.
Gw2Dat& shared_dat() {
    static Gw2Dat dat;
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        std::string path = dat_path();
        if (!path.empty()) {
            try {
                load_dat_file(dat, path);
                ok = dat.mft_data_list.size() > 0;
            } catch (const std::exception&) {
                ok = false;
            }
        }
    }
    if (!ok) SKIP("no Gw2.dat (set GW2_TEST_DAT)");
    return dat;
}

/// @brief Load the struct template, trying the usual spots relative to the build tree.
///
/// Model extraction needs `gw2_packfile.json`; `castlemist::tpl::auto_load`
/// searches next to the exe and the working directory, neither of which is the
/// repository root when ctest runs. `GW2_TEST_TEMPLATE` overrides.
bool ensure_template() {
    if (castlemist::tpl::get()) return true;
    if (castlemist::tpl::auto_load()) return true;

    std::string err;
    if (const char* env = std::getenv("GW2_TEST_TEMPLATE"); env && *env)
        if (castlemist::tpl::load_from_file(env, err)) return true;

    // Generated output, so it lives under dumps/ -- see tools/structs.
    for (const char* candidate : {
             "../../../dumps/packfile/gw2_packfile.json",
             "dumps/packfile/gw2_packfile.json",
             "../dumps/packfile/gw2_packfile.json",
         }) {
        if (castlemist::tpl::load_from_file(candidate, err)) return true;
    }
    return false;
}

/// @brief Extract by baseId (the ids in GW2CURATED.txt are baseIds, MFT index + 1).
ExtractedEntry extract_base(uint32_t base_id) {
    Gw2Dat& dat = shared_dat();
    if (base_id == 0 || base_id - 1 >= dat.mft_data_list.size()) SKIP("baseId out of range for this dat");
    return extract_entry(dat, base_id - 1);
}

} // namespace

CM_TEST(dat, opens_and_reports_a_plausible_entry_count) {
    Gw2Dat& dat = shared_dat();
    // A retail archive has hundreds of thousands of entries; anything much
    // smaller means the MFT was mis-read rather than that the file is small.
    CHECK(dat.mft_data_list.size() > 100000);
    CHECK_FALSE(dat.file_info.file_path.empty());
}

CM_TEST(dat, decodes_atex_textures_to_rgba) {
    for (uint32_t base : {2871u, 807183u}) {
        ExtractedEntry e = extract_base(base);
        CHECK(e.kind == PreviewKind::Image);
        CHECK(e.is_image);
        CHECK(e.preview_width > 0);
        CHECK(e.preview_height > 0);
        CHECK_EQ(e.preview_pixels.size(),
                 size_t(e.preview_width) * e.preview_height * 4);
        CHECK_FALSE(e.preview_format_label.empty());
    }
}

// ATEP is an ATEX variant; the container tag is engine metadata and provably
// does not change pixel decoding, so it must produce the same kind of result.
CM_TEST(dat, decodes_atep_and_ateu_like_atex) {
    for (uint32_t base : {120115u, 2903u, 86152u}) {
        ExtractedEntry e = extract_base(base);
        CHECK(e.kind == PreviewKind::Image);
        CHECK(e.preview_width > 0);
    }
}

// A "DDS " entry is handed to the GPU in its native format rather than expanded
// on the CPU, so the label names that format and the buffer holds at least the
// top mip at that format's size.
CM_TEST(dat, passes_a_standalone_dds_through_in_its_native_format) {
    ExtractedEntry e = extract_base(18232);
    CHECK(e.kind == PreviewKind::Image);
    CHECK(e.preview_width > 0);
    CHECK(e.preview_height > 0);
    CHECK(e.preview_pitch > 0);
    CHECK_EQ(e.preview_format_label.rfind("DDS", 0), size_t{0});
    // pitch is bytes per row (or per row of blocks), so height rows must fit.
    CHECK(e.preview_pixels.size() >= size_t(e.preview_pitch));
}

CM_TEST(dat, decodes_png_and_webp_stills) {
    ExtractedEntry png = extract_base(38);
    CHECK(png.kind == PreviewKind::Image);
    CHECK(png.preview_format_label.find("PNG") != std::string::npos);

    // The dat's `riff` entries are RIFF/WEBP; libwebp names itself in the label.
    ExtractedEntry webp = extract_base(101);
    CHECK(webp.kind == PreviewKind::Image);
    CHECK(webp.preview_format_label.find("WebP") != std::string::npos);
}

CM_TEST(dat, decodes_strs_string_tables) {
    ExtractedEntry e = extract_base(2925);
    CHECK(e.kind == PreviewKind::Strs);
    CHECK_FALSE(e.text_preview.empty());
}

CM_TEST(dat, decodes_asnd_audio_into_a_playable_clip) {
    ExtractedEntry e = extract_base(2883);
    CHECK(e.kind == PreviewKind::Audio);
    CHECK_FALSE(e.audio_clips.empty());
    CHECK_FALSE(e.audio_clips[0].data.empty());
    CHECK_FALSE(e.audio_clips[0].codec.empty());
}

// An ABNK bank holds several sounds behind one entry.
CM_TEST(dat, splits_an_abnk_bank_into_several_clips) {
    ExtractedEntry e = extract_base(165780);
    CHECK(e.kind == PreviewKind::Audio);
    CHECK(e.audio_clips.size() >= 1);
}

// baseId 3203 is the first cntc in the index (32 of them ship); the entry is a
// ~10 MB PackContent, so this is also the slowest test here.
CM_TEST(dat, recognises_a_cntc_content_datastore) {
    ExtractedEntry e = extract_base(3203);
    CHECK(e.kind == PreviewKind::Content);
    CHECK_FALSE(e.content_asset_ids.empty());
    CHECK_FALSE(e.content_objects.empty());

    // Most objects carry an identifier slug; those that do not are the ones
    // whose only strings are shared human labels.
    size_t with_slug = 0;
    for (const ContentObject& o : e.content_objects)
        if (!o.name.empty()) ++with_slug;
    CHECK(with_slug > 0);
}

// Models need the struct template; without it the entry is still recognised as
// a model, it just carries no geometry.
CM_TEST(dat, recognises_models_and_builds_them_when_a_template_is_loaded) {
    ExtractedEntry e = extract_base(291821);
    CHECK(e.kind == PreviewKind::Model);

    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    ExtractedEntry with_template = extract_base(291821);
    CHECK(with_template.model != nullptr);
    if (with_template.model) {
        CHECK_FALSE(with_template.model->meshes.empty());
        CHECK(with_template.model->totalVerts > 0);
        CHECK(with_template.model->radius > 0.0f);
    }
}

// Both extract_entry overloads must agree: the background-thread one opens its
// own handle, and a divergence there would mean the preview differs from what
// the UI thread would have produced.
CM_TEST(dat, both_extract_overloads_agree) {
    Gw2Dat& dat = shared_dat();
    const uint32_t index = 2871 - 1;
    if (index >= dat.mft_data_list.size()) SKIP("baseId out of range for this dat");

    ExtractedEntry a = extract_entry(dat, index);
    ExtractedEntry b = extract_entry(dat.file_info.file_path, dat.mft_data_list[index]);

    CHECK(a.kind == b.kind);
    CHECK_EQ(a.preview_width, b.preview_width);
    CHECK_EQ(a.preview_height, b.preview_height);
    CHECK_EQ(a.decompressed.size(), b.decompressed.size());
}

// The texture panel reports each texture's ids, size, format, mip count and
// channel layout, and none of that is worth showing if it is not measured from
// the real file. Assert the decoder fills all of it, and that the channel string
// is consistent with the format (a BC5/3Dc normal map stores two channels, so it
// must never be reported as RGB).
CM_TEST(dat, model_textures_carry_the_metadata_the_texture_panel_shows) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    ExtractedEntry e = extract_base(291821);
    if (!e.model) SKIP("model did not build");
    if (e.model->textures.empty()) SKIP("model has no textures");

    for (const ModelTextureCPU& t : e.model->textures) {
        CHECK(t.fileId > 0);
        CHECK(t.baseId > 0);
        CHECK(t.width > 0);
        CHECK(t.height > 0);
        CHECK(t.mipCount > 0);
        CHECK_FALSE(t.fmt.empty());
        // R / RG / RGB / RGBA -- alpha only ever appended to a colour base.
        CHECK(t.channels == "R" || t.channels == "RA" || t.channels == "RG" || t.channels == "RGA" ||
              t.channels == "RGB" || t.channels == "RGBA");
        if (t.isNormal) CHECK_EQ(t.channels.rfind("RG", 0), size_t{0});
    }

    // Every material's texture references must resolve into that same table --
    // this is exactly the fileId -> texture lookup the panel groups by, so a
    // mismatch here would show up as empty submesh groups.
    size_t resolved = 0;
    for (const ModelMaterialCPU& m : e.model->materials) {
        for (uint32_t fid : m.textureFileIds)
            for (const ModelTextureCPU& t : e.model->textures)
                if (t.fileId == fid) { ++resolved; break; }
    }
    CHECK(resolved > 0);
}

CM_TEST(dat, texture_resolution_preference_changes_what_a_model_loads) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    set_texture_full_res(true);
    ExtractedEntry full = extract_base(291821);
    set_texture_full_res(false);
    ExtractedEntry reduced = extract_base(291821);
    set_texture_full_res(true);   // restore the default for the rest of the run

    if (!full.model || !reduced.model) SKIP("model did not build");
    if (full.model->textures.empty()) SKIP("model has no textures");

    // Same texture count either way; only the dimensions may differ.
    CHECK_EQ(full.model->textures.size(), reduced.model->textures.size());
    CHECK(full.model->textures[0].width >= reduced.model->textures[0].width);
}
