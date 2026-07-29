/// @file
/// @brief Tests for `strs` string-table framing.
///
/// A strs file has no record count: records run back to back and each declares
/// its own length. That makes it trivially easy for a corrupt or truncated
/// entry to walk off the end, so most of what is checked here is where the
/// parser *stops*.

#include "test_framework.h"

#include "castlemist/format/strs_view.h"

#include <string>
#include <vector>

using namespace castlemist::strs;

namespace {

/// @brief Append a raw UTF-16 record (baseChar 0, rangeBits 16).
void put_raw(std::vector<uint8_t>& f, const std::wstring& text) {
    uint16_t len = uint16_t(6 + (text.size() + 1) * 2);
    f.push_back(uint8_t(len)); f.push_back(uint8_t(len >> 8));
    f.push_back(0); f.push_back(0);   // baseChar = 0
    f.push_back(16);                  // rangeBits = 16 -> raw UTF-16
    f.push_back(0);                   // padding
    for (wchar_t c : text) { f.push_back(uint8_t(c)); f.push_back(uint8_t(c >> 8)); }
    f.push_back(0); f.push_back(0);   // NUL terminator
}

/// @brief Append a bit-packed (RC4-locked) record.
void put_packed(std::vector<uint8_t>& f, uint16_t base_char, uint8_t range_bits, size_t payload) {
    uint16_t len = uint16_t(6 + payload);
    f.push_back(uint8_t(len)); f.push_back(uint8_t(len >> 8));
    f.push_back(uint8_t(base_char)); f.push_back(uint8_t(base_char >> 8));
    f.push_back(range_bits);
    f.push_back(0);
    f.insert(f.end(), payload, 0x5A);
}

std::vector<uint8_t> make_strs() {
    std::vector<uint8_t> f{'s', 't', 'r', 's'};
    return f;
}

} // namespace

CM_TEST(strs, recognises_the_container_magic) {
    const uint8_t good[] = {'s', 't', 'r', 's', 0};
    const uint8_t bad[] = {'P', 'F', 0, 0, 0};
    CHECK(is_strs(good, sizeof good));
    CHECK_FALSE(is_strs(bad, sizeof bad));
    CHECK_FALSE(is_strs(good, 3));   // too short to tell
    CHECK_FALSE(is_strs(nullptr, 0));
}

CM_TEST(strs, non_strs_input_yields_no_records) {
    const uint8_t bad[] = {'P', 'F', 0, 0, 0, 0, 0, 0};
    CHECK(records(bad, sizeof bad).empty());
}

CM_TEST(strs, reads_raw_utf16_records_verbatim) {
    auto f = make_strs();
    put_raw(f, L"Survival Survivor");
    put_raw(f, L"Fractals of the Mists");

    auto recs = records(f.data(), f.size());
    CHECK_EQ(recs.size(), size_t{2});
    CHECK_EQ(recs[0].index, 0u);
    CHECK_EQ(recs[0].text, std::wstring(L"Survival Survivor"));
    CHECK_FALSE(recs[0].packed);
    CHECK_FALSE(recs[0].locked);
    CHECK_EQ(recs[1].index, 1u);
    CHECK_EQ(recs[1].text, std::wstring(L"Fractals of the Mists"));
}

CM_TEST(strs, an_empty_record_decodes_to_an_empty_string) {
    auto f = make_strs();
    put_raw(f, L"");
    auto recs = records(f.data(), f.size());
    CHECK_EQ(recs.size(), size_t{1});
    CHECK(recs[0].text.empty());
}

// baseChar != 0 (or rangeBits != 16) means the record is bit-packed and RC4
// encrypted; without a captured key it can only be described, never read.
CM_TEST(strs, packed_records_are_flagged_locked_not_mis_decoded) {
    auto f = make_strs();
    put_packed(f, 0x0041, 5, 12);

    auto recs = records(f.data(), f.size());
    CHECK_EQ(recs.size(), size_t{1});
    CHECK(recs[0].packed);
    CHECK(recs[0].locked);
    CHECK(recs[0].text.empty());
    CHECK_EQ(recs[0].baseChar, uint16_t{0x0041});
    CHECK_EQ(recs[0].rangeBits, uint8_t{5});
}

CM_TEST(strs, mixed_files_keep_record_indices_contiguous) {
    auto f = make_strs();
    put_raw(f, L"one");
    put_packed(f, 0x20, 6, 8);
    put_raw(f, L"three");

    auto recs = records(f.data(), f.size());
    CHECK_EQ(recs.size(), size_t{3});
    CHECK_EQ(recs[0].index, 0u);
    CHECK_EQ(recs[1].index, 1u);
    CHECK_EQ(recs[2].index, 2u);
    CHECK_EQ(recs[2].text, std::wstring(L"three"));
}

// A length that runs past the buffer must stop the walk, not read past the end.
CM_TEST(strs, stops_at_a_record_that_overruns_the_buffer) {
    auto f = make_strs();
    put_raw(f, L"good");
    size_t good_end = f.size();
    f.push_back(0xFF); f.push_back(0xFF);       // recLen = 65535
    f.push_back(0); f.push_back(0);
    f.push_back(16); f.push_back(0);

    auto recs = records(f.data(), f.size());
    CHECK_EQ(recs.size(), size_t{1});
    CHECK_EQ(recs[0].recLen, uint16_t(good_end - 4));
}

// recLen < 6 cannot even hold the header; treating it as valid would loop forever.
CM_TEST(strs, stops_at_an_impossibly_short_record) {
    auto f = make_strs();
    f.push_back(2); f.push_back(0);
    f.push_back(0); f.push_back(0);
    f.push_back(16); f.push_back(0);
    CHECK(records(f.data(), f.size()).empty());
}

CM_TEST(strs, a_trailing_partial_header_is_ignored) {
    auto f = make_strs();
    put_raw(f, L"complete");
    f.push_back(0x10); f.push_back(0x00); f.push_back(0x00); // 3 stray bytes

    auto recs = records(f.data(), f.size());
    CHECK_EQ(recs.size(), size_t{1});
}

CM_TEST(strs, decode_renders_a_listing_and_names_locked_records) {
    auto f = make_strs();
    put_raw(f, L"Readable");
    put_packed(f, 0x30, 5, 4);

    std::wstring text = decode(f.data(), f.size());
    CHECK(text.find(L"Readable") != std::wstring::npos);
    CHECK(text.find(L"packed") != std::wstring::npos);

    const uint8_t junk[] = {'n', 'o', 'p', 'e'};
    CHECK(decode(junk, sizeof junk).find(L"not a strs") != std::wstring::npos);
}
