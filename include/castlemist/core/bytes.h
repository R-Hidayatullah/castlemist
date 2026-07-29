/// @file
/// @brief Bounds-checked little-endian reads over a raw byte buffer.
/// @ingroup core

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

/// @defgroup core core -- dependency-free primitives
/// @brief Byte readers, packfile traversal and text conversion.
///
/// Nothing in this layer touches Direct3D, the Win32 UI or the .dat; it is pure
/// data manipulation and is therefore the easiest part of castlemist to unit
/// test. Every other layer is allowed to depend on it.
/// @{

namespace castlemist::core {

/// @brief Read-only cursor over a byte buffer with saturating bounds checks.
///
/// Every accessor returns 0 (or an empty result) instead of reading out of
/// bounds. GW2 blobs are attacker-shaped often enough -- truncated entries,
/// mis-flagged compression, self-relative pointers that point outside the
/// chunk -- that a reader which cannot fault is worth more than one that
/// reports errors nobody checks.
///
/// @code
/// castlemist::core::Bytes b(data);
/// uint32_t magic = b.u32(0);
/// @endcode
class Bytes {
public:
    constexpr Bytes() = default;

    /// @brief Wrap @p data / @p size. The buffer must outlive the reader.
    constexpr Bytes(const uint8_t* data, size_t size) noexcept : data_(data), size_(size) {}

    /// @brief Wrap a whole vector.
    explicit Bytes(const std::vector<uint8_t>& v) noexcept : data_(v.data()), size_(v.size()) {}

    const uint8_t* data() const noexcept { return data_; }   ///< First byte, or nullptr.
    size_t         size() const noexcept { return size_; }   ///< Byte count.
    bool          empty() const noexcept { return size_ == 0; }

    /// @brief True when @p count bytes starting at @p offset are inside the buffer.
    bool has(size_t offset, size_t count) const noexcept {
        return offset <= size_ && count <= size_ - offset;
    }

    /// @brief Unsigned 8-bit read; 0 when out of range.
    uint8_t u8(size_t offset) const noexcept {
        return has(offset, 1) ? data_[offset] : uint8_t{0};
    }

    /// @brief Little-endian unsigned 16-bit read; 0 when out of range.
    uint16_t u16(size_t offset) const noexcept {
        if (!has(offset, 2)) return 0;
        return static_cast<uint16_t>(data_[offset] | (data_[offset + 1] << 8));
    }

    /// @brief Little-endian unsigned 32-bit read; 0 when out of range.
    uint32_t u32(size_t offset) const noexcept {
        if (!has(offset, 4)) return 0;
        return static_cast<uint32_t>(data_[offset]) |
               (static_cast<uint32_t>(data_[offset + 1]) << 8) |
               (static_cast<uint32_t>(data_[offset + 2]) << 16) |
               (static_cast<uint32_t>(data_[offset + 3]) << 24);
    }

    /// @brief Little-endian unsigned 64-bit read; 0 when out of range.
    uint64_t u64(size_t offset) const noexcept {
        if (!has(offset, 8)) return 0;
        uint64_t v = 0;
        std::memcpy(&v, data_ + offset, 8);
        return v;
    }

    /// @brief Little-endian signed 32-bit read; 0 when out of range.
    int32_t i32(size_t offset) const noexcept {
        if (!has(offset, 4)) return 0;
        int32_t v = 0;
        std::memcpy(&v, data_ + offset, 4);
        return v;
    }

    /// @brief Little-endian signed 64-bit read; 0 when out of range.
    int64_t i64(size_t offset) const noexcept {
        if (!has(offset, 8)) return 0;
        int64_t v = 0;
        std::memcpy(&v, data_ + offset, 8);
        return v;
    }

    /// @brief 32-bit float read; 0.0f when out of range.
    float f32(size_t offset) const noexcept {
        if (!has(offset, 4)) return 0.0f;
        float v = 0;
        std::memcpy(&v, data_ + offset, 4);
        return v;
    }

    /// @brief True when the @p n bytes at @p offset equal @p literal.
    bool match(size_t offset, const char* literal, size_t n) const noexcept {
        return has(offset, n) && std::memcmp(data_ + offset, literal, n) == 0;
    }

    /// @brief True when the buffer starts with the four-character @p fourcc.
    bool magic(const char* fourcc) const noexcept { return match(0, fourcc, 4); }

private:
    const uint8_t* data_ = nullptr;
    size_t         size_ = 0;
};

/// @brief Resolve a GW2 "filename reference" pair into a fileId.
///
/// GW2 does not store fileIds inline; it stores two 16-bit halves that the
/// engine folds back with `fileId = 0xFF00 * hi + lo - 0xFF00FF`. Both halves
/// must be >= 0x100 for the reference to be real -- anything smaller is a null
/// or a mis-parsed field, and a raw dword read of the same bytes yields a
/// plausible-looking but wrong id.
///
/// @param lo Low half of the reference.
/// @param hi High half of the reference.
/// @return The fileId, or 0 when the pair is not a valid reference.
inline uint32_t decode_fileref(uint32_t lo, uint32_t hi) noexcept {
    if (lo < 0x100 || hi < 0x100) return 0;
    return static_cast<uint32_t>(0xFF00LL * hi + lo - 0xFF00FF);
}

} // namespace castlemist::core

/// @}
