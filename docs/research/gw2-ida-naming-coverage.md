---
name: gw2-ida-naming-coverage
description: "Exactly how far the IDB naming pass has got — what is finished, what is deliberately left, and the prioritised worklist for the remaining register temporaries"
metadata:
  node_type: memory
  type: project
  modified: 2026-08-16
---

# IDB naming coverage

State of `gw2decomp/Gw2-64.exe.i64` as of 2026-08-16. Written so the next session does not
have to re-measure. Numbers come from a scan over the 324 functions the backup tracks.

## Finished

| item | state |
| --- | --- |
| Function names | **324 / 324 named**, 0 still carrying an address suffix |
| Function comments | 219 |
| Data symbols | 29 |
| Local variables named | 863 |
| Pseudocode labels named | 85 |
| Enums declared and wired in | 6 |
| Structs / typedefs declared | 44 |
| Functions filed into folders | 324 |
| Local types filed into folders | 68 |
| Fully clean functions (no `vNN`, no `LABEL_nn`) | 30 |

### Folders

Both the Functions window and Local Types now have a folder tree, rebuilt by
`restore_folders()` from prefix rules — so, unlike everything else keyed by
address, the folders survive a client patch.

```
/Arena/Core                       /External/bgfx
/Arena/Services/{Archive3,Packfile,Compress,Crypt,Encoding}
/Arena/Engine/{Text,Map,Model,ModelFileFormat,Video,Cinema}
/Arena/Engine/Gr/{Bgfx,Fvf,Img,Material}
/Gw2/Game/{ChatLink,Scene}
```

The layout mirrors the engine's own source tree — the same
`D:\Perforce\Live\NAEU\v2\Code\Arena\...` paths the naming pass navigates by, so
the two agree by construction.

> **This replaced an earlier taxonomy.** Before 2026-08-16 the tree was grouped
> by theme with reading-order prefixes: `Arena runtime/{Base64, CRC and hashing,
> Collections, Crypt (RC4), Errors and logging, Math, Memory, Token}`,
> `GW2 Decompression/{1 Entry points, 2 Method 0 (standalone), 3 Method 1 (delta
> vs older copy), 4 Huffman code tables}`, `GW2 Image decode/{1 Entry points,
> 2 ATEX game textures, 3 Pixel format info, 4 Standard file loaders, 5 DDS and
> DXT blocks, 6 BC7 (own pipeline)}`, `GW2 Render/{1 Vertex format (GrFvf),
> 2 bgfx vertex layout, 3 Shaders, 4 Materials, 5 Geometry and models,
> 6 Granny animation}`, `GW2 Text strs/{1 Decode entry, 2 Encryption keys,
> 3 Parser}`, `GW2 Chat links/{Entry points, Decoders, Encoders}`. It read
> better as a tour; the source-mirroring tree scales better as the named set
> grows past 300 across a dozen subsystems. Recorded here so it can be put back.

### The six enums

`GR_FORMAT` (38 members, from `ImgFmt_Names`), `CHAT_LINK_TYPE`, `IMG_FILE_TYPE`,
`CMP_METHOD`, `BGFX_ATTRIB`, `BGFX_ATTRIB_TYPE`. All are declared in
`tools/ida_restore_symbols.py` so they survive a restore.

### The struct set

Too large to inline in the script, so they live as real C headers that
`restore_types()` parses on the way in:

| file | covers |
| --- | --- |
| `tools/structs/gw2_ida_types.h` | `bgfx_VertexLayout` (18 attribs — see [[gw2-bgfx-vendored-version]]), `bgfx_Memory`, `bgfx_TextureInfo`, transient buffers, `GR_FVF`, `GrFvfVertexLayout`, `DdiTextureBgfx`, `DdiTextureLock`, `DDI_TEXTURE_*`, `ATEX_MAGIC`, `AtexHeader`, `AmatBgfxData`, `AmatTechnique`, `BgfxShader`, `GR_SHADER_QUALITY` |
| `tools/structs/gw2_ida_types_granny.h` | 15 `granny_*` structs, all under `#pragma pack(1)`, plus `GrannyAttribToFvf` |
| `tools/structs/gw2_ida_types_subsystems.h` | `ArchiveAllocEntry`, `ArchiveDescriptor`, `ARCHIVE_*`, `PackfileHeader`, `PackfileChunkHeader`, `PACKFILE_*`, `MODEL_LOAD_STAGE`, `SCN_CHATTER_LINE` — see [[gw2-archive-packfile-runtime]] |

All three parse with zero errors through `ida_typeinf.parse_decls`. Every offset in them
carries the address it was measured at, in a comment — they are meant to be read, not
just applied.

The payoff is visible in the decompiler:

```c
BgfxVertexLayout_Add(layout, BGFX_ATTRIB_WEIGHT, 4u, BGFX_ATTRIB_TYPE_UINT8, 1, 0);
if ( method == CMP_METHOD0_STANDALONE ) ...
case IMG_FILE_TYPE_DDS:
```

`GrFvf_BuildVertexLayout` now reads essentially as the original C++.

## Findings from the 2026-08-16 render pass

**bgfx is vendored upstream, and the version is pinnable.** Commit `a476c5b9`, rev 8775,
API 128 — read out of the immediates at the banner call site, not just the SHA string.
This matters because `bgfx-master` has 26 attribs where this build has 18, so a
`VertexLayout` copied from master is silently wrong. Full write-up and three
independent fingerprints in [[gw2-bgfx-vendored-version]].

**Texture upload is traced.** All 18 `BgfxTexture.cpp` functions named; entry point is
`BgfxTexture_UploadLevels` (`0x140B15850`). The trap is that `DdiTexture` stores
`GR_FORMAT` at `+0x0C` and `bgfx::TextureFormat` at `+0x10`. See [[gw2-texture-upload]].

**`0x140D4DDF0` was misnamed.** Was `ModelGranny_SampleTrackGroup`; its asserts name
`ModelAnimSubControlData.cpp`, so it is now
`ModelAnimSubControl_BindUvTransformTracks`. Correction recorded in
[[gw2-render-asset-pipeline]].

**Four more subsystems named (second pass, same day).** Archive3 (34) and
Packfile (30) — see [[gw2-archive-packfile-runtime]], which also flags three
wrong field names in our own `MftData`. ModelFile's load-stage state machine
(21). Video/Bink and the scene subtitle path (16) — see
[[gw2-scene-video-subtitles]]. Map is the thin one: only `Map.cpp` and
`MapData.cpp` entry points (8 funcs); `Terrain`, `Zones`, `Prop` and
`Environment` are enumerated but unnamed, and `EnvEnvironment.cpp` alone is
31 functions.

## Two findings from the earlier pass

**`ImgAtexCommon_DecodeRleBlocks` @ `0x140C213F0`** — the ATEX RLE constant-fill *decoder*,
which [[gw2-cmp-img-symbol-map]] had listed as an open question. Identified from its
asserts, which name the fields outright: `rleCode < 0xff`, `(rleCode & RLE_COMMON_MASK) == 0`,
`detail->rleLength <= detail->blockCount`. It is a far better place to study the
constant-fill behaviour than `ImgAtex_Decode`, where the same logic is inlined and tangled
with the streaming state machine.

**`CmpHuff_BootSymTbl` is 768 bytes, not 128** — three 256-byte sections sharing one index
`(runLength << 5) | codeLength`: `[0..255]` decode packed byte, `[256..511]` encode bit
count, `[512..767]` encode value. It is a bidirectional codec table. The earlier note
described only the decode third.

## Deliberately not done

**3543 generic `vNN` locals and 199 `LABEL_nn` remain**, concentrated in a few large
functions. This was a choice, not an oversight: most of those are single-use arithmetic
scratch inside decode loops, and naming them mechanically (`temp1`, `temp2`, …) would make
the code *look* annotated while conveying nothing — worse than leaving `v5`, because it
hides which names were actually derived.

Where a variable carries real meaning it has been named. What is left is the tail.

### Prioritised worklist

Ranked by how likely someone is to read the function, not by count:

| function | generic | labels | why it matters |
| --- | --- | --- | --- |
| `BgfxShaderD3D11_Create` | 133 | 2 | every shader passes through it; blob layout is documented in its comment |
| `ImgAtex_Decode` | 235 | 0 | already has the field-map legend; the fill passes are the gap |
| `ImgDds_Decode` | 221 | 72 | **72 labels** — the worst control-flow readability in the set |
| `Cmp_CompressMethod0/1` | 158 / 207 | 7 / 7 | compressor side; decompressors are already done |
| `GrFvf_ConvertVertices` | 117 | 10 | dispatcher over ~50 per-format converters |
| `ImgDxt_DecodeBlocksSse` | 121 | 1 | SSE, hard to name well without care |
| `ChatLink_DecodeBuildTemplate` | 77 | 5 | the most complex chat link |
| `ImgAtexCommon_DecodeRleBlocks` | 40 | 1 | small and high value — good next target |

`ImgDds_Decode`'s 72 labels are the single highest-value cheap win: labels are control
flow, so they can be named from context far more reliably than register temporaries.

### Named from call-site guard, not from the body

Four functions are named after the format check that selects them in `ImgAtex_Encode`,
because the guard is verified but the bodies were not read line by line. Their comments say
so explicitly:

`ImgAtex_EncodePlane_Dxt2Dxt3`, `ImgAtex_EncodePlane_Dxt4ToDxtl`,
`ImgAtex_EncodePlane_Color`, `ImgAtex_EncodePlane_Secondary`.

Treat those names as a hypothesis with good evidence, not as established fact.

## Backup

`tools/gw2_ida_symbols.json` + `tools/ida_restore_symbols.py` carry 324 functions,
219 comments, 863 locals, 85 labels, 29 data symbols, 4 inline comments, all six enums,
all three struct headers and both folder trees. Re-export with `export_symbols()`
after any further work — it
discovers by name prefix, so newly named functions are picked up automatically.

Two prefix traps worth knowing, both now handled in `NAME_PREFIXES`:

- `bgfx_` (lowercase, the vendored upstream API) is **not** matched by `"Bgfx"`;
  the tuple is case-sensitive.
- `ModelAnimSubControl_…` is not matched by `"Model_"` either, because that prefix
  requires the underscore. `"ModelAnim"` was added for it.

Anything named outside those prefixes is invisible to the backup — it will restore
cleanly and quietly lose the symbol. Add the prefix at the same time as the name.
