#ifndef STRS_KEYS_H
#define STRS_KEYS_H

// Optional RC4 decryption layer for "strs" string tables.
//
// GW2's packed strs records are RC4-encrypted with an 8-byte per-textId
// "password" the client receives at runtime (see gw2-strs-decrypt notes). This
// module loads two capture/derived tables so the browser can decrypt them:
//   * keys     : textId,key8_hex     (from a live-client capture -> textkeys.csv)
//   * textbase : fileId,baseTextId   (from TextPackManifest -> strs_textbase.csv)
// The record's global textId = baseTextId(for this file's fileId) + recordIndex.
// Without both tables the packed records stay flagged (same as before).

#include <cstdint>
#include <string>
#include <vector>

#include "castlemist/format/strs_view.h"   // castlemist::strs::Record

namespace castlemist::skeys {

/// Same framing as castlemist::strs::records(), but packed records are also RC4-decrypted
/// when a key for their textId is loaded (textId = baseTextId + index; pass
/// baseTextId < 0 when unknown, which leaves every packed record locked).
std::vector<castlemist::strs::Record> records(const uint8_t* data, size_t size, long long baseTextId);

/// Load "textId,key8_hex" rows (lines starting with '#' or "textId" ignored).
/// Merges into the existing table. Returns number of keys after load, -1 on open error.
long load_keys(const std::wstring& path);

/// Load "fileId,baseTextId" rows. Returns rows loaded, -1 on open error.
long load_textbase(const std::wstring& path);

bool   keys_ready();
size_t key_count();
bool   textbase_ready();

/// baseTextId for whichever of these fileIds appears in the textbase map, or -1.
long long base_for_fileids(const std::vector<uint32_t>& file_ids);

/// Full strs listing, decrypting packed records when a key is known.
///   baseTextId < 0  -> packed records are flagged (can't resolve textId).
/// Mirrors castlemist::strs::decode's output format so it can drop into the same surface.
std::wstring decode(const uint8_t* data, size_t size, long long baseTextId);

/// --- textId -> string resolution (uses the txtm-derived textbase map) --------
/// fileId of the strs file that holds `textId` = the entry whose baseTextId is the
/// greatest one <= textId (and within one file's span). 0 if unknown / no textbase.
uint32_t fileid_for_textid(long long textId);

/// Text of a single strs record (the `index`-th) in a decoded strs blob -- raw
/// UTF-16 verbatim, or RC4-decrypted when the key for its textId (baseTextId+index)
/// is loaded. Empty if `index` is out of range or the record is packed with no key.
std::wstring decode_record_text(const uint8_t* data, size_t size, long long baseTextId, uint32_t index);

} // namespace castlemist::skeys

#endif // STRS_KEYS_H
