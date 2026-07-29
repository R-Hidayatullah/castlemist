/// @file
/// @brief CINP/CSCN cinematics: subtitles, referenced movies and scene summary.

#include "internal.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <span>
#include <unordered_map>

#include "castlemist/native/cmp_decompress_method0.hpp"

#include "castlemist/core/packfile.h"
#include "castlemist/media/video_player.h"

namespace castlemist::extract {

// CSCN cinematic scene (SceneDataV0..V36). The scene struct has 37 versions and is
// large, so we report the scene version + a structural chunk listing rather than a
// full field decode.
// Pulls the timed dialogue out of a CINP's CSCN chunk.
//
// Layout (V30..V36 -- every shipped CINP is V36; all strides below were checked
// against the template AND against a real pack, base 482659):
//   SceneData        { qword startingSequence; array_ptr sequence; ResourceData resource }
//   ResourceData     { array_ptr ambientLightResource, fileNameRef, script,
//                      textResource, speciesResource }
//   TextResourceData { qword name; dword index; dword voiceId; array_ptr textEntry }   stride 16+AP
//   TextEntryData    { wchar_ptr text; byte language }                                  stride ptr+1
//   SequenceData     { token64 name, playScript, updateScript; filename environmentMap;
//                      wchar_ptr map, clientMap; float length; dword flags;
//                      array_ptr trackGroup }                          trackGroup @ 24+3*ptr+8
//   TrackGroupData   { token64 name; dword flags; array_ptr prop, track; byte type }    track @ 12+AP
//   TrackData        { qword name; array_ptr curveKey, flagKey, triggerKey; byte type } trigger @ 8+2*AP
//   TriggerKeyData   { float time; byte flags[4]; token64 token1, token2; float v[4] }  stride 40
// The link is triggerKey.token1 == textResource.name.
// CSCN ships in many versions (v6..v36 in this dat; v36=919, v33=450, v31=35 of
// 1446 packs) and the layout is NOT stable, so the offsets have to be derived
// per version -- assuming v36 everywhere silently mis-parses the 450 v33 packs:
//   ResourceData  v30-33: { dword crc; array_ptr ambientLight, fileNameRef, script, textResource }
//                 v34-36: { array_ptr ambientLight, fileNameRef, script, textResource, speciesResource }
//                 => textResource sits 4 bytes later in the older ones.
//   TriggerKeyData v30-31: { qword name; array_ptr triggerMarker }  -- a different thing entirely
//                  v32+  : { float time; byte flags[4]; token64 token1, token2; float v[4] }  (40 B)
//   PropertyData   v30-31: { qword name; qword tokenValue; float floatValue }  (20 B, no fileref)
//                  v32+  : { qword value; filename pathVal; byte type }
// Everything else (SequenceData / TrackGroupData / TrackData / TextResourceData /
// TextEntryData / FlagKeyData) is identical across v30..v36.
struct CscnLayout {
    int      version = 0;
    bool     ok = false;
    size_t   seq_arr = 0;   // array_ptr<SequenceData>
    size_t   text_arr = 0;  // array_ptr<TextResourceData>
    size_t   seq_stride = 0, group_stride = 0, track_stride = 0, prop_stride = 0;
    size_t   text_stride = 0, text_entry_stride = 0;
    size_t   group_off_in_seq = 0;
    bool     has_trigger_keys = false; // v32+ 40-byte trigger records
    bool     has_prop_fileref = false; // v32+ PropertyData carries a filename
};

CscnLayout cscn_layout(const castlemist::core::PackfileReader& r, size_t root) {
    CscnLayout L;
    L.version = static_cast<int>(r.u16(root - 16 + 8)); // chunk version @ chunkStart+8
    if (L.version < 30) return L;                       // pre-v30 shapes not modelled
    const size_t AP = static_cast<size_t>(r.array_ptr_size());
    const size_t PTR = static_cast<size_t>(r.pointer_size());
    L.seq_arr = root + 8;                       // after qword startingSequence
    size_t res = L.seq_arr + AP;                // ResourceData follows
    if (L.version <= 33) res += 4;              // ...preceded by its crc dword
    L.text_arr = res + 3 * AP;                  // ambientLight, fileNameRef, script, -> textResource
    L.seq_stride = 24 + 3 * PTR + 8 + AP;
    L.group_off_in_seq = 24 + 3 * PTR + 8;
    L.group_stride = 12 + 2 * AP + 1;
    L.track_stride = 8 + 3 * AP + 1;
    L.text_stride = 16 + AP;
    L.text_entry_stride = PTR + 1;
    L.has_trigger_keys = L.version >= 32;
    L.has_prop_fileref = L.version >= 32;
    L.prop_stride = L.has_prop_fileref ? (8 + PTR + 1) : 20;
    L.ok = true;
    return L;
}

bool parse_cscn_subtitles(const std::vector<uint8_t>& d, std::vector<SubtitleLine>& out) {
    castlemist::core::PackfileReader r;
    size_t root = 0;
    if (!castlemist::core::open_chunk(d, "CSCN", r, root)) return false;
    CscnLayout L = cscn_layout(r, root);
    if (!L.ok) return false;
    const int AP = r.array_ptr_size();
    const size_t pSeq = L.seq_arr;
    const size_t pText = L.text_arr;

    // token -> the line's text. Prefer language 0 (the authored source text);
    // shipped packs only carry that one, localized text lives in the strs packs.
    std::map<uint64_t, std::wstring> by_token;
    size_t t_off = 0;
    uint32_t t_count = r.array(pText, t_off);
    const size_t kTextRes = L.text_stride;
    const size_t kTextEntry = L.text_entry_stride;
    for (uint32_t i = 0; i < t_count && i < 8192; ++i) {
        size_t e = t_off + i * kTextRes;
        if (e + kTextRes > r.size()) break;
        uint64_t name = static_cast<uint64_t>(r.u32(e)) | (static_cast<uint64_t>(r.u32(e + 4)) << 32);
        size_t e_off = 0;
        uint32_t e_count = r.array(e + 16, e_off);
        for (uint32_t k = 0; k < e_count && k < 16; ++k) {
            size_t te = e_off + k * kTextEntry;
            if (te + kTextEntry > r.size()) break;
            std::wstring s = r.wstring(te);
            if (!s.empty() && by_token.find(name) == by_token.end()) by_token[name] = std::move(s);
        }
    }
    if (by_token.empty()) return false;

    // Walk sequence -> trackGroup. A subtitle group holds TWO tracks that have to
    // be read together:
    //   * a trigger track  -- each key LOADS a line (time + the text's token)
    //   * a flagKey track  -- a 0/1 step curve that says when a line is VISIBLE
    // The trigger time is NOT when the text appears: in the Heart of Thorns
    // trailer line 1 is loaded at 0.08 s but only shown 5.70-7.10 s. Pairing the
    // two gives a real start/end cue, exactly like an .srt entry. If a group has
    // triggers but no flag track we fall back to "until the next trigger".
    auto rdf = [&](size_t p) {
        uint32_t raw = r.u32(p);
        float f = 0.0f;
        std::memcpy(&f, &raw, 4);
        return static_cast<double>(f);
    };

    if (!L.has_trigger_keys) return false; // v30/31 triggers are a different record
    size_t s_off = 0;
    uint32_t s_count = r.array(pSeq, s_off);
    const size_t kSeq = L.seq_stride;
    const size_t kGroup = L.group_stride;
    const size_t kTrack = L.track_stride;
    const size_t kTrigger = 40;
    const size_t kFlag = 8; // FlagKeyData { float time; float value }

    for (uint32_t si = 0; si < s_count && si < 256; ++si) {
        size_t seq = s_off + si * kSeq;
        if (seq + kSeq > r.size()) break;
        size_t g_off = 0;
        uint32_t g_count = r.array(seq + L.group_off_in_seq, g_off);
        for (uint32_t gi = 0; gi < g_count && gi < 512; ++gi) {
            size_t grp = g_off + gi * kGroup;
            if (grp + kGroup > r.size()) break;

            std::vector<std::pair<double, std::wstring>> loads; // (time, text)
            std::vector<std::pair<double, double>> flags;       // (time, value)
            size_t tr_off = 0;
            uint32_t tr_count = r.array(grp + 12 + AP, tr_off);
            for (uint32_t ti = 0; ti < tr_count && ti < 512; ++ti) {
                size_t trk = tr_off + ti * kTrack;
                if (trk + kTrack > r.size()) break;
                size_t k_off = 0;
                uint32_t k_count = r.array(trk + 8 + 2 * AP, k_off);
                for (uint32_t ki = 0; ki < k_count && ki < 4096; ++ki) {
                    size_t key = k_off + ki * kTrigger;
                    if (key + kTrigger > r.size()) break;
                    double time = rdf(key);
                    uint64_t token = static_cast<uint64_t>(r.u32(key + 8)) |
                                     (static_cast<uint64_t>(r.u32(key + 12)) << 32);
                    auto it = by_token.find(token);
                    if (it != by_token.end() && time >= 0.0 && time < 100000.0) {
                        loads.emplace_back(time, it->second);
                    }
                }
                size_t f_off = 0;
                uint32_t f_count = r.array(trk + 8 + AP, f_off);
                for (uint32_t fi = 0; fi < f_count && fi < 4096; ++fi) {
                    size_t key = f_off + fi * kFlag;
                    if (key + kFlag > r.size()) break;
                    flags.emplace_back(rdf(key), rdf(key + 4));
                }
            }
            if (loads.empty()) continue;
            std::sort(loads.begin(), loads.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            std::sort(flags.begin(), flags.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            // Text of the last line loaded at or before `t`.
            auto text_at = [&](double t) -> const std::wstring& {
                size_t best = 0;
                for (size_t i = 0; i < loads.size(); ++i) {
                    if (loads[i].first <= t + 1e-4) best = i; else break;
                }
                return loads[best].second;
            };

            bool used_flags = false;
            for (size_t i = 0; i < flags.size(); ++i) {
                if (flags[i].second < 0.5) continue;             // only 0 -> 1 edges start a cue
                if (i > 0 && flags[i - 1].second >= 0.5) continue; // already on
                double start = flags[i].first;
                double end = start;
                for (size_t j = i + 1; j < flags.size(); ++j) {
                    if (flags[j].second < 0.5) { end = flags[j].first; break; }
                }
                if (end <= start) continue;
                out.push_back(SubtitleLine{start, end, static_cast<int>(si), text_at(start)});
                used_flags = true;
            }
            if (!used_flags) {
                // No visibility curve in this group -- show each line until the
                // next one loads (capped, so a trailing line doesn't sit forever).
                constexpr double kHold = 6.0;
                for (size_t i = 0; i < loads.size(); ++i) {
                    double end = (i + 1 < loads.size()) ? loads[i + 1].first : loads[i].first + kHold;
                    end = std::min(end, loads[i].first + kHold);
                    out.push_back(SubtitleLine{loads[i].first, end, static_cast<int>(si), loads[i].second});
                }
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const SubtitleLine& a, const SubtitleLine& b) {
        return a.sequence != b.sequence ? a.sequence < b.sequence : a.time < b.time;
    });
    return !out.empty();
}

// Every file this cinematic references, in timeline order: each TrackGroup's
// PropertyData carries a `pathVal` fileref, and that is where the CSCN names the
// movie it plays (plus sound scripts and the like). Layout notes as above.
void collect_cscn_filerefs(const std::vector<uint8_t>& d, std::vector<std::pair<uint32_t, int>>& out) {
    castlemist::core::PackfileReader r;
    size_t root = 0;
    if (!castlemist::core::open_chunk(d, "CSCN", r, root)) return;
    CscnLayout L = cscn_layout(r, root);
    if (!L.ok || !L.has_prop_fileref) return; // pre-v32 PropertyData has no filename
    const size_t kSeq = L.seq_stride;
    const size_t kGroup = L.group_stride;
    const size_t kProp = L.prop_stride;
    size_t s_off = 0;
    uint32_t s_count = r.array(L.seq_arr, s_off);
    for (uint32_t si = 0; si < s_count && si < 256; ++si) {
        size_t seq = s_off + si * kSeq;
        if (seq + kSeq > r.size()) break;
        size_t g_off = 0;
        uint32_t g_count = r.array(seq + L.group_off_in_seq, g_off);
        for (uint32_t gi = 0; gi < g_count && gi < 512; ++gi) {
            size_t grp = g_off + gi * kGroup;
            if (grp + kGroup > r.size()) break;
            size_t p_off = 0;
            uint32_t p_count = r.array(grp + 12, p_off); // prop comes before track
            for (uint32_t pi = 0; pi < p_count && pi < 256; ++pi) {
                size_t prop = p_off + pi * kProp;
                if (prop + kProp > r.size()) break;
                uint32_t fid = r.fileref(prop + 8);
                bool seen = false;
                for (const auto& p : out) if (p.first == fid) { seen = true; break; }
                // The sequence index is kept because each sequence plays its own
                // movie -- that is how a movie's subtitles are told apart.
                if (fid && !seen) out.emplace_back(fid, static_cast<int>(si));
            }
        }
    }
}

// Sequence / dialogue-slot counts, so a CINP we cannot play still describes itself.
void cscn_counts(const std::vector<uint8_t>& d, int& version, uint32_t& sequences, uint32_t& textRes) {
    version = 0; sequences = 0; textRes = 0;
    castlemist::core::PackfileReader r; size_t root = 0;
    if (!castlemist::core::open_chunk(d, "CSCN", r, root)) return;
    CscnLayout L = cscn_layout(r, root);
    version = L.version;
    if (!L.ok) return;
    size_t off = 0;
    sequences = r.array(L.seq_arr, off);
    textRes = r.array(L.text_arr, off);
    if (sequences > 100000) sequences = 0;   // guard against an unmodelled layout
    if (textRes > 100000) textRes = 0;
}

// Cheap "what is this entry?" probe: read only the head of the entry off disk and
// decompress 64 bytes. Enough for a magic test, and it does NOT pull a ~70 MB
// movie into memory just to find out it is a movie.
bool peek_is_bink_fileid(Gw2Dat& dat, uint32_t file_id) {
    uint32_t base = get_by_base_id(dat, file_id);
    if (!base || base - 1 >= dat.mft_data_list.size()) return false;
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> head;
    try {
        std::ifstream f(dat.file_info.file_path, std::ios::binary);
        if (!f) return false;
        size_t want = std::min<size_t>(e.size, 0x10000 + 64);
        std::vector<uint8_t> raw(want);
        f.seekg(static_cast<std::streamoff>(e.offset));
        if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(want))) return false;
        std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(raw);
        if (e.compression_flag != 0) {
            if (stripped.size() < 8) return false;
            head = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), 64);
        } else {
            head.assign(stripped.begin(), stripped.begin() + std::min<size_t>(64, stripped.size()));
        }
    } catch (const std::exception&) {
        return false;
    }
    return castlemist::vid::is_video(head.data(), head.size());
}

std::wstring parse_cscn_summary(const std::vector<uint8_t>& d) {
    if (!castlemist::core::is_packfile(d, "CSCN")) return {};
    castlemist::core::PackfileReader r; size_t root;
    uint32_t ver = 0;
    if (castlemist::core::open_chunk(d, "CSCN", r, root)) ver = r.u16(root - 16 + 8); // chunk version @ chunkStart+8
    std::wstring s = L"GW2 cinematic scene (CSCN / SceneDataV" + std::to_wstring(ver) + L")\r\n";
    s += L"A cutscene/scene packfile: entities, camera/animation tracks, physics and\r\n"
         L"referenced model/skeleton sub-files. Chunk layout:\r\n\r\n";
    s += castlemist::core::chunk_listing(d);
    return s;
}


} // namespace castlemist::extract

// ---- public API (declared in castlemist/extract/entry_extractor.h) ----

using namespace castlemist::extract;

uint32_t find_video_subtitles(const std::string& dat_path, const std::vector<uint32_t>& cinp_base_ids,
                              uint32_t video_file_id, std::vector<SubtitleLine>& out) {
    out.clear();
    if (dat_path.empty() || cinp_base_ids.empty() || video_file_id == 0) return 0;

    // ArenaNet file references are a {u16 lo, u16 hi} pair decoded as
    //   fileId = 0xFF00*hi + lo - 0xFF00FF
    // (same rule as castlemist::core::PackfileReader::fileref). Invert it to get the 4 bytes to look for --
    // far cheaper than parsing every CINP's filename table.
    uint64_t v = static_cast<uint64_t>(video_file_id) + 0xFF00FFull;
    uint32_t hi = static_cast<uint32_t>(v / 0xFF00ull);
    uint32_t lo = static_cast<uint32_t>(v % 0xFF00ull);
    if (hi > 0xFFFF || lo > 0xFFFF) return 0;
    const uint8_t pat[4] = {static_cast<uint8_t>(lo & 0xFF), static_cast<uint8_t>(lo >> 8),
                            static_cast<uint8_t>(hi & 0xFF), static_cast<uint8_t>(hi >> 8)};

    Gw2Dat dat;
    try {
        load_dat_file(dat, dat_path);
    } catch (const std::exception&) {
        return 0;
    }

    for (uint32_t base_id : cinp_base_ids) {
        uint32_t idx = base_id - 1;
        if (idx >= dat.mft_data_list.size()) continue;
        std::vector<uint8_t> dec;
        try {
            std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, dat.mft_data_list[idx]);
            std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(raw);
            if (dat.mft_data_list[idx].compression_flag != 0) {
                if (stripped.size() < 8) continue;
                uint32_t usz = read_u32_le(stripped, 4);
                dec = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
            } else {
                dec = std::move(stripped);
            }
        } catch (const std::exception&) {
            continue;
        }
        bool referenced = false;
        for (size_t i = 0; i + 4 <= dec.size(); ++i) {
            if (dec[i] == pat[0] && dec[i + 1] == pat[1] && dec[i + 2] == pat[2] && dec[i + 3] == pat[3]) {
                referenced = true;
                break;
            }
        }
        if (!referenced) continue;
        if (parse_cscn_subtitles(dec, out)) return base_id;
        out.clear(); // referenced but carried no dialogue -- keep looking
    }
    return 0;
}

std::vector<uint8_t> load_video_by_fileid(const std::string& dat_path, uint32_t file_id) {
    if (dat_path.empty() || file_id == 0) return {};
    try {
        Gw2Dat dat;
        load_dat_file(dat, dat_path);
        uint32_t base = get_by_base_id(dat, file_id);
        if (!base || base - 1 >= dat.mft_data_list.size()) return {};
        const MftData& e = dat.mft_data_list[base - 1];
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(raw);
        std::vector<uint8_t> dec;
        if (e.compression_flag != 0) {
            if (stripped.size() < 8) return {};
            dec = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8),
                                             read_u32_le(stripped, 4));
        } else {
            dec = std::move(stripped);
        }
        if (!castlemist::vid::is_video(dec.data(), dec.size())) return {};
        return dec;
    } catch (const std::exception&) {
        return {};
    }
}
