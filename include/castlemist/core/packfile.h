/// @file
/// @brief Traversal of GW2 "PF" packfiles: chunk lookup, array_ptr and string fields.
/// @ingroup core

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "castlemist/core/bytes.h"

namespace castlemist::core {

/// @addtogroup core
/// @{

/// @brief On-disk layout of a packfile header, for reference.
///
/// @verbatim
///   +0  'P' 'F'
///   +2  u16 pfVersion      bit 2 (value 4) set => 64-bit self-relative pointers
///   +4  u16 reserved
///   +6  u16 headerSize     offset of the first chunk
///   +8  fourcc container   'ASND', 'cntc', 'eula', 'MODL', ...
///
///   chunk:
///   +0  fourcc name        'Main', 'GEOM', 'BKCK', ...
///   +4  u32   size         bytes after the (name,size) pair
///   +8  u16   version
///   +10 u16   headerSize
///   +12 u32   descriptorOffset
///   +16 struct root
/// @endverbatim
///
/// Structures inside a chunk are tightly packed (no alignment padding) and
/// reference each other with *self-relative* pointers: the stored value is
/// added to the address of the field that holds it, not to the chunk base.
struct PackfileHeader {
    uint16_t version = 0;      ///< pfVersion word.
    uint16_t header_size = 0;  ///< Offset of the first chunk.
    char     container[5] = {};///< NUL-terminated container fourcc.
    int      pointer_size = 4; ///< 4 or 8, derived from `version & 4`.
};

/// @brief One chunk as found while walking a packfile.
struct PackfileChunk {
    char     name[5] = {};  ///< NUL-terminated chunk fourcc.
    uint32_t size = 0;      ///< Chunk payload size in bytes.
    uint16_t version = 0;   ///< Chunk struct version (the `V<n>` suffix in the template).
    size_t   offset = 0;    ///< Offset of the chunk header within the blob.
    size_t   root = 0;      ///< Offset of the chunk's struct root (`offset + 16`).
};

/// @brief Reader for the packed structures inside a packfile chunk.
///
/// A PackfileReader is a castlemist::core::Bytes plus the pointer width the file was
/// written with. The pointer width is not a property of the struct: the same
/// `ModelFileData` is 4-byte-pointered in an old MODL and 8-byte-pointered in a
/// new one, and reading a 64-bit blob as 32-bit silently produces zero-length
/// arrays rather than an error.
class PackfileReader {
public:
    PackfileReader() = default;

    /// @brief Construct over @p blob with an explicit @p pointer_size (4 or 8).
    PackfileReader(const std::vector<uint8_t>& blob, int pointer_size)
        : bytes_(blob), pointer_size_(pointer_size == 8 ? 8 : 4) {}

    const Bytes& bytes() const noexcept { return bytes_; }        ///< Underlying buffer.
    const uint8_t* data() const noexcept { return bytes_.data(); }///< First byte of the blob.
    size_t size() const noexcept { return bytes_.size(); }        ///< Blob size in bytes.
    int  pointer_size() const noexcept { return pointer_size_; }  ///< 4 or 8.
    bool valid() const noexcept { return bytes_.data() != nullptr; }

    uint32_t u16(size_t offset) const noexcept { return bytes_.u16(offset); }
    uint32_t u32(size_t offset) const noexcept { return bytes_.u32(offset); }
    uint8_t  u8 (size_t offset) const noexcept { return bytes_.u8(offset); }
    float    f32(size_t offset) const noexcept { return bytes_.f32(offset); }

    /// @brief Read the signed self-relative pointer stored at @p offset.
    /// @return The raw delta (not yet added to @p offset); 0 for a null pointer.
    int64_t sptr(size_t offset) const noexcept {
        return pointer_size_ == 8 ? bytes_.i64(offset) : static_cast<int64_t>(bytes_.i32(offset));
    }

    /// @brief Follow the self-relative pointer at @p offset.
    /// @return Absolute offset of the pointee, or 0 for a null pointer.
    size_t deref(size_t offset) const noexcept {
        int64_t rel = sptr(offset);
        return rel ? static_cast<size_t>(offset + rel) : 0;
    }

    /// @brief `sizeof(array_ptr)` for this file: a u32 count plus one pointer.
    size_t array_ptr_size() const noexcept { return 4 + static_cast<size_t>(pointer_size_); }

    /// @brief Read the `array_ptr` field at @p offset.
    /// @param offset      Offset of the array_ptr field.
    /// @param first_out   Receives the offset of element 0.
    /// @return Element count (0 when the array is empty or the field is truncated).
    ///
    /// @note The pointer half is relative to `offset + 4`, i.e. to itself, not
    ///       to the start of the array_ptr.
    uint32_t array(size_t offset, size_t& first_out) const noexcept {
        uint32_t count = bytes_.u32(offset);
        first_out = static_cast<size_t>((offset + 4) + sptr(offset + 4));
        return count;
    }

    /// @brief Read the NUL-terminated UTF-16 string pointed at by the field at @p offset.
    /// @param offset    Offset of the pointer field.
    /// @param max_chars Hard cap so a corrupt pointer cannot allocate the world.
    std::wstring wstring(size_t offset, size_t max_chars = 8000) const;

    /// @brief Resolve the filename reference pointed at by the field at @p offset.
    /// @return The fileId, or 0 when the pointer or the reference is null.
    /// @see castlemist::core::decode_fileref
    uint32_t fileref(size_t offset) const noexcept;

private:
    Bytes bytes_;
    int   pointer_size_ = 4;
};

/// @brief True when @p blob is a packfile (starts with `PF`).
bool is_packfile(const std::vector<uint8_t>& blob) noexcept;

/// @brief True when @p blob is a packfile whose container fourcc is @p container.
bool is_packfile(const std::vector<uint8_t>& blob, const char* container) noexcept;

/// @brief Parse the packfile header of @p blob.
/// @return false when @p blob is not a packfile.
bool read_header(const std::vector<uint8_t>& blob, PackfileHeader& out) noexcept;

/// @brief Enumerate every chunk in @p blob, in file order.
/// @param blob      Decompressed packfile.
/// @param max_chunks Guard against a corrupt size field looping forever.
std::vector<PackfileChunk> chunks(const std::vector<uint8_t>& blob, size_t max_chunks = 4096);

/// @brief True when @p blob contains a chunk named @p name.
bool has_chunk(const std::vector<uint8_t>& blob, const char* name);

/// @brief Locate a chunk and build a reader positioned on the file's pointer width.
/// @param blob     Decompressed packfile.
/// @param name     Chunk fourcc to find.
/// @param out      Receives a reader over the whole blob.
/// @param root_out Receives the chunk's struct root offset.
/// @return false when @p blob is not a packfile or has no such chunk.
///
/// @code
/// castlemist::core::PackfileReader r;
/// size_t root = 0;
/// if (castlemist::core::open_chunk(blob, "BKCK", r, root)) {
///     size_t first = 0;
///     uint32_t n = r.array(root, first);
/// }
/// @endcode
bool open_chunk(const std::vector<uint8_t>& blob, const char* name,
                PackfileReader& out, size_t& root_out);

/// @brief Human-readable one-line-per-chunk listing, as shown in the info panel.
std::wstring chunk_listing(const std::vector<uint8_t>& blob);

/// @brief Name of a GW2 `GameLanguage` enum value (0 English ... 5 Chinese).
const wchar_t* language_name(uint32_t language) noexcept;

/// @}

} // namespace castlemist::core
