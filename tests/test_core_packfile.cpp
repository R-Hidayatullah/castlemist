/// @file
/// @brief Tests for packfile header/chunk traversal and self-relative pointers.
///
/// The fixtures here are synthesised rather than copied out of Gw2.dat: the
/// interesting cases (a 64-bit-pointer file, a truncated array, a null string
/// pointer) are hard to find in real data and trivial to write by hand.

#include "test_framework.h"

#include "castlemist/core/packfile.h"

#include <cstring>

using namespace castlemist::core;

namespace {

/// Little-endian appenders for building fixtures.
void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put4cc(std::vector<uint8_t>& v, const char* s) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(s[i]));
}

/// @brief Build a packfile with the given container and chunks.
/// @param wide64 Sets pfVersion bit 2, i.e. 8-byte self-relative pointers.
std::vector<uint8_t> make_packfile(const char* container, bool wide64,
                                   const std::vector<std::pair<const char*, std::vector<uint8_t>>>& chunks) {
    std::vector<uint8_t> f;
    f.push_back('P'); f.push_back('F');
    // pfVersion: the 4 bit selects 64-bit self-relative pointers. Shipped values
    // are 5 (64-bit, e.g. cntc/AMSP) and 1 (32-bit, e.g. ABIX).
    put16(f, wide64 ? 0x0005 : 0x0001);
    put16(f, 0);                        // reserved
    put16(f, 12);                       // headerSize -> first chunk
    put4cc(f, container);

    for (const auto& [name, payload] : chunks) {
        put4cc(f, name);
        // `size` covers everything after the (name,size) pair, i.e. the
        // remaining 8 header bytes plus the payload.
        put32(f, uint32_t(8 + payload.size()));
        put16(f, 3);                    // chunk struct version
        put16(f, 16);                   // chunk headerSize
        put32(f, 0);                    // descriptorOffset
        f.insert(f.end(), payload.begin(), payload.end());
    }
    return f;
}

} // namespace

CM_TEST(packfile, rejects_non_packfiles) {
    std::vector<uint8_t> junk{'M', 'Z', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(is_packfile(junk));
    PackfileHeader h;
    CHECK_FALSE(read_header(junk, h));
    CHECK(chunks(junk).empty());

    std::vector<uint8_t> too_short{'P', 'F'};
    CHECK_FALSE(is_packfile(too_short));
}

CM_TEST(packfile, reads_the_header_and_container) {
    auto f = make_packfile("ASND", false, {{"ASND", std::vector<uint8_t>(32, 0)}});
    PackfileHeader h;
    CHECK(read_header(f, h));
    CHECK_EQ(std::string(h.container), std::string("ASND"));
    CHECK_EQ(h.header_size, uint16_t{12});
    CHECK_EQ(h.pointer_size, 4);

    CHECK(is_packfile(f, "ASND"));
    CHECK_FALSE(is_packfile(f, "MODL"));
}

// pfVersion bit 2 is the only signal for pointer width. Reading a 64-bit blob
// as 32-bit yields zero-length arrays instead of an error, which is how the
// granny animation parser silently produced 0.0s clips before it was fixed.
CM_TEST(packfile, pointer_width_comes_from_the_version_word) {
    auto narrow = make_packfile("MODL", false, {{"GEOM", std::vector<uint8_t>(16, 0)}});
    auto wide   = make_packfile("MODL", true,  {{"GEOM", std::vector<uint8_t>(16, 0)}});

    PackfileHeader h;
    CHECK(read_header(narrow, h));
    CHECK_EQ(h.pointer_size, 4);
    CHECK(read_header(wide, h));
    CHECK_EQ(h.pointer_size, 8);

    PackfileReader r;
    size_t root = 0;
    CHECK(open_chunk(wide, "GEOM", r, root));
    CHECK_EQ(r.pointer_size(), 8);
    CHECK_EQ(r.array_ptr_size(), size_t{12});
}

CM_TEST(packfile, walks_every_chunk_in_order) {
    auto f = make_packfile("cntc", true, {
        {"Main", std::vector<uint8_t>(24, 0xAA)},
        {"BIDX", std::vector<uint8_t>(8, 0xBB)},
    });

    auto cs = chunks(f);
    CHECK_EQ(cs.size(), size_t{2});
    CHECK_EQ(std::string(cs[0].name), std::string("Main"));
    CHECK_EQ(std::string(cs[1].name), std::string("BIDX"));
    CHECK_EQ(cs[0].root, size_t{12 + 16});
    CHECK_EQ(cs[1].offset, size_t{12 + 16 + 24});

    CHECK(has_chunk(f, "BIDX"));
    CHECK_FALSE(has_chunk(f, "GEOM"));
}

// A zero-size chunk would make the walk step backwards forever.
CM_TEST(packfile, stops_on_a_corrupt_chunk_size) {
    auto f = make_packfile("cscn", false, {{"CSCN", std::vector<uint8_t>(16, 0)}});
    f[12 + 4] = 0; f[12 + 5] = 0; f[12 + 6] = 0; f[12 + 7] = 0; // size := 0
    auto cs = chunks(f);
    CHECK_EQ(cs.size(), size_t{1});   // the bad chunk itself, then stop
}

CM_TEST(packfile, open_chunk_reports_a_missing_chunk) {
    auto f = make_packfile("prlt", false, {{"mfst", std::vector<uint8_t>(8, 0)}});
    PackfileReader r;
    size_t root = 0;
    CHECK_FALSE(open_chunk(f, "GEOM", r, root));
    CHECK(open_chunk(f, "mfst", r, root));
}

// array_ptr = {u32 count, ptr rel}, and `rel` is relative to *itself*
// (field + 4), not to the chunk or to the start of the array_ptr.
CM_TEST(packfile, array_ptr_is_relative_to_the_pointer_field) {
    std::vector<uint8_t> payload;
    put32(payload, 3);       // count            @ root
    put64(payload, 16);      // rel: 16 bytes on from this field
    put32(payload, 0);       // padding the pointer skips over
    put32(payload, 0);
    put32(payload, 0xCAFE);  // element 0        @ root + 4 + 16

    auto f = make_packfile("test", true, {{"Main", payload}});
    PackfileReader r;
    size_t root = 0;
    CHECK(open_chunk(f, "Main", r, root));

    size_t first = 0;
    CHECK_EQ(r.array(root, first), 3u);
    CHECK_EQ(first, root + 4 + 16);
    CHECK_EQ(r.u32(first), 0xCAFEu);
}

CM_TEST(packfile, null_pointers_yield_empty_results) {
    std::vector<uint8_t> payload;
    put64(payload, 0);   // null wchar_ptr
    put64(payload, 0);   // null fileref

    auto f = make_packfile("test", true, {{"Main", payload}});
    PackfileReader r;
    size_t root = 0;
    CHECK(open_chunk(f, "Main", r, root));

    CHECK_EQ(r.deref(root), size_t{0});
    CHECK(r.wstring(root).empty());
    CHECK_EQ(r.fileref(root + 8), 0u);
}

CM_TEST(packfile, reads_a_utf16_string_through_a_pointer) {
    std::vector<uint8_t> payload;
    put64(payload, 8);                       // pointer to +8 from itself
    const wchar_t kText[] = L"Fractal";
    for (wchar_t c : std::wstring(kText)) put16(payload, uint16_t(c));
    put16(payload, 0);                       // NUL terminator

    auto f = make_packfile("test", true, {{"Main", payload}});
    PackfileReader r;
    size_t root = 0;
    CHECK(open_chunk(f, "Main", r, root));
    CHECK_EQ(r.wstring(root), std::wstring(L"Fractal"));
}

// An unterminated string must be cut off by max_chars, not run to the end of
// an 8 MB entry.
CM_TEST(packfile, string_reads_respect_max_chars) {
    std::vector<uint8_t> payload;
    put64(payload, 8);
    for (int i = 0; i < 100; ++i) put16(payload, uint16_t(L'x'));  // no NUL

    auto f = make_packfile("test", true, {{"Main", payload}});
    PackfileReader r;
    size_t root = 0;
    CHECK(open_chunk(f, "Main", r, root));
    CHECK_EQ(r.wstring(root, 10).size(), size_t{10});
}

CM_TEST(packfile, fileref_resolves_the_two_half_encoding) {
    std::vector<uint8_t> payload;
    put64(payload, 8);       // pointer to the {lo,hi} pair
    put16(payload, 0x0200);  // lo
    put16(payload, 0x0101);  // hi

    auto f = make_packfile("test", true, {{"Main", payload}});
    PackfileReader r;
    size_t root = 0;
    CHECK(open_chunk(f, "Main", r, root));
    CHECK_EQ(r.fileref(root), decode_fileref(0x0200, 0x0101));
}

CM_TEST(packfile, chunk_listing_names_every_chunk) {
    auto f = make_packfile("cscn", false, {
        {"CSCN", std::vector<uint8_t>(8, 0)},
        {"TEXT", std::vector<uint8_t>(4, 0)},
    });
    std::wstring s = chunk_listing(f);
    CHECK(s.find(L"CSCN") != std::wstring::npos);
    CHECK(s.find(L"TEXT") != std::wstring::npos);
    CHECK(s.find(L"v3") != std::wstring::npos);
}

CM_TEST(packfile, language_names_cover_the_game_languages) {
    CHECK_EQ(std::wstring(language_name(0)), std::wstring(L"English"));
    CHECK_EQ(std::wstring(language_name(5)), std::wstring(L"Chinese"));
    CHECK_EQ(std::wstring(language_name(99)), std::wstring(L"?"));
}
