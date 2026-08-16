---
name: gw2-render-asset-pipeline
description: "How GW2 loads shaders, models, textures and animation — the two shader sources (58 packages baked into the exe vs AMAT files from the dat) confirmed at the branch, plus the definitive GrFvf → bgfx::VertexLayout builder"
metadata:
  node_type: memory
  type: reference
  modified: 2026-08-16
---

# Render asset pipeline — shaders, FVF, models, animation

Applied to `gw2decomp/Gw2-64.exe.i64` on 2026-08-16. Re-anchors [[vertex-fvf]] and
[[gw2-exe-shaders]], whose addresses are from older builds. Method and caveats as in
[[gw2-cmp-img-symbol-map]].

## The two shader sources — confirmed at the branch

**Yes: some shaders are precompiled inside the exe, and others come from files.** Both
paths were traced to the code that chooses between them.

```
material shaderId
   |
   |  shaderId 0..57  -> McMaterial_GetBuiltInShaderBlob (0x1402F48D0)
   |                     a 58-case switch returning a POINTER INTO .rdata
   |                       case 0 -> unk_14193E0B0, 22716 bytes, 1 technique
   |                       case 2 -> unk_141944920, 27676 bytes, 2 techniques
   |                     complete AMAT packages baked into Gw2-64.exe. No dat file.
   |
   |  shaderId 58      -> MATERIAL_SHADER_CUSTOM
   |     = "MATERIAL_SHADER_CUSTOM must be loaded from a file"
   |                  -> BgfxShader_LoadFromAmat (0x140BFCFF0)   BgfxShader.cpp:698
   |                       GrMat_GetShaderChunk(shaderId, 'BGFX', version 3, &techCount)
   |                     i.e. MODL -> material -> AMAT file in the dat -> BGFX chunk
   v
BgfxShaderD3D11_Create (0x140B5E890)    <- BOTH paths converge here
   parse bgfx blob -> Murmur2A content hash -> CreateVertex/Pixel/ComputeShader
```

`MATERIAL_SHADERS_BUILT_IN = 0x3A = 58`, read straight off the `shaderId >= 0x3A` compare
in `McMaterial_GetOrCreate` (`0x1402F3560`, McMaterial.cpp:1529). `TEXTURE_COUNT_MAX = 4`
from the neighbouring assert at :1528.

| addr | name |
| --- | --- |
| `0x140B5E890` | `BgfxShaderD3D11_Create` — the common sink, blob → GPU |
| `0x1402F48D0` | `McMaterial_GetBuiltInShaderBlob` — the 58 exe-embedded packages |
| `0x140BFCFF0` | `BgfxShader_LoadFromAmat` — the file path |
| `0x140AAA1D0` | `GrMat_GetShaderChunk` — GrMat.cpp:2069, `(shaderId, fourcc, version, &techCount)` |
| `0x140A81610` | `GrResource_Lookup` — `(type, id)`; type 10 = shader, 11 = shader data |
| `0x140BFFB20` | `BgfxShader_BuildPasses` |
| `0x1402F3560` | `McMaterial_GetOrCreate` |
| `0x1402F1D80` | `McMaterial_Create` |

Note this is the *material* shader story. Per [[gw2-exe-shaders]] the exe also carries
~1094 standalone **engine** shader blobs (depth/shadow, SH lighting, sky, deferred, post)
plus 11 compute shaders, which are not reached through `McMaterial_*` at all. Those were
not re-anchored in this pass.

## GrFvf — the definitive vertex layout builder

`GrFvf_BuildVertexLayout` (`0x140B9C310`, BgfxBuffer.cpp:406). **The order of its Add calls
is the byte order in the vertex** — this is the authority, not any table written by hand.

```
mask        field              -> BgfxVertexLayout_Add(attrib, num, type, normalized)
0x00000001  POSITION              (0 Position,  3, Float)
0x08000000  POSITION4             (0 Position,  4, Float)
0x00000002  WEIGHTS               (9 Weight,    4, Uint8, normalized)
0x00000004  GROUP                 (8 Indices,   4, Uint8, RAW)
0x00000008  NORMAL                (1 Normal,    3, Float)
0x04000000  NORMAL_COMPRESSED     (1 Normal,    4, Uint8, normalized)
0x10000000  POSITION_COMPRESSED   (1 Normal,    4, Uint8, normalized)   <- see below
0x00000010  COLOR                 (4 Color0,    4, Uint8, normalized)   BGRA
0x00000020  TANGENT               (2 Tangent,   3, Float)
0x00000040  BITANGENT             (3 Bitangent, 3, Float)
0x00000080  TANGENT_FRAME         THREE adds: Normal, Tangent, Bitangent, each 4 x Uint8 norm
0x0000FF00  texcoords F32         (s_kTexCoordRemap[i], 2, Float)
0x00FF0000  texcoords F16         (s_kTexCoordRemap[i], 2, Half)
```

Type enum: `0 Uint8, 1 Uint10, 2 Int16, 3 Half, 4 Float`.
`s_kTexCoordRemap` @ `0x141C94420` = `{10,11,12,13,14,15,16,17}` — confirmed by reading the
data. IDA had auto-typed it as the string `"\n"` because the first entry is 10; that is now
fixed to a dword array.

**`0x10000000` really is emitted as a Normal slot.** [[vertex-fvf]] flagged this as the odd
one (DDI stride 6 bytes, looks like 3×int16 position) and it holds in this build too: the
GPU layout gets a 4×Uint8 Normal. So the on-disk field is decompressed before upload, and
the two strides genuinely describe different things. For parsing a MODL use the DDI stride;
for reading a GPU buffer use this builder.

### Independent confirmation of the bit meanings

`GrFvf_ToString` (`0x140BA8E90`, GrFvf.cpp:1659) prints a mnemonic, one letter per bit —
the engine naming its own fields:

```
0x001 'p' position   0x002 'w' weights   0x004 'i' indices   0x008 'n' normal
0x010 'c' color      0x020 't' tangent   0x040 'b' bitangent 0x080 'f' tangentFrame
0x20000000 'F'
then one digit = F32 texcoord count, one digit = F16 texcoord count
```

That is a second, independent source agreeing with the layout builder.

### Helpers

| addr | name | note |
| --- | --- | --- |
| `0x140BA8CE0` | `GrFvf_TexCoordCountF32` | `highestSetBit(fvf & 0xFF00) - 7` |
| `0x140BA8D00` | `GrFvf_TexCoordCountF16` | `highestSetBit(fvf & 0xFF0000) - 15` |
| `0x140BA8DA0` | `GrFvf_FieldIndex` | GrFvf.cpp:1647, `popcount(fvf & (field-1))` |
| `0x140BA7E60` | `GrFvf_ConvertVertices` | dispatcher over ~50 per-format converters |
| `0x140B9D110` | `GrFvf_CreateVertexLayout` | asserts `DDI_STRIDE(fvf) == CalcStride()` |
| `0x1409C1A00` | `Math_HighestBitIndex` | bit scan reverse |
| `0x140A87310` | `Math_BitCount` | popcount |

The counts use `highestSetBit - k` rather than popcount because texcoord bits are always
allocated contiguously upward from bit 8.

**Careful:** the asserts `fvf & GR_FVF_POSITION`, `IS_TRUE(TANGENT) == IS_TRUE(BITANGENT)`
and `!(fvf & ~supportedFvf)` are **duplicated across several source files**. In this build
one copy lives in `GrCloud.cpp:1192-1194` (`sub_140ACC540`), which is a cloud mesh creator,
not the layout builder. Do not identify the builder by those strings alone.

## Geometry

A geoset is one drawable mesh chunk, and its vertex data **may be split across several
buffers, each with its own fvf** — `vertexBufferCount < GR_GEOSET_MAX_VERTEX_BUFFERS`.
Parsing a MODL therefore means walking a list of `(fvf, buffer)` pairs, not assuming one
interleaved stream.

| addr | name |
| --- | --- |
| `0x140AF81F0` | `GrGeoset_Create` |
| `0x140AFA690` | `GrGeoset_SetVertexFormats` |
| `0x140B9E6C0` | `BgfxBuffer_UploadGeoset` |
| `0x140CFBBD0` | `ModelFile_GetLodGeosets` — per-LOD geoset array |

## Animation — Granny keeps its own vertex formats

| addr | name |
| --- | --- |
| `0x141904B60` | `GrannyFvf_Register` |
| `0x140D11310` | `Model_GetGrannyModel` |
| `0x140D4DDF0` | `ModelAnimSubControl_BindUvTransformTracks` |

`GrannyFvf_Register` overflows with *"Granny FVF Spare list to Small. Locate and increase
MAX_GRANNY_VERTEX_FORMAT_TYPES…"* — Granny maintains a **separate** vertex-format table
from GrFvf. Do not conflate the two when parsing; see also [[gw2-granny-64bit]] on the
32- vs 64-bit granny blob trap.

> **Correction, 2026-08-16.** `0x140D4DDF0` was previously labelled
> `ModelGranny_SampleTrackGroup`. That was wrong: its asserts name
> `Engine\Model\ModelAnimSubControlData.cpp:1052-1115`, not `ModelGrannyUtil.cpp`, and
> the body binds UV-transform sub-control entries to granny vector tracks rather than
> sampling a track group. Renamed. It is still worth reading — it is the cleanest place
> in the binary to see granny struct packing, because it reads
> `group->VectorTrackCount` at `+0x08` and `group->VectorTracks` at `+0x0C`, a QWORD
> on a 4-aligned offset. That corroborates [[gw2-granny-64bit]] for the *runtime*
> structs, not just the file blob: declare granny types without `#pragma pack(1)` and
> every field after the first int/pointer pair shifts.

### Granny vertex attribute → GrFvf

`ModelFileFormatGranny_VertexTypeToFvf` (`0x140D556B0`,
`ModelFileFormatGrannyUtils.cpp:108`) is where a granny vertex type becomes an fvf
mask. It walks the `granny_data_type_definition` member list and `strcmp`s each member
`Name` against `s_grannyAttribToFvf` (`0x141EE7CC0`, 28 entries × 24 bytes), OR-ing the
bits together. Unknown name is **fatal**, not skipped: *"Unknown granny vertex
attribute - %s"*. At most 16 elements (`MAX_GRANNY_VERTEX_ELEMENTS`).

| granny name | fvf bit | granny member type | comps |
| --- | --- | --- | --- |
| `Position` | `0x00000001` | 10 `Real32` | 3 |
| `Position` | `0x10000000` | 21 `Real16` | 3 |
| `BoneWeights` | `0x00000002` | 14 `NormalUInt8` | 4 |
| `BoneIndices` | `0x00000004` | 12 `UInt8` | 4 |
| `Normal` | `0x00000008` | 10 `Real32` | 3 |
| `Normal` | `0x04000000` | 19 `Int32` | 1 |
| `DiffuseColor` / `DiffuseColor0` | `0x00000010` | 11 `Int8` | 4 |
| `Tangent` | `0x00000020` | 10 `Real32` | 3 |
| `Binormal` | `0x00000040` | 10 `Real32` | 3 |
| `TangentFrame` | `0x00000080` | 19 `Int32` | 3 |
| `TangentFrame` | `0x20000000` | 19 `Int32` | 3 |
| `TextureCoordinates0..7` | `0x100 << n` | 10 `Real32` | 2 |
| `TextureCoordinatesF16_0..7` | `0x10000 << n` | 21 `Real16` | 2 |

Two rows settle things the fvf bits alone left ambiguous: the packed `Normal` and both
`TangentFrame` variants are `Int32` — a normal packed into a single uint32, and a
tangent frame as three of them. And `0x20000000`, which `GrFvf_ToString` only ever
prints as the letter `F`, is a second `TangentFrame` encoding.

## Module map for further work

Source-path anchors, for picking up where this stopped:

| module | anchor | funcs |
| --- | --- | --- |
| `GrFvf.cpp` | `0x141C94958` | 57 (mostly per-format converters) |
| `BgfxBuffer.cpp` | `0x141C94230` | 7 |
| `BgfxShader.cpp` | `0x141D12310` | 11 |
| `BgfxDraw.cpp` | `0x141C327A0` | 28 |
| `BgfxTexture.cpp` | `0x141C7EA10` | 18 |
| `GrModel.cpp` | `0x141C00E00` | 67 |
| `Model.cpp` | `0x141ED9C50` | 51 |
| `ModelFile.cpp` | `0x141EDB498` | 36 |
| `ModelAnimation.cpp` | `0x141ED9810` | 8 |
| `ModelGrannyUtil.cpp` | `0x141EDDFA0` | 4 |
| `McMaterial.cpp` | `0x141AC3B90` | — |

## Not done

- ~~**Texture upload was not traced.**~~ **Done 2026-08-16** — all 18 `BgfxTexture.cpp`
  functions are named and the decoded-pixels → GPU path is written up in
  [[gw2-texture-upload]]. Entry point is `BgfxTexture_UploadLevels` (`0x140B15850`).
- The ~1094 standalone engine shader blobs and the 11 compute shaders were not re-anchored;
  [[gw2-exe-shaders]] still has the extraction recipe but its addresses are stale.
- `Model.cpp` (51 funcs) and `GrModel.cpp` (67 funcs) were enumerated only. The MODL chunk
  parse itself — where fvf, vertex buffer and index buffer are read out of the packfile —
  is still unnamed, and is the obvious next target.
- Animation naming stops at three functions; the track/keyframe decode path is untouched.
