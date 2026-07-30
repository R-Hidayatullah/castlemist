/// @file
/// @brief The format dispatcher: one MFT entry in, a previewable payload out.
/// @ingroup extract

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "castlemist/native/gw2dat.h"

#include "castlemist/extract/content_types.h"
#include "castlemist/extract/map_types.h"
#include "castlemist/extract/media_types.h"
#include "castlemist/extract/model_types.h"

/// @addtogroup extract
/// @{

/// @brief What the Preview tab should show for a given entry.
enum class PreviewKind {
    None,      ///< Unrecognized binary -- hex view only.
    Image,     ///< ATEX family / DDS / PNG / JPEG / BMP / WebP.
    Model,     ///< MODL packfile; ExtractedEntry::model may be null without a struct template.
    Map,       ///< mapc/area packfile with placed props -> ExtractedEntry::map.
    Text,      ///< Printable text (html/css/js/xml/...) -> ExtractedEntry::text_preview.
    Strs,      ///< `strs` string table -> the string table view.
    Audio,     ///< ASND / ABNK / AMSP / raw audio -> ExtractedEntry::audio_clips.
    Content,   ///< cntc content datastore -> the interactive asset browser.
    Video,     ///< Bink cinematic (KB2*/BIK*), played straight out of `decompressed`.
    Cinematic, ///< CINP scene script -> the movies it drives plus its subtitles.
};

/// @brief Everything the UI needs to display one .dat entry.
///
/// Both the raw and decompressed bytes are kept so the two hex tabs and both
/// export commands work without re-reading the archive; everything else is
/// derived from `decompressed`.
struct ExtractedEntry {
    /// @brief Raw on-disk bytes for this MFT entry, CRC32C still present ("before").
    std::vector<uint8_t> compressed;
    /// @brief Fully decompressed file bytes ("after") -- exactly what "Export Decompressed" writes.
    std::vector<uint8_t> decompressed;

    PreviewKind kind = PreviewKind::None; ///< Which surface the preview tab should show.
    bool is_image = false;                ///< `kind == PreviewKind::Image`; kept for existing call sites.

    /// @name Normalized preview image
    /// @brief Filled whenever @ref is_image, regardless of which decoder produced it.
    ///
    /// Callers do not need to know the source format:
    /// @code
    /// castlemist::gfx::set_texture(e.preview_dxgi_format, e.preview_width, e.preview_height,
    ///                     e.preview_pitch, e.preview_pixels.data());
    /// @endcode
    /// ATEX-family textures and PNG/JPEG/BMP/WebP are fully decoded to RGBA8888
    /// (`preview_dxgi_format == DXGI_FORMAT_R8G8B8A8_UNORM`); a passthrough
    /// "DDS " file keeps its native BCn blocks for the GPU to decode.
    /// @{
    uint32_t preview_dxgi_format = 0;  ///< DXGI format of @ref preview_pixels.
    uint32_t preview_width = 0;        ///< Pixels.
    uint32_t preview_height = 0;       ///< Pixels.
    uint32_t preview_pitch = 0;        ///< Row pitch in bytes (block pitch for BCn).
    std::vector<uint8_t> preview_pixels;    ///< The pixel data itself.
    std::string preview_format_label;  ///< Source format and decoder, e.g. "WebP (lossy)  [libwebp 1.6.0]".
    /// @}

    /// @brief Filled for PreviewKind::Text and PreviewKind::Strs.
    std::wstring text_preview;

    /// @brief Filled for PreviewKind::Model; null if no struct template was loaded.
    std::shared_ptr<ModelPreview> model;

    /// @brief Filled for PreviewKind::Map (a mapc/area packfile with a `prp2` chunk).
    std::shared_ptr<MapScene> map;

    /// @brief Filled for PreviewKind::Audio: one entry per embedded sound.
    std::vector<AudioClipCPU> audio_clips;

    /// @brief Filled for PreviewKind::Content: every external asset fileId the blob references.
    std::vector<uint32_t> content_asset_ids;

    /// @brief Filled for PreviewKind::Video.
    ///
    /// The video data itself stays in @ref decompressed -- the player decodes it
    /// in place, so nothing is duplicated.
    VideoMeta video;

    /// @name Filled for PreviewKind::Cinematic (a CINP pack)
    /// @brief Everything the scene script ties together.
    ///
    /// The movie bytes are deliberately *not* loaded here; the UI pulls the
    /// selected one from the dat on demand, since each is tens of megabytes.
    /// @{
    std::vector<uint32_t>     cinp_video_ids;      ///< Referenced Bink movies, in sequence order.
    std::vector<int>          cinp_video_seq;      ///< CSCN sequence each movie belongs to (parallel array).
    std::vector<uint32_t>     cinp_other_ids;      ///< Other referenced assets (sound scripts, ...).
    int                       cinp_version = 0;    ///< CSCN chunk version.
    uint32_t                  cinp_sequences = 0;  ///< Scenes on the timeline.
    uint32_t                  cinp_text_resources = 0; ///< Dialogue slots (may hold no inline text).
    std::vector<SubtitleLine> subtitles;           ///< Timed dialogue from the CSCN timeline.
    /// @}

    /// @brief Filled for PreviewKind::Content: the objects that hold those assets.
    std::vector<ContentObject> content_objects;
};

/// @brief Extract and decompress one MFT entry.
///
/// Everything is first run through `castlemist::cmp::decompress_method0()` (the generic
/// ANet "method 0" stream), then the decompressed bytes are sniffed for a known
/// payload:
///
/// | Signature | Handled as |
/// |-----------|------------|
/// | `ATEX`/`ATTX`/`ATEC`/`ATEP`/`ATEU`/`ATET` | texture, via `gw2_atex.hpp` |
/// | `DDS ` | native BCn, decoded on the GPU |
/// | PNG / JPEG / BMP / RIFF-WebP | still image, via the reference codecs |
/// | `strs` | string table |
/// | `PF` + `GEOM` | model (needs a struct template) |
/// | `PF` + `prp2` | map scene |
/// | `PF` + `ASND`/`ABNK`/`AMSP`, raw `asnd` | audio |
/// | `PF` + `cntc` | content datastore |
/// | `KB2*`/`BIK*` | Bink video |
/// | anything printable | plain text |
///
/// @param data_gw2   An open archive. Only safe to call from the thread that owns it.
/// @param mft_index  Zero-based MFT index (the UI's row number).
/// @throws std::runtime_error, castlemist::cmp::decode_error on malformed data.
/// @see extract_entry(const std::string&, const MftData&) for background-thread use.
ExtractedEntry extract_entry(Gw2Dat& data_gw2, uint32_t mft_index);

/// @brief Background-thread-safe overload.
///
/// Takes a copy of the one MftData entry and the archive path, touching no
/// shared mutable state: it opens its own file handle and copies everything
/// else. The struct template, if any, is captured internally as a shared_ptr
/// snapshot.
///
/// @param file_path Path to the .dat.
/// @param entry     A copy of the MFT record to extract.
ExtractedEntry extract_entry(const std::string& file_path, const MftData& entry);

/// @brief Identify and preview a file that is not inside a .dat.
///
/// Runs the same format detection as an archive entry, so anything the browser
/// can preview from Gw2.dat -- ATEX family, DDS, PNG/JPEG, Bink video, audio,
/// packfiles, text -- previews the same way off disk. That covers assets exported
/// from the archive as well as ones that were never in it.
///
/// Accepts both shapes this app can write: a finished file ("Export
/// Decompressed", or any ordinary .dds/.bik), and a raw MFT dump ("Export
/// Compressed") that still carries CRC32C framing and possibly Method0
/// compression. The two are indistinguishable by extension, so the reading is
/// chosen by trying the harmless one first and unwrapping only if that fails.
///
/// Never throws for unrecognised input -- it comes back as PreviewKind::None with
/// the bytes intact for the hex view.
///
/// @param bytes       The whole file.
/// @param source_path Where it came from; used to resolve model textures and shown in the UI.
ExtractedEntry extract_loose_file(std::vector<uint8_t> bytes, const std::string& source_path);

/// @name Texture resolution preference
/// @brief Which member of a full/reduced texture pair model materials should load.
///
/// GW2 stores many textures as a pair at consecutive MFT indices (full at
/// baseId B, reduced at B-1, same format, exactly half the dimensions) and a
/// material may reference *either*. Following the reference literally rendered
/// weapons and armour at half resolution.
///
/// Takes effect on the next model (re)load.
/// @{
void set_texture_full_res(bool full); ///< true (default) = prefer the full-res member.
bool texture_full_res();              ///< Current preference.
/// @}

/// @brief Lazily build the zone-model layer of an already-decompressed map packfile.
///
/// Parses `zon2` and loads the zone definition models in parallel. Instances are
/// tagged layer 3 (zone). Safe to call from a worker thread.
///
/// @param map_bytes The map's decompressed packfile bytes.
/// @param dat_path  Path to the .dat the models live in.
/// @return null when the map has no zone models.
std::shared_ptr<MapScene> build_map_zone_layer(const std::vector<uint8_t>& map_bytes,
                                               const std::string& dat_path);

/// @brief Lazily extract the real game (bgfx DXBC) shaders for a built map scene, in place.
///
/// Re-reads each unique model's MODL bytes by fileId and rebuilds it with game
/// shaders. Only touches models whose `gameMaterials` are still empty and whose
/// fileId is known. Kept out of the initial map load so map loading stays fast;
/// the UI calls it the first time "Shader" mode is selected on a map.
///
/// @param scene    Scene to augment; MapScene::gameMaterialsLoaded is set on success.
/// @param dat_path Path to the .dat (a private handle is opened).
/// @return true when at least one model gained game shaders.
bool augment_map_scene_game(MapScene& scene, const std::string& dat_path);

/// @brief Find the cinematic script that drives a Bink movie and pull its subtitles.
///
/// GW2's pre-rendered movies are played by a CINP cinematic (chunk `CSCN`): the
/// CINP references the `.bik` by fileId, carries the dialogue as TextResource
/// entries, and fires them from a track of trigger keys that each hold a time
/// and the text's token. So a subtitle is
/// `(trigger.time, textResource[trigger.token1])`.
///
/// @param dat_path      Path to the .dat.
/// @param cinp_base_ids CINP packs to search, from the index DB.
/// @param video_file_id The movie to match.
/// @param out           Receives the subtitles, sorted by time.
/// @return The CINP baseId that matched, or 0.
///
/// @warning Reads and decompresses every candidate: ~1400 CINP packs take a
///          couple of seconds. Call it off the UI thread or behind a wait cursor.
uint32_t find_video_subtitles(const std::string& dat_path,
                              const std::vector<uint32_t>& cinp_base_ids,
                              uint32_t video_file_id, std::vector<SubtitleLine>& out);

/// @brief Read one Bink movie out of the dat by fileId.
/// @return The bytes `castlemist::vid::open` wants; empty on failure or if the entry is not a Bink.
std::vector<uint8_t> load_video_by_fileid(const std::string& dat_path, uint32_t file_id);

/// @}
