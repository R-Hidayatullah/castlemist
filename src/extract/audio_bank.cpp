/// @file
/// @brief AMSP sound scripts resolved into playable clips.

#include "internal.h"

#include <cstring>
#include <unordered_map>

#include "castlemist/native/gw2_audio.hpp"

#include <set>
#include "castlemist/native/cmp_decompress_method0.hpp"

namespace castlemist::extract {

// ---- AMSP (sound bank) ----------------------------------------------------
// An AMSP packfile is a sound SCRIPT/bank (PF v5, 64-bit pointers): its metaSound
// array references the actual audio in EXTERNAL ASND entries by fileId. We collect
// those fileIds, load each ASND, and present them as a playable multi-sound bank.
// Layout (chunk AMSP v33): metaSound array_ptr @chunk+60 {u32 count, i64 selfRelPtr};
// MetaSoundDataV31 stride 326, fileName array_ptr @elem+60; FileNameDataV31 stride 36,
// fileName field @fe+24 = a 64-bit self-relative ptr to a {u16 lo, u16 hi} fileref
// (fileId = 0xFF00*hi + lo - 0xFF00FF, same decode as PIMG/model filenames).
std::vector<uint32_t> parse_amsp_sound_ids(const std::vector<uint8_t>& d) {
    std::vector<uint32_t> ids;
    if (d.size() < 12 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "AMSP", 4) != 0) return ids;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (uint32_t)(d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto rd64 = [&](size_t p) -> int64_t {
        if (p + 8 > d.size()) return 0;
        int64_t v = 0; std::memcpy(&v, d.data() + p, 8); return v;
    };
    size_t pos = rd16(6);
    while (pos + 16 <= d.size() && std::memcmp(d.data() + pos, "AMSP", 4) != 0) {
        uint32_t csz = rd32(pos + 4); size_t next = pos + 8 + csz; if (next <= pos) return ids; pos = next;
    }
    if (pos + 72 > d.size() || std::memcmp(d.data() + pos, "AMSP", 4) != 0) return ids;

    uint32_t msCount = rd32(pos + 60);
    int64_t  msRel   = rd64(pos + 64);
    if (msRel == 0 || msCount == 0 || msCount > 500000) return ids;
    size_t ms0 = (pos + 64) + (size_t)msRel;

    const size_t MS_STRIDE = 326, FN_STRIDE = 36;
    const size_t kMaxSounds = 512;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < msCount && ids.size() < kMaxSounds; ++i) {
        size_t elem = ms0 + (size_t)i * MS_STRIDE;
        if (elem + 72 > d.size()) break;
        uint32_t fnCount = rd32(elem + 60);
        int64_t  fnRel   = rd64(elem + 64);
        if (fnRel == 0 || fnCount == 0 || fnCount > 4096) continue;
        size_t fn0 = (elem + 64) + (size_t)fnRel;
        for (uint32_t j = 0; j < fnCount && ids.size() < kMaxSounds; ++j) {
            size_t fe = fn0 + (size_t)j * FN_STRIDE;
            if (fe + 32 > d.size()) break;
            int64_t rel = rd64(fe + 24);
            if (rel == 0) continue;
            size_t rec = (fe + 24) + (size_t)rel;
            uint32_t lo = rd16(rec), hi = rd16(rec + 2);
            if (lo < 0x100 || hi < 0x100) continue;
            uint32_t fid = (uint32_t)(0xFF00L * hi + lo - 0xFF00FF);
            if (fid && seen.insert(fid).second) ids.push_back(fid);
        }
    }
    return ids;
}

// Load the AMSP's referenced ASND sounds into result.audio_clips. Returns true if any
// playable sound was found. Builds a fileId->baseId map once (O(1) lookups).
bool build_amsp_audio(const std::vector<uint8_t>& amsp, const std::string& dat_path, ExtractedEntry& result) {
    std::vector<uint32_t> ids = parse_amsp_sound_ids(amsp);
    if (ids.empty() || dat_path.empty()) return false;
    Gw2Dat dat;
    try { load_dat_file(dat, dat_path); } catch (const std::exception&) { return false; }
    std::unordered_map<uint32_t, uint32_t> fid2base;
    fid2base.reserve(dat.mft_file_id_data_list.size() * 2);
    for (const auto& e : dat.mft_file_id_data_list) fid2base[e.file_id] = e.base_id;

    for (uint32_t fid : ids) {
        auto it = fid2base.find(fid);
        if (it == fid2base.end()) continue;
        uint32_t base = it->second;
        if (base == 0 || base - 1 >= dat.mft_data_list.size()) continue;
        std::vector<uint8_t> bytes;
        try {
            const MftData& e = dat.mft_data_list[base - 1];
            std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
            std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
            if (e.compression_flag == 0) bytes = std::move(stripped);
            else if (stripped.size() >= 8) {
                uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
                bytes = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
            }
        } catch (const std::exception&) { continue; }
        if (bytes.empty()) continue;
        for (auto& c : castlemist::audio::extract(bytes.data(), bytes.size())) {
            AudioClipCPU cc; cc.codec = castlemist::audio::codecName(c.codec); cc.data = std::move(c.data);
            result.audio_clips.push_back(std::move(cc));
        }
    }
    return !result.audio_clips.empty();
}

} // namespace castlemist::extract
