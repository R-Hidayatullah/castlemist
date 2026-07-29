/// @file
/// @brief Tests for the text conversion, sniffing and formatting helpers.

#include "test_framework.h"

#include "castlemist/core/text.h"

using namespace castlemist::core;

namespace {
std::vector<uint8_t> bytes_of(const char* s) {
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(s),
                                reinterpret_cast<const uint8_t*>(s) + std::strlen(s));
}
} // namespace

CM_TEST(text, ansi_round_trip_for_ascii) {
    CHECK_EQ(to_ansi(L"Guild Wars 2"), std::string("Guild Wars 2"));
    CHECK_EQ(from_ascii("Guild Wars 2"), std::wstring(L"Guild Wars 2"));
    CHECK(to_ansi(L"").empty());
    CHECK(from_ascii("").empty());
}

CM_TEST(text, decodes_utf8) {
    // "Löwe" in UTF-8: the two-byte sequence must survive as one wide char.
    std::vector<uint8_t> utf8{'L', 0xC3, 0xB6, 'w', 'e'};
    std::wstring w = bytes_to_wide(utf8);
    CHECK_EQ(w.size(), size_t{4});
    CHECK_EQ(uint32_t(w[1]), 0xF6u);
}

// Invalid UTF-8 must fall back to the ANSI code page instead of returning
// nothing; ANSI can never fail, which is exactly why it is tried second.
CM_TEST(text, falls_back_to_ansi_on_invalid_utf8) {
    std::vector<uint8_t> bad{'a', 0xFF, 0xFE, 'b'};
    CHECK_FALSE(bytes_to_wide(bad).empty());
}

CM_TEST(text, wide_conversion_respects_the_byte_cap) {
    std::vector<uint8_t> big(10000, 'x');
    CHECK_EQ(bytes_to_wide(big, 100).size(), size_t{100});
}

CM_TEST(text, empty_input_yields_empty_output) {
    CHECK(bytes_to_wide({}).empty());
    CHECK_FALSE(looks_like_text({}));
}

CM_TEST(text, recognises_readable_buffers) {
    CHECK(looks_like_text(bytes_of("The Eternal Alchemy\r\n\tline two\n")));
    CHECK(looks_like_text(bytes_of("high bytes \xC3\xA9 count as text")));
}

CM_TEST(text, rejects_binary_buffers) {
    std::vector<uint8_t> blob(512);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i % 17); // control bytes
    CHECK_FALSE(looks_like_text(blob));
}

// The threshold is 95%, so a handful of NULs in an otherwise readable buffer
// still previews as text (real GW2 entries have padding).
CM_TEST(text, tolerates_a_few_stray_control_bytes) {
    std::vector<uint8_t> mostly(100, 'A');
    mostly[10] = 0x00;
    mostly[20] = 0x01;
    CHECK(looks_like_text(mostly));
    for (int i = 0; i < 10; ++i) mostly[i] = 0x00;
    CHECK_FALSE(looks_like_text(mostly));
}

CM_TEST(text, formats_durations_as_mmss) {
    CHECK_EQ(format_mmss(0), std::wstring(L"0:00"));
    CHECK_EQ(format_mmss(9.4), std::wstring(L"0:09"));
    CHECK_EQ(format_mmss(59.6), std::wstring(L"1:00"));
    CHECK_EQ(format_mmss(125), std::wstring(L"2:05"));
    CHECK_EQ(format_mmss(-3), std::wstring(L"0:00"));   // never negative
}

CM_TEST(text, names_channel_layouts) {
    CHECK_EQ(std::wstring(channel_layout_name(1)), std::wstring(L"Mono"));
    CHECK_EQ(std::wstring(channel_layout_name(2)), std::wstring(L"Stereo"));
    CHECK_EQ(std::wstring(channel_layout_name(6)), std::wstring(L"5.1"));
    CHECK_EQ(std::wstring(channel_layout_name(0)), std::wstring(L"?"));
    CHECK_EQ(std::wstring(channel_layout_name(8)), std::wstring(L"multi-channel"));
}

CM_TEST(text, formats_byte_counts) {
    CHECK_EQ(format_bytes(0), std::wstring(L"0 B"));
    CHECK_EQ(format_bytes(1023), std::wstring(L"1023 B"));
    CHECK_EQ(format_bytes(1024), std::wstring(L"1.0 KB"));
    CHECK_EQ(format_bytes(1024ull * 1024), std::wstring(L"1.00 MB"));
    CHECK_EQ(format_bytes(1024ull * 1024 * 1024), std::wstring(L"1.00 GB"));
}
