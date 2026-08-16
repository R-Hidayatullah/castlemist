---
name: gw2-atex-atep-decode
description: ATEX-family (ATEX/ATEP/ATEU/ATET/ATEC/ATTX) texture decode; the container magic flag is write-only so ATEP decodes identically to ATEX
metadata: 
  node_type: memory
  type: reference
  originSessionId: bd66fef5-a7e9-4fd2-b271-b282e18fbb85
---

> **Addresses in this note are from an older client build and no longer resolve
> (checked 2026-08-16).** The conclusions below were re-verified against the current
> binary and stand — including the write-only container tag. Current map:
> [[gw2-cmp-img-symbol-map]] (`ImgAtex_Decode` @ `0x140B86B70`, `ImgFmt_Flags` @
> `0x141C80410`, `ImgFmt_InfoTable` @ `0x141C801A0`). Two corrections: the probe chain
> has **nine** loaders, not seven, and `byte_141C86390` has no counterpart in the
> current map.

GW2 ImgAtex decoder = `ImgAtex_Decode` (sub_140B83040, `Arena\Engine\Gr\Img\ImgAtex.cpp`), a streaming state machine driven by `ImgDecode.cpp`/`sub_140B14CB0`. Magic detector = `ImgAtex_IsAtexMagic` (sub_140B85760): six accepted signatures, masked with `& 0xFFFFFFF9` — **ATEX, ATTX, ATEC, ATEP, ATEU, ATET**.

**Key finding (IDA-verified, decisive for castlemist ATEP):** the per-filetype "magic number" `v22` (ATEX/ATTX=0, ATEC=1, ATEP=2, ATEU=4, ATET=0x20; plus `(v19&4)?0x10` and `(v19&2)*4` bits that are always 0 for the 'A'=0x41 first byte) is stored at **decoder+0x4C** (`(_DWORD*)a1+19`) but is **WRITE-ONLY**: touched exactly twice in the whole pipeline, both writes (compute in ImgAtex_Decode, `=0` reset in ImgDecode). Zero reads — not in the decoder, not in `IGrImageLoadAlloc` (GrImage.cpp / sub_140A7AE30), not in any of the 7 image loaders. => **ATEP pixel data decodes byte-identically to ATEX**; there is NO premultiplied-alpha or any container-driven pixel transform. The tag is engine metadata (texture-usage hint) only.

Pixel decode depends ONLY on:
1. **2nd fourCC → GR_FORMAT enum** (DXT1=22, DXT2=23, DXT3=24, DXT4=25, DXT5=26, DXTA=27, DXTL=28, DXTN=29, 3DCX=30, BC5X=31, BC7X=32; + uncompressed).
2. **Per-mip flags dword** (`*((_DWORD*)state+3)`, low byte gates RLE constant-fill passes: 0x1/0x2/0x4/0x8; 0x10 = 256×256 terrain border-mirror; 0x200 = no block-bitmap / stored raw).

Format tables: `ImgFmt_Flags` (dword_141C73CF0, cap flags, bit0=block-compressed, 0x210/0x280 select planes), `ImgFmt_BitsPerPixel` (unk_141C73A90 stride 3), `ImgFmt_BlockDims` (4×4 if flags&1 else 1×1). RLE Huffman table byte_141C86390 (64 len/val pairs, top 6 bits).

**Fix applied** to [[castlemist-app]] `castlemist/include/gw2_atex.hpp`: removed the speculative `unpremultiply_alpha` auto-applied on `CONT_ATEP` (was corrupting .atep output). `decode()` Auto now never transforms; ForceOn kept as manual experiment only. Relates to [[gw2mcp-server]] gw2_decode_texture.

**"ATEP renders red/green" was NOT a decode bug (2026-07-15).** fileId 1796643 (magic ATEP, fourCC DXT5, 512²) decodes correctly (byte-matches gw2mcp gw2_decode_texture). It is a **90%-transparent decal** (mean alpha 15); its transparent texels store a **don't-care flat red (224,0,0)** as color. The castlemist preview pixel shader (`d3d_renderer.cpp` PSMain) returned `tex0.Sample(...)` — showing **RGB while ignoring alpha**, so the red dominated (psychedelic red/magenta/green blobs). Same root cause made PIMG atlas tiles (often ATEP) look garbled. FIX: PSMain now alpha-composites over an 8px checkerboard (`lerp(checker, texel.rgb, texel.a)`), returns opaque; alpha==1 texels unaffected. Rebuild via the [[castlemist-app]] g++ recipe for it to take effect. Lesson: for GW2 textures, judge decode correctness by the **premultiplied/alpha-aware** view, not raw RGB — transparent regions carry garbage color.
