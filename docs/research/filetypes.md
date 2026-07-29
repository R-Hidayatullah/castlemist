# GW2 filetype research & castlemist support — handoff

Status snapshot for resuming after a context reset. Covers the file types added to
**castlemist** (audio, PIMG, cntc) plus the reverse-engineering of the **content
datastore (cntc) ↔ text-pack (txt*) ↔ asset** system.

Environment: IDA has `Gw2-64-disable-aslr.exe` loaded (imagebase 0x140000000). gw2 MCP
(`gw2_sniff`/`gw2_extract`/`gw2_parse_packfile`/`gw2_lookup`/`gw2_decode_texture`) is
the go-to for the dat. Steam dat: `C:/Program Files (x86)/Steam/steamapps/common/Guild Wars 2/Gw2.dat`.

Build recipe + all implementation detail live in memory `castlemist-app.md`. This doc
is the *research narrative* + the unfinished threads.

---

## 1. DONE — implemented & verified in castlemist

### Audio (ASND / ABNK / raw asnd / AMSP) — plays MP3 + Ogg
- **ASND** (container `ASND`, PF v1): chunk ASND → `AsndFileData` → embedded **MP3**
  (0xFF sync + Xing). `gw2_audio.hpp` finds it by signature; `dr_mp3` decodes; Win32
  **waveOut** plays. UI: `PreviewKind::Audio` + ▶Play/■Stop.
- **ABNK** (container `ABNK`, chunk `BKCK`, PF v5): a bank of N MP3s. `gw2_audio.hpp`
  **frame-walks** MP3 frames (`mp3FrameLen`, full MPEG tables) to split sounds. UI adds
  a sound-selector combo (ID_AUDIO_COMBO). Verified 159071 = 10 sounds.
- **raw `asnd`** (magic `asnd`, not PF, e.g. 756508): 36 B header + MP3, found by scan.
- **AMSP** (container `AMSP`, PF v5, 64-bit ptrs, chunk AMSP v33): a sound BANK/SCRIPT
  whose audio lives in **external ASND entries by fileId**. `parse_amsp_sound_ids`
  (entry_extractor.cpp): metaSound array_ptr @chunk+60 `{u32 count, i64 selfRelPtr}`;
  `MetaSoundDataV31` stride 326, fileName array_ptr @elem+60; `FileNameDataV31` stride
  36, fileName @fe+24 = 64-bit self-rel ptr → `{u16 lo,u16 hi}` → **fileId = 0xFF00*hi +
  lo - 0xFF00FF**. `build_amsp_audio` loads up to 512 referenced ASND → clips. Some AMSP
  sounds are **Ogg Vorbis** → added `stb_vorbis` (compiled as C, `src/stb_vorbis_impl.c`).
  Verified 9235/18312/831542/87633(Ogg)/69550 → 451-512 clips.

### PIMG (paged image atlas) — composited preview
- Container `PIMG`, chunk `PGTB` v3. NOT pixels: a grid of tiles, each an EXTERNAL
  texture by fileId. `parse_pimg`: `layers[0].strippedDims` = tile WxH, `strippedFormat`
  fourcc (DXT5); `strippedPages` (24 B/page: layer, coord(cx,cy)@+4, **filename@+12**,
  flags, solidColor). **filename = self-relative ptr → {lo,hi} → same fileId decode.**
  `composite_pimg` downscales the whole atlas to ≤2048, decodes ≤300 tiles, blits →
  `PreviewKind::Image`. Verified small atlas 3010939 (80 tiles = a zone map) & giant
  map 100169 (67×104 tiles).

### cntc (content datastore) — summary text preview
- Container `cntc`, PF v5, single chunk `Main` = **`PackContent`**. `parse_cntc_summary`
  reads 11 `{u32 count, i64 selfRelPtr}` arrays @ base+4+i·12 (base = chunk+16), order:
  typeInfos, namespaces, fileRefs, indexEntries, localOffsets, externalOffsets,
  fileIndices, stringIndices, trackedReferences, **strings**(wchar_ptr[], codenames like
  `FOyVJ.AL3Ye`), **content**(byte[] blob ~8.5 MB). Shows counts + namespace names +
  first 60 codenames → `PreviewKind::Text`. Verified 1282844/1282837/1282833.
- **Asset-reference view (§2c)**: the cntc preview now also lists the external assets it
  references — `cntc_referenced_asset_ids` (fileIndices→fileId) + `classify_fileid` (peek
  each entry's magic) → e.g. 1282844 = "References 2178 fileIds: 73 texture / 22 model / 17
  audio / 1 AMAT". Confirms cntc = asset DB referencing texture/model/audio by fileId.

### Also identified (not fully parsed)
- **prlt** container → chunk `mfst` v2 = `ContentPortalManifest` (portal→asset dep table).
- **AMSP** was earlier mis-called "metadata only" — corrected (it references sounds).

---

## 2. RESEARCH — cntc ↔ text-pack ↔ asset (the "asset database" question)

**User hypothesis (confirmed):** cntc is GW2's content/asset database; content objects
reference textures/models by fileId AND text (names/descriptions) by textId.

### 2a. Text-pack system (txt*) — CONFIRMED via IDA + template
Three separate packfile containers, all part of GW2 localization:
- **`txtm`** → `TextPackManifest` = `{ dword stringsPerFile, TextPackLanguage languages[] }`;
  `TextPackLanguage = { filename filenames[] }`. **This is the index.**
- **`txtv`** → `TextPackVoices` = `{ TextPackVoice voices[] }`, `TextPackVoice={textId,voiceId}`.
- **`txtV`** loads chunk **`vari`** → `TextPackVariants` = `{TextPackVariant variants[]}`,
  `TextPackVariant={textId, dword variantTextIds[]}` (gendered/plural forms).
- **`txtp`** → `TextPackPasswords` = `{ dword stringCount, TextPackPassword passwords[] }`,
  `TextPackPassword={dword textId, qword password}` — **per-string RC4 keys** (encrypted strings).

IDA: text-pack class vtable @ `0x142a28860`. Lazy-load methods (each pulls one chunk via
generic chunk-loader `sub_140DDA130`):
- `sub_1410CAA40` loads `txtm` (fourcc 1836349556) → field +0x48
- `sub_1410CAAF0` loads `txtV`/`vari` → +0x58
- `sub_1410CABA0` loads `txtv` → +0x60
- caller/holder: `sub_1410CBC80`.

### 2b. The resolution chain — WORKS, real strings extracted
```
cntc content object → textId  (a name/description string-ID)
  → txtm manifest: file  = languages[lang].filenames[ textId / stringsPerFile ]
                   index =                            textId % stringsPerFile
  → that data file is a `strs` string table
  → UTF-16 string at [index] = the text
```
Verified with txtm **110865**: stringsPerFile=**1024**, **6 languages** × **1169**
data-file fileIds. Lang0 (English) `filenames[0]` = fileId **2440724** → a `strs` file
(castlemist already decodes strs). Real strings decoded from it:
- "Wildflame Caverns", "Double-click to gain karma.",
  "Double-click to unpack a full set of level 80 armor."
- (Locked/encrypted records show trailing garbage; plaintext ones decode directly. The
  RC4-locked ones need the `txtp` password for that textId.)

Lang→fileId bases in 110865: lang0=2440724.., lang2=2441716.., lang3=2442708..,
lang4=2443700.. (each +992 apart, 1169 files each; lang1 empty).

### 2c. cntc → asset (texture/model) reference mechanism — **CONFIRMED** ✅
**cntc content records reference textures/models DIRECTLY by fileId, and the
`fileIndices` fixup table marks exactly where those fileIds live in the content blob.**

Mechanism (verified on cntc index **6858**, 10.5 MB, a big master-ish content file):
- `fileIndices` array (here count 15170) = `PackContentFileIndexFixup{ u32 relocOffset }`,
  4 bytes each, packed at its dataOff.
- For each fixup: **`fileId = rd32( contentDataOff + relocOffset )`** — i.e. relocOffset
  is relative to the **content byte-array start** (`content` array dataOff = 1475460 here),
  and the dword at that spot is a **raw fileId** pointing to an external asset.
- Proven: those fileIds resolve to real assets — e.g. 74924 = ATEX **3DCX normal map**;
  50214/50352/51465/51466/51469/51470/84506 = **DXT1/3DCX textures**. (~3400 distinct
  "big" fileIds in this one file; a chunk are textures. Some values don't resolve = dat
  drift or non-fileId slots.) The other fixup tables analogously: `stringIndices`→codename
  strings, `externalOffsets{relocOffset,targetFileIndex}`→other content files,
  `localOffsets`→within-file self-rel pointers.
- So YES — cntc is the asset/content DB and points at texture/model **locations by fileId**.
  fileRefs=0 in these files is fine: the fileIds are inline in the content blob (not a
  separate table), just flagged by fileIndices.

Repro (python): read `cntc_master.bin` (idx 6858), `fidx_off=1132364`, `content_off=1475460`;
`for i: fileId = rd32(content_off + rd32(fidx_off+i*4))`; sniff the big ones → textures.

- **IDA oddity (still true):** fourcc `cntc` (63 6E 74 63) has **ZERO occurrences** in the
  exe — the client doesn't parse cntc with a hardcoded fourcc; content is consumed via the
  generic packfile/type registry. (txtm/txtv/txtV DO appear as literals.) So to fully build
  the content→asset map you still need the per-type SCHEMA (which record field is icon vs
  model vs name-textId) — but the raw asset fileIds are right there in the blob.

---

## 3. TODO / next steps (unfinished)

1. ~~Prove cntc → texture/model~~ **DONE (§2c)**: fileIds are inline in the content blob at
   the `fileIndices` fixup offsets (`rd32(contentOff+relocOffset)`); confirmed textures.
   Only 1 cntc container found via scanpf (idx 6858); all cntc have fileRefs=0 (inline refs).
2. **Content schema** — per `PackContentTypeInfo` (guidOffset, uidOffset, dataIdOffset,
   nameOffset, trackReferences), figure out which field of a content record is the icon
   texture vs model vs name-textId. Needs the master manifest's typeInfos + the record
   layout in the blob. Big effort ≈ reimplementing GW2's content datastore.
3. **RC4-locked strings** — for encrypted `strs` records, pull the qword password from the
   `txtp` chunk for that textId and RC4-decrypt. (Plaintext records already readable.)
   castlemist's `strs_view.cpp`/`gw2strs` already flags packed/`baseChar!=0` (RC4) records.
4. **Optional castlemist feature**: a "text pack" browser — given a txtm, resolve any
   textId to its string by following the chain above (reuse the existing strs decoder).
5. **Optional**: lazy-load AMSP/ABNK sounds on selection instead of loading up to 512
   upfront (removes the 512 cap; instant open).

## Key reusable facts
- **GW2 filename→fileId decode** (used everywhere: model/PIMG/AMSP/cntc/txtm filerefs):
  field is a self-relative ptr (32-bit in PF v1, 64-bit in PF v5) → record `{u16 lo,u16
  hi}` → `fileId = 0xFF00*hi + lo - 0xFF00FF`. (= `castlemist::model::decodeFilenameAt`.)
- **PF v1 = 32-bit ptrs, PF v5 = 64-bit ptrs.** array_ptr = `{u32 count, ptr}` (ptr 4 or 8
  bytes, self-relative from its own position). Chunk header = 16 bytes (fields at chunk+16).
- Sniff→mftIndex map used in tests: txtm 110865→58569, txtv 198300→69219, txtV 198298→156893,
  cntc 1282844→790199, AMSP 9235→11470 / 87633→95992.
