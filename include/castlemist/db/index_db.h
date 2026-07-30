#ifndef INDEX_DB_H
#define INDEX_DB_H

// Read-only accessor for a gw2index / gw2local SQLite index (see gw2index-tool).
// Lets the browser navigate/filter a huge .dat by pre-computed type/container/
// chunk metadata without re-scanning. Preview still extracts from the .dat.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace castlemist::db {

bool open(const std::wstring& db_path, std::string& err);
void close();
bool is_open();

std::wstring dat_path();      // meta.dat_path (the archive this index was built from)
size_t       entry_count();

std::vector<std::string> types();       // distinct entries.type,      sorted
std::vector<std::string> containers();  // distinct non-empty container, sorted

/// Streams every (base_id, type, container) row so the caller can build an
/// in-memory lookup for the list columns (one query, no per-row round trips).
void load_meta_map(const std::function<void(uint32_t base_id, const char* type, const char* container)>& cb);

/// Streams every (base_id, size_final) row -- the REAL decompressed size, which the
/// MFT header does not carry for compressed entries (its uncompressed_size field is
/// 0 whenever comp_flag != 0). Lets the browser show/sort a true "Uncompressed"
/// column without decompressing anything.
void load_size_map(const std::function<void(uint32_t base_id, long long size_final)>& cb);

/// @brief Narrows a query by what an entry actually *contains*, not just its type.
///
/// The container fourcc is often too coarse to be useful. 198330 entries declare
/// the `MODL` container, but only 183166 carry a `GEOM` chunk -- the other 15164
/// are animation-only files reusing the same container, and they have no mesh to
/// show. Likewise `texture` covers ATEX/ATEU/ATEP/CTEX/DDS, which decode
/// differently and are rarely wanted together.
///
/// Empty fields mean "don't care". Both chunk predicates are backed by
/// `ix_chunks_fourcc` / `ix_chunks_base`, so they stay cheap over ~1.4M rows.
struct ContentFilter {
    /// entries.magic must be one of these, e.g. {"ATEX","ATEU"} or {"DDS "} (note
    /// the trailing pad). A set rather than one value because the ArenaNet texture
    /// magics are a FAMILY -- the decoder accepts ATEX/ATTX/ATEC/ATEP/ATEU/ATET,
    /// and the archive also carries CTEX -- and "any texture" has to span them.
    std::vector<std::string> magics;
    std::string require_chunk;  ///< keep only entries carrying this chunk fourcc, e.g. "GEOM".
    std::string exclude_chunk;  ///< keep only entries NOT carrying this chunk fourcc.
    bool empty() const { return magics.empty() && require_chunk.empty() && exclude_chunk.empty(); }
};

/// Filtered base_ids. type/container empty = any. If id_active, restrict to the
/// given id (by base_id, or by file_id when id_by_fileid). Capped at `limit`.
std::vector<uint32_t> query_base_ids(const std::string& type, const std::string& container,
                                     uint32_t id_value, bool id_by_fileid, bool id_active, int limit);

/// As above, additionally narrowed by what the entry contains.
std::vector<uint32_t> query_base_ids(const std::string& type, const std::string& container,
                                     const ContentFilter& content,
                                     uint32_t id_value, bool id_by_fileid, bool id_active, int limit);

/// @brief One indexed entry: type, sizes, verified compression truth and chunk list.
struct EntryInfo {
    bool found = false;
    std::string type, container, error, magic;
    long long size = 0, size_stripped = 0, size_final = 0;
    int comp_flag = 0, actually_compressed = 0;
    std::vector<std::string> chunks;  // "fourcc v# (variant)"
};
EntryInfo lookup(uint32_t base_id);

} // namespace castlemist::db

#endif // INDEX_DB_H
