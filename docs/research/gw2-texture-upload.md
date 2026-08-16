---
name: gw2-texture-upload
description: "Decoded pixels to GPU texture — BgfxTexture.cpp traced end to end, the DdiTexture layout with both format fields, and the lock/staging protocol"
metadata:
  node_type: memory
  type: project
  modified: 2026-08-16
---

# Texture upload

Closes the open item in [[gw2-render-asset-pipeline]]: *"Texture upload was not traced.
`BgfxTexture.cpp` was enumerated (18 functions) but no entry point was named."*

All 18 are now named. The ATEX/DDS *decode* side stays where it was, in
[[gw2-cmp-img-symbol-map]] — this note picks up where that one stops, at decoded
pixels in system memory.

## The chain

```
Gw2.dat entry
  -> Cmp_Decompress                      (dat block codec)
  -> ImgDecode_Run                       (probe magic, pick codec)
  -> ImgAtex_Decode / ImgDds_Decode / …  (decoded pixels, GR_FORMAT)
  -> BgfxTexture_UploadLevels   0x140B15850   <- entry point
       -> BgfxTexture_Lock3d    0x140B16D50   staging buffer per mip
       -> GrFvf-style convert into staging
       -> BgfxTexture_UnlockBoxes 0x140B17BE0 flush dirty boxes
  -> BgfxTexture_CreateBgfxTexture 0x140B16B20
       -> bgfx::createTexture2D / 3D / Cube
```

`BgfxTexture_UploadLevels` is the one to breakpoint. Its signature falls out of the
asserts at `BgfxTexture.cpp:1485-1489`:

```
(texture, startLevel, endLevel, srcFormat, srcBits, startSlice, endSlice, dims, …)
  texture     != nullptr
  startLevel  <  endLevel
  BgfxTextureIsValidFormat(srcFormat)      -> srcFormat <= 0x25
  _srcBits    != nullptr
  startSlice  <  endSlice
```

`srcFormat <= 0x25` is exactly the `GR_FORMAT` range (38 members, `GR_FORMATS = 38`),
which is a nice independent confirmation that the enum in
`tools/ida_restore_symbols.py` is complete.

## DdiTexture — two format fields, not one

This is the part that will bite anyone reading the create path casually. The engine
format and the bgfx format are **both** stored, four bytes apart:

| off | field | note |
| --- | --- | --- |
| `+0x00` | `u16 m_handle` | `bgfx::TextureHandle`, `0xFFFF` = invalid |
| `+0x04` | `u32 m_type` | `DDI_TEXTURE_2D` 0, `_3D` 1, `_CUBE` 2 |
| `+0x0C` | `u32 m_format` | **GR_FORMAT** — what the lock paths use |
| `+0x10` | `u32 m_bgfxFormat` | **bgfx::TextureFormat** — what create passes |
| `+0x14` | `u32 m_dimsX` | |
| `+0x18` | `u32 m_dimsY` | |
| `+0x1C` | `u16 m_depth` | 3D only |
| `+0x1E` | `u8 m_numMips` | last create arg, only when mips are on |
| `+0x20` | `u16 m_levels` | asserted `== 1` for 3D |
| `+0x24` | `u32 m_flags` | see below |
| `+0x28` | `DdiTextureLock *m_lock` | non-null only while locked |

`BgfxTexture_Lock2d` reads `ImgCalc_LevelPitchAndBytes((GR_FORMAT)*(a1 + 12))` while
`BgfxTexture_CreateBgfxTexture` passes `*(a1 + 16)` straight to `bgfx::createTexture2D`.
Conflate them and every pitch calculation comes out for the wrong format.

`BgfxTexture_ResolveFormat` (`0x140B14FC0`) is what fills `+0x10` from `+0x0C`, and it
warns on two failure modes worth knowing about: *"Even though we found a supported
actual format, we still resolve an unknown internal format"* and *"Expected a depth
format, but we got something else…"*.

## Flags

Recovered from the shift/mask code rather than from any name table, so the bit
positions are certain but three of the names are not:

| bit | value | name | evidence |
| --- | --- | --- | --- |
| 5 | `0x00000020` | `DDI_TEXTURE_SRGB` | disables mips in create |
| 15 | `0x00008000` | *unnamed* | -> bgfx flags bit 44 |
| 22 | `0x00400000` | `DDI_TEXTURE_LOCKED` | `(m_flags >> 22) & 1`, then `|= 0x400000` |
| 23 | `0x00800000` | `DDI_TEXTURE_MSAA` | pulls sample count from config id 7 |
| 25 | `0x02000000` | `DDI_TEXTURE_CREATE_FAILED` | set when bgfx returns `0xFFFF` |
| 27 | `0x08000000` | *unnamed* | -> bgfx flags bit 46 |
| 28 | `0x10000000` | `DDI_TEXTURE_RENDER_TARGET` | -> bit 47, disables mips |
| 31 | `0x80000000` | *unnamed* | -> bgfx flags bit 43 |

The unnamed three are named `DDI_TEXTURE_FLAG_BIT15/27/31` in
`tools/structs/gw2_ida_types.h`. Treat those as placeholders — the bit position is
measured, the meaning is not.

## The lock protocol

One allocation of `bytes + 16`; the header is the descriptor, the tail is the pixels.

```c
struct DdiTextureLock {
    u32 level;       // +0x00
    u32 pitch;       // +0x04  returned through outPitch
    u32 slicePitch;  // +0x08  returned through outSlice
    u32 bytes;       // +0x0C
    // +0x10 : staging pixels -- this is what TextureDataPtr() returns
};
```

Three lock entry points, one per texture type, each asserting its own `m_type`:
`BgfxTexture_Lock2d` (`0x140B17180`), `Lock3d` (`0x140B16D50`), `LockCube`
(`0x140B16F70`). Unlock is split by geometry rather than by type —
`BgfxTexture_UnlockBoxes` for volumes, `BgfxTexture_UnlockRects` for surfaces, both
funnelling into `BgfxTexture_UploadLockedRect` (`0x140B18290`), which asserts
`rectMemStart + rectMemSize <= m_lock->bytes`.

Nesting is not supported, and the code says so in an unusually candid fatal:
*"Attempting to take a second lock on a texture. This might be ok buts its untrod
territory."*

## Resize

`BgfxTexture_DownRes` (`0x140B17500`) halves through `bgfx::blit` and needs
`BGFX_CAPS_TEXTURE_BLIT` plus `GetType() == DDI_TEXTURE_2D`. `BgfxTexture_UpRes`
(`0x140B17820`) refuses render targets outright (*"cannot upscale render targets"*)
and validates both the current and target formats before touching anything.

## Not done

- The mip-generation path. `BgfxTexture_UploadLevels` takes `startLevel`/`endLevel`
  and uploads what it is given; whoever decides those bounds was not traced.
- `bgfx::updateTexture2D/3D/Cube` themselves are still `sub_*` — only the create and
  makeRef wrappers were named.
- `GrTexturePool.cpp` (`0x1422F8450`) is unexplored, and `BgfxTexture_Destroy` warns
  *"Attempting to delete unknown texture"* against a pool this note never opened.

See also: [[gw2-bgfx-vendored-version]], [[gw2-cmp-img-symbol-map]],
[[gw2-atex-atep-decode]], [[gw2-render-asset-pipeline]].
