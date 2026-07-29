#include "castlemist/format/strs_view.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace castlemist::strs {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

} // namespace

bool is_strs(const uint8_t* data, size_t size) {
    return size >= 4 && std::memcmp(data, "strs", 4) == 0;
}

std::vector<Record> records(const uint8_t* data, size_t size) {
    std::vector<Record> out;
    if (!is_strs(data, size)) return out;
    size_t off = 4;
    uint32_t index = 0;
    // Records run back-to-back with no count field: keep reading while at least a
    // 6-byte header remains and the declared length stays in bounds.
    while (off + 6 <= size) {
        uint16_t rec_len = rd16(data + off);
        if (rec_len < 6 || off + rec_len > size) break;
        Record r;
        r.index = index;
        r.baseChar = rd16(data + off + 2);
        r.rangeBits = data[off + 4];
        r.recLen = rec_len;
        const uint8_t* payload = data + off + 6;
        size_t payload_len = static_cast<size_t>(rec_len) - 6;
        if (r.baseChar == 0 && r.rangeBits == 16 && (payload_len % 2) == 0) {
            r.text.assign(reinterpret_cast<const wchar_t*>(payload), payload_len / 2);
            if (!r.text.empty() && r.text.back() == L'\0') r.text.pop_back();
        } else {
            r.packed = true;
            r.locked = true;   // castlemist::skeys::records() may still decrypt it
        }
        out.push_back(std::move(r));
        off += rec_len;
        ++index;
    }
    return out;
}

std::wstring decode(const uint8_t* data, size_t size) {
    if (!is_strs(data, size)) {
        return L"(not a strs container)";
    }

    std::wstring out;
    wchar_t line[128];

    size_t off = 4;
    uint32_t index = 0;
    uint32_t raw_count = 0;
    uint32_t packed_count = 0;

    // Records run back-to-back with no count field: keep reading while at least
    // a 6-byte header remains and the declared length stays in bounds.
    while (off + 6 <= size) {
        uint16_t rec_len = rd16(data + off);
        if (rec_len < 6 || off + rec_len > size) {
            break;
        }
        uint16_t base_char = rd16(data + off + 2);
        uint8_t range_bits = data[off + 4];
        const uint8_t* payload = data + off + 6;
        size_t payload_len = static_cast<size_t>(rec_len) - 6;

        if (base_char == 0 && range_bits == 16 && (payload_len % 2) == 0) {
            // Raw UTF-16LE fast path -- payload IS the string, verbatim.
            std::wstring text(reinterpret_cast<const wchar_t*>(payload), payload_len / 2);
            // Trim a single trailing NUL if present.
            if (!text.empty() && text.back() == L'\0') {
                text.pop_back();
            }
            swprintf(line, 128, L"#%u  ", index);
            out += line;
            out += text;
            out += L"\r\n";
            ++raw_count;
        } else {
            swprintf(line, 128,
                     L"#%u  [packed base=0x%04X rangeBits=%u len=%u]  "
                     L"<encrypted -- RC4 key unavailable>\r\n",
                     index, base_char, range_bits, rec_len);
            out += line;
            ++packed_count;
        }

        off += rec_len;
        ++index;
    }

    wchar_t header[256];
    swprintf(header, 256, L"=== strs string table ===\r\n%u records (%u raw UTF-16, %u packed/encrypted)\r\n\r\n",
             index, raw_count, packed_count);
    return std::wstring(header) + out;
}

} // namespace castlemist::strs
