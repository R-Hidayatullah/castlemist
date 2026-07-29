// msgdef.h - MsgConn message-definition model + field type codes.
//
// Mirrors the client's MsgUnpack/MsgPack def-array system (RE'd from
// Msg::DispatchStream / Msg::MsgUnpack). A message on the wire is:
//     [msgId u16 LE][fields in def order]
// with NO per-message length prefix -- each field's size comes from its type
// code (fixed) or an inline count/length (variable). To split messages out of
// a stream you MUST know the def, which is why the registry is authoritative.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gw2 {

// Field type codes as decoded from the client's wireSize table (dword_1420C7FC0)
// and MsgUnpack. Names use the official MP_ scheme where known.
enum MpType : uint8_t {
    MP_END        = 0x00, // terminator (also 0x18/0x24 in def-array control)
    MP_WORD       = 0x01, // 2B
    MP_BYTE       = 0x02, // 1B
    MP_WORD2      = 0x03, // 2B
    MP_NESTED     = 0x04, // embedded struct (nested def)
    MP_QWORD      = 0x05, // 8B
    MP_DWORD      = 0x06, // 4B
    MP_QWORD2     = 0x07, // 8B
    MP_VEC3       = 0x08, // 12B (3xf32)
    MP_VEC4       = 0x09, // 16B (4xf32)
    MP_VEC3X      = 0x0A, // 12B + ext
    MP_OWORD      = 0x0B, // 16B
    MP_STRUCT28   = 0x0C, // 28B fixed struct
    MP_STRING     = 0x0D, // UTF-16LE, null-terminated, max = count code units
    MP_CSTRING    = 0x0E, // UTF-8, null-terminated, max = count bytes
    MP_OPTIONAL   = 0x0F, // 1B present-flag, then nested if present
    MP_ARR_FIXED  = 0x10, // fixed array: count * nested
    MP_ARR_U8     = 0x11, // u8 count, then count * nested
    MP_ARR_U16    = 0x12, // u16 count, then count * nested
    MP_BLOB_FIXED = 0x13, // fixed byte blob of `count`
    MP_BLOB_U8    = 0x14, // u8 length (max count), then bytes
    MP_BLOB_U16   = 0x15, // u16 length (max count), then bytes
    MP_DWORD2     = 0x17, // 4B
    MP_SRV_END    = 0x18, // pad to 4B (server); no client bytes
    MP_DWORD3     = 0x19, // 4B
    MP_QWORD3     = 0x1A, // 8B
};

struct Field {
    MpType   type;
    uint32_t count = 0;   // max chars / max bytes / fixed array or blob length
    const struct MsgDef* nested = nullptr; // for NESTED/ARR_*/OPTIONAL
    const char* label = "";
};

struct MsgDef {
    uint16_t             id;
    const char*          name;
    std::vector<Field>   fields;
    // dispatchType 0/1 (handler calling convention) - informational here.
    int                  dispatchType = 0;
};

// Fixed wire size of a type, or 0 if variable (needs inline count/length).
inline size_t fixed_size(MpType t) {
    switch (t) {
        case MP_BYTE: return 1;
        case MP_WORD: case MP_WORD2: return 2;
        case MP_DWORD: case MP_DWORD2: case MP_DWORD3: return 4;
        case MP_QWORD: case MP_QWORD2: case MP_QWORD3: return 8;
        case MP_VEC3: case MP_VEC3X: return 12;
        case MP_VEC4: case MP_OWORD: return 16;
        case MP_STRUCT28: return 28;
        case MP_SRV_END: return 0; // client sends nothing
        default: return 0;         // variable / nested
    }
}

} // namespace gw2
