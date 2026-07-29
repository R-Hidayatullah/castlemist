#include "castlemist/format/content_map.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <unordered_map>

#include "castlemist/native/cmp_decompress_method0.hpp"

namespace castlemist::cmap {
namespace {

// key = (contentType << 32) | numericId -> all in-object asset fileIds (capped).
std::unordered_map<uint64_t, std::vector<uint32_t>> g_map;
const std::vector<uint32_t> g_empty;

constexpr size_t kMaxRefsPerObject = 16;

inline uint64_t key(uint32_t type, uint32_t id) { return (static_cast<uint64_t>(type) << 32) | id; }

// Decompress one MFT entry to its raw file bytes (mirrors decompress_by_index in
// entry_extractor.cpp). Own file handle inside read_entry_bytes -> thread-safe.
std::vector<uint8_t> decompress(const std::string& dat_path, const MftData& e) {
    std::vector<uint8_t> raw = read_entry_bytes(dat_path, e);
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0) return s;
    if (s.size() < 8) return {};
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | (static_cast<uint32_t>(s[7]) << 24);
    return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
}

// Parse one decompressed cntc packfile, adding every (contentType,id)->fileId it
// finds. Layout (validated): PF "cntc" -> chunk "Main"; base = mainPos+16; then
// 11 arrays {u32 count, i64 self-rel ptr} at base+4+i*12. Array 3 = indexEntries
// (16 B each: object offset at +4), array 6 = fileIndices (u32 reloc into content),
// array 10 = content bytes. Each object: contentType@+16, id@+20, primary asset
// fileId at the fileIndices reloc == object+64 (else the first reloc in the object).
void parse_cntc(const std::vector<uint8_t>& d) {
    const size_t n = d.size();
    if (n < 16 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "cntc", 4) != 0) return;
    auto u16 = [&](size_t p) -> uint32_t { return (p + 2 <= n) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto u32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= n) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto i64 = [&](size_t p) -> int64_t { if (p + 8 > n) return 0; int64_t v; std::memcpy(&v, d.data() + p, 8); return v; };

    size_t pos = u16(6);
    while (pos + 8 <= n && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        size_t next = pos + 8 + u32(pos + 4);
        if (next <= pos) return;
        pos = next;
    }
    if (pos + 16 > n) return;
    size_t base = pos + 16;
    auto arr = [&](int i, size_t& off) -> uint32_t {
        size_t p = base + 4 + (size_t)i * 12;
        off = (size_t)((p + 4) + i64(p + 4));
        return u32(p);
    };
    size_t ieOff, fiOff, cOff;
    uint32_t ieCnt = arr(3, ieOff);
    uint32_t fiCnt = arr(6, fiOff);
    uint32_t cCnt = arr(10, cOff);
    if (ieCnt == 0 || cCnt == 0 || cOff >= n) return;

    // Object offsets (sorted, unique) so we know each object's extent.
    std::vector<uint32_t> offs;
    offs.reserve(ieCnt);
    for (uint32_t i = 0; i < ieCnt; ++i) offs.push_back(u32(ieOff + (size_t)i * 16 + 4));
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());

    // fileIndices relocs, sorted for range queries.
    std::vector<uint32_t> fi;
    fi.reserve(fiCnt);
    for (uint32_t i = 0; i < fiCnt; ++i) fi.push_back(u32(fiOff + (size_t)i * 4));
    std::sort(fi.begin(), fi.end());

    for (size_t k = 0; k < offs.size(); ++k) {
        uint32_t o = offs[k];
        uint32_t nextOff = (k + 1 < offs.size()) ? offs[k + 1] : cCnt;
        if (cOff + o + 24 > n) continue;
        uint32_t type = u32(cOff + o + 16);
        uint32_t id = u32(cOff + o + 20);

        // Collect every fileIndices reloc inside this object [o, nextOff) as an
        // asset fileId, in object order. The reloc at +64 (the primary/model)
        // sorts first naturally; skins list icon textures + model + variants.
        std::vector<uint32_t> refs;
        for (auto f = std::lower_bound(fi.begin(), fi.end(), o);
             f != fi.end() && *f < nextOff && refs.size() < kMaxRefsPerObject; ++f) {
            uint32_t v = u32(cOff + *f);
            if (v > 0 && v < 0xFFFFFF) refs.push_back(v);
        }
        if (!refs.empty()) g_map[key(type, id)] = std::move(refs);
    }
}

} // namespace

bool built() { return !g_map.empty(); }
size_t size() { return g_map.size(); }
void clear() { g_map.clear(); }

size_t build(const std::string& dat_path, const std::vector<MftData>& cntc_entries,
             const std::function<void(size_t, size_t)>& progress) {
    size_t total = cntc_entries.size();
    for (size_t i = 0; i < total; ++i) {
        std::vector<uint8_t> bytes = decompress(dat_path, cntc_entries[i]);
        if (!bytes.empty()) parse_cntc(bytes);
        if (progress) progress(i + 1, total);
    }
    return g_map.size();
}

uint32_t content_type_for_header(uint8_t header) {
    switch (header) {
    case 0x02: return CONTENT_TYPE_ITEM;    // item link
    case 0x0A: return CONTENT_TYPE_SKIN;    // wardrobe skin link
    case 0x0B: return CONTENT_TYPE_OUTFIT;  // outfit link
    default: return 0;
    }
}

const std::vector<uint32_t>& resolve_all(uint32_t content_type, uint32_t id) {
    auto it = g_map.find(key(content_type, id));
    return it == g_map.end() ? g_empty : it->second;
}

uint32_t resolve(uint32_t content_type, uint32_t id) {
    const std::vector<uint32_t>& v = resolve_all(content_type, id);
    return v.empty() ? 0 : v.front();
}

// ---- binary cache: "GC2N" magic, u32 count, then count * {u64 key, u8 n, n*u32 fileId}.
bool save(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    uint32_t magic = 0x4E324347;  // 'GC2N'
    uint32_t count = static_cast<uint32_t>(g_map.size());
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&count, 4, 1, f);
    for (const auto& [k, v] : g_map) {
        uint8_t n = static_cast<uint8_t>(std::min<size_t>(v.size(), 255));
        std::fwrite(&k, 8, 1, f);
        std::fwrite(&n, 1, 1, f);
        std::fwrite(v.data(), 4, n, f);
    }
    std::fclose(f);
    return true;
}

bool load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    uint32_t magic = 0, count = 0;
    if (std::fread(&magic, 4, 1, f) != 1 || magic != 0x4E324347) { std::fclose(f); return false; }
    if (std::fread(&count, 4, 1, f) != 1) { std::fclose(f); return false; }
    g_map.reserve(count + 16);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t k; uint8_t n;
        if (std::fread(&k, 8, 1, f) != 1 || std::fread(&n, 1, 1, f) != 1) break;
        std::vector<uint32_t> v(n);
        if (n && std::fread(v.data(), 4, n, f) != n) break;
        g_map[k] = std::move(v);
    }
    std::fclose(f);
    return !g_map.empty();
}

} // namespace castlemist::cmap
