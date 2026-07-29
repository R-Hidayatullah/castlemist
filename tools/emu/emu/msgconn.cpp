#include "msgconn.h"
#include <cstdio>

namespace gw2 {

static std::string to_hex(const std::vector<uint8_t>& b) {
    static const char* H = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s.push_back(H[c >> 4]); s.push_back(H[c & 0xf]); }
    return s;
}

void Protocol::decode_field(Reader& r, const Field& f, Value& v) const {
    v.label = f.label;
    v.type  = f.type;

    size_t fx = fixed_size(f.type);
    if (fx) { v.raw = r.bytes(fx); return; }

    switch (f.type) {
        case MP_STRING: {                 // UTF-16LE, null-term, max f.count units
            size_t start = r.offset();
            v.text = r.wstr(f.count ? f.count : 256);
            (void)start;
            break;
        }
        case MP_CSTRING: {                // UTF-8, null-term, max f.count bytes
            v.text = r.cstr(f.count ? f.count : 256);
            break;
        }
        case MP_BLOB_FIXED: {             // fixed byte blob
            v.raw = r.bytes(f.count);
            break;
        }
        case MP_BLOB_U8: {                // u8 length + bytes
            uint8_t len = r.u8();
            v.raw = r.bytes(len);
            break;
        }
        case MP_BLOB_U16: {               // u16 length + bytes
            uint16_t len = r.u16();
            v.raw = r.bytes(len);
            break;
        }
        case MP_ARR_U8:                   // u8 count + count*nested
        case MP_ARR_U16: {                // u16 count + count*nested
            uint32_t cnt = (f.type == MP_ARR_U8) ? r.u8() : r.u16();
            for (uint32_t i = 0; i < cnt && f.nested; ++i) {
                for (const auto& nf : f.nested->fields) {
                    Value nv; decode_field(r, nf, nv);
                }
            }
            break;
        }
        case MP_ARR_FIXED: {              // f.count * nested
            for (uint32_t i = 0; i < f.count && f.nested; ++i)
                for (const auto& nf : f.nested->fields) { Value nv; decode_field(r, nf, nv); }
            break;
        }
        case MP_OPTIONAL: {               // 1B flag + nested if set
            uint8_t present = r.u8();
            if (present && f.nested)
                for (const auto& nf : f.nested->fields) { Value nv; decode_field(r, nf, nv); }
            break;
        }
        case MP_NESTED: {                 // embedded struct
            if (f.nested)
                for (const auto& nf : f.nested->fields) { Value nv; decode_field(r, nf, nv); }
            break;
        }
        default:
            // Unknown/unsupported variable type: cannot safely continue.
            throw std::runtime_error("unsupported field type in def");
    }
}

bool Protocol::decode_one(const uint8_t* data, size_t n, DecodedMsg& out, bool& need_more) const {
    need_more = false;
    if (n < 2) { need_more = true; return false; }

    Reader r(data, n);
    uint16_t id = r.u16();
    const MsgDef* def = find(id);
    if (!def) return false;              // unknown id -> caller logs raw, stops

    out.id = id;
    out.name = def->name;
    out.fields.clear();
    try {
        for (const auto& f : def->fields) {
            Value v;
            decode_field(r, f, v);
            if (!v.raw.empty() && v.text.empty()) v.text = to_hex(v.raw);
            out.fields.push_back(std::move(v));
        }
    } catch (const std::exception&) {
        need_more = true;                // truncated: wait for more bytes
        return false;
    }
    out.wire_len = r.offset();
    return true;
}

std::vector<uint8_t> make_connect_packet() {
    // [type=0][len=0x16][20-byte nonce]. Total 22 bytes == the client's length
    // check. Nonce value is arbitrary once RC4 is identity.
    std::vector<uint8_t> p;
    p.push_back(0x00);
    p.push_back(0x16);
    for (int i = 0; i < 20; ++i) p.push_back(0x00);
    return p;
}

std::vector<uint8_t> make_message(uint16_t id, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p;
    p.push_back(id & 0xff);
    p.push_back((id >> 8) & 0xff);
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

} // namespace gw2
