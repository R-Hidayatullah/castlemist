/// @file
/// @brief Reading and decoding model textures out of the archive.

#include "internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <string>

#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2_atex.hpp"

#include "castlemist/core/packfile.h"

namespace castlemist::extract {

// Texture resolution preference. GW2 stores many textures as a full/reduced pair
// at consecutive MFT indices (baseId B = full resolution, B-1 = the exact-half
// version, same format). A material may reference EITHER member of the pair, so the
// "full" texture is sometimes at the resolved index and sometimes one entry above
// it. When true (default) we load the full-resolution member; when false the reduced
// one (faster / less VRAM). Read on the bg thread; set from the UI.
static std::atomic<bool> g_tex_full_res{true};

// Decompress one MFT entry (by 0-based array index) into its raw ATEX/CTEX bytes.
static bool read_mft_atex_bytes(Gw2Dat& dat, size_t i0, std::vector<uint8_t>& bytes) {
    if (i0 >= dat.mft_data_list.size()) return false;
    const MftData& e = dat.mft_data_list[i0];
    try {
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
        if (e.compression_flag == 0) {
            bytes = std::move(stripped);
        } else {
            if (stripped.size() < 8) return false;
            uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
            bytes = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
        }
        if (bytes.size() >= 4 && bytes[0] == 'C') bytes[0] = 'A'; // CTEX -> ATEX alias
        return bytes.size() >= 12;
    } catch (const std::exception&) {
        return false;
    }
}

// Peek an entry's ATEX dimensions + format from the header only (no pixel decode).
static bool peek_mft_atex(Gw2Dat& dat, size_t i0, int& w, int& h, std::string& fmt) {
    std::vector<uint8_t> bytes;
    if (!read_mft_atex_bytes(dat, i0, bytes)) return false;
    try {
        castlemist::atex::Texture t = castlemist::atex::parse(bytes.data(), bytes.size());
        w = t.width; h = t.height; fmt = t.fmt_name;
        return w > 0 && h > 0;
    } catch (const std::exception&) {
        return false;
    }
}

// Given the entry a fileId resolved to (i0), find the full/reduced member of its
// pair. The pair is a same-format neighbor with exactly double/half dimensions; the
// full member is always the higher index (baseId B vs B-1). Returns i0 unchanged if
// there is no paired sibling.
static size_t resolve_res_index(Gw2Dat& dat, size_t i0, bool wantFull) {
    int w0, h0; std::string f0;
    if (!peek_mft_atex(dat, i0, w0, h0, f0)) return i0;
    int w1, h1; std::string f1;
    // A full sibling one entry above => i0 is the reduced member.
    if (i0 + 1 < dat.mft_data_list.size() && peek_mft_atex(dat, i0 + 1, w1, h1, f1)
        && f1 == f0 && w1 == 2 * w0 && h1 == 2 * h0)
        return wantFull ? i0 + 1 : i0;
    // A reduced sibling one entry below => i0 is the full member.
    if (i0 >= 1 && peek_mft_atex(dat, i0 - 1, w1, h1, f1)
        && f1 == f0 && w0 == 2 * w1 && h0 == 2 * h1)
        return wantFull ? i0 : i0 - 1;
    return i0;
}

// Does this texture's alpha channel act as GW2's alpha-test coverage mask?
//
// The game's opaque material (and normal-prepass) pixel shaders discard where the
// diffuse alpha is below 0.25 -- they spell it `saturate(2a) < 0.5`. Applying that
// blindly is unsafe: plenty of opaque materials ship a diffuse whose alpha is
// uniformly 0 because the shader variant they use never reads it, and cutting those
// out would erase the mesh. So require the alpha channel to genuinely partition the
// image: a real-but-minority set of texels below the threshold, and a solid majority
// above it. Fully-opaque maps (the common case) return false and cost nothing.
static bool compute_alpha_cutout(const std::vector<uint8_t>& rgba) {
    if (rgba.size() < 4) return false;
    size_t n = rgba.size() / 4;
    size_t step = n > 65536 ? n / 65536 : 1;   // cap the scan at ~64k samples
    size_t below = 0, sampled = 0;
    for (size_t p = 0; p < n; p += step) {
        if (rgba[p * 4 + 3] < 64) below++;      // 64/255 ~= the 0.25 threshold
        sampled++;
    }
    if (sampled == 0) return false;
    double frac = static_cast<double>(below) / static_cast<double>(sampled);
    return frac > 0.005 && frac < 0.95;
}

// Which channels of the decoded RGBA actually carry information.
//
// The container format is only an upper bound: every BCn decode lands in an
// RGBA8888 buffer, and plenty of DXT5 diffuses ship a uniformly-opaque alpha, so
// "DXT5" alone would report RGBA for what is really an RGB texture. Measure it
// from the pixels instead, with one format-driven exception: a BC5/3Dc normal map
// stores only two channels and the decoder *synthesises* blue, so reporting RGB
// for it would be a lie about the file.
//
// Alpha counts as a channel only when it varies. A constant alpha -- all 255
// (opaque) or all 0 (the "shader never reads it" case that `compute_alpha_cutout`
// also guards against) -- carries nothing.
static std::string compute_channels(const std::vector<uint8_t>& rgba, bool isNormal) {
    if (rgba.size() < 4) return "";
    size_t n = rgba.size() / 4;
    size_t step = n > 65536 ? n / 65536 : 1;   // cap the scan at ~64k samples
    bool colored = false;
    uint8_t aMin = 255, aMax = 0;
    for (size_t p = 0; p < n; p += step) {
        const uint8_t* t = &rgba[p * 4];
        if (t[0] != t[1] || t[1] != t[2]) colored = true;
        aMin = std::min(aMin, t[3]);
        aMax = std::max(aMax, t[3]);
    }
    std::string ch = isNormal ? "RG" : (colored ? "RGB" : "R");
    if (aMin != aMax) ch += "A";
    return ch;
}

// Decode the ATEX at a 0-based MFT index to RGBA.
static bool decode_mft_atex(Gw2Dat& dat, size_t i0, ModelTextureCPU& out) {
    std::vector<uint8_t> bytes;
    if (!read_mft_atex_bytes(dat, i0, bytes)) return false;
    try {
        castlemist::atex::Texture t = castlemist::atex::parse(bytes.data(), bytes.size());
        castlemist::atex::Image im = castlemist::atex::decode(t, 0);
        if (im.width <= 0 || im.height <= 0 || im.rgba.empty()) return false;
        out.width = im.width;
        out.height = im.height;
        out.fmt = t.fmt_name;
        out.mipCount = static_cast<int>(t.mips.size());
        out.baseId = static_cast<uint32_t>(i0 + 1);   // MFT array index -> baseId
        out.rgba = std::move(im.rgba);
        out.isNormal = is_normal_format(t.fmt_name);
        out.channels = compute_channels(out.rgba, out.isNormal);
        out.hasCutout = !out.isNormal && compute_alpha_cutout(out.rgba);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Decompress one MFT entry addressed by fileId, then decode its ATEX to RGBA,
// choosing the full- or reduced-resolution member of its pair per g_tex_full_res.
// Returns false if the fileId isn't found or isn't a decodable texture.
bool decode_texture_by_fileid(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) {
        return false;
    }
    size_t i0 = resolve_res_index(dat, base - 1, g_tex_full_res.load());
    if (!decode_mft_atex(dat, i0, out)) return false;
    out.fileId = fileId;
    return true;
}

} // namespace castlemist::extract

// ---- public API (declared in castlemist/extract/entry_extractor.h) ----

using namespace castlemist::extract;

// Public texture-resolution API (g_tex_full_res lives in the anonymous namespace
// above but is reachable throughout this translation unit).
void set_texture_full_res(bool full) { g_tex_full_res.store(full); }
bool texture_full_res() { return g_tex_full_res.load(); }
