// cmp_compress_method0.hpp -- single-header C++20 library
//
// ENCODER for ArenaNet's CmpDecompress Method 0 (Huffman + LZ77). This is the
// exact inverse of cmp_decompress_method0.hpp: it produces a bitstream that the
// game's CmpDecompress_Method0_Inflate (0x140D96FC0) -- and our own
// castlemist::cmp::decompress_method0 -- decode back to the original bytes.
//
// The tricky part of writing a compatible encoder is that ArenaNet's canonical
// Huffman code assignment is unusual (codes assigned DESCENDING per length group
// with an unsigned wrap-around counter). We do NOT reinvent it: we reuse the
// decoder's *exact* assignment routine (see assign_codes below, a line-for-line
// mirror of HuffTable::build) to map symbol -> (code,length). Because the encode
// table and the decode table are built by the same algorithm from the same code
// lengths, every code we emit round-trips by construction.
//
// The bit ordering is the mirror of castlemist::cmp::detail::BitReader: bits are packed
// MSB-first into 32-bit little-endian words.
//
// Pipeline (plaintext -> Gw2.dat entry):
//   1. LZ77-tokenize the input (hash-chain matcher, 40 KB window).
//   2. Split tokens into fixed 4096-token blocks; for each block build two
//      canonical Huffman tables (literal/length + distance) via length-limited
//      Huffman (package-merge), emit the tables, then the tokens.
//   3. Prepend the 4-bit Method(0) + 4-bit (minMatchAdd-1) stream header.
//   4. (compress_entry) prepend the 8-byte {flag,uncompressedSize} header and
//      re-insert the periodic CRC32C framing (inverse of strip_crc32). The CRC
//      words are real CRC-32C (Castagnoli) checksums -- the exact variant GW2
//      embeds, verified against live Gw2.dat entries -- so the output is a
//      byte-faithful on-disk entry payload that passes the client's validation.
//
// Usage:
//   std::vector<uint8_t> raw  = ...;                       // original bytes
//   std::vector<uint8_t> bits = castlemist::cmp::compress_method0(raw);   // bare bitstream
//   std::vector<uint8_t> ent  = castlemist::cmp::compress_entry(raw);     // full framed entry
//   // verify: castlemist::cmp::decompress_entry(ent) == raw
//
#ifndef GW2CMP_COMPRESS_METHOD0_HPP
#define GW2CMP_COMPRESS_METHOD0_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "castlemist/native/cmp_decompress_method0.hpp" // shares the static tables + decode_error

namespace castlemist::cmp {

namespace detail {

// ---- bit writer: exact inverse of BitReader (MSB-first, 32-bit LE words) -----
class BitWriter {
public:
    // Append the low `n` bits of `value`, most-significant bit first.
    void write(uint32_t value, int n) {
        if (n == 0) return;
        uint64_t v = (n >= 32) ? value : (value & ((uint32_t{1} << n) - 1));
        acc_ = (acc_ << n) | v;
        bits_ += n;
        while (bits_ >= 32) {
            uint32_t w = static_cast<uint32_t>(acc_ >> (bits_ - 32));
            emit_word(w);
            bits_ -= 32;
            acc_ &= (bits_ == 0) ? 0 : ((uint64_t{1} << bits_) - 1);
        }
    }

    // Flush any partial word (MSB-aligned, zero-padded low) and return the bytes.
    std::vector<uint8_t> finish() {
        if (bits_ > 0) {
            uint32_t w = static_cast<uint32_t>(acc_ << (32 - bits_));
            emit_word(w);
            acc_ = 0;
            bits_ = 0;
        }
        return std::move(out_);
    }

private:
    void emit_word(uint32_t w) {
        out_.push_back(static_cast<uint8_t>(w & 0xFF));
        out_.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
        out_.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
        out_.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
    }

    std::vector<uint8_t> out_;
    uint64_t acc_ = 0;
    int bits_ = 0;
};

// ---- meta-huffman encoder: inverse of decode_meta -----------------------------
// decode_meta maps a variable-length code -> META_VALUES[idx]. Each META_TABLE
// row covers a contiguous block of indices [prev_offset+1 .. offset] with a fixed
// bit length; within a row idx = offset - rel and the consumed code bits equal
// (mask >> (32-bitlen)) + rel. We invert that into value -> {code,bitlen}.
struct MetaEnc { uint32_t code; uint8_t bitlen; };

inline const std::array<MetaEnc, 256>& meta_encoder() {
    static const std::array<MetaEnc, 256> tbl = [] {
        std::array<MetaEnc, 256> t{};
        int prev = -1;
        for (const auto& row : META_TABLE) {
            int off = row.offset;
            int bitlen = row.bitlen;
            uint32_t codebase = (bitlen >= 32) ? 0u : (row.mask >> (32 - bitlen));
            for (int idx = prev + 1; idx <= off && idx < 256; ++idx) {
                uint32_t rel = static_cast<uint32_t>(off - idx);
                uint8_t val = META_VALUES[idx];
                t[val] = { codebase + rel, static_cast<uint8_t>(bitlen) };
            }
            prev = off;
        }
        return t;
    }();
    return tbl;
}

// ---- canonical Huffman code assignment: exact mirror of HuffTable::build -------
// Given per-symbol code lengths, returns per-symbol code values assigned by the
// identical descending/wrap-around scheme the decoder reconstructs. Emitting
// codes[s] as lengths[s] bits therefore decodes back to symbol s.
inline std::vector<uint32_t> assign_codes(const std::vector<uint8_t>& lengths) {
    uint32_t total = static_cast<uint32_t>(lengths.size());
    uint8_t max_len = lengths.empty() ? 0 : *std::max_element(lengths.begin(), lengths.end());

    constexpr uint32_t NIL = 0xFFFFFFFFu;
    std::vector<uint32_t> v66(static_cast<size_t>(max_len) + 1, NIL);
    std::vector<uint32_t> v63(total, 0);
    long idx = static_cast<long>(total) - 1;
    while (idx >= 0) {
        uint8_t l = lengths[static_cast<size_t>(idx)];
        if (l != 0) {
            v63[static_cast<size_t>(idx)] = v66[l];
            v66[l] = static_cast<uint32_t>(idx);
        }
        idx--;
    }

    std::vector<uint32_t> codes(total, 0);
    uint32_t v35 = 0;
    for (uint32_t length = 0; length <= max_len; ++length) {
        uint32_t sym = v66[length];
        while (sym != NIL && v35 < (uint32_t{1} << length) && sym < total) {
            codes[sym] = v35;
            v35 = v35 - 1;      // unsigned wraparound intended (matches decoder)
            sym = v63[sym];
        }
        v35 = 2u * v35 + 1u;    // unsigned wraparound intended
    }
    return codes;
}

// ---- length-limited Huffman (package-merge) -----------------------------------
// Optimal code lengths with a hard ceiling of `max_len` bits (<= 15 keeps us well
// under both the decoder's 24-bit read cap and the 5-bit RLE length field). The
// result satisfies the Kraft equality, so assign_codes fills a complete tree.
inline std::vector<uint8_t> length_limited_huffman(const std::vector<uint32_t>& freq, int max_len) {
    int n = static_cast<int>(freq.size());
    std::vector<uint8_t> lengths(n, 0);

    std::vector<int> syms;
    for (int i = 0; i < n; ++i) if (freq[i] > 0) syms.push_back(i);
    int m = static_cast<int>(syms.size());
    if (m == 0) return lengths;
    if (m == 1) { lengths[syms[0]] = 1; return lengths; } // single symbol -> 1 bit

    struct Item { uint64_t w; std::vector<int> members; };
    std::vector<Item> leaves;
    leaves.reserve(m);
    for (int s : syms) leaves.push_back({ freq[s], { s } });
    std::sort(leaves.begin(), leaves.end(), [](const Item& a, const Item& b) { return a.w < b.w; });

    std::vector<Item> prev = leaves;
    for (int level = 1; level < max_len; ++level) {
        std::vector<Item> packages;
        packages.reserve(prev.size() / 2);
        for (size_t k = 0; k + 1 < prev.size(); k += 2) {
            Item it;
            it.w = prev[k].w + prev[k + 1].w;
            it.members = prev[k].members;
            it.members.insert(it.members.end(), prev[k + 1].members.begin(), prev[k + 1].members.end());
            packages.push_back(std::move(it));
        }
        std::vector<Item> merged;
        merged.reserve(leaves.size() + packages.size());
        size_t a = 0, b = 0;
        while (a < leaves.size() || b < packages.size()) {
            if (b >= packages.size() || (a < leaves.size() && leaves[a].w <= packages[b].w))
                merged.push_back(leaves[a++]);
            else
                merged.push_back(packages[b++]);
        }
        prev = std::move(merged);
    }

    int take = 2 * m - 2;
    std::vector<int> count(n, 0);
    for (int i = 0; i < take && i < static_cast<int>(prev.size()); ++i)
        for (int s : prev[i].members) count[s]++;
    for (int s : syms) lengths[static_cast<size_t>(s)] = static_cast<uint8_t>(count[s]);
    return lengths;
}

// ---- reverse lookups for the length/distance code tables ----------------------
// Find the length code (0..28) whose [base, base+2^extra-1] range contains `v`.
inline int len_code_for(uint32_t v, uint32_t& extra_val, int& extra_bits) {
    for (int li = 0; li <= 28; ++li) {
        int eb = LEN_EXTRA[li];
        uint32_t base = LEN_BASE[li];
        uint32_t span = (uint32_t{1} << eb);
        if (v >= base && v < base + span) { extra_val = v - base; extra_bits = eb; return li; }
    }
    return -1;
}
inline int dist_code_for(uint32_t v, uint32_t& extra_val, int& extra_bits) {
    for (int di = 0; di <= 29; ++di) {
        int eb = DIST_EXTRA[di];
        uint32_t base = DIST_BASE[di];
        uint32_t span = (uint32_t{1} << eb);
        if (v >= base && v < base + span) { extra_val = v - base; extra_bits = eb; return di; }
    }
    return -1;
}

// ---- LZ77 token stream --------------------------------------------------------
struct Token {
    bool is_match;
    uint8_t lit;       // literal byte (is_match == false)
    uint32_t length;   // copy length (is_match == true), >= MIN_MATCH
    uint32_t dist;     // real back-distance D >= 1 (copy source = pos - D)
};

// Constants tied to the code tables:
//   MIN_MATCH 3         -- shorter runs aren't worth a match token
//   MAX_MATCH 256       -- LEN_BASE tops out at lenval 255; length = lenval+minMatchAdd
//   WINDOW    32768     -- DIST_BASE[29]=24576 + (1<<DIST_EXTRA[29]=13) tops out at
//                          distval 32767; real distance D = distval+1 <= 32768
constexpr int MIN_MATCH = 3;
constexpr int MAX_MATCH = 256;
constexpr int WINDOW = 32768;
constexpr int MIN_MATCH_ADD = 1; // stream header nibble = MIN_MATCH_ADD-1 = 0

inline uint32_t hash3(const uint8_t* p) {
    uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16);
    return (v * 2654435761u) >> 16; // 16-bit hash
}

inline std::vector<Token> lz_tokenize(const std::vector<uint8_t>& in) {
    int n = static_cast<int>(in.size());
    std::vector<Token> toks;
    if (n == 0) return toks;

    std::vector<int> head(1 << 16, -1);
    std::vector<int> prev(n, -1);
    constexpr int MAX_CHAIN = 128;

    auto insert = [&](int i) {
        if (i + MIN_MATCH <= n) {
            uint32_t h = hash3(&in[i]);
            prev[i] = head[h];
            head[h] = i;
        }
    };

    int i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + MIN_MATCH <= n) {
            uint32_t h = hash3(&in[i]);
            int cand = head[h];
            int chain = 0;
            int maxl = std::min(MAX_MATCH, n - i);
            while (cand >= 0 && (i - cand) <= WINDOW && chain < MAX_CHAIN) {
                if (in[cand + best_len] == in[i + best_len]) { // quick reject
                    int l = 0;
                    while (l < maxl && in[cand + l] == in[i + l]) ++l;
                    if (l > best_len) {
                        best_len = l;
                        best_dist = i - cand;
                        if (l >= maxl) break;
                    }
                }
                cand = prev[cand];
                ++chain;
            }
        }
        if (best_len >= MIN_MATCH) {
            toks.push_back({ true, 0, static_cast<uint32_t>(best_len), static_cast<uint32_t>(best_dist) });
            int end = i + best_len;
            while (i < end) { insert(i); ++i; }
        } else {
            toks.push_back({ false, in[i], 0, 0 });
            insert(i);
            ++i;
        }
    }
    return toks;
}

// A token resolved to concrete symbols + extra-bit payloads for one block.
struct ResolvedToken {
    bool is_match;
    uint16_t lit_sym;      // literal (0..255) or length code (256..284)
    int len_extra_bits;
    uint32_t len_extra_val;
    uint16_t dist_sym;
    int dist_extra_bits;
    uint32_t dist_extra_val;
};

inline ResolvedToken resolve_token(const Token& t) {
    ResolvedToken r{};
    r.is_match = t.is_match;
    if (!t.is_match) {
        r.lit_sym = t.lit;
        return r;
    }
    uint32_t lenval = t.length - MIN_MATCH_ADD;   // decoder: length = base+extra+minMatchAdd
    uint32_t distval = t.dist - 1;                // decoder: dist stored = D-1
    uint32_t lev = 0, dev = 0; int leb = 0, deb = 0;
    int li = len_code_for(lenval, lev, leb);
    int di = dist_code_for(distval, dev, deb);
    if (li < 0 || di < 0)
        throw decode_error("compress: match out of representable range (length=" +
                           std::to_string(t.length) + " dist=" + std::to_string(t.dist) +
                           " lenval=" + std::to_string(lenval) + " distval=" + std::to_string(distval) + ")");
    r.lit_sym = static_cast<uint16_t>(256 + li);
    r.len_extra_bits = leb; r.len_extra_val = lev;
    r.dist_sym = static_cast<uint16_t>(di);
    r.dist_extra_bits = deb; r.dist_extra_val = dev;
    return r;
}

// Emit a canonical Huffman table exactly the way HuffTable::build reads it:
// 16-bit symbol count, then the code lengths RLE-coded (idx high->low) through
// the meta-huffman. Returns the per-symbol codes for subsequent symbol emission.
inline std::vector<uint32_t> write_table(BitWriter& bw, const std::vector<uint8_t>& lengths) {
    uint32_t total = static_cast<uint32_t>(lengths.size());
    bw.write(total, 16);
    const auto& me = meta_encoder();
    long idx = static_cast<long>(total) - 1;
    while (idx >= 0) {
        uint8_t l = lengths[static_cast<size_t>(idx)];
        int run = 0;
        long j = idx;
        while (j >= 0 && lengths[static_cast<size_t>(j)] == l && run < 8) { --j; ++run; }
        uint8_t rle = static_cast<uint8_t>(((run - 1) << 5) | (l & 0x1F));
        const MetaEnc& e = me[rle];
        bw.write(e.code, e.bitlen);
        idx -= run;
    }
    return assign_codes(lengths);
}

} // namespace detail

// Compresses `data` into a bare Method-0 bitstream (no {flag,size} header, no CRC
// framing). Feed the result to castlemist::cmp::decompress_method0(bits, data.size()).
inline std::vector<uint8_t> compress_method0(std::span<const uint8_t> data) {
    using namespace detail;
    std::vector<uint8_t> in(data.begin(), data.end());

    BitWriter bw;
    bw.write(0, 4);                       // Method 0
    bw.write(MIN_MATCH_ADD - 1, 4);       // minMatchAdd - 1

    if (in.empty()) return bw.finish();

    std::vector<Token> toks = lz_tokenize(in);

    // Fixed 4096-token blocks (block_symbols nibble = 0 -> (0+1)<<12 = 4096).
    // Intermediate blocks must contain exactly 4096 tokens; the final block may
    // contain fewer because the decoder stops as soon as output_size is reached.
    constexpr size_t BLOCK = 4096;
    for (size_t b = 0; b < toks.size(); b += BLOCK) {
        size_t end = std::min(b + BLOCK, toks.size());

        std::vector<uint32_t> lit_freq(285, 0), dist_freq(30, 0);
        std::vector<ResolvedToken> resolved;
        resolved.reserve(end - b);
        for (size_t k = b; k < end; ++k) {
            ResolvedToken r = resolve_token(toks[k]);
            lit_freq[r.lit_sym]++;
            if (r.is_match) dist_freq[r.dist_sym]++;
            resolved.push_back(r);
        }

        std::vector<uint8_t> lit_len = length_limited_huffman(lit_freq, 15);
        std::vector<uint8_t> dist_len = length_limited_huffman(dist_freq, 15);

        std::vector<uint32_t> lit_codes = write_table(bw, lit_len);
        std::vector<uint32_t> dist_codes = write_table(bw, dist_len);
        bw.write(0, 4);                    // block_symbols nibble -> 4096

        for (const ResolvedToken& r : resolved) {
            bw.write(lit_codes[r.lit_sym], lit_len[r.lit_sym]);
            if (r.is_match) {
                if (r.len_extra_bits) bw.write(r.len_extra_val, r.len_extra_bits);
                bw.write(dist_codes[r.dist_sym], dist_len[r.dist_sym]);
                if (r.dist_extra_bits) bw.write(r.dist_extra_val, r.dist_extra_bits);
            }
        }
    }
    return bw.finish();
}

// CRC-32C (Castagnoli, reflected poly 0x82F63B78, init/xorout 0xFFFFFFFF). This
// is the exact checksum GW2 embeds in the .dat chunk framing -- verified against
// live Gw2.dat entries: crc32c(content_chunk) reproduces every stored word.
inline uint32_t crc32c(const uint8_t* data, size_t n) {
    static const std::array<uint32_t, 256> TAB = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = (crc >> 8) ^ TAB[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}

// Re-inserts the periodic CRC32C framing (exact inverse of strip_crc32): after
// every 0xFFFC content bytes a little-endian CRC32C of that chunk, plus a
// trailing CRC32C over the final partial chunk. `content` = 8-byte header +
// Method-0 bitstream. The result is a byte-faithful on-disk MFT entry payload
// (the live client's CRC validation passes).
inline std::vector<uint8_t> add_crc32_framing(std::span<const uint8_t> content) {
    constexpr size_t STRIDE = 0x10000 - 4; // 0xFFFC bytes of content per chunk
    std::vector<uint8_t> out;
    size_t U = content.size();
    size_t pos = 0;
    auto put_crc = [&](const uint8_t* chunk, size_t len) {
        uint32_t c = crc32c(chunk, len);
        out.push_back(c & 0xFF); out.push_back((c >> 8) & 0xFF);
        out.push_back((c >> 16) & 0xFF); out.push_back((c >> 24) & 0xFF);
    };
    while (U - pos > STRIDE) {
        out.insert(out.end(), content.begin() + pos, content.begin() + pos + STRIDE);
        put_crc(content.data() + pos, STRIDE);
        pos += STRIDE;
    }
    out.insert(out.end(), content.begin() + pos, content.end());
    put_crc(content.data() + pos, U - pos); // trailing (final partial chunk)
    return out;
}

// Compresses `data` into a full on-disk MFT entry payload: CRC-framed
// {flag,uncompressedSize} header + Method-0 bitstream. castlemist::cmp::decompress_entry
// (== the CLI's decompress path) reproduces `data` exactly.
inline std::vector<uint8_t> compress_entry(std::span<const uint8_t> data) {
    std::vector<uint8_t> bits = compress_method0(data);

    std::vector<uint8_t> content;
    content.reserve(8 + bits.size());
    // flag dword: low 16 bits = 8 ("ANet compress"), rest 0.
    content.insert(content.end(), { 8, 0, 0, 0 });
    uint32_t usize = static_cast<uint32_t>(data.size());
    content.push_back(static_cast<uint8_t>(usize & 0xFF));
    content.push_back(static_cast<uint8_t>((usize >> 8) & 0xFF));
    content.push_back(static_cast<uint8_t>((usize >> 16) & 0xFF));
    content.push_back(static_cast<uint8_t>((usize >> 24) & 0xFF));
    content.insert(content.end(), bits.begin(), bits.end());

    return add_crc32_framing(content);
}

} // namespace castlemist::cmp

#endif // GW2CMP_COMPRESS_METHOD0_HPP
