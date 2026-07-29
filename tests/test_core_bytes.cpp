/// @file
/// @brief Tests for castlemist::core::Bytes and the filename-reference decoder.

#include "test_framework.h"

#include "castlemist/core/bytes.h"

using castlemist::core::Bytes;

namespace {
const uint8_t kRaw[] = {0x78, 0x56, 0x34, 0x12, 0xFF, 0xFF, 0xFF, 0xFF};
}

CM_TEST(bytes, reads_little_endian) {
    Bytes b(kRaw, sizeof kRaw);
    CHECK_EQ(b.u8(0), 0x78u);
    CHECK_EQ(b.u16(0), 0x5678u);
    CHECK_EQ(b.u32(0), 0x12345678u);
    CHECK_EQ(b.u64(0), 0xFFFFFFFF12345678ull);
}

CM_TEST(bytes, signed_reads_keep_the_sign) {
    const uint8_t neg[] = {0xFF, 0xFF, 0xFF, 0xFF};
    Bytes b(neg, sizeof neg);
    CHECK_EQ(b.i32(0), -1);
}

// Out-of-range reads must saturate to zero rather than fault: entry_extractor
// walks self-relative pointers that routinely land outside a truncated chunk.
CM_TEST(bytes, out_of_range_reads_return_zero) {
    Bytes b(kRaw, sizeof kRaw);
    CHECK_EQ(b.u32(5), 0u);          // straddles the end
    CHECK_EQ(b.u32(1000), 0u);       // far past the end
    CHECK_EQ(b.u16(sizeof kRaw), 0u);// exactly at the end
    CHECK_EQ(b.u8(sizeof kRaw), 0u);
}

CM_TEST(bytes, empty_reader_is_safe) {
    Bytes b;
    CHECK(b.empty());
    CHECK_EQ(b.size(), size_t{0});
    CHECK_EQ(b.u32(0), 0u);
    CHECK_FALSE(b.magic("PF\0\0"));
}

CM_TEST(bytes, has_does_not_overflow_on_huge_counts) {
    Bytes b(kRaw, sizeof kRaw);
    CHECK(b.has(0, 8));
    CHECK_FALSE(b.has(0, 9));
    CHECK_FALSE(b.has(4, SIZE_MAX));   // count + offset would wrap
    CHECK_FALSE(b.has(SIZE_MAX, 1));
}

CM_TEST(bytes, magic_matches_a_fourcc) {
    const uint8_t pf[] = {'P', 'F', 0x0A, 0x00};
    Bytes b(pf, sizeof pf);
    CHECK(b.match(0, "PF", 2));
    CHECK_FALSE(b.match(0, "FP", 2));
    CHECK_FALSE(b.match(3, "PF", 2));   // would read past the end
}

CM_TEST(fileref, decodes_the_two_half_form) {
    // fileId = 0xFF00 * hi + lo - 0xFF00FF, with both halves >= 0x100.
    CHECK_EQ(castlemist::core::decode_fileref(0x100, 0x100), 0xFF00u * 0x100 + 0x100 - 0xFF00FFu);
    CHECK_EQ(castlemist::core::decode_fileref(0x1FF, 0x101), 0xFF00u * 0x101 + 0x1FF - 0xFF00FFu);
}

// A raw dword read of the same bytes gives a plausible but wrong id, so the
// "both halves >= 0x100" guard is what separates a real reference from noise.
CM_TEST(fileref, rejects_halves_below_0x100) {
    CHECK_EQ(castlemist::core::decode_fileref(0x00, 0x100), 0u);
    CHECK_EQ(castlemist::core::decode_fileref(0x100, 0x00), 0u);
    CHECK_EQ(castlemist::core::decode_fileref(0xFF, 0xFF), 0u);
}
