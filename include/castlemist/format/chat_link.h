#ifndef CHAT_LINK_H
#define CHAT_LINK_H

// Guild Wars 2 chat-link decoder.
//
// A chat link is "[&" + base64(payload) + "]" (the "[&" / "]" wrapper is
// optional on input; a bare base64 blob is accepted too). The payload is a
// 1-byte type header followed by little-endian fields. This mirrors the game's
// own scheme: the base64 layer is Arena\Services\Base64 (standard alphabet,
// DecodeSize = 3*(chars/4)); the classic links are a header byte + LE id, while
// build/wardrobe/travel templates carry richer fixed/variable field sets.
//
// The decoder is self-contained (no game code): it turns pasted text into a
// typed breakdown so the browser can surface the ids and hand them to its
// base-id / file-id search.  See the gw2-chat-links memory note.

#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::chat {

/// One decoded field: a human label and its numeric value. `is_id` marks values
/// that are meaningful ids the user might want to look up (item/skill/skin/...).
struct Field {
    std::string label;
    uint64_t value = 0;
    bool is_id = false;
};

/// @brief A decoded chat link: its type, its field breakdown, and the id worth searching.
struct Decoded {
    bool ok = false;
    std::string error;

    uint8_t header = 0;
    std::string type;                 // "Item", "Skill", "Build template", ...
    std::vector<Field> fields;        // ordered breakdown of the payload
    std::vector<uint8_t> raw;         // the base64-decoded payload bytes

    /// The single id most worth searching (0 = none/not applicable), plus a
    /// label for it. For item links this is the item id, for skill/trait/map/
    /// recipe/skin/outfit/achievement it is that entity id.
    uint32_t primary_id = 0;
    std::string primary_label;
};

/// Standard base64 decode (ignores whitespace, tolerates missing '=' padding).
std::vector<uint8_t> base64_decode(const std::string& in);

/// Decode a chat link. Accepts "[&....]", "&....", or a bare "....".
Decoded decode(const std::string& text);

/// Multi-line human-readable report (CRLF terminated lines) for a text control.
std::string to_report(const Decoded& d);

} // namespace castlemist::chat

#endif // CHAT_LINK_H
