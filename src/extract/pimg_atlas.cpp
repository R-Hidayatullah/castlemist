/// @file
/// @brief PIMG paged-image atlases composited into one preview image.

#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <dxgiformat.h>

#include "castlemist/core/packfile.h"

namespace castlemist::extract {

// ---- PIMG (paged image atlas) --------------------------------------------
// A PIMG packfile (chunk PGTB) is not pixels: it's a grid of tiles, each an
// EXTERNAL texture referenced by fileId at a (cx,cy) coordinate. We composite the
// referenced tiles into a single downscaled preview image.
// Decode a GW2 "filename" field at position p -> fileId. The field is a self-relative
// pointer to a {uint16 lo, uint16 hi, ...} record; fileId = 0xFF00*hi + lo - 0xFF00FF
// (mirrors castlemist::model::decodeFilenameAt).
uint32_t decode_pimg_fileref(const std::vector<uint8_t>& d, size_t p) {
    auto rd32 = [&](size_t q) -> uint32_t {
        return (q + 4 <= d.size()) ? (d[q] | (d[q + 1] << 8) | (d[q + 2] << 16) | ((uint32_t)d[q + 3] << 24)) : 0;
    };
    auto rd16 = [&](size_t q) -> uint32_t { return (q + 2 <= d.size()) ? (uint32_t)(d[q] | (d[q + 1] << 8)) : 0; };
    uint32_t v = rd32(p);
    if (v == 0) return 0;
    size_t t = p + v;
    uint32_t lo = rd16(t), hi = rd16(t + 2);
    if (lo < 0x100 || hi < 0x100) return 0;
    return (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
}

// Parse the PGTB v3 chunk (layers[0] gives tile size/format; strippedPages gives the
// tile list). Returns false for other versions / malformed data.
bool parse_pimg(const std::vector<uint8_t>& d, PimgAtlas& out) {
    if (d.size() < 12 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "PIMG", 4) != 0) return false;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    // Locate the PGTB chunk.
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "PGTB", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz;
        if (next <= pos) return false;
        pos = next;
    }
    if (pos + 16 > d.size() || std::memcmp(d.data() + pos, "PGTB", 4) != 0) return false;
    if (rd16(pos + 8) != 3) return false;            // only the V3 layout is implemented
    size_t fb = pos + rd16(pos + 10);                // field base (chunk header size)
    // layers[0]: strippedDims @+8 (tile w,h), strippedFormat @+20.
    uint32_t layerCount = rd32(fb);
    size_t layer0 = (fb + 4) + rd32(fb + 4);         // self-relative array_ptr
    if (layerCount >= 1 && layer0 + 24 <= d.size()) {
        out.tileW = (int)rd32(layer0 + 8);
        out.tileH = (int)rd32(layer0 + 12);
        out.format = rd32(layer0 + 20);
    }
    // strippedPages: array of PagedImagePageDataV3 (24 bytes: layer, coord(cx,cy), fileId, flags, solidColor).
    uint32_t pageCount = rd32(fb + 16);
    size_t pages0 = (fb + 20) + rd32(fb + 20);
    if (pageCount == 0 || pageCount > 500000) return false;
    out.minX = out.minY = 1 << 30; out.maxX = out.maxY = -(1 << 30);
    for (uint32_t i = 0; i < pageCount; ++i) {
        size_t pp = pages0 + (size_t)i * 24;
        if (pp + 24 > d.size()) break;
        PimgAtlas::Page pg{ (int)rd32(pp + 4), (int)rd32(pp + 8), decode_pimg_fileref(d, pp + 12) };
        if (pg.fileId == 0) continue;
        out.pages.push_back(pg);
        out.minX = std::min(out.minX, pg.cx); out.maxX = std::max(out.maxX, pg.cx);
        out.minY = std::min(out.minY, pg.cy); out.maxY = std::max(out.maxY, pg.cy);
    }
    return !out.pages.empty() && out.tileW > 0 && out.tileH > 0;
}

// Composite an atlas into a single RGBA preview (downscaled to fit kMaxDim, tiles
// capped so huge map atlases still preview quickly). Fills result.preview_* + returns
// a summary line. `dat` must be loaded.
std::string composite_pimg(const PimgAtlas& a, Gw2Dat& dat, ExtractedEntry& result) {
    const int kMaxDim = 2048;   // max preview dimension
    const int kMaxTiles = 300;  // cap on tiles actually decoded (subsample beyond) -- keeps
                                // giant map atlases (1000+ tiles) previewing in reasonable time

    long long gridW = (long long)a.maxX - a.minX + 1, gridH = (long long)a.maxY - a.minY + 1;
    long long fullW = gridW * a.tileW, fullH = gridH * a.tileH;
    double scale = std::min(1.0, (double)kMaxDim / std::max(fullW, fullH));
    int outW = std::max(1, (int)(fullW * scale)), outH = std::max(1, (int)(fullH * scale));
    result.preview_pixels.assign((size_t)outW * outH * 4, 0);
    uint8_t* dst = result.preview_pixels.data();

    int stride = 1 + (int)(a.pages.size() / (size_t)kMaxTiles); // subsample if too many
    int loaded = 0;
    for (size_t pi = 0; pi < a.pages.size(); pi += stride) {
        const auto& pg = a.pages[pi];
        ModelTextureCPU tex;
        if (!decode_texture_by_fileid(dat, pg.fileId, tex) || tex.rgba.empty()) continue;
        ++loaded;
        // Destination rectangle for this tile in the downscaled canvas.
        int dx0 = (int)(((long long)(pg.cx - a.minX) * a.tileW) * scale);
        int dy0 = (int)(((long long)(pg.cy - a.minY) * a.tileH) * scale);
        int dw = std::max(1, (int)(a.tileW * scale)), dh = std::max(1, (int)(a.tileH * scale));
        for (int y = 0; y < dh; ++y) {
            int oy = dy0 + y; if (oy < 0 || oy >= outH) continue;
            int sy = (int)((long long)y * tex.height / dh); if (sy >= tex.height) sy = tex.height - 1;
            for (int x = 0; x < dw; ++x) {
                int ox = dx0 + x; if (ox < 0 || ox >= outW) continue;
                int sx = (int)((long long)x * tex.width / dw); if (sx >= tex.width) sx = tex.width - 1;
                const uint8_t* s = &tex.rgba[((size_t)sy * tex.width + sx) * 4];
                // Alpha-aware src-over compositing. A plain RGBA overwrite would
                // stamp a transparent tile's don't-care colour (often flat red)
                // over whatever is beneath and destroy the alpha; blending keeps
                // transparent regions transparent so the checkerboard shows and
                // overlapping tiles combine correctly.
                uint32_t sa = s[3];
                if (sa == 0) continue;                    // fully transparent: keep background
                uint8_t* o = &dst[((size_t)oy * outW + ox) * 4];
                if (sa == 255) {                          // opaque: straight copy
                    o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255;
                } else {                                  // partial: non-premultiplied src-over
                    uint32_t da = o[3], ia = 255 - sa;
                    uint32_t outA = sa + da * ia / 255;
                    if (outA == 0) { o[0] = o[1] = o[2] = o[3] = 0; }
                    else {
                        for (int k = 0; k < 3; ++k)
                            o[k] = (uint8_t)(((uint32_t)s[k] * sa + (uint32_t)o[k] * da * ia / 255) / outA);
                        o[3] = (uint8_t)outA;
                    }
                }
            }
        }
    }
    result.preview_dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    result.preview_width = outW;
    result.preview_height = outH;
    result.preview_pitch = outW * 4;
    char fourcc[5] = {0}; std::memcpy(fourcc, &a.format, 4);
    for (int i = 0; i < 4; ++i) if (fourcc[i] < 32) fourcc[i] = '?';
    result.preview_format_label = std::string("PIMG atlas ") + fourcc;
    char buf[256];
    std::snprintf(buf, sizeof buf, "PIMG paged-image atlas: %zu tiles (%lldx%lld grid, %dx%d px each, %s), %d tiles loaded into a %dx%d preview.",
                  a.pages.size(), gridW, gridH, a.tileW, a.tileH, fourcc, loaded, outW, outH);
    return buf;
}

} // namespace castlemist::extract
