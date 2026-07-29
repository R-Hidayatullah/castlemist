# Generating the data files

castlemist reads four data files it does not ship. Each is produced by a tool
under [`tools/`](../tools), each lands in [`dumps/`](../dumps), and each is
optional -- the browser starts without any of them, it just cannot do the
corresponding job.

| file | goes in | produced by | without it |
| ---- | ------- | ----------- | ---------- |
| `gw2_packfile.json` | `dumps/packfile/` | `tools/structs/gen_gw2_json.py` in IDA | models show hex, not geometry |
| `gw2_index.db` | `dumps/index/` | `tools/gw2index` | no type/container columns or filters |
| `textkeys.csv`, `strs_textbase.csv` | `dumps/strs/` | `tools/strs/*.py` | packed strings stay locked |
| `bink2w64.dll` | `dumps/binaries/` | extracted from the dat | cinematics do not play |

None of it is in git. It is derived from *your* `Gw2.dat`: a different game
build produces a different MFT, so a committed index would be wrong for
everyone but the person who committed it.

The lookups climb from the executable's directory towards the repository root,
so running out of `build/<preset>/bin/` finds `dumps/` with no configuration.

---

## 1. The struct template — `gw2_packfile.json`

### What it is

GW2 packfiles are tightly packed structs with no field names on disk. The
client carries a **reflection table** describing every chunk version:
`chunk_info` records pointing at descriptor arrays that name each field and its
kind. `gw2_packfile.json` is that table, dumped.

Without it the extractor can still tell that an entry *is* a MODL — the `PF`
header and the `GEOM` chunk are structural — but it cannot walk the geometry.

### Schema

```json
{
  "format": "gw2packfile",
  "pointerSize": 64,
  "chunks": { "MODL": { "70": "ModelFileDataV70" } },
  "types":  { "ModelFileDataV70": { "fields": [ { "name": "...", "kind": "..." } ] } }
}
```

Field kinds:

| Group | Kinds |
|-------|-------|
| Fixed-size primitives | `byte byte3 byte4 byte16 word word3 dword dword2 dword4 qword float float2 float3 float4 double fileref token32 token64` |
| Pointer-width data | `filename wchar_ptr char_ptr` |
| Composite | `struct` (+`type`), `array` (+`element`,`count`), `array_ptr` (+`element`), `ptr_array_ptr` (+`element`), `ptr` (+`target`) |

`array_ptr` is `{u32 count, ptr rel}` where `rel` is relative to **itself**
(field + 4), not to the chunk.

### How to regenerate it

You need IDA Pro with a Guild Wars 2 x64 IDB (ASLR disabled makes the addresses
stable).

```
IDA  ->  File  ->  Script file...  ->  tools/structs/gen_gw2_json.py
```

It writes `gw2_packfile.json` next to the IDB. Move it to
`dumps/packfile/gw2_packfile.json`.

The companion `tools/structs/dump_gw2_structs.py` writes the same reflection
data as readable text, for building the 010 Editor templates in
`tools/templates/` -- useful when checking a field layout by hand.

### When to regenerate

After a game patch that changes a chunk version. Symptoms: models that used to
load now come up empty, or a chunk shows a version number with no struct
variant in `gw2index`'s output.

### Loading it

Automatic at startup, or **File → Load Struct JSON…**. Reloading is safe while
a background extraction is in flight: `gw2tpl` hands out a `shared_ptr`
snapshot, so an in-progress read keeps the template it started with.

---

## 2. The index database — `gw2_index.db`

### What it is

A SQLite index of every MFT entry, so the browser can filter 800 000 rows by
type and container, and show real decompressed sizes, without re-scanning the
87 GB archive. It also records **verified compression truth**: GW2 has entries
flagged compressed that are not, and the indexer tries the decompress and
records what actually happened.

### Building the indexer

```bash
bash ../gw2index/build.sh          # caches sqlite3.o, then builds gw2index.exe
```

### Indexing an archive

```bash
cd ../gw2index

# The small one first — your account's Local.dat, ~322 entries, about a second.
./gw2index.exe --dat "$APPDATA/Guild Wars 2/Local.dat" \
               --out local_index.db --template gw2_packfile.json

# The real one. 87 GB; give it threads.
./gw2index.exe --dat "C:/Program Files (x86)/Steam/steamapps/common/Guild Wars 2/Gw2.dat" \
               --out gw2_index.db --template gw2_packfile.json --threads 16
```

| Flag | Meaning |
|------|---------|
| `--dat` | The archive to scan |
| `--out` | The SQLite file to write |
| `--template` | `gw2_packfile.json`, so chunks get their struct variant |
| `--threads` | Worker count |
| `--full` | Force a complete re-index |

**Resumable**: a re-run skips entries whose fingerprint
(`offset|size|crc|flag`) is unchanged, so re-indexing after a game patch only
touches what the patch touched.

### Schema

```sql
entries(base_id PK, offset, size, comp_flag, crc, mft_usize, fingerprint,
        actually_compressed, size_stripped, size_final, uncompressed_size,
        magic, type, container, error)
file_ids(file_id PK, base_id)
chunks(base_id, seq, fourcc, version, struct_variant, chunk_size)
meta(key, value)              -- 'dat_path' is the archive it was built from
```

`uncompressed_size` was added later; `castlemist::db` falls back to
`size_final` when a database predates it, so old indexes keep working.

### Loading it

**File → Open Index DB…**, or automatically at startup. Opening an index also
opens the archive recorded in `meta.dat_path`, so previews work immediately.

### Querying it outside the browser

`mcp/gw2index/` is an MCP server over the same database: `idx_stats`,
`idx_lookup`, `idx_query`, `idx_find_chunk`, `idx_chunk_variants`. See
[`mcp/README.md`](../mcp/README.md).

---

## 3. String keys — `textkeys.csv` and `strs_textbase.csv`

### What they are

`strs` string tables hold two kinds of record. Raw UTF-16 records
(`baseChar == 0`, `rangeBits == 16`) decode byte-exactly. **Packed** records are
bit-packed and RC4-encrypted with a per-`textId` 8-byte key that exists only in
the running client's memory — no key, no text.

- `textkeys.csv` — `textId,key8_hex`, captured from a live client.
- `strs_textbase.csv` — `fileId,baseTextId`, derived from the TextPackManifest
  for all five languages. A record's global `textId` is
  `baseTextId + recordIndex`, which is how a record finds its key.

### How to regenerate them

Attach to a running client to capture the keys:

```bash
python tools/strs/gw2_capture_textkeys.py
```

Then walk the txtm manifests for the base ids:

```bash
python tools/strs/strs_decode.py
```

Both write into `dumps/strs/`.

### Loading them

Automatic at startup, or **File -> Load String Keys...**. Records whose key was
never captured stay flagged `<no key>` in the string table rather than showing
wrong text.

---

## 4. The Bink runtime -- `bink2w64.dll`

It lives *inside* the dat: BINARIES `MZx`, baseId 140117 / fileId 1247272.
Extract it with `gw2dat_cli` (or copy it out of the game directory) into
`dumps/binaries/`. `GW2_BINK_DLL` overrides the location.

Every entry point is resolved with `GetProcAddress`, so its absence only
disables playback.

The shipped DLL is **2.7p** and castlemist builds against the **2.7d** headers
in `external/bink2-2.7d/`, because that is the ABI it matches. Do not swap in
the newer SDK headers -- see `external/bink2-2.7d/README.md`.
