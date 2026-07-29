// bytebuf.h - little-endian read/write helpers for MsgConn wire framing.
// GW2 MsgConn is little-endian; msgId is u16 LE, scalars follow def order.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace gw2 {

// Bounds-checked reader over a byte span. Throws on short read so the caller
// can treat a truncated/partial message as "need more bytes".
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n), off_(0) {}

    size_t remaining() const { return n_ - off_; }
    size_t offset() const { return off_; }
    bool eof() const { return off_ >= n_; }

    void need(size_t k) const {
        if (off_ + k > n_) throw std::runtime_error("short read");
    }

    uint8_t  u8()  { need(1); return p_[off_++]; }
    uint16_t u16() { need(2); uint16_t v = p_[off_] | (p_[off_+1] << 8); off_ += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = (uint32_t)p_[off_] | ((uint32_t)p_[off_+1] << 8)
                                          | ((uint32_t)p_[off_+2] << 16) | ((uint32_t)p_[off_+3] << 24);
                     off_ += 4; return v; }
    uint64_t u64() { uint64_t lo = u32(), hi = u32(); return lo | (hi << 32); }

    std::vector<uint8_t> bytes(size_t k) {
        need(k);
        std::vector<uint8_t> v(p_ + off_, p_ + off_ + k);
        off_ += k;
        return v;
    }

    // UTF-16LE null-terminated string, bounded by maxChars code units.
    std::string wstr(size_t maxChars) {
        std::string out;
        for (size_t i = 0; i < maxChars; ++i) {
            uint16_t c = u16();
            if (c == 0) break;
            if (c < 0x80) out.push_back((char)c);       // scaffold: ASCII fold
            else out.push_back('?');
        }
        return out;
    }
    // UTF-8 null-terminated string, bounded by maxBytes.
    std::string cstr(size_t maxBytes) {
        std::string out;
        for (size_t i = 0; i < maxBytes; ++i) {
            uint8_t c = u8();
            if (c == 0) break;
            out.push_back((char)c);
        }
        return out;
    }

private:
    const uint8_t* p_;
    size_t n_;
    size_t off_;
};

class Writer {
public:
    void u8(uint8_t v)  { buf_.push_back(v); }
    void u16(uint16_t v){ buf_.push_back(v & 0xff); buf_.push_back((v >> 8) & 0xff); }
    void u32(uint32_t v){ for (int i = 0; i < 4; ++i) buf_.push_back((v >> (8*i)) & 0xff); }
    void u64(uint64_t v){ for (int i = 0; i < 8; ++i) buf_.push_back((v >> (8*i)) & 0xff); }
    void raw(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
    void raw(const std::vector<uint8_t>& v) { buf_.insert(buf_.end(), v.begin(), v.end()); }

    const std::vector<uint8_t>& data() const { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

} // namespace gw2
