// gw2_atex_encode.hpp -- ArenaNet ATEX texture ENCODER, single header (C++20)
//
// Inverse of gw2_atex.hpp. Takes an RGBA8888 image and writes an ATEX container
// that castlemist::atex::parse + castlemist::atex::decode (and the game) read back.
//
// Two pieces:
//   1. A straightforward BCn block compressor -- BC1 (DXT1) colour blocks and
//      BC4-style interpolated-alpha blocks (the alpha half of DXT5). Bounding-box
//      endpoint selection + nearest-index assignment: fast, good enough, and
//      bit-exactly decodable by the existing decoders.
//   2. A container writer that emits the ATEX header + a full mip chain using the
//      *raw* per-mip encoding (flags == 0). inflate_mip's flags==0 path skips all
//      the RLE constant-fill passes and just reads plane-separated block bytes, so
//      we write exactly those planes:
//        DXT1: [endpoints plane 4B/blk][indices plane 4B/blk]
//        DXT5: [alpha plane 8B/blk][endpoints plane 4B/blk][indices plane 4B/blk]
//
// Usage:
//   std::vector<uint8_t> atex = castlemist::atex::encode(rgba, W, H, "DXT5");
//   // verify: castlemist::atex::decode(castlemist::atex::parse(atex.data(), atex.size()), 0)
//
#ifndef GW2_ATEX_ENCODE_HPP
#define GW2_ATEX_ENCODE_HPP

#include <cstdint>
#include <cstring>
#include <array>
#include <string>
#include <vector>
#include <stdexcept>

namespace castlemist::atex {

namespace enc {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Gather one 4x4 block from an RGBA image, clamping reads at the edges so that
// partial edge blocks (non-multiple-of-4 dimensions) repeat the last valid texel.
inline void gather_block(const uint8_t* rgba, int W, int H, int bx, int by, uint8_t out[16][4]) {
    for (int j = 0; j < 4; ++j) {
        int y = clampi(by * 4 + j, 0, H - 1);
        for (int i = 0; i < 4; ++i) {
            int x = clampi(bx * 4 + i, 0, W - 1);
            const uint8_t* s = &rgba[(static_cast<size_t>(y) * W + x) * 4];
            uint8_t* d = out[j * 4 + i];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
}

inline uint16_t pack565(int r, int g, int b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline void unpack565(uint16_t c, int& r, int& g, int& b) {
    int rr = (c >> 11) & 0x1F, gg = (c >> 5) & 0x3F, bb = c & 0x1F;
    r = (rr << 3) | (rr >> 2); g = (gg << 2) | (gg >> 4); b = (bb << 3) | (bb >> 2);
}

// BC1 colour block (8 bytes): endpoints(4) + 2-bit indices(4). We always emit
// the c0 > c1 four-colour mode (opaque), matching how DXT5's colour half is used.
inline std::array<uint8_t, 8> encode_bc1_color(const uint8_t px[16][4]) {
    // Bounding box of RGB.
    int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
    for (int k = 0; k < 16; ++k)
        for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], (int)px[k][c]);
            mx[c] = std::max(mx[c], (int)px[k][c]);
        }
    // Inset the box slightly (classic 1/16 inset) to reduce banding.
    int inset[3];
    for (int c = 0; c < 3; ++c) inset[c] = (mx[c] - mn[c]) >> 4;
    for (int c = 0; c < 3; ++c) { mn[c] = clampi(mn[c] + inset[c], 0, 255); mx[c] = clampi(mx[c] - inset[c], 0, 255); }

    uint16_t c0 = pack565(mx[0], mx[1], mx[2]);
    uint16_t c1 = pack565(mn[0], mn[1], mn[2]);
    // Guarantee c0 > c1 so the decoder uses 4-colour (opaque) interpolation.
    if (c0 < c1) std::swap(c0, c1);
    if (c0 == c1) { // flat block: nudge so all indices resolve to endpoint 0
        if (c0 > 0) c1 = c0 - 1; else c0 = 1;
        if (c0 < c1) std::swap(c0, c1);
    }

    int r0, g0, b0, r1, g1, b1;
    unpack565(c0, r0, g0, b0); unpack565(c1, r1, g1, b1);
    int pal[4][3] = {
        { r0, g0, b0 },
        { r1, g1, b1 },
        { (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3 },
        { (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3 },
    };

    uint32_t bits = 0;
    for (int k = 0; k < 16; ++k) {
        int best = 0, bestd = 1 << 30;
        for (int p = 0; p < 4; ++p) {
            int dr = px[k][0] - pal[p][0], dg = px[k][1] - pal[p][1], db = px[k][2] - pal[p][2];
            int d = dr * dr + dg * dg + db * db;
            if (d < bestd) { bestd = d; best = p; }
        }
        bits |= static_cast<uint32_t>(best) << (2 * k);
    }

    std::array<uint8_t, 8> out{};
    out[0] = c0 & 0xFF; out[1] = (c0 >> 8) & 0xFF;
    out[2] = c1 & 0xFF; out[3] = (c1 >> 8) & 0xFF;
    out[4] = bits & 0xFF; out[5] = (bits >> 8) & 0xFF;
    out[6] = (bits >> 16) & 0xFF; out[7] = (bits >> 24) & 0xFF;
    return out;
}

// BC4-style interpolated alpha block (8 bytes): a0,a1 + 3-bit indices. We emit
// the a0 > a1 eight-value mode (matches bc3_alpha's a0>a1 branch).
inline std::array<uint8_t, 8> encode_bc4_alpha(const uint8_t px[16][4], int channel) {
    int amin = 255, amax = 0;
    for (int k = 0; k < 16; ++k) { int a = px[k][channel]; amin = std::min(amin, a); amax = std::max(amax, a); }

    int a0 = amax, a1 = amin;            // a0 > a1 -> 8-value interpolation
    if (a0 == a1) { if (a0 > 0) a1 = a0 - 1; else a0 = 1; } // keep a0 > a1

    int lut[8];
    lut[0] = a0; lut[1] = a1;
    for (int i = 1; i <= 6; ++i) lut[i + 1] = ((7 - i) * a0 + i * a1) / 7;

    uint64_t bits = 0;
    for (int k = 0; k < 16; ++k) {
        int a = px[k][channel];
        int best = 0, bestd = 1 << 30;
        for (int p = 0; p < 8; ++p) { int d = a - lut[p]; d = d < 0 ? -d : d; if (d < bestd) { bestd = d; best = p; } }
        bits |= static_cast<uint64_t>(best) << (3 * k);
    }

    std::array<uint8_t, 8> out{};
    out[0] = static_cast<uint8_t>(a0);
    out[1] = static_cast<uint8_t>(a1);
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
    return out;
}

// Box-downsample an RGBA image by 2 (each axis), min size 1.
inline std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, int W, int H, int& oW, int& oH) {
    oW = W > 1 ? W / 2 : 1;
    oH = H > 1 ? H / 2 : 1;
    std::vector<uint8_t> dst(static_cast<size_t>(oW) * oH * 4);
    for (int y = 0; y < oH; ++y)
        for (int x = 0; x < oW; ++x) {
            int sx = x * 2, sy = y * 2;
            for (int c = 0; c < 4; ++c) {
                int sum = 0, n = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int xx = std::min(sx + dx, W - 1), yy = std::min(sy + dy, H - 1);
                        sum += src[(static_cast<size_t>(yy) * W + xx) * 4 + c]; ++n;
                    }
                dst[(static_cast<size_t>(y) * oW + x) * 4 + c] = static_cast<uint8_t>(sum / n);
            }
        }
    return dst;
}

// Encode one mip level to the plane-separated payload the flags==0 reader expects.
inline std::vector<uint8_t> encode_mip_payload(const std::vector<uint8_t>& rgba, int W, int H, bool dxt5) {
    int bw = (W + 3) / 4, bh = (H + 3) / 4;
    int nb = bw * bh;
    std::vector<uint8_t> alpha, endp, index;
    if (dxt5) alpha.reserve(static_cast<size_t>(nb) * 8);
    endp.reserve(static_cast<size_t>(nb) * 4);
    index.reserve(static_cast<size_t>(nb) * 4);

    uint8_t px[16][4];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            gather_block(rgba.data(), W, H, bx, by, px);
            if (dxt5) {
                auto a = encode_bc4_alpha(px, 3);
                alpha.insert(alpha.end(), a.begin(), a.end());
            }
            auto c = encode_bc1_color(px);
            endp.insert(endp.end(), c.begin(), c.begin() + 4);   // offB plane
            index.insert(index.end(), c.begin() + 4, c.end());   // offC plane
        }

    std::vector<uint8_t> payload;
    payload.reserve(alpha.size() + endp.size() + index.size());
    payload.insert(payload.end(), alpha.begin(), alpha.end());
    payload.insert(payload.end(), endp.begin(), endp.end());
    payload.insert(payload.end(), index.begin(), index.end());
    return payload;
}

inline void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}

} // namespace enc

// Encode an RGBA8888 image (row-major, top-left) into an ATEX container.
// `fmt` is "DXT1" (opaque, 8B/block) or "DXT5" (RGBA, 16B/block). A full mip
// chain down to 1x1 is written; every mip uses the raw flags==0 encoding.
inline std::vector<uint8_t> encode(const uint8_t* rgba, int W, int H, const std::string& fmt) {
    using namespace enc;
    if (W <= 0 || H <= 0) throw std::runtime_error("ATEX encode: bad dimensions");
    if (W > 65535 || H > 65535) throw std::runtime_error("ATEX encode: dimensions exceed 16-bit header");

    bool dxt5;
    if (fmt == "DXT5") dxt5 = true;
    else if (fmt == "DXT1") dxt5 = false;
    else throw std::runtime_error("ATEX encode: unsupported format '" + fmt + "' (use DXT1 or DXT5)");

    std::vector<uint8_t> out;
    out.insert(out.end(), { 'A', 'T', 'E', 'X' });
    for (char c : fmt) out.push_back(static_cast<uint8_t>(c)); // 4-byte fourcc
    out.push_back(W & 0xFF); out.push_back((W >> 8) & 0xFF);
    out.push_back(H & 0xFF); out.push_back((H >> 8) & 0xFF);

    std::vector<uint8_t> cur(rgba, rgba + static_cast<size_t>(W) * H * 4);
    int cw = W, ch = H;
    while (true) {
        std::vector<uint8_t> payload = encode_mip_payload(cur, cw, ch, dxt5);
        put_u32(out, static_cast<uint32_t>(payload.size() + 8)); // dataSize incl. 8-byte record header
        put_u32(out, 0);                                         // flags == 0 (raw)
        out.insert(out.end(), payload.begin(), payload.end());
        if (cw == 1 && ch == 1) break;
        int nw, nh;
        cur = downsample(cur, cw, ch, nw, nh);
        cw = nw; ch = nh;
    }
    return out;
}

} // namespace castlemist::atex

#endif // GW2_ATEX_ENCODE_HPP
