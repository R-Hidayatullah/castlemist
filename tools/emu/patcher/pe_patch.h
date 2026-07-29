// pe_patch.h - minimal PE64 VA<->file-offset mapping + patch application.
// Target: Gw2-64-disable-aslr.exe (imagebase 0x140000000, ASLR off).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gw2pe {

struct Section {
    char     name[9];
    uint32_t vaddr;      // VirtualAddress (RVA)
    uint32_t vsize;      // VirtualSize
    uint32_t raw_ptr;    // PointerToRawData (file offset)
    uint32_t raw_size;   // SizeOfRawData
};

class Image {
public:
    bool load(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf_.resize(sz);
        size_t rd = fread(buf_.data(), 1, sz, f);
        fclose(f);
        if (rd != (size_t)sz) return false;
        return parse();
    }

    bool save(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        size_t wr = fwrite(buf_.data(), 1, buf_.size(), f);
        fclose(f);
        return wr == buf_.size();
    }

    uint64_t imagebase() const { return imagebase_; }

    // Virtual address -> file offset. Returns SIZE_MAX if unmapped.
    size_t va_to_off(uint64_t va) const {
        if (va < imagebase_) return SIZE_MAX;
        uint32_t rva = (uint32_t)(va - imagebase_);
        for (const auto& s : sections_) {
            if (rva >= s.vaddr && rva < s.vaddr + s.vsize) {
                uint32_t d = rva - s.vaddr;
                if (d >= s.raw_size) return SIZE_MAX; // in virtual-only tail
                return s.raw_ptr + d;
            }
        }
        return SIZE_MAX;
    }

    // Read/patch bytes at a VA. Returns false if unmapped or verify mismatch.
    bool read_at(uint64_t va, uint8_t* out, size_t n) const {
        size_t off = va_to_off(va);
        if (off == SIZE_MAX || off + n > buf_.size()) return false;
        memcpy(out, &buf_[off], n);
        return true;
    }

    // Apply a patch, verifying the current bytes match `expect` first.
    // expect/replace equal length. Pass empty `expect` to skip verification.
    bool patch_at(uint64_t va, const std::vector<uint8_t>& expect,
                  const std::vector<uint8_t>& replace, std::string& err) {
        size_t off = va_to_off(va);
        if (off == SIZE_MAX || off + replace.size() > buf_.size()) {
            err = "VA unmapped or out of range";
            return false;
        }
        if (!expect.empty()) {
            if (expect.size() != replace.size()) { err = "expect/replace length mismatch"; return false; }
            if (memcmp(&buf_[off], expect.data(), expect.size()) != 0) {
                err = "current bytes != expected (wrong build or already patched)";
                return false;
            }
        }
        memcpy(&buf_[off], replace.data(), replace.size());
        return true;
    }

    // Search the whole file for a byte pattern; returns file offsets.
    std::vector<size_t> find(const std::vector<uint8_t>& pat) const {
        std::vector<size_t> hits;
        if (pat.empty() || pat.size() > buf_.size()) return hits;
        for (size_t i = 0; i + pat.size() <= buf_.size(); ++i) {
            if (memcmp(&buf_[i], pat.data(), pat.size()) == 0) hits.push_back(i);
        }
        return hits;
    }

    bool patch_file_off(size_t off, const std::vector<uint8_t>& replace) {
        if (off + replace.size() > buf_.size()) return false;
        memcpy(&buf_[off], replace.data(), replace.size());
        return true;
    }

private:
    bool parse() {
        if (buf_.size() < 0x40 || buf_[0] != 'M' || buf_[1] != 'Z') return false;
        uint32_t pe = rd32(0x3C);
        if (pe + 24 > buf_.size() || memcmp(&buf_[pe], "PE\0\0", 4) != 0) return false;
        uint16_t nsec = rd16(pe + 6);
        uint16_t opt_sz = rd16(pe + 20);
        uint32_t opt = pe + 24;
        uint16_t magic = rd16(opt);
        if (magic != 0x20b) return false;              // PE32+ only
        imagebase_ = rd64(opt + 24);
        uint32_t sec = opt + opt_sz;
        for (uint16_t i = 0; i < nsec; ++i) {
            uint32_t s = sec + i * 40;
            Section sc{};
            memcpy(sc.name, &buf_[s], 8); sc.name[8] = 0;
            sc.vsize   = rd32(s + 8);
            sc.vaddr   = rd32(s + 12);
            sc.raw_size= rd32(s + 16);
            sc.raw_ptr = rd32(s + 20);
            sections_.push_back(sc);
        }
        return true;
    }
    uint16_t rd16(size_t o) const { return buf_[o] | (buf_[o+1] << 8); }
    uint32_t rd32(size_t o) const { return (uint32_t)buf_[o] | ((uint32_t)buf_[o+1]<<8)
                                         | ((uint32_t)buf_[o+2]<<16) | ((uint32_t)buf_[o+3]<<24); }
    uint64_t rd64(size_t o) const { return (uint64_t)rd32(o) | ((uint64_t)rd32(o+4) << 32); }

    std::vector<uint8_t> buf_;
    std::vector<Section> sections_;
    uint64_t imagebase_ = 0;
};

} // namespace gw2pe
