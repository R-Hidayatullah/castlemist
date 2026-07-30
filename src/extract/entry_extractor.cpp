/// @file
/// @brief The dispatcher: sniffs a decompressed entry and routes it to a decoder.
///
/// Detection order matters. Cheap magic checks come first, structural packfile
/// checks next, and the "looks like text" heuristic last -- it accepts almost
/// anything, so every real format must get a chance before it.

#include "internal.h"

#include <algorithm>
#include <cstring>
#include <span>

#include <dxgiformat.h>

#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2_atex.hpp"
#include "castlemist/native/gw2_audio.hpp"

#include "castlemist/core/packfile.h"
#include "castlemist/core/text.h"
#include "castlemist/format/sdk_image.h"
#include "castlemist/format/strs_keys.h"
#include "castlemist/format/strs_view.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/format/wic_image.h"
#include "castlemist/media/video_player.h"

namespace castlemist::extract {

// All the CPU-bound decompression/format-detection work, independent of how
// `raw_bytes` was read off disk -- this is what makes it safe to run on a
// background thread. `dat_path` is used to resolve model textures.
ExtractedEntry decompress_raw_entry(std::vector<uint8_t> raw_bytes, uint16_t compression_flag,
                                    const std::string& dat_path, bool already_plain) {
    ExtractedEntry result;
    result.compressed = raw_bytes;

    std::vector<uint8_t> file_bytes;
    if (already_plain) {
        // A loose file off disk is the finished asset already: it carries no MFT
        // CRC32C framing and no Method0 header, so stripping or inflating it here
        // would corrupt it. Only the unwrapping is skipped -- every format sniffer
        // below reads `decompressed` and neither knows nor cares where it came from.
        file_bytes = std::move(raw_bytes);
    } else {
        std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(raw_bytes);
        if (compression_flag != 0) {
            if (stripped.size() < 8) {
                throw std::runtime_error("Entry too small to contain a Method0 header.");
            }
            uint32_t uncompressed_size = read_u32_le(stripped, 4);
            file_bytes = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), uncompressed_size);
        } else {
            file_bytes = std::move(stripped);
        }
    }

    result.decompressed = std::move(file_bytes);

    // Bink cinematic ("KB2i"/"KB2j" in this dat). Checked first: the magic is
    // unambiguous, and these entries are ~100 MB, so there is no point running
    // them past the image/audio/packfile sniffers. Nothing is copied here -- the
    // player decodes straight out of result.decompressed.
    {
        castlemist::vid::Info vi;
        if (castlemist::vid::probe(result.decompressed.data(), result.decompressed.size(), vi)) {
            result.kind = PreviewKind::Video;
            result.video.ok = true;
            result.video.fourcc = vi.fourcc;
            result.video.codec = vi.codec;
            result.video.width = vi.width;
            result.video.height = vi.height;
            result.video.frames = vi.frames;
            result.video.fps_num = vi.fps_num;
            result.video.fps_den = vi.fps_den;
            result.video.tracks = vi.tracks;
            result.video.has_alpha = vi.has_alpha;
            result.video.seconds = vi.seconds;
            return result;
        }
    }

    // CINP cinematic script: the thing that actually ties a movie to its dialogue.
    // Preview it as a video player -- the referenced Bink(s) plus the subtitles
    // read straight off this timeline (no CINP scan needed, we ARE the CINP).
    if (castlemist::core::is_packfile(result.decompressed, "CINP") && castlemist::core::has_chunk(result.decompressed, "CSCN")) {
        std::vector<std::pair<uint32_t, int>> refs;
        collect_cscn_filerefs(result.decompressed, refs);
        parse_cscn_subtitles(result.decompressed, result.subtitles);
        if (!refs.empty() && !dat_path.empty()) {
            Gw2Dat dat;
            try {
                load_dat_file(dat, dat_path);
                for (const auto& [fid, seq] : refs) {
                    if (peek_is_bink_fileid(dat, fid)) {
                        result.cinp_video_ids.push_back(fid);
                        result.cinp_video_seq.push_back(seq);
                    } else {
                        result.cinp_other_ids.push_back(fid);
                    }
                }
            } catch (const std::exception&) {
                for (const auto& [fid, seq] : refs) result.cinp_other_ids.push_back(fid);
            }
        }
        cscn_counts(result.decompressed, result.cinp_version, result.cinp_sequences,
                    result.cinp_text_resources);
        // A CINP is ALWAYS previewed as a cinematic now. Most of them are in-engine
        // scenes with no movie and no inline text (their dialogue lives in the
        // localized string packs), and those used to fall through to
        // "(no preview available)" -- describing the timeline is more useful.
        result.kind = PreviewKind::Cinematic;
        return result;
    }

    if (is_atex_family(result.decompressed)) {
        result.is_image = fill_preview_from_atex(result);
    } else if (starts_with_magic(result.decompressed, "DDS ")) {
        result.is_image = fill_preview_from_dds(result);
    } else if (castlemist::img::is_image(result.decompressed.data(), result.decompressed.size())) {
        // PNG / JPEG / WebP(RIFF) / BMP / GIF via the reference libraries first
        // (libjpeg-turbo, libwebp), then WIC, then stb_image -- see sdk_image.h.
        auto img = castlemist::img::decode(result.decompressed.data(), result.decompressed.size());
        if (img) {
            result.preview_dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            result.preview_width = img->width;
            result.preview_height = img->height;
            result.preview_pitch = img->width * 4;
            result.preview_pixels = std::move(img->rgba);
            // Label carries the decoder so the info panel shows which library ran.
            result.preview_format_label = img->format + "  [" + img->decoder + "]";
            result.is_image = true;
        }
    }

    if (result.is_image) {
        result.kind = PreviewKind::Image;
        return result;
    }

    // strs string table -> text listing.
    if (castlemist::strs::is_strs(result.decompressed.data(), result.decompressed.size())) {
        result.kind = PreviewKind::Strs;
        result.text_preview = castlemist::strs::decode(result.decompressed.data(), result.decompressed.size());
        return result;
    }

    // ASND audio packfile (or raw audio) with an embedded MP3/Ogg -> playable audio.
    // AMSP sound-pool metadata has no embedded samples, so extract() returns empty
    // and we fall through to the generic handling.
    if (castlemist::audio::isAudio(result.decompressed.data(), result.decompressed.size())) {
        bool isAmsp = result.decompressed.size() >= 12 &&
                      std::memcmp(result.decompressed.data() + 8, "AMSP", 4) == 0;
        if (isAmsp) {
            // AMSP sound bank: the audio lives in external ASND entries it references.
            if (build_amsp_audio(result.decompressed, dat_path, result)) {
                result.kind = PreviewKind::Audio;
                return result;
            }
        } else {
            // ASND / ABNK bank / raw asnd: the audio is embedded.
            auto clips = castlemist::audio::extract(result.decompressed.data(), result.decompressed.size());
            if (!clips.empty()) {
                result.kind = PreviewKind::Audio;
                for (auto& c : clips) {
                    AudioClipCPU cc;
                    cc.codec = castlemist::audio::codecName(c.codec);
                    cc.data = std::move(c.data);
                    result.audio_clips.push_back(std::move(cc));
                }
                return result;
            }
        }
    }

    // PIMG paged-image atlas -> composite its referenced tiles into a preview image.
    if (result.decompressed.size() >= 12 && result.decompressed[0] == 'P' && result.decompressed[1] == 'F' &&
        std::memcmp(result.decompressed.data() + 8, "PIMG", 4) == 0) {
        PimgAtlas atlas;
        if (parse_pimg(result.decompressed, atlas) && !dat_path.empty()) {
            Gw2Dat dat;
            try { load_dat_file(dat, dat_path); } catch (const std::exception&) {}
            composite_pimg(atlas, dat, result);
            result.kind = PreviewKind::Image;
            result.is_image = true;
            return result;
        }
    }

    // cntc content datastore -> a readable summary (counts + content codenames).
    if (result.decompressed.size() >= 12 && result.decompressed[0] == 'P' && result.decompressed[1] == 'F' &&
        std::memcmp(result.decompressed.data() + 8, "cntc", 4) == 0) {
        std::wstring summary = parse_cntc_summary(result.decompressed);
        if (!summary.empty()) {
            // Every external asset fileId the content references -> an interactive list.
            result.content_asset_ids = cntc_referenced_asset_ids(result.decompressed, 100000);
            result.content_objects = parse_cntc_objects(result.decompressed, 100000);
            summary += L"\r\nReferences ";
            summary += std::to_wstring(result.content_asset_ids.size());
            summary += L" external asset fileIds across ";
            summary += std::to_wstring(result.content_objects.size());
            summary += L" content objects (item/skin/outfit/...).\r\n"
                       L"Pick an object (master) -> its assets (child) -> preview (detail).\r\n\r\n"
                       L"Content names/descriptions are stored as textIds, resolved through the\r\n"
                       L"text-pack family: txtm (manifest: textId -> strs file+slot), txtv (voices:\r\n"
                       L"textId -> voice audio), txtV (variants: textId -> gender/variant lines).";
            result.kind = result.content_objects.empty() ? PreviewKind::Text : PreviewKind::Content;
            result.text_preview = std::move(summary);
            return result;
        }
    }

    // Small non-renderable PF packfiles we decode into a readable text summary:
    // eula (agreement URLs), ABIX (audio bank index), the txt* localization packs
    // (that cntc content textIds resolve through), and CSCN cinematic scenes.
    {
        const auto& dec = result.decompressed;
        std::wstring summary;
        if      (castlemist::core::is_packfile(dec, "eula")) summary = parse_eula_summary(dec);
        else if (castlemist::core::is_packfile(dec, "ABIX")) summary = parse_abix_summary(dec);
        else if (castlemist::core::is_packfile(dec, "CSCN")) summary = parse_cscn_summary(dec);
        else if (castlemist::core::is_packfile(dec, "txtm") || castlemist::core::is_packfile(dec, "txtv") || castlemist::core::is_packfile(dec, "txtV"))
            summary = parse_textpack_summary(dec, dat_path);
        if (!summary.empty()) {
            result.kind = PreviewKind::Text;
            result.text_preview = std::move(summary);
            return result;
        }
    }

    // PF packfile with a prop-placement chunk -> map scene (mapc/area). Checked
    // before GEOM since a map has prp2 but no GEOM of its own.
    if (castlemist::core::has_chunk(result.decompressed, "prp2")) {
        result.kind = PreviewKind::Map;
        auto tpl = castlemist::tpl::get();
        if (tpl && !dat_path.empty()) {
            result.map = build_map_scene(result.decompressed, dat_path, *tpl);
        }
        return result;
    }

    // PF packfile with geometry -> model (parsed only if a template is loaded).
    if (castlemist::core::has_chunk(result.decompressed, "GEOM")) {
        result.kind = PreviewKind::Model;
        auto tpl = castlemist::tpl::get();
        if (tpl && !dat_path.empty()) {
            // want_game=true: also extract the real game (bgfx DXBC) shaders per
            // material for the "Shader" render mode (single-model path only).
            result.model = build_model_preview(result.decompressed, dat_path, *tpl, /*want_game=*/true);
        }
        return result;
    }

    // Fallback: printable text (html/css/js/xml/...).
    if (castlemist::core::looks_like_text(result.decompressed)) {
        result.kind = PreviewKind::Text;
        result.text_preview = castlemist::core::bytes_to_wide(result.decompressed);
        return result;
    }

    return result;
}


} // namespace castlemist::extract

// ---- public API (declared in castlemist/extract/entry_extractor.h) ----

using namespace castlemist::extract;

ExtractedEntry extract_entry(Gw2Dat& data_gw2, uint32_t mft_index) {
    std::vector<uint8_t> raw = extract_compressed_data(data_gw2, mft_index);
    return decompress_raw_entry(std::move(raw), data_gw2.mft_data_list[mft_index].compression_flag,
                                data_gw2.file_info.file_path);
}

ExtractedEntry extract_entry(const std::string& file_path, const MftData& entry) {
    std::vector<uint8_t> raw = read_entry_bytes(file_path, entry);
    return decompress_raw_entry(std::move(raw), entry.compression_flag, file_path);
}

ExtractedEntry extract_loose_file(std::vector<uint8_t> bytes, const std::string& source_path) {
    // Two kinds of file land here and they cannot be told apart by extension:
    // a finished asset (a .dds/.bik, or this app's own "Export Decompressed"),
    // and a raw MFT dump from "Export Compressed" that still has CRC32C framing
    // and possibly a Method0 payload. Try the plain reading first because it
    // cannot throw or mangle anything, and only fall back to unwrapping when it
    // yields nothing recognisable.
    ExtractedEntry plain;
    bool plain_ok = false;
    try {
        plain = decompress_raw_entry(bytes, 0, source_path, /*already_plain=*/true);
        plain_ok = true;
    } catch (const std::exception&) {
    }
    if (plain_ok && plain.kind != PreviewKind::None) return plain;

    // Compressed first: an entry that was stored uncompressed still needs its
    // CRC32C stripped, which flag 0 handles on the second pass.
    for (uint16_t flag : {uint16_t{1}, uint16_t{0}}) {
        try {
            ExtractedEntry unwrapped = decompress_raw_entry(bytes, flag, source_path, /*already_plain=*/false);
            if (unwrapped.kind != PreviewKind::None) return unwrapped;
        } catch (const std::exception&) {
        }
    }

    // Nothing recognised it. Return the plain reading anyway so the hex view still
    // shows the real file rather than a failed unwrap of it.
    if (plain_ok) return plain;
    return decompress_raw_entry(std::move(bytes), 0, source_path, /*already_plain=*/true);
}
