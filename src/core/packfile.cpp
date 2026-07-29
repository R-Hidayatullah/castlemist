/// @file
/// @brief Implementation of the packfile traversal helpers.

#include "castlemist/core/packfile.h"

#include <cstdio>
#include <cstring>

namespace castlemist::core {

std::wstring PackfileReader::wstring(size_t offset, size_t max_chars) const {
    size_t at = deref(offset);
    if (!at) return {};
    std::wstring s;
    for (size_t p = at; bytes_.has(p, 2) && s.size() < max_chars; p += 2) {
        wchar_t ch = static_cast<wchar_t>(bytes_.u16(p));
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

uint32_t PackfileReader::fileref(size_t offset) const noexcept {
    size_t at = deref(offset);
    if (!at) return 0;
    return decode_fileref(bytes_.u16(at), bytes_.u16(at + 2));
}

bool is_packfile(const std::vector<uint8_t>& blob) noexcept {
    return blob.size() >= 12 && blob[0] == 'P' && blob[1] == 'F';
}

bool is_packfile(const std::vector<uint8_t>& blob, const char* container) noexcept {
    return is_packfile(blob) && std::memcmp(blob.data() + 8, container, 4) == 0;
}

bool read_header(const std::vector<uint8_t>& blob, PackfileHeader& out) noexcept {
    if (!is_packfile(blob)) return false;
    const Bytes b(blob);
    out.version = static_cast<uint16_t>(b.u16(2));
    out.header_size = static_cast<uint16_t>(b.u16(6));
    std::memcpy(out.container, blob.data() + 8, 4);
    out.container[4] = '\0';
    out.pointer_size = (out.version & 4) ? 8 : 4;
    return true;
}

std::vector<PackfileChunk> chunks(const std::vector<uint8_t>& blob, size_t max_chunks) {
    std::vector<PackfileChunk> out;
    PackfileHeader h;
    if (!read_header(blob, h)) return out;

    const Bytes b(blob);
    size_t pos = h.header_size;
    while (b.has(pos, 16) && out.size() < max_chunks) {
        PackfileChunk c;
        std::memcpy(c.name, blob.data() + pos, 4);
        c.name[4] = '\0';
        c.size = b.u32(pos + 4);
        c.version = static_cast<uint16_t>(b.u16(pos + 8));
        c.offset = pos;
        c.root = pos + 16;
        out.push_back(c);

        // `size` counts the bytes after the (name,size) pair, so a chunk cannot
        // legitimately be smaller than the 8 header bytes that follow it. A
        // smaller value is corruption: stepping over it lands mid-chunk and the
        // walk then invents chunks out of whatever bytes it finds, so stop.
        if (c.size < 8) break;
        pos = pos + 8 + c.size;
    }
    return out;
}

bool has_chunk(const std::vector<uint8_t>& blob, const char* name) {
    for (const PackfileChunk& c : chunks(blob)) {
        if (std::memcmp(c.name, name, 4) == 0) return true;
    }
    return false;
}

bool open_chunk(const std::vector<uint8_t>& blob, const char* name,
                PackfileReader& out, size_t& root_out) {
    PackfileHeader h;
    if (!read_header(blob, h)) return false;
    for (const PackfileChunk& c : chunks(blob)) {
        if (std::memcmp(c.name, name, 4) == 0) {
            out = PackfileReader(blob, h.pointer_size);
            root_out = c.root;
            return true;
        }
    }
    return false;
}

std::wstring chunk_listing(const std::vector<uint8_t>& blob) {
    std::wstring s;
    for (const PackfileChunk& c : chunks(blob)) {
        wchar_t line[96];
        swprintf(line, 96, L"  %hs  v%u  (%u bytes)\r\n", c.name, c.version, c.size);
        s += line;
    }
    return s;
}

const wchar_t* language_name(uint32_t language) noexcept {
    switch (language) { // GW2 GameLanguage enum
    case 0: return L"English";
    case 1: return L"Korean";
    case 2: return L"French";
    case 3: return L"German";
    case 4: return L"Spanish";
    case 5: return L"Chinese";
    default: return L"?";
    }
}

} // namespace castlemist::core
