/// @file
/// @brief Tests for the standalone "DDS " header parser.
///
/// The pitch is the interesting part: `DdsInfo::sys_mem_pitch` is what D3D11
/// wants (bytes per row of *blocks*), which is not what the DDS file's
/// `dwPitchOrLinearSize` holds for a block-compressed texture -- there it is the
/// total size of the top mip. Handing D3D the file's value produces a garbled
/// or crashing upload, so the fixtures below deliberately store a wrong value in
/// that field and check the parser ignores it.

#include "test_framework.h"

#include "castlemist/format/dds.h"

#include <cstring>
#include <dxgiformat.h>
#include <vector>

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

constexpr uint32_t kDdpfFourCC = 0x4;

/// @brief Build a minimal DDS file.
/// @param fourcc    Pixel-format fourcc ("DXT1", "DXT5", "DX10", ...).
/// @param dx10_fmt  DXGI format for the DX10 extension header (0 = no extension).
std::vector<uint8_t> make_dds(const char* fourcc, uint32_t w, uint32_t h,
                              uint32_t dx10_fmt = 0, size_t payload = 4096) {
    std::vector<uint8_t> f;
    f.insert(f.end(), {'D', 'D', 'S', ' '});
    put32(f, 124);                 // header size
    put32(f, 0x000A1007);          // flags
    put32(f, h);
    put32(f, w);
    put32(f, 0xDEADBEEF);          // dwPitchOrLinearSize: deliberately not the D3D pitch
    put32(f, 0);                   // depth
    put32(f, 1);                   // mip count
    for (int i = 0; i < 11; ++i) put32(f, 0); // reserved1

    put32(f, 32);                  // pixel format size
    put32(f, kDdpfFourCC);
    f.insert(f.end(), fourcc, fourcc + 4);
    for (int i = 0; i < 5; ++i) put32(f, 0); // bit counts / masks

    put32(f, 0x1000);              // caps
    put32(f, 0);                   // caps2
    put32(f, 0);                   // caps3
    put32(f, 0);                   // caps4
    put32(f, 0);                   // reserved2

    if (dx10_fmt) {
        put32(f, dx10_fmt);
        put32(f, 3);               // D3D10_RESOURCE_DIMENSION_TEXTURE2D
        put32(f, 0);               // misc flag
        put32(f, 1);               // array size
        put32(f, 0);               // misc flags 2
    }
    f.resize(f.size() + payload, 0xAB);
    return f;
}

} // namespace

CM_TEST(dds, rejects_non_dds_buffers) {
    std::vector<uint8_t> junk(256, 0);
    CHECK_FALSE(castlemist::dds::parse_dds(junk.data(), junk.size()).has_value());

    std::vector<uint8_t> empty;
    CHECK_FALSE(castlemist::dds::parse_dds(empty.data(), empty.size()).has_value());
    CHECK_FALSE(castlemist::dds::parse_dds(nullptr, 0).has_value());
}

CM_TEST(dds, rejects_a_truncated_header) {
    auto f = make_dds("DXT1", 64, 64);
    f.resize(60);
    CHECK_FALSE(castlemist::dds::parse_dds(f.data(), f.size()).has_value());
}

CM_TEST(dds, parses_dxt1_as_bc1) {
    auto f = make_dds("DXT1", 256, 128);
    auto info = castlemist::dds::parse_dds(f.data(), f.size());
    CHECK(info.has_value());
    CHECK_EQ(info->dxgi_format, uint32_t{DXGI_FORMAT_BC1_UNORM});
    CHECK_EQ(info->width, 256u);
    CHECK_EQ(info->height, 128u);
    CHECK_EQ(info->data_offset, size_t{128}); // 4 magic + 124 header
}

CM_TEST(dds, parses_dxt5_as_bc3) {
    auto f = make_dds("DXT5", 64, 64);
    auto info = castlemist::dds::parse_dds(f.data(), f.size());
    CHECK(info.has_value());
    CHECK_EQ(info->dxgi_format, uint32_t{DXGI_FORMAT_BC3_UNORM});
}

CM_TEST(dds, parses_the_dx10_extension_header) {
    auto f = make_dds("DX10", 32, 32, DXGI_FORMAT_BC7_UNORM);
    auto info = castlemist::dds::parse_dds(f.data(), f.size());
    CHECK(info.has_value());
    CHECK_EQ(info->dxgi_format, uint32_t{DXGI_FORMAT_BC7_UNORM});
    CHECK_EQ(info->data_offset, size_t{148}); // 128 + 20-byte DX10 header
}

// 8 bytes per 4x4 block for BC1: a 256-wide texture is 64 blocks wide = 512 B.
CM_TEST(dds, pitch_is_bytes_per_row_of_blocks_not_the_file_field) {
    auto f = make_dds("DXT1", 256, 128);
    auto info = castlemist::dds::parse_dds(f.data(), f.size());
    CHECK(info.has_value());
    CHECK_EQ(info->sys_mem_pitch, 512u);
    CHECK_NE(info->sys_mem_pitch, 0xDEADBEEFu); // the file's dwPitchOrLinearSize
}

// 16 bytes per block for BC3.
CM_TEST(dds, pitch_accounts_for_the_block_size) {
    auto f = make_dds("DXT5", 256, 128);
    auto info = castlemist::dds::parse_dds(f.data(), f.size());
    CHECK(info.has_value());
    CHECK_EQ(info->sys_mem_pitch, 1024u);
}

// A 1x1 or 2x2 BCn texture still occupies one whole 4x4 block.
CM_TEST(dds, sub_block_sized_textures_round_up_to_one_block) {
    auto f = make_dds("DXT1", 1, 1);
    auto info = castlemist::dds::parse_dds(f.data(), f.size());
    CHECK(info.has_value());
    CHECK_EQ(info->sys_mem_pitch, 8u);
}

CM_TEST(dds, rejects_an_unrecognized_fourcc) {
    auto f = make_dds("XXXX", 64, 64);
    CHECK_FALSE(castlemist::dds::parse_dds(f.data(), f.size()).has_value());
}
