#ifndef GW2_STRS_VIEW_H
#define GW2_STRS_VIEW_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::strs {

/// True if `data` starts with the "strs" (TEXT_STRINGS_SIGNATURE) magic.
bool is_strs(const uint8_t* data, size_t size);

/// One decoded string-table record, for the tabular preview. `text` is filled for
/// raw UTF-16 records and for packed ones whose RC4 key was available (the caller
/// passes a baseTextId so gw2skeys can look the key up); otherwise it's empty and
/// `locked` is true.
struct Record {
    uint32_t index = 0;        // record ordinal within this file
    long long textId = -1;     // global textId (baseTextId + index), -1 if unknown
    uint16_t baseChar = 0;     // packing base character (0 = raw UTF-16)
    uint8_t  rangeBits = 0;    // bits per packed symbol (16 = raw)
    uint16_t recLen = 0;       // on-disk record length incl. the 6-byte header
    bool     packed = false;   // bit-packed + RC4 (vs raw UTF-16)
    bool     locked = false;   // packed and no key -> text unavailable
    std::wstring text;
};

/// Parse every record's framing. Text is decoded for raw UTF-16 records; packed
/// records are left `locked` (use castlemist::skeys::records() to also decrypt those).
std::vector<Record> records(const uint8_t* data, size_t size);

/// Decodes a "strs" string-table container to a human-readable, line-per-record
/// listing (for the text preview control). Records stored as raw UTF-16LE
/// (baseChar == 0, rangeBits == 16) are decoded byte-exact; bit-packed records
/// (baseChar != 0) can't be decoded from a standalone file -- they need a
/// runtime RC4 key that isn't present here -- so they're listed with their
/// framing metadata and flagged "[encrypted -- RC4 key unavailable]".
/// See strs_decode.py for the reverse-engineering notes this mirrors.
std::wstring decode(const uint8_t* data, size_t size);

} // namespace castlemist::strs

#endif // GW2_STRS_VIEW_H
