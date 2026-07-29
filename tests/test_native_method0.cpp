/// @file
/// @brief Round-trip tests for the dat's "method 0" codec (Huffman + LZ77).
///
/// Method 0 is ArenaNet's own codec, recovered from CmpDecompress_Method0_Inflate
/// in the client. There is no reference implementation to diff against, so the
/// encoder is the test oracle: anything the encoder produces must survive the
/// decoder unchanged, and the decoder is the half that was validated against
/// every entry in a real Gw2.dat.
///
/// The cases below cover the shapes that historically broke it -- inputs with a
/// single distinct symbol (degenerate Huffman table), long runs that stress the
/// LZ77 match window, and payloads that cross the 0xFFFC-byte CRC framing
/// stride, where a checksum word lands mid-bitstream.

#include "test_framework.h"

#include "castlemist/native/cmp_compress_method0.hpp"
#include "castlemist/native/cmp_decompress_method0.hpp"

#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace {

/// @brief Compress @p input as an on-disk entry and decompress it back.
std::vector<uint8_t> round_trip(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> entry = castlemist::cmp::compress_entry(input);
    return castlemist::cmp::decompress_entry(entry);
}

/// @brief Deterministic pseudo-random bytes; a fixed seed keeps failures reproducible.
std::vector<uint8_t> noise(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> out(n);
    for (uint8_t& b : out) b = static_cast<uint8_t>(rng() & 0xFF);
    return out;
}

} // namespace

CM_TEST(method0, round_trip_text) {
    std::vector<uint8_t> in;
    const char* line = "the quick brown fox jumps over the lazy dog. ";
    for (int i = 0; i < 64; ++i)
        for (const char* p = line; *p; ++p) in.push_back(static_cast<uint8_t>(*p));

    CHECK_EQ(round_trip(in), in);
}

CM_TEST(method0, round_trip_single_symbol) {
    // One distinct symbol means every code length is equal, which is exactly the
    // case where the code-length table degenerates. The decoder reads such a
    // symbol in zero bits; get this wrong and the output is empty, not corrupt.
    std::vector<uint8_t> in(4096, 0x00);
    CHECK_EQ(round_trip(in), in);
}

CM_TEST(method0, round_trip_incompressible) {
    // Random data has no matches to find: the encoder falls back to literals
    // and the entry ends up larger than its input. That is allowed.
    std::vector<uint8_t> in = noise(8192, 0xC0FFEE);
    CHECK_EQ(round_trip(in), in);
}

CM_TEST(method0, round_trip_long_runs) {
    // Runs longer than the maximum match length force the matcher to emit
    // back-to-back copies whose distances stay inside the 40 KB window.
    std::vector<uint8_t> in;
    for (int block = 0; block < 16; ++block)
        in.insert(in.end(), 2048, static_cast<uint8_t>(block));

    CHECK_EQ(round_trip(in), in);
}

CM_TEST(method0, round_trip_across_crc_stride) {
    // The dat interleaves a CRC32C word every 0xFFFC content bytes. A payload
    // this size puts at least one checksum in the middle of the bitstream, so
    // the framing has to be stripped before the Huffman decoder ever sees it.
    std::vector<uint8_t> in = noise(0x30000, 0x5EED);
    std::vector<uint8_t> entry = castlemist::cmp::compress_entry(in);

    CHECK(entry.size() > 0xFFFC);
    CHECK_EQ(castlemist::cmp::decompress_entry(entry), in);
}

CM_TEST(method0, empty_input) {
    std::vector<uint8_t> in;
    CHECK_EQ(round_trip(in), in);
}

CM_TEST(method0, framing_is_the_inverse_of_stripping) {
    std::vector<uint8_t> content = noise(0x25000, 0xABCD);
    std::vector<uint8_t> framed = castlemist::cmp::add_crc32_framing(content);

    // One CRC word per full stride plus one trailing word.
    CHECK(framed.size() > content.size());
    CHECK_EQ(castlemist::cmp::strip_crc32(framed), content);
}

CM_TEST(method0, truncated_entry_is_rejected) {
    // A header-sized read is the first thing decompress_entry does; short input
    // must raise rather than walk off the end of the buffer.
    std::vector<uint8_t> tiny{0x08, 0x00, 0x00};
    bool threw = false;
    try {
        castlemist::cmp::decompress_entry(tiny);
    } catch (const castlemist::cmp::decode_error&) {
        threw = true;
    }
    CHECK(threw);
}
