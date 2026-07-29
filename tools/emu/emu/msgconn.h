// msgconn.h - plaintext MsgConn framing for the emulator.
//
// After the RC4-identity patch, the client speaks plaintext:
//   * mode 2 -> 3 transition: the SERVER sends the raw connect packet
//         [00][0x16][20-byte nonce]
//     which the client's ClientRecvEncrypt consumes (the derived key is
//     irrelevant once RC4 is identity). We send this first, on accept.
//   * thereafter, a continuous stream of  [msgId u16][fields...]  messages
//     with no length prefix; we split them using the registered MsgDef.
#pragma once
#include "msgdef.h"
#include "../common/bytebuf.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gw2 {

// A decoded field value (scaffold: we keep the raw bytes + a printable form).
struct Value {
    const char*          label;
    MpType               type;
    std::vector<uint8_t> raw;
    std::string          text;   // for strings; else hex in the logger
};

struct DecodedMsg {
    uint16_t             id;
    const char*          name;
    std::vector<Value>   fields;
    size_t               wire_len; // total bytes consumed (incl. msgId)
};

class Protocol {
public:
    void register_def(const MsgDef& d) { defs_[d.id] = d; }
    const MsgDef* find(uint16_t id) const {
        auto it = defs_.find(id);
        return it == defs_.end() ? nullptr : &it->second;
    }

    // Decode ONE message starting at the front of `data`. On success fills
    // `out` and returns true. If the def is unknown or the buffer is short,
    // returns false and sets `need_more` when it was merely truncated.
    bool decode_one(const uint8_t* data, size_t n, DecodedMsg& out, bool& need_more) const;

private:
    void decode_field(Reader& r, const Field& f, Value& v) const;
    std::map<uint16_t, MsgDef> defs_;
};

// Build the raw [00][0x16][20-byte nonce] connect packet (nonce arbitrary).
std::vector<uint8_t> make_connect_packet();

// Serialize a server->client message: [msgId u16][payload bytes as-is].
// (Higher-level typed packing can be layered on Writer later.)
std::vector<uint8_t> make_message(uint16_t id, const std::vector<uint8_t>& payload);

} // namespace gw2
