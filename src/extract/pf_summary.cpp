/// @file
/// @brief Readable summaries for the small non-renderable packfiles (eula, ABIX, text packs).

#include "internal.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "castlemist/core/packfile.h"
#include "castlemist/format/strs_keys.h"
#include "castlemist/format/strs_view.h"

namespace castlemist::extract {

// ---- eula / ABIX(BIDX) / CSCN / text-pack summaries -----------------------
// These PF packfiles aren't renderable assets, but their small structs decode
// into readable text. All use ArenaNet self-relative pointers whose WIDTH comes
// from the PF version: (pfVersion & 4) => 64-bit pointers (v5), else 32-bit (v1,
// e.g. ABIX). array_ptr = {u32 count, ptr rel}; structs are tightly packed (no
// alignment padding). filename ref -> fileId = 0xFF00*hi + lo - 0xFF00FF.
// Layouts verified against gw2_packfile.json + live parse (gw2mcp).

// eula (PackEulaV0): the user-agreement / privacy URLs, per language.
std::wstring parse_eula_summary(const std::vector<uint8_t>& d) {
    castlemist::core::PackfileReader r; size_t root;
    if (!castlemist::core::is_packfile(d, "eula") || !castlemist::core::open_chunk(d, "eula", r, root)) return {};
    size_t off; uint32_t cnt = r.array(root, off);
    uint32_t version = r.u16(root + r.array_ptr_size()) & 0xFF; // Version byte
    const size_t stride = 1 + r.pointer_size();                  // PackEulaLanguageV0
    std::wstring s = L"GW2 End-User License / Agreement (eula / PackEulaV0)\r\n";
    s += L"Version " + std::to_wstring(version) + L",  " + std::to_wstring(cnt) + L" language(s)\r\n\r\n";
    uint32_t nn = cnt < 32 ? cnt : 32;
    for (uint32_t i = 0; i < nn; ++i) {
        size_t e = off + (size_t)i * stride;
        if (e + stride > d.size()) break;
        uint32_t lang = r.u16(e) & 0xFF;
        std::wstring text = r.wstring(e + 1);
        s += L"[" + std::wstring(castlemist::core::language_name(lang)) + L"]\r\n" + text + L"\r\n\r\n";
    }
    return s;
}

// ABIX (BankIndexDataV0): audio-bank index -- per-language lists of bank ASND
// filenames (fileIds). Note ABIX is PF v1 => 32-bit pointers.
std::wstring parse_abix_summary(const std::vector<uint8_t>& d) {
    castlemist::core::PackfileReader r; size_t root;
    if (!castlemist::core::is_packfile(d, "ABIX") || !castlemist::core::open_chunk(d, "BIDX", r, root)) return {};
    size_t langOff; uint32_t langCnt = r.array(root, langOff);
    std::wstring s = L"GW2 audio bank index (ABIX / BankIndexDataV0)\r\n";
    s += std::to_wstring(langCnt) + L" language slot(s)";
    s += (r.pointer_size() == 4) ? L"  (PF v1, 32-bit ptrs)\r\n\r\n" : L"  (PF v5, 64-bit ptrs)\r\n\r\n";
    const size_t langStride = r.array_ptr_size();               // BankLanguageDataV0 = one array_ptr
    uint32_t total = 0;
    uint32_t nn = langCnt < 24 ? langCnt : 24;
    for (uint32_t i = 0; i < nn; ++i) {
        size_t le = langOff + (size_t)i * langStride;
        size_t fnOff; uint32_t fnCnt = r.array(le, fnOff);
        wchar_t hdr[64];
        swprintf(hdr, 64, L"[%ls]  %u bank file(s)\r\n", castlemist::core::language_name(i), fnCnt);
        s += hdr;
        uint32_t fn = fnCnt < 64 ? fnCnt : 64;
        for (uint32_t j = 0; j < fn; ++j) {
            size_t fe = fnOff + (size_t)j * r.pointer_size();    // BankFileNameDataV0 = one filename
            uint32_t fid = r.fileref(fe);
            if (fid) { s += L"    fileId " + std::to_wstring(fid) + L"\r\n"; total++; }
        }
    }
    if (total == 0) s += L"(no bank files referenced in this build)\r\n";
    return s;
}

// Resolve a global textId to its (English) localized string, using the same
// txtm-derived textbase + RC4 keys the strs preview uses (gw2skeys). `dat` supplies
// the strs bytes; `cache` dedupes strs files across the many lookups in one summary.
// Returns empty when unresolvable (no textbase/keys loaded, out of range, or the
// record is RC4-packed and its key wasn't captured).
std::wstring resolve_textid_string(Gw2Dat& dat, uint32_t textId,
                                   std::unordered_map<uint32_t, std::vector<uint8_t>>& cache) {
    uint32_t fid = castlemist::skeys::fileid_for_textid(textId);
    if (!fid) return {};
    long long base = castlemist::skeys::base_for_fileids(std::vector<uint32_t>{fid});
    if (base < 0 || textId < base) return {};
    auto it = cache.find(fid);
    if (it == cache.end()) {
        std::vector<uint8_t> bytes;
        try { bytes = load_modl_bytes_by_fileid(dat, fid); } catch (const std::exception&) {}
        it = cache.emplace(fid, std::move(bytes)).first;
    }
    const std::vector<uint8_t>& b = it->second;
    if (b.size() < 4) return {};
    return castlemist::skeys::decode_record_text(b.data(), b.size(), base, (uint32_t)(textId - base));
}

// One-line, quote-wrapped rendering of a resolved textId (or "" if unresolved),
// with newlines/CRs collapsed so it stays on a single summary line.
std::wstring textid_inline(Gw2Dat& dat, uint32_t textId,
                           std::unordered_map<uint32_t, std::vector<uint8_t>>& cache) {
    std::wstring s = resolve_textid_string(dat, textId, cache);
    if (s.empty()) return {};
    std::wstring one;
    for (wchar_t c : s) { if (c == L'\r' || c == L'\n') { one += L' '; } else one += c; }
    if (one.size() > 80) { one.resize(77); one += L"..."; }
    return L"  \"" + one + L"\"";
}

// txt* text-pack family: the localization layer that cntc content textIds resolve
// through. txtm = manifest (textId -> strs file+slot), txtv = voices (textId ->
// voice audio id), txtV/vari = variants (textId -> gendered/variant textIds).
// `dat_path` (when non-empty and string keys/textbase are loaded) lets txtv/txtV
// resolve each shown textId to its actual localized string.
std::wstring parse_textpack_summary(const std::vector<uint8_t>& d, const std::string& dat_path) {
    castlemist::core::PackfileReader r; size_t root;
    std::wstring s;
    // Optional live textId -> string resolution: needs the dat (for the strs bytes)
    // plus the loaded textbase + RC4 key tables. Opened lazily on first lookup.
    Gw2Dat resDat;
    bool resReady = false;
    std::unordered_map<uint32_t, std::vector<uint8_t>> strsCache;
    auto resolver = [&](uint32_t textId) -> std::wstring {
        if (dat_path.empty() || !castlemist::skeys::textbase_ready()) return {};
        if (!resReady) {
            try { load_dat_file(resDat, dat_path); resReady = true; }
            catch (const std::exception&) { return {}; }
        }
        return textid_inline(resDat, textId, strsCache);
    };
    if (castlemist::core::is_packfile(d, "txtm") && castlemist::core::open_chunk(d, "txtm", r, root)) {           // TextPackManifest
        uint32_t stringsPerFile = r.u32(root);
        size_t off; uint32_t langCnt = r.array(root + 4, off);
        uint32_t totalFiles = 0;
        const size_t langStride = r.array_ptr_size();                          // TextPackLanguage
        for (uint32_t i = 0; i < langCnt && i < 64; ++i) {
            size_t fnOff; uint32_t fnCnt = r.array(off + (size_t)i * langStride, fnOff);
            totalFiles += fnCnt;
        }
        s  = L"GW2 text manifest (txtm / TextPackManifest)\r\n";
        s += std::to_wstring(stringsPerFile) + L" strings per file, " + std::to_wstring(langCnt) +
             L" language(s), " + std::to_wstring(totalFiles) + L" strs file(s) referenced.\r\n\r\n"
             L"Maps a global textId to a localized string: strs file = textId / stringsPerFile,\r\n"
             L"string slot = textId % stringsPerFile. This is what cntc content names/descriptions\r\n"
             L"resolve through (see txtv for voices, txtV for gender variants).\r\n";
        return s;
    }
    if (castlemist::core::is_packfile(d, "txtv") && castlemist::core::open_chunk(d, "txtv", r, root)) {           // TextPackVoices
        size_t off; uint32_t cnt = r.array(root, off);
        s  = L"GW2 voice map (txtv / TextPackVoices)\r\n";
        s += std::to_wstring(cnt) + L" textId -> voiceId mapping(s).\r\n\r\n"
             L"Resolves a spoken line's textId to its voice-audio id (ASND/ABNK).\r\n\r\n";
        uint32_t nn = cnt < 40 ? cnt : 40;
        for (uint32_t i = 0; i < nn; ++i) {
            size_t e = off + (size_t)i * 8; // {dword textId; dword voiceId}
            uint32_t tid = r.u32(e);
            wchar_t line[64];
            swprintf(line, 64, L"  textId %-10u -> voiceId %-10u", tid, r.u32(e + 4));
            s += line;
            s += resolver(tid);   // the actual spoken line, when resolvable
            s += L"\r\n";
        }
        if (cnt > nn) s += L"  ...\r\n";
        return s;
    }
    if (castlemist::core::is_packfile(d, "txtV") && castlemist::core::open_chunk(d, "vari", r, root)) {           // TextPackVariants
        size_t off; uint32_t cnt = r.array(root, off);
        s  = L"GW2 text variants (txtV / TextPackVariants)\r\n";
        s += std::to_wstring(cnt) + L" textId -> variant textId list(s).\r\n\r\n"
             L"Resolves a base textId to gender/race/profession variant lines that cntc\r\n"
             L"content references (chosen per character at display time).\r\n\r\n";
        const size_t stride = 4 + r.array_ptr_size();          // {dword textId; array_ptr variantTextIds}
        uint32_t nn = cnt < 40 ? cnt : 40;
        for (uint32_t i = 0; i < nn; ++i) {
            size_t e = off + (size_t)i * stride;
            size_t vo; uint32_t vc = r.array(e + 4, vo);
            uint32_t tid = r.u32(e);
            wchar_t line[64];
            swprintf(line, 64, L"  textId %-10u -> %-2u variant(s)", tid, vc);
            s += line;
            s += resolver(tid);   // the base line the variants are cast from
            s += L"\r\n";
            // The variant textIds themselves (each an alternate line), resolved too.
            for (uint32_t v = 0; v < vc && v < 4; ++v) {
                uint32_t vtid = r.u32(vo + (size_t)v * 4);
                std::wstring vs = resolver(vtid);
                if (vs.empty()) continue;
                swprintf(line, 64, L"      variant %u: %u", v, vtid);
                s += line; s += vs; s += L"\r\n";
            }
        }
        if (cnt > nn) s += L"  ...\r\n";
        return s;
    }
    return {};
}


} // namespace castlemist::extract
