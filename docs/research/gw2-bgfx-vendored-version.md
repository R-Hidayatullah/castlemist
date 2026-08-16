---
name: gw2-bgfx-vendored-version
description: "Which bgfx GW2 actually vendors — commit a476c5b9, rev 8775, API 128 — and three offsets in the binary that pin it without trusting the SHA string"
metadata:
  node_type: memory
  type: project
  modified: 2026-08-16
---

# Which bgfx GW2 vendors

GW2's renderer is not "bgfx-like". It is **upstream bgfx, vendored verbatim**, with
an ArenaNet wrapper on top. The Perforce paths in `.rdata` say so outright:

```
Engine\Gr\Bgfx\External\bgfx\src\bgfx.cpp
Engine\Gr\Bgfx\External\bgfx\src\bgfx_p.h
Engine\Gr\Bgfx\External\bgfx\src\renderer_d3d11.cpp
Engine\Gr\Bgfx\External\bgfx\src\renderer_noop.cpp
Engine\Gr\Bgfx\External\bgfx\src\dxgi.cpp
Engine\Gr\Bgfx\External\bgfx\src\topology.cpp
Engine\Gr\Bgfx\External\bx\src\file.cpp
Engine\Gr\Bgfx\External\bimg\src\image.cpp
Engine\Gr\Bgfx\External\bimg\src\image_gnf.cpp
```

`renderer_d3d11` and `renderer_noop` are the only backends compiled in. The wrapper
lives one directory up as `BgfxDdi/Draw/Texture/Shader/Buffer/Window/UiRenderer/Utils.cpp`.

## The version

Pulled from the debug-stats banner call at `0x140B7D324`:

```asm
mov  [rsp+6A0h+var_650], 2247h   ; BGFX_REV_NUMBER  = 8775
mov  [rsp+6A0h+var_658], 80h     ; BGFX_API_VERSION = 128
lea  rax, bgfx_aVersionBanner    ; "... / Version 1.%d.%d (commit: a476c5b9a42...)"
```

> **bgfx 1.128.8775, commit `a476c5b9a42d3779af59a0099d4d222fa8898d36`**

```bash
git clone https://github.com/bkaradzic/bgfx && cd bgfx
git checkout a476c5b9a42d3779af59a0099d4d222fa8898d36
```

`src/version.h` is committed in the repo, so after checkout it should read back
`BGFX_REV_NUMBER 8775` and that same SHA, and `include/bgfx/defines.h` should read
`BGFX_API_VERSION UINT32_C(128)`. Three numbers, all checkable offline — no guessing.

`bx` and `bimg` are separate repos with no submodule pinning at that commit, so
date-match them:

```bash
git -C bgfx show -s --format=%cI a476c5b9a42d3779af59a0099d4d222fa8898d36
git -C bx   rev-list -n 1 --before="<that date>" master
git -C bimg rev-list -n 1 --before="<that date>" master
```

## Why this matters more than it sounds

A `bgfx-master` checkout is **not** a drop-in reference. As of writing, master is
`f446c319`, rev 9149, API 153 — 374 commits and 25 API versions ahead — and two of
the differences are ABI-visible:

| | this binary | master |
| --- | --- | --- |
| `Attrib::Count` | **18** (TexCoord0..7) | 26 (TexCoord0..15) |
| `AttribType::Count` | **5** `{Uint8,Uint10,Int16,Half,Float}` | 9 (adds Int8, Uint16, Int32, Uint32) |

`bgfx::VertexLayout` is sized off both counts. Copy the struct from master and every
offset past `m_stride` is wrong, silently — the decompiler will happily show you
plausible garbage.

## Three fingerprints that do not depend on the SHA

Useful after a client patch, when the addresses have moved and you want to
re-establish the version from scratch:

**1. `Attrib::Count`, from the offset arithmetic in `BgfxVertexLayout_Add`**

```c
*(_WORD *)(layout + 2 * attrib + 42) = ...;   // m_attributes[]
*(_WORD *)(layout + 2 * attrib +  6) = ...;   // m_offset[]
```

`(42 - 6) / 2 == 18`. The layout is therefore
`{u32 m_hash; u16 m_stride; u16 m_offset[18]; u16 m_attributes[18];}`.

**2. `AttribType::Count`, from `bgfx_s_attribTypeSize` @ `0x141C84D18`**

20 bytes per renderer entry, i.e. 5 types × 4 element counts:

| type | 1 | 2 | 3 | 4 |
| --- | --- | --- | --- | --- |
| `Uint8` | 1 | 2 | 4 | 4 |
| `Uint10` | 4 | 4 | 4 | 4 |
| `Int16` | 2 | 4 | 8 | 8 |
| `Half` | 2 | 4 | 8 | 8 |
| `Float` | 4 | 8 | 12 | 16 |

The companion `bgfx_s_attribTypeAsIntAllowed` @ `0x141C92844` is `"\x1\x1\x1\x0\x0"` —
five entries, one per type, matching upstream's `asInt` guard.

**3. Shader chunk magic version byte**

The embedded shader blobs in `.rdata` carry `VSH\x0b` and `FSH\x0b`
(e.g. `0x14193F81B`), i.e. `BGFX_CHUNK_MAGIC_VSH = BX_MAKEFOURCC('V','S','H',0xb)`.
Version byte **0x0B**. In current master that constant has been renamed and moved
out of `bgfx_p.h` entirely, which is itself a tell that you are on the wrong tree.

## Wrapper entry points named so far

| addr | name |
| --- | --- |
| `0x140B70260` | `bgfx_makeRef` |
| `0x140B643F0` | `bgfx_createVertexBuffer` |
| `0x140B64E80` | `bgfx_destroyVertexBuffer` |
| `0x140B7E2D0` | `bgfx_topologyConvert` |
| `0x140B62A20` | `bgfx_createTexture2D` |
| `0x140B62D20` | `bgfx_createTexture3D` |
| `0x140B63360` | `bgfx_createTextureCube` |
| `0x140B5E890` | `BgfxShaderD3D11_Create` |

Note the case split: lowercase `bgfx_` is vendored upstream API, capitalised `Bgfx*`
is ArenaNet's wrapper. `tools/ida_restore_symbols.py` carries both prefixes.

## Caveat

The commit hash is read out of the binary; it has not been resolved against GitHub
from here. If the checkout fails, rev 8775 brackets it —
`git rev-list --count HEAD` on master minus 8775 is how far back to walk.

See also: [[gw2-render-asset-pipeline]], [[gw2-texture-upload]], [[vertex-fvf]].
