---
name: gw2index-tool
description: gw2index — fast resumable multithreaded GW2 .dat indexer (SQLite) + query MCP
metadata: 
  node_type: memory
  type: project
  originSessionId: 13b23349-78bf-4e5b-8435-3d0f28873c5a
  modified: 2026-07-25T15:47:35.216Z
---

**`gw2app/gw2index/`** = a fast, resumable, multithreaded indexer for a GW2 `.dat` → a
queryable **SQLite** DB, so you never re-scan/extract/decompress to answer "what is
fileId X / type / chunks / mis-flagged". Built 2026-07 at the user's request (test target
= account `Local.dat` in `%APPDATA%\Guild Wars 2\`).

**Tool** `gw2index.cpp` (+ bundled `sqlite3.c/.h` amalgamation, built via `build.sh`:
`gcc -O2 sqlite3.c -c` once, then `g++ -std=c++20 gw2index.cpp gw2mcp/native/src/gw2dat.cpp
+BinaryParser.cpp sqlite3.o`, `-I gw2mcp/native/{include,third_party}`). Run: `gw2index.exe
--dat <path> --out <db> --template gw2mcp/templates/gw2_packfile.json [--threads N] [--full]`.
Producer/consumer: N worker threads (`read_entry_bytes` opens its own ifstream/entry — thread
safe) do read+`strip_crc32`+decompress+classify+chunk-walk → a mutex/condvar queue → ONE
writer thread commits batched SQLite transactions (WAL). **Resumable**: per-entry fingerprint
`offset|size|crc|flag`; re-run loads existing fingerprints, skips unchanged, reprocesses only
patched/new (verified: Local.dat 322 entries full=1.2s, unchanged re-run=0.2s, 1 patched→1).

**Per entry stored** (no raw data): baseId(=MFT idx+1) PK, all fileIds, MFT header
(offset/size/comp_flag/crc/mft_usize), **verified compression truth** `actually_compressed`
0/1 (tries Method0; `error='flagged-compressed-but-not'` for entries flagged comp_flag!=0 but
decompress fails — Local.dat had 4, e.g. base 23 = 1MB "compressed" but isn't), sizes
on-disk→`size_stripped`(after strip_crc32)→`size_final`(after decompress), sniffed `type`

**SIZES: use `uncompressed_size`, NOT `mft_usize` (2026-07-25).** The MFT header's own
uncompressed-size field is **0 for every COMPRESSED entry** — measured on a retail
Gw2.dat index: `mft_usize` is 0 for all **803112** comp_flag=8 rows and non-zero for all
**5043** comp_flag=0 rows. So it is useless as "the uncompressed size" and reading it
gives 0 almost everywhere (this bit the castlemist MFT list's "Uncompressed" column).
The real decompressed byte count has always been in `size_final`; the schema now also
writes it to a clearly-named **`uncompressed_size`** column (same value, `size_final`
kept for backward compat), and the MCP `idx_query` returns it. Existing DBs migrate in
~2.5s without re-indexing: `ALTER TABLE entries ADD COLUMN uncompressed_size INTEGER;
UPDATE entries SET uncompressed_size = size_final;` (done for gw2_index.db,
castlemist/gw2_index.db, local_index.db). Verified accurate against extraction: base
3203 → 10926020 bytes = exactly what `gw2_extract` wrote. Only 13/808155 rows are 0
(genuinely empty entries); the 462 error rows all have a non-zero value. Readers should
prefer `uncompressed_size` and fall back to `size_final` when the column is absent
(castlemist's `index_db.cpp` probes it via `PRAGMA table_info`).
(packfile/texture/strs/asnd/dds/…) + packfile `container` fourcc, and for packfiles the
**chunks** = fourcc+version(rd16 at chunk-data start)+**template struct variant**
(`fileTypes[container][chunk][ver]` → global `chunks[chunk][ver]`; empty=not in template).
Chunk walk: `pos=rd16(6)`; loop fourcc(4)+chunkSize=rd32(pos+4)+ver=rd16(pos+8), `next=pos+8+chunkSize`.
Schema: `entries`, `file_ids(file_id PK,base_id)`, `chunks(base_id,seq,fourcc,version,struct_variant,chunk_size)`, `meta`.

**MCP** `gw2index/mcp/server.py` (FastMCP "gw2index", registered in `gw2app/.mcp.json`, env
`GW2_INDEX_DB`→the db; **restart Claude Code to load**). Read-only SQLite. Tools:
`idx_stats` (meta+counts+type/container histograms+compression-truth), `idx_lookup(file_id|base_id)`
(full record+fileIds+chunks), `idx_query(type/container/compressed/false_compressed/has_error,limit,offset)`,
`idx_find_chunk(fourcc,container?,version?)` (every entry with a chunk), `idx_chunk_variants`
(distinct container→chunk→version→struct map — resolves duplicate-chunk ambiguity). Selftest:
`GW2_INDEX_DB=… python server.py --selftest`. See `gw2index/README.md`. Local.dat result:
300 packfile(299 ARMF+1 locl)/12 empty/10 binary; ARMF/MFST v6→PackAssetManifest ×298; locl
chunks (audo/core/grfx) have empty variant (not in template yet).
