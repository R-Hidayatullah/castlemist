---
name: gw2-cmp-img-symbol-map
description: "Named IDA symbol map for the Compress (CmpApi/CmpDict/CmpHuff) and image-decode (ImgAtex/ImgDecode/ImgFmt) clusters, recovered from embedded Perforce source paths — supersedes the stale addresses in older notes"
metadata:
  node_type: memory
  type: reference
  modified: 2026-08-16
---

# Compress + image-decode symbol map (Gw2-64.exe, imagebase 0x140000000)

Applied to `gw2decomp/Gw2-64.exe.i64` on 2026-08-16 — 66 functions/globals renamed,
24 function and data comments written. Re-apply with
`tools/ida_apply_cmp_img_names.py` (idempotent, verifies before writing).

## Read this first: older notes have stale addresses

**Every `sub_14…` address in [[gw2-atex-atep-decode]], [[gw2-method0-degenerate-huff]] and
[[atex-debug-notes]] is from an earlier client build and does not resolve in the current
one.** They all land *inside* a function rather than at its entry, which makes them look
plausible while being wrong. Two of them are actively misleading:

| old note says | actually, in this build |
| --- | --- |
| `sub_140D9F8C0` = Method 0 inflate | `0x140D9F8C0` is inside `Cmp_CompressMethod1` — a **compressor**. Method 0 inflate is `Cmp_DecompressMethod0` @ `0x140DA27F0` |
| `sub_140D9D720` = Method 1 | inside `Cmp_Decompress` (the API entry), not a codec |
| `sub_140DA6800` = CmpHuff table build | that range is now a block of generated match-encoder variants; the builder is `CmpHuff_BuildDecodeTable` @ `0x140DA9730` |
| `sub_140B83040` = ImgAtex_Decode | `0x140B82FA0`, 423 bytes — far too small. Decoder is `ImgAtex_Decode` @ `0x140B86B70` (9848 bytes) |

**Do not chase addresses across builds.** The reliable anchor is the retail binary's
embedded Perforce source paths (`D:\Perforce\Live\NAEU\v2\Code\Arena\...`). Every
translation unit's assert strings point at its own path, so xrefs to that string
enumerate the functions belonging to that `.cpp`. That is how this map was rebuilt and
how it should be rebuilt after the next patch.

```python
# the whole technique, essentially
for xr in idautils.XrefsTo(ea_of_source_path_string):
    ida_funcs.get_func(xr.frm)   # -> belongs to that .cpp
```

## Arena\Services\Compress

Module spans `0x140D9CF70`–`0x140DABFFF` (114 functions). Source-path anchors:
CmpApi.cpp `0x1420A1E20`, CmpDict.cpp `0x1420A2450`, CmpHuff.cpp `0x1420A2C40`,
CmpIo.h `0x141C92B68`.

| addr | name | notes |
| --- | --- | --- |
| `0x140D9D7B0` | `Cmp_Compress` | CmpApi.cpp:38 |
| `0x140D9D9F0` | `Cmp_Decompress` | CmpApi.cpp:111 — **the entry point for every compressed Gw2.dat MFT entry** |
| `0x140DA27F0` | `Cmp_DecompressMethod0` | 2 Huffman tables/block, self-contained |
| `0x140DA0650` | `Cmp_DecompressMethod1` | 3 tables/block, delta vs `oldSourceData` |
| `0x140DA1220` | `Cmp_CompressMethod0` | |
| `0x140D9E970` | `Cmp_CompressMethod1` | |
| `0x140DA9730` | `CmpHuff_BuildDecodeTable` | shared by both decompressors |
| `0x140DA9FC0` | `CmpHuff_BuildCanonicalCodes` | encoder side only |
| `0x140D9E8F0` | `Cmp_EncSymLutLookup` | sole reader of `Cmp_EncSymLut` |

`CmpHuff_Enc_140DA9DA0 / _140DAA430 / _140DAA550 / _140DAA7A0` are module-prefixed but
their individual roles are unverified — all four are reachable only from
`CmpHuff_BuildCanonicalCodes`, so none of them are on the decode path.

### Method 0 bit layout (verified against the decompilation)

```
header:  4 bits -> minMatchLen - 1
         4 bits -> (blockSymbolCount >> 12) - 1      # block <= 4096 symbols
per block, two tables from CmpHuff_BuildDecodeTable:
  litLenTable   sym <  0x100 -> literal byte
                sym >= 0x100 -> len  = Cmp_LenBase[sym-256]
                                     + read(Cmp_LenExtraBits[sym-256])
                                     + minMatchLen
  distTable     dist = Cmp_DistBase[d] + read(Cmp_DistExtraBits[d])
                copy source = outPtr - (dist + 1)
```

Bitstream struct (CmpIo.h, `m_rackData0`): `+0` read ptr, `+8` end ptr, `+16` bitCount,
`+20` rack0, `+24` rack1. Words are consumed 32 bits at a time, MSB-first.

### Tables

| addr | name | shape |
| --- | --- | --- |
| `0x1420A2210` | `Cmp_DistBase` | `word[30]` = 0,1,2,3,4,6,8,12,…,24576 |
| `0x1420A2250` | `Cmp_LenBase` | `byte[29]` = 0..8,10,12,14,16,20,…,224,255 |
| `0x1420A2390` | `Cmp_DistExtraBits` | `byte[30]` = 0,0,0,0,1,1,2,2,…,14,14 |
| `0x1420A2430` | `Cmp_LenExtraBits` | `byte[28]` = 0×8, 1×4, 2×4, 3×4, 4×4, 5×4 |
| `0x1420A2270` | `Cmp_EncSymLut` | `byte[288]`, encoder only |
| `0x1420A28D0` | `CmpHuff_BootRangeTbl` | 14 × `{u32 threshold, u32 baseIndex}` |
| `0x1420A2940` | `CmpHuff_BootSymTbl` | packed `(runLength << 5) \| codeLength` |

`Cmp_DistExtraBits` sits immediately after `Cmp_EncSymLut`, so the decompiler renders it
as `Cmp_EncSymLut[sym + 288]` — the two were one array in IDA until they were split.
That displacement is expected, not a mis-analysis.

### The degenerate Huffman table, re-confirmed at source

The patch described in [[gw2-method0-degenerate-huff]] is real and is right here in
`CmpHuff_BuildDecodeTable`:

```c
if ( totalSymbolCount && !assignedCount )   // length block decoded to all zeros
{
    nextSymbol[totalSymbolCount - 1] = headByCodeLen[0];
    headByCodeLen[0]  = totalSymbolCount - 1;   // LAST symbol, not symbol 0
    countByCodeLen[0] = 1;
}
```

The alphabet collapses to symbol `totalSymbolCount - 1`, carried in **zero bits**. This is
legal input, not corruption. Substituting symbol 0 appears to work only while
`Cmp_DistExtraBits[0] == 0`, and desyncs wherever `Cmp_DistExtraBits[total-1] != 0`,
because the symbol also selects how many extra bits follow.

## Arena\Engine\Gr\Img

| addr | name | notes |
| --- | --- | --- |
| `0x140B86B70` | `ImgAtex_Decode` | ATEX/ATTX/ATEC/ATEP/ATEU/ATET, 9848 bytes |
| `0x140B892D0` | `ImgAtex_Encode` | ImgAtex.cpp:750–1548, writes containers |
| `0x140B86620` | `ImgAtex_MirrorTerrainBorders` | ImgAtex.cpp:1107 |
| `0x140B86A90` | `ImgAtex_DecodeCleanup` | ImgAtex.cpp:1712, frees the 168-byte `detail` |
| `0x140B187E0` | `ImgDecode_Run` | streaming driver |
| `0x140B18B40` | `ImgDecode_ProbeFormat` | ImgDecode.cpp:181 |
| `0x140B8BCC0` | `ImgDds_Decode` | |
| `0x140B3FC80` | `ImgFmt_GetFlags` | ImgFmt.cpp:198 |
| `0x140B40740` | `ImgFmt_GetBitCount` | ImgFmt.cpp:267 |
| `0x140B40890` | `ImgFmt_GetBlockDims` | ImgFmt.cpp:293 |
| `0x140B3F730` | `ImgCalc_LevelSize` | name recovered verbatim from an assert string |
| `0x141C80410` | `ImgFmt_Flags` | `dword[38]`, `GR_FORMATS == 0x26` |
| `0x141C801A0` | `ImgFmt_InfoTable` | stride 3 dwords, `[0]` = bits per pixel |

`ImgAtex_Enc_140B84C20 / _140B861B0 / _140B85280 / _140B85990 / _140B84B60` are the
constant-fill / plane-split block passes. They are named by module because all five are
reachable only from `ImgAtex_Encode`; the decode-side mirrors of these were not
separately isolated.

### Probe chain — nine formats, not seven

`ImgDecode_ProbeFormat` tries nine `(probe, decodeProc)` pairs in order, latching the
winner into `detail->proc`, and falls through to `"Unknown image file format"`:

| probe | decode |
| --- | --- |
| `0x140B8AD50` `ImgBmp_Probe` | `0x140B8AAE0` `ImgBmp_Decode` |
| `0x140B8B650` `ImgDcx_Probe` | `0x140B8B500` `ImgDcx_Decode` |
| `0x140B96480` `ImgGif_Probe` | `0x140B961D0` `ImgGif_Decode` |
| `0x140B94300` `ImgJpeg_Probe` | `0x140B941A0` `ImgJpeg_Decode` |
| `0x140B971A0` `ImgPcx_Probe` | `0x140B97050` `ImgPcx_Decode` |
| `0x140B97A70` `ImgPng_Probe` | `0x140B979A0` `ImgPng_Decode` |
| `0x140B986A0` `ImgPsd_Probe` | `0x140B983A0` `ImgPsd_Decode` |
| `0x140B9A760` `ImgTiff_Probe` | `0x140B9A630` `ImgTiff_Decode` |
| `0x140B9B150` `ImgTga_Probe` | `0x140B9AE50` `ImgTga_Decode` |

[[gw2-atex-atep-decode]] says "7 image loaders" — that was a miscount, and it does not
affect that note's conclusion. ATEX and DDS are dispatched by `ImgDecode_Run` *before*
this chain, so they are not among the nine.

### GR_FORMAT enum, read off the encoder's switch

Confirms the values in [[gw2-atex-atep-decode]] exactly:

```
DXT1=22 DXT2=23 DXT3=24 DXT4=25 DXT5=26 DXTA=27
DXTL=28 DXTN=29 3DCX=30 BC5X=31 BC7X=32     (GR_FORMATS = 38)
```

### The container tag is still write-only

`ImgAtex_Decode` references all six container magics and every fourCC, but the per-magic
tag (`ATEC=1 ATEP=2 ATEU=4 ATET=0x20`) it computes is stored at `decoder+0x4C` and never
read anywhere in the pipeline. Nothing found in this pass contradicts
[[gw2-atex-atep-decode]]: **ATEP decodes byte-identically to ATEX**, and there is no
container-driven pixel transform.

The `0x10` per-mip flag is likewise confirmed as the terrain-border case:
`ImgAtex_MirrorTerrainBorders` mirrors the outermost two rows and columns of a 64×64
block grid, i.e. a 256×256 texture, with `TERRAIN_BORDERS == 0xC0000003`.

## Arena engine runtime (binary-wide, not specific to these clusters)

These are shared primitives. Naming them pays off in *every* function you open, not just
the two clusters above — `Arena_AssertFailed` alone has ~46,000 call sites.

| addr | name | notes |
| --- | --- | --- |
| `0x1409DDA80` | `Arena_AssertFailed` | `(expression, sourceFile, line)`. ExeError.cpp. **~46,000 xrefs** |
| `0x1409DDC30` | `Arena_FatalFormatted` | `(sourceFile, line, format, ...)`; the `"No valid case for switch variable"` arms |
| `0x1409BDC00` | `Arena_Log` | Log.cpp:288, `(severity 0..4, format, ...)`, fans out to 5 sinks |
| `0x140235910` | `Arena_ReportError2Args` | Array.h bounds: `"Index: {}, Count: {}"` |
| `0x140239260` | `Arena_ReportError3Args` | Array.h growth: `"New Count: {}, Count: {}, Alloc: {}"` |
| `0x1409CFEF0` | `Arena_Alloc` | ExeMemFlexible.cpp:1146; asserts `bytes < 0x7FFFFFEFFFF`, tags the block |
| `0x1409CF780` | `Arena_Free` | null-safe; un-bills the block's category |
| `0x1409D0170` | `Arena_TryResize` | ExeMemFlexible.cpp:1205, in-place grow/shrink → bool |
| `0x1409CF760` | `Arena_TagAlloc` | **returns its 2nd argument unchanged** — tracking compiled out in retail |
| `0x1409D0280` | `Arena_GetAllocSize` | block size, via the allocator vtable |
| `0x1409B9DC0` / `0x1409B9D90` | `Arena_MemZero` / `Arena_MemSet` | memset wrappers |
| `0x140A1FF20` | `Arena_ArraySetCount` | Array.h:732; array is `+0x10` capacity, `+0x14` count |
| `0x140B84B60` | `Arena_AllocatorSetSize` | Allocator.h:620; `+0` live, `+2` category, `+8` buffer |

**Memory categories.** `Arena_Alloc`'s last argument is a category id produced by a
magic-static getter. The category names are not guesses — each category object's vtable
slot 0 returns its own label, which is how these were identified:

| addr | name | vtable slot 0 returns |
| --- | --- | --- |
| `0x1409B9840` | `Arena_MemCategory_Uncategorized` | `"Uncategorized"` |
| `0x1409B6E20` | `Arena_MemCategory_Array` | `"Array"` |
| `0x140A6E8F0` | `Arena_MemCategory_GrImg` | `"Gr Img"` |

Others visible in the same vtable block and worth naming later: `"Root"`, `"BTree"`,
`"Gr Geometry"`.

## The complete GR_FORMAT enum

`ImgFmt_GetName` (`0x140B40900`) indexes `ImgFmt_Names` (`0x142581B20`), a
`wchar_t*[38]` of ArenaNet's own labels. That gives the whole enum by name rather than
the 22–32 range the older notes covered. A `GR_FORMAT` enum with these members is
declared in the IDB, so format comparisons now render symbolically
(`case GR_FORMAT_DXTL:` instead of `case 28:`).

Cross-check: `GR_FORMAT_P_8 == 20` matches the `"tmpFormat != FORMAT_P_8"` assert fired
from case 20 of the encoder's switch — independent confirmation the indices line up.

| # | name | flags | bpp | block |
| --- | --- | --- | --- | --- |
| 0–6 | `ARGB 32323232F`, `ARGB 16161616F`, `ARGB 2101010`, `ARGB 8888`, `XRGB 8888`, `ARGB 4444`, `ARGB 1555` | `0xb2`/`0x12`/`0x72` | 128→16 | — |
| 7–14 | `RGB 888`, `RGB 565`, `RGB 555`, `RG 1616`, `RG 1616F`, `RG 3232F`, `R 16F`, `R 32F` | `0x12`/`0x100` | 24→16 | — |
| 15–21 | `AL 88`, `AL 44`, `AL 8`, `L 8`, `A 8`, `P 8`, `VU 88` | `0x1a4`/`0x104`/`0xa2`/`0x78`/`0x400` | 16→8 | — |
| 22–29 | `DXT1`, `DXT2`, `DXT3`, `DXT4`, `DXT5`, `DXTA`, `DXTL`, `DXTN` | `0x71`/`0xb1`/`0xa1`/`0x11`/`0x201` | 4 or 8 | 4×4 |
| 30–32 | `3DC`, `BC5`, `BC7` | `0x201`/`0xb1` | 8 | 4×4 |
| 33–37 | `D24`, `SHADOWMAP`, `ABGR 8888`, `R 32UINT`, `RGBE 9995` | `0x00`/`0xb2`/`0x12` | 32 | — |

Note the official spellings are `3DC`, `BC5`, `BC7` — the *fourCCs* in the container are
`3DCX`, `BC5X`, `BC7X`. Do not conflate the two.

## The ATEX RLE Huffman table — resolved

`ImgAtex_RleHuffTable` @ `0x141C92AC0`. This is the table the older notes recorded as
`byte_141C86390` and left as an open question ("is the byte-pair order `(val,len)` or
`(len,val)`?"). It is reachable from `ImgAtex_Decode`'s `rle_decodeSymbol` label.

**The order is `(codeLength, value)`, and that is proven by use, not inferred:** the
decoder does `bitBuf <<= ImgAtex_RleHuffTable[2*i]`, so the first byte of the pair is the
bit count; the second byte is the symbol. The index `i` is the **top 6 bits** of the bit
buffer (`bitBuf >> 58`).

All 64 entries reduce to a 3-code prefix set:

| index range | bits consumed | code | value |
| --- | --- | --- | --- |
| 32..63 | 1 | `0` | 0 |
| 16..31 | 2 | `10` | 17 |
| 0..15 | 6 | `11nnnn` | 16, 15, 14, … 1 |

Two consequences for [[atex-debug-notes]], whose analysis of this table can now be closed:

- The 6-bit values in **this** build are strictly descending 16→1, i.e. exactly
  `value = 16 - index`. That is what `gw2_atex.hpp`'s `huff_tables()` already does for
  the first 16 entries, so that part was never wrong.
- The "corrected" block-of-4 ordering (`13-16, 9-12, 5-8, 1-4`) that was tried and
  measured as *worse* does not match this build at all. Reverting it was correct.
- The real gap is entries **16..63**, which are not part of the 6-bit run: they are
  shorter codes (2 bits → 17, 1 bit → 0) with different lengths. A decoder that applies
  `value = 16 - index` and a fixed 6-bit consumption across all 64 slots will desync as
  soon as a `0` or `17` symbol appears. That is the thing to check in `huff_tables()`.

This has **not** been re-tested against sample files here — it is a reading of the
binary, and the measurement still needs doing.

## Backing up the work: two files, two jobs

`tools/ida_apply_cmp_img_names.py` — rebuilds names, types and **folders**. Anchored on
the source-path strings, so it refuses to run against a build where the addresses have
rotted. Idempotent, and stable across repeated runs.

`tools/gw2_ida_symbols.json` + `tools/ida_restore_symbols.py` — the fuller backup:
80 functions, 44 function comments, **449 local variable names**, **85 pseudocode label
names**, 10 data symbols, 4 inline comments, and the `GR_FORMAT` enum. Local variable
names *and* label names exist only inside the `.i64`; nothing else recovers them, which
is why this sidecar exists. Re-export with `export_symbols()` after doing more work.

The JSON is keyed by absolute address and matches locals by index, so it is valid for
**one build only**. After a patch, re-derive with `rebuild_from_source_paths()` and
export fresh.

## Still open

- The decode-side counterparts of the four `ImgAtex_Enc_*` block passes were not isolated;
  `ImgAtex_Decode` inlines much of that work, which is most of why it is 9848 bytes.
- The RLE constant-fill bit order questioned in [[atex-debug-notes]] was **not** re-derived
  here. Note that that file's premise is superseded — the blotchy output it chases was
  later traced to the preview pixel shader ignoring alpha, not to the decoder.
- ~~`byte_141C86390` has no counterpart named in this pass~~ — **resolved**, it is
  `ImgAtex_RleHuffTable` @ `0x141C92AC0`; see the section above. What remains is to
  re-test `gw2_atex.hpp`'s `huff_tables()` against real samples with entries 16..63
  corrected.
- ~~The decode-side counterparts of the four `ImgAtex_Enc_*` block passes were not
  isolated~~ — **partly resolved**: the RLE constant-fill decoder is
  `ImgAtexCommon_DecodeRleBlocks` @ `0x140C213F0` (ImgAtexCommon.cpp:103), identified from
  asserts naming `rleCode`, `RLE_COMMON_MASK` and `detail->rleLength`. It is a much better
  place to read that logic than `ImgAtex_Decode`, where it is inlined into the state
  machine. See [[gw2-ida-naming-coverage]].
- A real `ImgDecoder` struct would be the biggest remaining readability win — it would
  turn `*((_DWORD *)decoder + 17)` into `decoder->format` everywhere. The field offsets
  were extracted (they are in `ImgAtex_Decode`'s comment legend) but a few slots show
  conflicting widths, e.g. `+0x38` used as both dword and qword, so some are unions or
  the inference is incomplete. A wrong struct makes the pseudocode *worse*, so this needs
  a dedicated pass rather than a guess.
- The 234 short-lived register temporaries inside `ImgAtex_Decode`'s fill passes are
  deliberately still `vNN`. They are single-use arithmetic scratch; naming them without
  first working out the per-format fill algorithms would be invention, not documentation.
- Other Arena memory categories are visible in the same vtable block and cheap to name:
  `"Root"`, `"BTree"`, `"Gr Geometry"`.
