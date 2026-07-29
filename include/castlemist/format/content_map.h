#ifndef CONTENT_MAP_H
#define CONTENT_MAP_H

// Offline chat-link id -> dat asset map, built from the GW2 content datastore
// (cntc / PackContent packfiles). A chat link carries a game/API id (item id,
// skin id, ...); the .dat addresses assets by fileId. The content datastore
// bridges the two: each content object has a contentType (item = 35) at +16, a
// numeric id at +20, and its primary asset (icon texture / model / sound) as a
// fileId reference recorded in the pack's `fileIndices` fixup table, at object
// offset +64. See the gw2-chat-links memory note for how this was reversed
// (CnData.cpp loader + empirical validation against real cntc files).
//
// build() parses every cntc pack once into an in-memory (contentType,id)->fileId
// table; resolve() answers lookups. The table can be cached to disk so it only
// has to be built once per game version.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "castlemist/native/gw2dat.h"

namespace castlemist::cmap {

/// GW2 Content::CONTENT_TYPE_* values (from the client, IDA-verified). The map
/// stores every type it sees; these are the ones the chat link decoder maps.
constexpr uint32_t CONTENT_TYPE_ITEM = 35;
constexpr uint32_t CONTENT_TYPE_OUTFIT = 51;
constexpr uint32_t CONTENT_TYPE_SKIN = 66;

/// The chat link header byte -> content type it resolves against (0 = unmapped).
uint32_t content_type_for_header(uint8_t header);

bool built();
size_t size();  // number of (type,id) entries currently held
void clear();

/// Parse the supplied cntc entries (raw MftData, copied by the caller so this is
/// safe on a background thread) and build the map. `dat_path` is the archive the
/// entries live in. `progress(done,total)` is called after each pack. Returns the
/// number of (type,id) entries collected. Adds to whatever is already loaded.
size_t build(const std::string& dat_path, const std::vector<MftData>& cntc_entries,
             const std::function<void(size_t done, size_t total)>& progress);

/// All asset fileIds a content object references (in object order; models are
/// literal fileIds, textures are the object's icons/variants). Empty = not found.
/// The caller classifies each fileId (texture/model/audio) via the dat/index.
const std::vector<uint32_t>& resolve_all(uint32_t content_type, uint32_t id);

/// The object's primary asset fileId (first reference, at +64 when present).
uint32_t resolve(uint32_t content_type, uint32_t id);
inline uint32_t resolve_item(uint32_t item_id) { return resolve(CONTENT_TYPE_ITEM, item_id); }

/// Simple binary cache (magic + records). Lets the map survive across sessions.
bool save(const std::wstring& path);
bool load(const std::wstring& path);

} // namespace castlemist::cmap

#endif // CONTENT_MAP_H
