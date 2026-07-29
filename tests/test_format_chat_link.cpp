/// @file
/// @brief Tests for the `&[base64]` chat-link decoder.
///
/// Links here are built from their raw payload bytes rather than pasted from a
/// wiki, so the test says exactly which byte drives which field. The variable
/// length item link is the one worth care: its flag byte decides which optional
/// fields follow, so every absent field shifts the offsets of the ones after it.

#include "test_framework.h"

#include "castlemist/format/chat_link.h"

#include <string>
#include <vector>

using namespace castlemist::chat;

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Encode raw payload bytes the way the game does (standard alphabet, padded).
std::string b64(const std::vector<uint8_t>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i += 3) {
        uint32_t n = uint32_t(v[i]) << 16;
        if (i + 1 < v.size()) n |= uint32_t(v[i + 1]) << 8;
        if (i + 2 < v.size()) n |= v[i + 2];
        s.push_back(kB64[(n >> 18) & 63]);
        s.push_back(kB64[(n >> 12) & 63]);
        s.push_back(i + 1 < v.size() ? kB64[(n >> 6) & 63] : '=');
        s.push_back(i + 2 < v.size() ? kB64[n & 63] : '=');
    }
    return s;
}

std::string link(const std::vector<uint8_t>& payload) { return "[&" + b64(payload) + "]"; }

/// @brief Find a decoded field by label.
const Field* field(const Decoded& d, const std::string& label) {
    for (const Field& f : d.fields)
        if (f.label == label) return &f;
    return nullptr;
}

} // namespace

CM_TEST(base64, decodes_standard_input) {
    std::vector<uint8_t> v = base64_decode("TWFu");
    CHECK_EQ(v.size(), size_t{3});
    CHECK_EQ(v[0], uint8_t{'M'});
    CHECK_EQ(v[1], uint8_t{'a'});
    CHECK_EQ(v[2], uint8_t{'n'});
}

CM_TEST(base64, tolerates_missing_padding_and_whitespace) {
    CHECK_EQ(base64_decode("TWE=").size(), size_t{2});
    CHECK_EQ(base64_decode("TWE").size(), size_t{2});
    CHECK_EQ(base64_decode("T W\tF u\n").size(), size_t{3});
}

CM_TEST(base64, empty_and_garbage_input_yield_nothing) {
    CHECK(base64_decode("").empty());
    CHECK(base64_decode("!!!!").empty());
}

CM_TEST(chatlink, accepts_all_three_wrapper_forms) {
    std::vector<uint8_t> skill{0x06, 0x2A, 0x00, 0x00};
    for (const std::string& text : {link(skill), "&" + b64(skill), b64(skill)}) {
        Decoded d = decode(text);
        CHECK(d.ok);
        CHECK_EQ(d.primary_id, 42u);
    }
}

CM_TEST(chatlink, reports_an_error_for_empty_or_unparseable_input) {
    Decoded d = decode("");
    CHECK_FALSE(d.ok);
    CHECK_FALSE(d.error.empty());

    Decoded g = decode("[&not-base64!!]");
    CHECK_FALSE(g.ok);
}

CM_TEST(chatlink, decodes_a_coin_link_into_gold_silver_copper) {
    // 0x01 + u32 little-endian copper. 12,345,678 c = 1234 g 56 s 78 c.
    std::vector<uint8_t> p{0x01, 0x4E, 0x61, 0xBC, 0x00};
    Decoded d = decode(link(p));
    CHECK(d.ok);
    CHECK_EQ(d.type, std::string("Coin"));
    const Field* gold = field(d, "Gold");
    CHECK(gold != nullptr);
    CHECK_EQ(gold->value, uint64_t{1234});
    CHECK_EQ(field(d, "Silver")->value, uint64_t{56});
    CHECK_EQ(field(d, "Copper")->value, uint64_t{78});
}

CM_TEST(chatlink, decodes_the_simple_three_byte_id_family) {
    struct Case { uint8_t header; const char* type; const char* label; };
    const Case cases[] = {
        {0x06, "Skill", "Skill id"},
        {0x07, "Trait", "Trait id"},
        {0x09, "Recipe", "Recipe id"},
        {0x0A, "Wardrobe skin", "Skin id"},
        {0x0B, "Outfit", "Outfit id"},
        {0x0E, "Achievement", "Achievement id"},
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> p{c.header, 0x34, 0x12, 0x00}; // id = 0x1234
        Decoded d = decode(link(p));
        CHECK(d.ok);
        CHECK_EQ(d.type, std::string(c.type));
        CHECK_EQ(d.primary_id, 0x1234u);
        CHECK_EQ(d.primary_label, std::string(c.label));
    }
}

CM_TEST(chatlink, decodes_a_plain_item_link) {
    // 0x02, quantity, 3-byte item id, flags = 0 (no optional fields).
    std::vector<uint8_t> p{0x02, 0x19, 0xD3, 0x12, 0x00, 0x00};
    Decoded d = decode(link(p));
    CHECK(d.ok);
    CHECK_EQ(d.type, std::string("Item"));
    CHECK_EQ(field(d, "Quantity")->value, uint64_t{25});
    CHECK_EQ(d.primary_id, 0x12D3u);
    CHECK(field(d, "Skin id") == nullptr);
}

// The flag byte is a presence bitmap, and each absent field shifts every later
// one: reading upgrade 2 at a fixed offset would silently return skin bytes.
CM_TEST(chatlink, item_link_optional_fields_shift_with_the_flag_byte) {
    // flags 0x80 = skin only. Skin id occupies a full 4-byte slot.
    std::vector<uint8_t> skin_only{0x02, 0x01, 0x01, 0x00, 0x00, 0x80,
                                   0xAA, 0x00, 0x00, 0x00};
    Decoded a = decode(link(skin_only));
    CHECK(a.ok);
    CHECK(field(a, "Skin id") != nullptr);
    CHECK_EQ(field(a, "Skin id")->value, uint64_t{0xAA});
    CHECK(field(a, "Upgrade 1 id") == nullptr);

    // flags 0x40 = upgrade 1 only: the same bytes now mean something else.
    std::vector<uint8_t> upg_only{0x02, 0x01, 0x01, 0x00, 0x00, 0x40,
                                  0xAA, 0x00, 0x00, 0x00};
    Decoded b = decode(link(upg_only));
    CHECK(b.ok);
    CHECK(field(b, "Skin id") == nullptr);
    CHECK_EQ(field(b, "Upgrade 1 id")->value, uint64_t{0xAA});

    // Both, in flag order: skin first, then upgrade 1.
    std::vector<uint8_t> both{0x02, 0x01, 0x01, 0x00, 0x00, 0xC0,
                              0xAA, 0x00, 0x00, 0x00,
                              0xBB, 0x00, 0x00, 0x00};
    Decoded c = decode(link(both));
    CHECK(c.ok);
    CHECK_EQ(field(c, "Skin id")->value, uint64_t{0xAA});
    CHECK_EQ(field(c, "Upgrade 1 id")->value, uint64_t{0xBB});
}

CM_TEST(chatlink, truncated_payloads_fail_instead_of_reading_past_the_end) {
    Decoded d = decode(link({0x06, 0x01})); // skill link missing a byte of its id
    CHECK_FALSE(d.ok);
    CHECK_FALSE(d.error.empty());

    Decoded item = decode(link({0x02, 0x01})); // item link missing id and flags
    CHECK_FALSE(item.ok);
}

CM_TEST(chatlink, decodes_a_wvw_objective_link) {
    std::vector<uint8_t> p{0x0C, 0x0A, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00};
    Decoded d = decode(link(p));
    CHECK(d.ok);
    CHECK_EQ(d.primary_id, 10u);
    CHECK_EQ(field(d, "Map id")->value, uint64_t{0x26});
}

CM_TEST(chatlink, unknown_headers_are_reported_not_guessed) {
    Decoded d = decode(link({0x7F, 0x00, 0x00, 0x00}));
    CHECK_FALSE(d.ok);
    CHECK_EQ(d.header, uint8_t{0x7F});
}

CM_TEST(chatlink, report_mentions_the_type_and_the_primary_id) {
    Decoded d = decode(link({0x06, 0x2A, 0x00, 0x00}));
    std::string r = to_report(d);
    CHECK(r.find("Skill") != std::string::npos);
    CHECK(r.find("42") != std::string::npos);
}
