/// @file
/// @brief cntc PackContent datastore: summary, objects and referenced assets.

#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#include "castlemist/core/bytes.h"

#include <set>
#include "castlemist/core/packfile.h"

namespace castlemist::extract {

// ---- cntc (content datastore) ---------------------------------------------
// A "cntc" packfile (PF v5, 64-bit ptrs, single chunk "Main" = PackContent) is GW2's
// content database: content type/namespace definitions, an index, fixup tables, a
// string table of content codenames, and a big content-data blob. It's not a viewable
// asset, so we parse the header into a readable summary + a sample of the codenames.
std::wstring parse_cntc_summary(const std::vector<uint8_t>& d) {
    if (d.size() < 12 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "cntc", 4) != 0) return {};
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (uint32_t)(d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto rd64 = [&](size_t p) -> int64_t { if (p + 8 > d.size()) return 0; int64_t v = 0; std::memcpy(&v, d.data() + p, 8); return v; };
    // Locate the Main chunk.
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz; if (next <= pos) return {}; pos = next;
    }
    if (pos + 16 > d.size() || std::memcmp(d.data() + pos, "Main", 4) != 0) return {};
    size_t base = pos + 16;                       // PackContent fields (flags @base, arrays follow)
    // Each array_ptr = {u32 count, i64 self-relative ptr} (12 bytes); ptr from its own position.
    auto arr = [&](int idx, size_t& dataOff) -> uint32_t {
        size_t p = base + 4 + (size_t)idx * 12;   // skip flags(4)
        uint32_t c = rd32(p);
        int64_t rel = rd64(p + 4);
        dataOff = (size_t)((p + 4) + rel);
        return c;
    };
    static const wchar_t* kNames[11] = {
        L"content types", L"namespaces", L"file references", L"index entries", L"local fixups",
        L"external fixups", L"file-index fixups", L"string-index fixups", L"tracked references",
        L"strings (codenames)", L"content data bytes" };
    size_t off[11]; uint32_t cnt[11];
    for (int i = 0; i < 11; ++i) cnt[i] = arr(i, off[i]);

    std::wstring s = L"GW2 content datastore (cntc / PackContent)\r\n";
    s += L"An internal content database -- not a viewable/playable asset.\r\n\r\n";
    wchar_t line[128];
    for (int i = 0; i < 11; ++i) { swprintf(line, 128, L"  %-22ls %u\r\n", kNames[i], cnt[i]); s += line; }

    // Namespace names (present in master-manifest cntc files): wchar_ptr + domain + parentIndex (16 B).
    if (cnt[1] > 0) {
        s += L"\r\nNamespaces:\r\n";
        uint32_t nn = cnt[1] < 60 ? cnt[1] : 60;
        for (uint32_t i = 0; i < nn; ++i) {
            size_t ep = off[1] + (size_t)i * 16;
            int64_t rel = rd64(ep);
            size_t sp = (size_t)(ep + rel);
            std::wstring name;
            for (size_t q = sp; q + 2 <= d.size() && name.size() < 120; q += 2) {
                wchar_t ch = (wchar_t)(d[q] | (d[q + 1] << 8)); if (!ch) break; name += ch;
            }
            if (rel == 0) name = L"(root)";
            s += L"  " + name + L"\r\n";
        }
    }

    // Sample of the content codename string table.
    if (cnt[9] > 0) {
        s += L"\r\nContent codenames (first 60 of "; s += std::to_wstring(cnt[9]); s += L"):\r\n";
        uint32_t ns = cnt[9] < 60 ? cnt[9] : 60;
        for (uint32_t i = 0; i < ns; ++i) {
            size_t ep = off[9] + (size_t)i * 8;
            int64_t rel = rd64(ep);
            if (rel == 0) continue;
            size_t sp = (size_t)(ep + rel);
            std::wstring name;
            for (size_t q = sp; q + 2 <= d.size() && name.size() < 120; q += 2) {
                wchar_t ch = (wchar_t)(d[q] | (d[q + 1] << 8)); if (!ch) break; name += ch;
            }
            s += L"  " + name + L"\r\n";
        }
    }
    return s;
}

// Collect the unique external asset fileIds a cntc content blob references. Each
// `fileIndices` fixup {u32 relocOffset} marks a spot in the content byte-array that
// holds a raw fileId (verified: they resolve to textures/models). Returns up to `cap`
// unique ids.
std::vector<uint32_t> cntc_referenced_asset_ids(const std::vector<uint8_t>& d, size_t cap) {
    std::vector<uint32_t> out;
    if (d.size() < 12 || std::memcmp(d.data() + 8, "cntc", 4) != 0) return out;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (uint32_t)(d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto rd64 = [&](size_t p) -> int64_t { if (p + 8 > d.size()) return 0; int64_t v = 0; std::memcpy(&v, d.data() + p, 8); return v; };
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz; if (next <= pos) return out; pos = next;
    }
    if (pos + 16 > d.size()) return out;
    size_t base = pos + 16;
    auto arr = [&](int i, size_t& off) { size_t p = base + 4 + (size_t)i * 12; off = (size_t)((p + 4) + rd64(p + 4)); return rd32(p); };
    size_t fidxOff, contentOff;
    uint32_t fidxCount = arr(6, fidxOff);  // fileIndices
    arr(10, contentOff);                   // content byte-array start
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < fidxCount && out.size() < cap; ++i) {
        uint32_t reloc = rd32(fidxOff + (size_t)i * 4);
        uint32_t fid = rd32(contentOff + reloc);
        if (fid > 40000 && fid < 0xFFFFFF && seen.insert(fid).second) out.push_back(fid);
    }
    return out;
}


// Parse a cntc content blob into its content OBJECTS (item/skin/outfit/...) with
// the asset fileIds each references, for the master->child content browser. Same
// layout as cntc_referenced_asset_ids: indexEntries (offset@+4) -> objects, each
// with contentType@+16, id@+20, and fileIndices relocs inside [o,next) as assets.
// Only objects with >=1 asset are returned, sorted by (type, id) so the master
// list groups by kind. Capped at `cap` objects.
std::vector<ContentObject> parse_cntc_objects(const std::vector<uint8_t>& d, size_t cap) {
    std::vector<ContentObject> out;
    const size_t n = d.size();
    if (n < 16 || std::memcmp(d.data() + 8, "cntc", 4) != 0) return out;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= n) ? (uint32_t)(d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= n) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto rd64 = [&](size_t p) -> int64_t { if (p + 8 > n) return 0; int64_t v; std::memcpy(&v, d.data() + p, 8); return v; };
    size_t pos = rd16(6);
    while (pos + 8 <= n && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        size_t next = pos + 8 + rd32(pos + 4);
        if (next <= pos) return out;
        pos = next;
    }
    if (pos + 16 > n) return out;
    size_t base = pos + 16;
    auto arr = [&](int i, size_t& off) -> uint32_t {
        size_t p = base + 4 + (size_t)i * 12;
        off = (size_t)((p + 4) + rd64(p + 4));
        return rd32(p);
    };
    size_t ieOff, fiOff, siOff, stOff, cOff;
    uint32_t ieCnt = arr(3, ieOff), fiCnt = arr(6, fiOff), siCnt = arr(7, siOff),
             stCnt = arr(9, stOff), cCnt = arr(10, cOff);
    if (!ieCnt || !cCnt || cOff >= n) return out;

    // PackContentIndexEntry (16 bytes), field order confirmed against the client's
    // own reflection table + value distribution over all 32549 entries of a live
    // cntc: {u32 contentType, u32 relocOffset, u32, u32}. The contentType is here,
    // NOT at content+16 (that offset only coincidentally agrees for some objects --
    // 6573 distinct values there vs 103 clean type values here, with 35=Item and
    // 64=Skill dominating exactly as expected).
    struct IE { uint32_t off, type; };
    std::vector<IE> ies;
    ies.reserve(ieCnt);
    for (uint32_t i = 0; i < ieCnt; ++i) {
        size_t e = ieOff + (size_t)i * 16;
        ies.push_back({rd32(e + 4), rd32(e)});
    }
    std::sort(ies.begin(), ies.end(), [](const IE& a, const IE& b) { return a.off < b.off; });

    // fileIndices / stringIndices fixups: each is a u32 offset into the content
    // blob marking a field to relocate. Those falling inside an object's byte range
    // belong to that object (assets / its codename).
    std::vector<uint32_t> fi, si;
    fi.reserve(fiCnt);
    for (uint32_t i = 0; i < fiCnt; ++i) fi.push_back(rd32(fiOff + (size_t)i * 4));
    std::sort(fi.begin(), fi.end());
    si.reserve(siCnt);
    for (uint32_t i = 0; i < siCnt; ++i) si.push_back(rd32(siOff + (size_t)i * 4));
    std::sort(si.begin(), si.end());

    // strings[k] = an 8-byte self-relative pointer to a UTF-16 codename.
    auto codename = [&](uint32_t idx) -> std::string {
        if (idx >= stCnt) return {};
        size_t ep = stOff + (size_t)idx * 8;
        int64_t rel = rd64(ep);
        if (rel == 0) return {};
        size_t sp = (size_t)(ep + rel);
        std::string s;
        for (size_t q = sp; q + 2 <= n && s.size() < 96; q += 2) {
            uint32_t ch = d[q] | (d[q + 1] << 8);
            if (!ch) break;
            s += (ch < 0x80) ? (char)ch : '?';   // codenames are ASCII in practice
        }
        return s;
    };

    for (size_t k = 0; k < ies.size(); ++k) {
        uint32_t o = ies[k].off;
        uint32_t nextOff = (k + 1 < ies.size()) ? ies[k + 1].off : cCnt;
        if (cOff + o + 24 > n) continue;
        ContentObject obj;
        obj.type = ies[k].type;
        obj.id = rd32(cOff + o + 20);
        for (auto f = std::lower_bound(fi.begin(), fi.end(), o);
             f != fi.end() && *f < nextOff && obj.assets.size() < 16; ++f) {
            uint32_t v = rd32(cOff + *f);
            if (v > 0 && v < 0xFFFFFF) obj.assets.push_back(v);
        }
        // Every string field in range, split by SHAPE (not position -- the slug is
        // frequently not the first string): the "xxxxx.yyyyy" slug is the object's
        // id, everything else is a readable, shared parameter/condition label.
        for (auto s = std::lower_bound(si.begin(), si.end(), o);
             s != si.end() && *s < nextOff; ++s) {
            std::string str = codename(rd32(cOff + *s));
            if (str.empty()) continue;
            if (obj.name.empty() && is_content_slug(str)) obj.name = std::move(str);
            else if (obj.labels.size() < 12) obj.labels.push_back(std::move(str));
        }
        // Keep anything identifiable: an object with an id or a readable label is
        // worth listing even with no previewable asset (it says what the data IS).
        if (!obj.assets.empty() || !obj.name.empty() || !obj.labels.empty())
            out.push_back(std::move(obj));
        if (out.size() >= cap) break;
    }
    std::sort(out.begin(), out.end(), [](const ContentObject& a, const ContentObject& b) {
        return a.type != b.type ? a.type < b.type : a.id < b.id;
    });
    return out;
}

// Lightweight type peek for an entry addressed by fileId: decompress its header and
// classify by magic/container. Used to characterize cntc asset references.
std::string classify_fileid(Gw2Dat& dat, uint32_t fileId) {
    std::vector<uint8_t> b = load_modl_bytes_by_fileid(dat, fileId); // generic decompress
    if (b.size() < 12) return "";
    if (b[0] == 'P' && b[1] == 'F') {
        char c[5] = {0}; std::memcpy(c, b.data() + 8, 4);
        std::string cs(c);
        if (castlemist::core::has_chunk(b, "GEOM")) return "model";
        if (cs == "ASND" || cs == "ABNK" || cs == "AMSP") return "audio";
        if (cs == "PIMG") return "image atlas";
        return "packfile " + cs;
    }
    static const char* atex[] = {"ATEX", "ATTX", "ATEC", "ATEP", "ATEU", "ATET", "CTEX"};
    for (const char* m : atex) if (std::memcmp(b.data(), m, 4) == 0) return "texture";
    if (b[0] == 'D' && b[1] == 'D' && b[2] == 'S') return "texture";
    if (std::memcmp(b.data(), "strs", 4) == 0) return "strings";
    return "other";
}

} // namespace castlemist::extract

// ---- public API (declared in castlemist/extract/entry_extractor.h) ----

using namespace castlemist::extract;

// True for an ArenaNet content IDENTIFIER slug: two short base64-ish groups joined
// by a dot, e.g. "vl8Av.4gynM" / "sEhjD.9j1Ju". Measured over a live cntc's string
// table, 20316 of 21997 entries have this shape and the remaining 1681 are readable
// English labels ("Damage Multiplier", "Within Range"), so shape cleanly separates
// "which string is this object's id" from "which strings describe it".
bool is_content_slug(const std::string& s) {
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot < 3 || dot > 8) return false;
    if (s.size() - dot - 1 < 3 || s.size() - dot - 1 > 8) return false;
    if (s.find(' ') != std::string::npos) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == dot) continue;
        unsigned char c = (unsigned char)s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// Human label for a GW2 content type number. Only the chat-link-relevant types
// are named (IDA-verified); others fall back to "Type N" at the call site.
const char* content_type_name(uint32_t type) {
    // Item/Outfit/Skin/Minipet are IDA-verified (CONTENT_TYPE_* asserts in the
    // client). The remaining labels are read off a live cntc type table for this
    // build (T3D content browser); type 35/51/48 there match the IDA constants,
    // which cross-validates the rest. Unknown types fall through to "Type N".
    switch (type) {
    case 0: return "Achievement";
    case 1: return "Achievement Category";
    case 12: return "Crafting Recipe";
    case 27: return "Guild Upgrade";
    case 35: return "Item";
    case 45: return "Map";
    case 48: return "Minipet";
    case 51: return "Outfit";
    case 64: return "Skill";
    case 66: return "Skin";
    default: return nullptr;
    }
}
