/// @file
/// @brief Internal seam of the extract layer -- helpers shared between its .cpp files.
///
/// @warning Not a public header. Nothing outside `src/extract/` may include it:
///          these declarations follow the .dat's quirks and change whenever a new
///          format is reverse-engineered. The layer's public API is
///          `castlemist/extract/entry_extractor.h`.
///
/// The extract layer used to be one 2500-line translation unit whose helpers all
/// lived in an anonymous namespace. Splitting it by *format family* -- one file
/// per thing the .dat can contain -- meant giving those helpers a real namespace;
/// this header is that namespace's declaration.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"

#include "castlemist/extract/entry_extractor.h"

namespace castlemist::extract {

// ---------------------------------------------------------------- threading --

/// @brief Run `fn(0..n-1)` across a small worker pool.
///
/// A dynamic index queue rather than a static split, so uneven work (map props
/// range from a 200-triangle rock to a 40 000-triangle building) balances
/// itself. Falls back to a serial loop for `n <= 1` or a single-core machine.
template <class F>
void parallel_for(size_t n, F&& fn) {
    if (n == 0) return;
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned nt = static_cast<unsigned>(std::min<size_t>(hw, n));
    if (nt <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (unsigned t = 0; t < nt; ++t) {
        pool.emplace_back([&] {
            for (size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) fn(i);
        });
    }
    for (auto& th : pool) th.join();
}

// ------------------------------------------------------- signature sniffing --
// image_preview.cpp

/// @brief True when @p data starts with the four bytes of @p magic.
bool starts_with_magic(const std::vector<uint8_t>& data, const char (&magic)[5]);

/// @brief True for any ATEX-family container `castlemist::atex::parse` understands.
///
/// Covers ATEX/ATTX/ATEC/ATEP/ATEU/ATET and their `C`-prefixed twins
/// (CTEX/CTTX/...), which are the same format under a different magic.
bool is_atex_family(const std::vector<uint8_t>& data);

bool is_png(const std::vector<uint8_t>& data);  ///< PNG signature check.
bool is_jpeg(const std::vector<uint8_t>& data); ///< JPEG SOI check.
bool is_bmp(const std::vector<uint8_t>& data);  ///< Windows BMP `BM` check.

/// @brief Unchecked little-endian 32-bit read (callers have already bounds-checked).
uint32_t read_u32_le(const std::vector<uint8_t>& data, size_t offset);

/// @brief Short human name for a DXGI format, e.g. `DXGI_FORMAT_BC3_UNORM` -> "BC3".
const char* dxgi_short_name(uint32_t format);

/// @brief True when a texture format label names a two-channel normal map.
bool is_normal_format(const std::string& fmt);

/// @brief Heuristic: a greyscale texture with large near-black areas is a glow/alpha mask.
///
/// Ported from `gw2viewer.cpp::looks_like_effect`. Samples every 97th pixel.
bool looks_like_effect(const std::vector<uint8_t>& rgba);

/// @brief Decode a standalone "DDS " file into the preview fields, keeping native BCn.
/// @return false when the DDS is not a recognized BC format.
bool fill_preview_from_dds(ExtractedEntry& result);

/// @brief Decode an ATEX-family container to RGBA8888 via `gw2_atex.hpp`.
/// @return false on any parse or decode failure, so the entry falls back to hex.
bool fill_preview_from_atex(ExtractedEntry& result);

// --------------------------------------------------------- texture sourcing --
// texture_source.cpp

/// @brief Decompress one MFT entry addressed by physical index (== baseId - 1).
std::vector<uint8_t> decompress_by_index(Gw2Dat& dat, uint32_t index);

/// @brief Decompress the entry a fileId resolves to and decode its ATEX to RGBA.
///
/// Honours ::texture_full_res: a material may reference either member of a
/// full/reduced texture pair, so the resolved index is adjusted to the wanted
/// one before decoding.
///
/// @return false when the fileId is unknown or is not a decodable texture.
bool decode_texture_by_fileid(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out);

// -------------------------------------------------------------- game shaders --
// game_shader.cpp

/// @brief Parse a bgfx shader blob (v11) into raw DXBC plus its uniform table.
///
/// Byte-for-byte the layout `gw2gsviewer.cpp::parse_bgfx` reads: a `V`/`F`/`C`
/// stage tag and version, hashes, then the uniform records and the bytecode.
///
/// @param blob      The bgfx blob.
/// @param dxbc      Receives the shader bytecode.
/// @param uniforms  Receives the uniform table.
/// @param constBuf  Receives the constant-buffer size marker.
void parse_bgfx(const std::vector<uint8_t>& blob, std::vector<uint8_t>& dxbc,
                std::vector<GameShaderUniform>& uniforms, uint32_t& constBuf);

/// @brief Trace a material to its AMAT package and extract the game's own VS+PS.
///
/// The AMAT sits at `baseId - 1` of the material's `materialFile` texture. This
/// picks a vertex/pixel pair, parses both with ::parse_bgfx, resolves the
/// sampler bindings against the model's textures, and pulls the material
/// constant defaults.
///
/// @param dat        Open archive.
/// @param tpl        Struct template (`gw2_packfile.json`).
/// @param m          The material to resolve.
/// @param get_tex    Callback mapping a texture fileId to a ModelPreview::textures index.
/// @param effectHint True when the material was classified as an effect.
/// @return A GameMaterial whose `ok` is false when no usable shaders were found.
GameMaterial extract_game_material(Gw2Dat& dat, const nlohmann::json& tpl,
                                   const castlemist::model::Material& m,
                                   const std::function<int(uint32_t)>& get_tex, bool effectHint);

// ------------------------------------------------------------ model preview --
// model_preview.cpp

/// @brief Read the MODL bytes a fileId points at (generic decompress, no parsing).
std::vector<uint8_t> load_modl_bytes_by_fileid(Gw2Dat& dat, uint32_t fileId);

/// @brief Build a renderable model from MODL bytes, using an already-open archive.
///
/// @param modl_bytes Decompressed MODL packfile.
/// @param dat        Open archive, for the material textures.
/// @param tpl        Struct template.
/// @param want_game  Also extract the game's bgfx shaders (single-model path only:
///                   it costs an extra AMAT trace per material, which would make
///                   map loads crawl).
/// @return null when the file carries no usable geometry.
std::shared_ptr<ModelPreview> build_model_preview(const std::vector<uint8_t>& modl_bytes, Gw2Dat& dat,
                                                  const nlohmann::json& tpl, bool want_game = false);

/// @brief Overload that opens its own archive handle, for background threads with no shared Gw2Dat.
std::shared_ptr<ModelPreview> build_model_preview(const std::vector<uint8_t>& modl_bytes,
                                                  const std::string& dat_path,
                                                  const nlohmann::json& tpl, bool want_game = false);

// ---------------------------------------------------------------- map scene --
// map_scene.cpp

/// @brief Turn a map's height grid into a renderable terrain mesh.
///
/// @param t         Parsed terrain block.
/// @param hasWaterZ A real water plane height was found in the `havk` chunk.
/// @param waterZ    That height (GW2 is Z-up, so this is an altitude).
/// @return null when the map has no terrain.
std::shared_ptr<ModelPreview> build_terrain_model(const castlemist::model::Extractor::MapTerrain& t,
                                                  bool hasWaterZ, float waterZ);

/// @brief Turn a map's collision hull into a renderable wireframe-ish mesh.
std::shared_ptr<ModelPreview> build_collision_model(const castlemist::model::Extractor::MapCollision& c);

/// @brief Load a map packfile's props, terrain and collision into one scene.
std::shared_ptr<MapScene> build_map_scene(const std::vector<uint8_t>& map_bytes,
                                          const std::string& dat_path, const nlohmann::json& tpl);

// -------------------------------------------------------------- PIMG atlases --
// pimg_atlas.cpp

/// @brief A paged-image atlas: a grid of tiles, each an external texture.
///
/// A PIMG packfile (chunk `PGTB`) holds no pixels of its own. Each page names a
/// texture by fileId and a `(cx, cy)` grid coordinate; the preview is the
/// composite.
struct PimgAtlas {
    int tileW = 0;        ///< Tile width in pixels.
    int tileH = 0;        ///< Tile height in pixels.
    uint32_t format = 0;  ///< Tile fourcc, e.g. `'DXT5'`.
    int minX = 0, minY = 0, maxX = 0, maxY = 0; ///< Grid extent.
    /// @brief One tile.
    struct Page {
        int cx;            ///< Grid column.
        int cy;            ///< Grid row.
        uint32_t fileId;   ///< The tile texture.
    };
    std::vector<Page> pages; ///< Every tile in the atlas.
};

/// @brief Parse a PIMG packfile's `PGTB` chunk into a PimgAtlas.
/// @return false when @p blob is not a PIMG.
bool parse_pimg(const std::vector<uint8_t>& blob, PimgAtlas& out);

/// @brief Decode the atlas's tiles and blit them into one preview image.
///
/// Downscales to fit a 2048-pixel bound and decodes at most a few hundred tiles
/// (subsampling beyond), so a 67x104-tile world map still previews in seconds.
///
/// @return A human-readable format label, or an empty string on failure.
std::string composite_pimg(const PimgAtlas& atlas, Gw2Dat& dat, ExtractedEntry& result);

// -------------------------------------------------------------- audio banks --
// audio_bank.cpp

/// @brief Collect the ASND fileIds an AMSP sound script references.
///
/// AMSP holds no samples: its `metaSound` array names external ASND entries.
std::vector<uint32_t> parse_amsp_sound_ids(const std::vector<uint8_t>& blob);

/// @brief Load every sound an AMSP script references into `result.audio_clips`.
/// @return false when the script references nothing loadable.
bool build_amsp_audio(const std::vector<uint8_t>& amsp, const std::string& dat_path,
                      ExtractedEntry& result);

// ------------------------------------------------------------ content store --
// content_store.cpp

/// @brief Human-readable summary of a cntc `PackContent` header.
std::wstring parse_cntc_summary(const std::vector<uint8_t>& blob);

/// @brief Every external asset fileId a cntc references, deduped.
/// @param cap Stop after this many, so a 8.5 MB content blob cannot stall the UI.
std::vector<uint32_t> cntc_referenced_asset_ids(const std::vector<uint8_t>& blob, size_t cap);

/// @brief Parse a cntc into its content objects (type, id, assets, strings).
std::vector<ContentObject> parse_cntc_objects(const std::vector<uint8_t>& blob, size_t cap);

/// @brief Peek an entry by fileId and name what it is ("texture", "model", "audio", ...).
std::string classify_fileid(Gw2Dat& dat, uint32_t fileId);

// ---------------------------------------------------------- packfile summaries --
// pf_summary.cpp

std::wstring parse_eula_summary(const std::vector<uint8_t>& blob);  ///< `eula` PackEulaV0.
std::wstring parse_abix_summary(const std::vector<uint8_t>& blob);  ///< `ABIX`/`BIDX` bank index.

/// @brief `txtm` / `txtv` / `txtV` text-pack manifests.
/// @param dat_path Needed to resolve referenced string entries inline.
std::wstring parse_textpack_summary(const std::vector<uint8_t>& blob, const std::string& dat_path);

// ---------------------------------------------------------------- cinematics --
// cinematic.cpp

std::wstring parse_cscn_summary(const std::vector<uint8_t>& blob); ///< CSCN scene overview.

/// @brief Pull the timed dialogue out of a CINP's CSCN chunk.
///
/// A subtitle is `(triggerKey.time, textResource[triggerKey.token1])`. The CSCN
/// struct layout is **not** stable across versions (v30..v36 all ship in this
/// dat), so the field offsets are derived per version rather than assumed.
///
/// @return false when the pack carries no dialogue.
bool parse_cscn_subtitles(const std::vector<uint8_t>& blob, std::vector<SubtitleLine>& out);

/// @brief Collect `(fileId, sequenceIndex)` for every asset a CSCN references.
void collect_cscn_filerefs(const std::vector<uint8_t>& blob,
                           std::vector<std::pair<uint32_t, int>>& out);

/// @brief Read a CSCN's version, sequence count and text-resource count.
void cscn_counts(const std::vector<uint8_t>& blob, int& version, uint32_t& sequences, uint32_t& textRes);

/// @brief Peek an entry by fileId and report whether it is a Bink movie.
bool peek_is_bink_fileid(Gw2Dat& dat, uint32_t file_id);

// ------------------------------------------------------------ the dispatcher --
// entry_extractor.cpp

/// @brief Everything CPU-bound about turning raw entry bytes into an ExtractedEntry.
///
/// Independent of how @p raw_bytes was read off disk, which is what makes it
/// safe to run on a background thread.
///
/// @param raw_bytes        On-disk bytes including the CRC32C.
/// @param compression_flag The MFT entry's flag (0 = stored).
/// @param dat_path         Used to resolve model textures and external references.
ExtractedEntry decompress_raw_entry(std::vector<uint8_t> raw_bytes, uint16_t compression_flag,
                                    const std::string& dat_path);

} // namespace castlemist::extract
