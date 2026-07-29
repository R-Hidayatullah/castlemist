/// @file
/// @brief Signature sniffing and the DDS / ATEX still-image preview path.

#include "internal.h"

#include <algorithm>
#include <cstring>

#include <dxgiformat.h>

#include "castlemist/native/gw2_atex.hpp"

#include "castlemist/format/dds.h"

namespace castlemist::extract {

bool starts_with_magic(const std::vector<uint8_t>& data, const char (&magic)[5]) {
    return data.size() >= 4 && std::memcmp(data.data(), magic, 4) == 0;
}

bool is_atex_family(const std::vector<uint8_t>& data) {
    // Every ATE*/CTE* container castlemist::atex::parse() understands. The C-prefixed
    // variants (CTEX/CTEU/CTEP/CTEC/CTET/CTTX) are the same format with a 'C'
    // magic instead of 'A' -- fill_preview_from_atex() aliases C->A before
    // parsing (matching gw2viewer.cpp's load_texture_rgba).
    if (data.size() < 4) {
        return false;
    }
    char c0 = static_cast<char>(data[0]);
    if (c0 != 'A' && c0 != 'C') {
        return false;
    }
    static const char* suffixes[] = {"TEX", "TTX", "TEC", "TEP", "TEU", "TET"};
    for (const char* s : suffixes) {
        if (data[1] == static_cast<uint8_t>(s[0]) && data[2] == static_cast<uint8_t>(s[1]) &&
            data[3] == static_cast<uint8_t>(s[2])) {
            return true;
        }
    }
    return false;
}

bool is_png(const std::vector<uint8_t>& data) {
    static constexpr uint8_t kSig[4] = {0x89, 'P', 'N', 'G'};
    return data.size() >= 4 && std::memcmp(data.data(), kSig, 4) == 0;
}

bool is_jpeg(const std::vector<uint8_t>& data) {
    static constexpr uint8_t kSig[3] = {0xFF, 0xD8, 0xFF};
    return data.size() >= 3 && std::memcmp(data.data(), kSig, 3) == 0;
}

bool is_bmp(const std::vector<uint8_t>& data) { return data.size() >= 2 && data[0] == 'B' && data[1] == 'M'; }

uint32_t read_u32_le(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

const char* dxgi_short_name(uint32_t format) {
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM: return "BC1";
    case DXGI_FORMAT_BC2_UNORM: return "BC2";
    case DXGI_FORMAT_BC3_UNORM: return "BC3";
    case DXGI_FORMAT_BC4_UNORM: return "BC4";
    case DXGI_FORMAT_BC5_UNORM: return "BC5";
    case DXGI_FORMAT_BC6H_UF16: return "BC6H";
    case DXGI_FORMAT_BC7_UNORM: return "BC7";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8 sRGB";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8 sRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "BGRX8";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2";
    case DXGI_FORMAT_B5G6R5_UNORM: return "B5G6R5";
    case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1";
    case DXGI_FORMAT_B4G4R4A4_UNORM: return "B4G4R4A4";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    default: return "DDS";
    }
}

// Decodes a real standalone "DDS " file into the preview_* fields, keeping its
// native BCn blocks for the GPU. Returns false (leaving is_image untouched) if
// the DDS isn't a recognized BC format.
bool fill_preview_from_dds(ExtractedEntry& result) {
    auto info = castlemist::dds::parse_dds(result.decompressed.data(), result.decompressed.size());
    if (!info) {
        return false;
    }
    result.preview_dxgi_format = info->dxgi_format;
    result.preview_width = info->width;
    result.preview_height = info->height;
    result.preview_pitch = info->sys_mem_pitch;
    result.preview_pixels.assign(result.decompressed.begin() + static_cast<ptrdiff_t>(info->data_offset),
                                  result.decompressed.end());
    result.preview_format_label = std::string("DDS ") + dxgi_short_name(info->dxgi_format);
    return true;
}

// Decodes an ATEX-family container to RGBA8888 via the accurate gw2_atex
// decoder. Returns false on any parse/decode failure so the entry falls back to
// a plain binary with no preview.
bool fill_preview_from_atex(ExtractedEntry& result) {
    if (result.decompressed.empty()) {
        return false;
    }
    // CTEX/CTEU/CTEP/... share the ATEX layout but start with 'C'; alias to 'A'
    // just for the parse, then restore so "Export Decompressed" stays byte-exact.
    uint8_t saved0 = result.decompressed[0];
    bool aliased = (saved0 == 'C');
    if (aliased) {
        result.decompressed[0] = 'A';
    }

    bool ok = false;
    try {
        castlemist::atex::Texture tex = castlemist::atex::parse(result.decompressed.data(), result.decompressed.size());
        castlemist::atex::Image img = castlemist::atex::decode(tex, 0);
        if (img.width > 0 && img.height > 0 && !img.rgba.empty()) {
            result.preview_dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            result.preview_width = static_cast<uint32_t>(img.width);
            result.preview_height = static_cast<uint32_t>(img.height);
            result.preview_pitch = static_cast<uint32_t>(img.width) * 4;
            result.preview_pixels = std::move(img.rgba);
            result.preview_format_label = tex.fmt_name;
            ok = true;
        }
    } catch (const std::exception&) {
        ok = false;
    }

    if (aliased) {
        result.decompressed[0] = saved0;
    }
    return ok;
}


bool is_normal_format(const std::string& fmt) {
    return fmt.find("3DC") != std::string::npos || fmt.find("BC5") != std::string::npos ||
           fmt.find("ATI2") != std::string::npos;
}

// Grayscale texture with large near-black regions -> used as a glow/alpha mask
// (an "effect" material). Ported from gw2viewer.cpp::looks_like_effect.
bool looks_like_effect(const std::vector<uint8_t>& px) {
    long n = 0, colored = 0, dark = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4 * 97) {
        int r = px[i], g = px[i + 1], b = px[i + 2];
        int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        int lum = (r * 77 + g * 150 + b * 29) >> 8;
        if (mx - mn > 18) colored++;
        if (lum < 32) dark++;
        n++;
    }
    if (n == 0) return false;
    bool grayscale = colored < n / 15;
    bool masky = dark > n / 8;
    return grayscale && masky;
}

} // namespace castlemist::extract
