#include "castlemist/format/chat_link.h"

#include <array>
#include <cstdio>
#include <sstream>

namespace castlemist::chat {
namespace {

// Reverse lookup for the standard base64 alphabet (ABC..xyz0-9+/). 0xFF = skip.
std::array<uint8_t, 256> build_rev() {
    std::array<uint8_t, 256> t{};
    t.fill(0xFF);
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<uint8_t>(i);
    return t;
}

// Little-endian read of `n` bytes (n<=8) at `off`; returns 0 if out of range.
uint64_t rd_le(const std::vector<uint8_t>& b, size_t off, size_t n, bool& ok) {
    if (off + n > b.size()) { ok = false; return 0; }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return v;
}

void add(Decoded& d, const std::string& label, uint64_t value, bool is_id = false) {
    d.fields.push_back({label, value, is_id});
}

// Classic "header + 3-byte LE id + reserved byte" links (skill/trait/map/...).
void parse_simple_id(Decoded& d, const std::string& id_label) {
    bool ok = true;
    uint32_t id = static_cast<uint32_t>(rd_le(d.raw, 1, 3, ok));
    if (!ok) { d.error = "truncated: expected a 3-byte id"; return; }
    add(d, id_label, id, true);
    d.primary_id = id;
    d.primary_label = id_label;
    d.ok = true;
}

void parse_item(Decoded& d) {
    bool ok = true;
    uint8_t qty = static_cast<uint8_t>(rd_le(d.raw, 1, 1, ok));
    uint32_t item_id = static_cast<uint32_t>(rd_le(d.raw, 2, 3, ok));
    uint8_t flags = static_cast<uint8_t>(rd_le(d.raw, 5, 1, ok));
    if (!ok) { d.error = "truncated item link"; return; }
    add(d, "Quantity", qty);
    add(d, "Item id", item_id, true);
    add(d, "Flags", flags);
    d.primary_id = item_id;
    d.primary_label = "Item id";
    d.ok = true;

    // Variable-length: each present flag appends its field; absent fields are
    // omitted entirely, so the offsets shift. Walk them in order. Skin/upgrade
    // ids are each a full 4-byte (u32) slot (3-byte id + a reserved zero byte);
    // the name/desc keys are 8 bytes.
    size_t off = 6;
    auto take_id4 = [&](const char* label) {
        uint32_t v = static_cast<uint32_t>(rd_le(d.raw, off, 4, ok));
        if (ok) { add(d, label, v, true); off += 4; }
    };
    auto take8 = [&](const char* label) {
        uint64_t v = rd_le(d.raw, off, 8, ok);
        if (ok) { add(d, label, v); off += 8; }
    };
    if (flags & 0x80) take_id4("Skin id");
    if (flags & 0x40) take_id4("Upgrade 1 id");
    if (flags & 0x20) take_id4("Upgrade 2 id");
    if (flags & 0x10) take8("Name decryption key");
    if (flags & 0x08) take8("Desc decryption key");
}

void parse_coin(Decoded& d) {
    bool ok = true;
    uint32_t copper = static_cast<uint32_t>(rd_le(d.raw, 1, 4, ok));
    if (!ok) { d.error = "truncated coin link"; return; }
    add(d, "Total copper", copper);
    add(d, "Gold", copper / 10000);
    add(d, "Silver", (copper / 100) % 100);
    add(d, "Copper", copper % 100);
    d.ok = true;
}

void parse_wvw(Decoded& d) {
    bool ok = true;
    uint32_t obj = static_cast<uint32_t>(rd_le(d.raw, 1, 3, ok));
    uint32_t map = static_cast<uint32_t>(rd_le(d.raw, 5, 3, ok));
    if (!ok) { d.error = "truncated WvW objective link"; return; }
    add(d, "Objective id", obj, true);
    add(d, "Map id", map, true);
    d.primary_id = obj;
    d.primary_label = "Objective id";
    d.ok = true;
}

void parse_user(Decoded& d) {
    if (d.raw.size() < 17) { d.error = "truncated user link"; return; }
    char guid[48];
    // API GUID layout: 4-2-2-2-6, first three groups byte-swapped vs storage.
    std::snprintf(guid, sizeof(guid),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  d.raw[4], d.raw[3], d.raw[2], d.raw[1], d.raw[6], d.raw[5], d.raw[8], d.raw[7],
                  d.raw[9], d.raw[10], d.raw[11], d.raw[12], d.raw[13], d.raw[14], d.raw[15], d.raw[16]);
    d.fields.push_back({std::string("GUID ") + guid, 0, false});
    // Character name: UTF-16LE from byte 17, null terminated. Emit as ascii-ish.
    std::string name;
    for (size_t off = 17; off + 1 < d.raw.size(); off += 2) {
        uint16_t c = static_cast<uint16_t>(d.raw[off] | (d.raw[off + 1] << 8));
        if (c == 0) break;
        name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    if (!name.empty()) d.fields.push_back({std::string("Name ") + name, 0, false});
    d.ok = true;
}

const char* kProfession[] = {"?",       "Guardian",     "Warrior",   "Engineer", "Ranger",
                             "Thief",   "Elementalist", "Mesmer",    "Necromancer", "Revenant"};

void parse_build(Decoded& d) {
    bool ok = true;
    uint8_t prof = static_cast<uint8_t>(rd_le(d.raw, 1, 1, ok));
    if (!ok) { d.error = "truncated build template"; return; }
    add(d, std::string("Profession ") + (prof < 10 ? kProfession[prof] : "?"), prof);
    // 3 specializations: specId byte + trait-choice byte (2 bits per major, reversed).
    for (int i = 0; i < 3; ++i) {
        uint8_t spec = static_cast<uint8_t>(rd_le(d.raw, 2 + 2 * i, 1, ok));
        uint8_t tr = static_cast<uint8_t>(rd_le(d.raw, 3 + 2 * i, 1, ok));
        if (!ok) break;
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "Spec %d id", i + 1);
        add(d, lbl, spec, spec != 0);
        char tl[48];
        std::snprintf(tl, sizeof(tl), "Spec %d traits (T/M/B)", i + 1);
        add(d, tl, static_cast<uint64_t>((tr & 3)) | ((tr >> 2 & 3) << 8) | ((tr >> 4 & 3) << 16));
    }
    // 10 skill palette ids (heal/util1-3/elite, terrestrial+aquatic interleaved).
    for (int i = 0; i < 10; ++i) {
        uint16_t pal = static_cast<uint16_t>(rd_le(d.raw, 8 + 2 * i, 2, ok));
        if (!ok) break;
        char lbl[40];
        static const char* slot[] = {"Heal",  "Heal(aq)",  "Util1", "Util1(aq)", "Util2",
                                     "Util2(aq)", "Util3", "Util3(aq)", "Elite", "Elite(aq)"};
        std::snprintf(lbl, sizeof(lbl), "Skill palette %s", slot[i]);
        add(d, lbl, pal);
    }
    // 16 profession-specific bytes @28 (ranger pets / revenant legends) then the
    // SotO weapon + skill-override arrays. Report their sizes; full decode is
    // profession dependent and not needed for id search.
    size_t off = 28 + 16;
    uint8_t nweap = static_cast<uint8_t>(rd_le(d.raw, off, 1, ok));
    if (ok) {
        add(d, "Terrestrial weapon count", nweap);
        ++off;
        for (int i = 0; i < nweap; ++i) {
            uint16_t w = static_cast<uint16_t>(rd_le(d.raw, off, 2, ok));
            if (!ok) break;
            add(d, "Weapon type id", w, true);
            off += 2;
        }
        uint8_t nov = static_cast<uint8_t>(rd_le(d.raw, off, 1, ok));
        if (ok) {
            add(d, "Skill override count", nov);
            ++off;
            for (int i = 0; i < nov; ++i) {
                uint32_t s = static_cast<uint32_t>(rd_le(d.raw, off, 4, ok));
                if (!ok) break;
                add(d, "Skill override id", s, true);
                off += 4;
            }
        }
    }
    d.ok = true;
}

// Generic dump of little-endian u16 slots for wardrobe/travel templates.
void parse_u16_slots(Decoded& d, const char* what) {
    bool ok = true;
    size_t n = (d.raw.size() - 1) / 2;
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "%s: %zu x u16 slots", what, n);
    d.fields.push_back({hdr, n, false});
    for (size_t i = 0; i < n; ++i) {
        uint16_t v = static_cast<uint16_t>(rd_le(d.raw, 1 + 2 * i, 2, ok));
        if (!ok) break;
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "slot[%zu]", i);
        add(d, lbl, v);
    }
    d.ok = true;
}

} // namespace

std::vector<uint8_t> base64_decode(const std::string& in) {
    static const std::array<uint8_t, 256> rev = build_rev();
    std::vector<uint8_t> out;
    int bits = 0, acc = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        uint8_t v = rev[c];
        if (v == 0xFF) continue;  // skip whitespace/invalid
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

Decoded decode(const std::string& text) {
    Decoded d;

    // Trim, then strip an optional "[&" prefix and "]" suffix.
    std::string s;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') s.push_back(c);
    if (!s.empty() && s.front() == '[') s.erase(s.begin());
    if (!s.empty() && s.front() == '&') s.erase(s.begin());
    if (!s.empty() && s.back() == ']') s.pop_back();
    if (s.empty()) { d.error = "empty input"; return d; }

    d.raw = base64_decode(s);
    if (d.raw.empty()) { d.error = "not valid base64"; return d; }

    d.header = d.raw[0];
    switch (d.header) {
    case 0x01: d.type = "Coin";              parse_coin(d); break;
    case 0x02: d.type = "Item";              parse_item(d); break;
    case 0x03: d.type = "NPC text";          parse_simple_id(d, "Text id"); break;
    case 0x04: d.type = "Map (PoI/waypoint)"; parse_simple_id(d, "PoI id"); break;
    case 0x05: d.type = "PvP game";          d.error = "PvP game link format unknown"; break;
    case 0x06: d.type = "Skill";             parse_simple_id(d, "Skill id"); break;
    case 0x07: d.type = "Trait";             parse_simple_id(d, "Trait id"); break;
    case 0x08: d.type = "User";              parse_user(d); break;
    case 0x09: d.type = "Recipe";            parse_simple_id(d, "Recipe id"); break;
    case 0x0A: d.type = "Wardrobe skin";     parse_simple_id(d, "Skin id"); break;
    case 0x0B: d.type = "Outfit";            parse_simple_id(d, "Outfit id"); break;
    case 0x0C: d.type = "WvW objective";     parse_wvw(d); break;
    case 0x0D: d.type = "Build template";    parse_build(d); break;
    case 0x0E: d.type = "Achievement";       parse_simple_id(d, "Achievement id"); break;
    case 0x0F: d.type = "Wardrobe template"; parse_u16_slots(d, "Wardrobe template"); break;
    case 0x10: d.type = "Travel template";   parse_u16_slots(d, "Travel template"); break;
    default: {
        char e[64];
        std::snprintf(e, sizeof(e), "unknown header byte 0x%02X", d.header);
        d.error = e;
        break;
    }
    }
    return d;
}

std::string to_report(const Decoded& d) {
    std::ostringstream os;
    if (!d.ok && d.error.empty() && d.type.empty()) { os << "(nothing decoded)\r\n"; return os.str(); }
    os << "Type: " << (d.type.empty() ? "?" : d.type) << "  (header 0x";
    char hb[4]; std::snprintf(hb, sizeof(hb), "%02X", d.header); os << hb << ")\r\n";
    if (!d.error.empty()) os << "Error: " << d.error << "\r\n";
    for (const auto& f : d.fields) {
        os << "  " << f.label;
        // Fields whose value is embedded in the label (GUID/name) carry value 0.
        if (!(f.value == 0 && f.label.size() > 5 &&
              (f.label.rfind("GUID", 0) == 0 || f.label.rfind("Name", 0) == 0)))
            os << " = " << f.value;
        if (f.is_id) os << "   [id]";
        os << "\r\n";
    }
    // Raw bytes as hex.
    os << "Raw (" << d.raw.size() << " bytes): ";
    for (size_t i = 0; i < d.raw.size(); ++i) {
        char hx[4]; std::snprintf(hx, sizeof(hx), "%02X", d.raw[i]); os << hx;
        if (i + 1 < d.raw.size()) os << ' ';
    }
    os << "\r\n";
    return os.str();
}

} // namespace castlemist::chat
