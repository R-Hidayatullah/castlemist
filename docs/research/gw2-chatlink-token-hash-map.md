---
name: gw2-chatlink-token-hash-map
description: "Named IDA symbols for chat links (full 17-entry decoder table + 15 encoders), Base64, the two Token packing schemes, CRC-32/CRC-32C and Murmur2A — recovered from Perforce source-path anchors"
metadata:
  node_type: memory
  type: reference
  modified: 2026-08-16
---

# Chat links, Token, CRC and hashing — symbol map

Applied to `gw2decomp/Gw2-64.exe.i64` on 2026-08-16. Companion to
[[gw2-cmp-img-symbol-map]]; same method (Perforce source-path anchors), same caveat
(**addresses are build-specific and rot silently**).

Source-path anchors used:

| module | anchor string |
| --- | --- |
| Base64 | `0x1420A0BF0` `Arena\Services\Base64\Base64.cpp` |
| Token | `0x1420B5750` `Arena\Services\Token\Token.cpp` |
| CRC | `0x1420A3D40` `Arena\Services\Crc\Crc.cpp` |
| chat links | *no source path* — found via xrefs to the Base64 wrappers |

## Chat links

IDA folders: `/GW2 Chat links/{Entry points, Encoders, Decoders}`.

### The decode path is a plain jump table

```
ChatLink_Decode (0x14130D510)
   1. Base64_DecodeWideToByteArray   [&<base64>]  ->  raw payload bytes
   2. ChatLink_ReadHeaderByte        consume 1 byte
   3. ChatLink_DecoderTable[header]  dispatch
```

`ChatLink_ReadHeaderByte` (`0x141310FC0`) is four lines: read one byte, return it if
`< 0x11`, else return `17` meaning invalid. **So the header byte is the table index
directly** — there is no remapping, no switch, no per-type registration.

`ChatLink_DecoderTable` @ `0x1425ED7C0` — 17 function pointers, index = header byte:

| hdr | decoder | encoder |
| --- | --- | --- |
| 0x00 | `ChatLink_DecodeHdr00` | *(none found)* |
| 0x01 | `ChatLink_DecodeCoin` | `ChatLink_EncodeCoin` |
| 0x02 | `ChatLink_DecodeItem` | `ChatLink_EncodeItem` |
| 0x03 | `ChatLink_DecodeNpcText` | `ChatLink_EncodeNpcText` |
| 0x04 | `ChatLink_DecodeMapPoi` | `ChatLink_EncodeMapPoi` |
| 0x05 | `ChatLink_DecodePvpGame` | `ChatLink_EncodePvpGame` |
| 0x06 | `ChatLink_DecodeSkill` | `ChatLink_EncodeSkill` |
| 0x07 | `ChatLink_DecodeTrait` | `ChatLink_EncodeTrait` |
| 0x08 | `ChatLink_DecodeUser` | `ChatLink_EncodeUser` |
| 0x09 | `ChatLink_DecodeRecipe` | `ChatLink_EncodeRecipe` |
| 0x0A | `ChatLink_DecodeSkin` | `ChatLink_EncodeSkin` |
| 0x0B | `ChatLink_DecodeOutfit` | `ChatLink_EncodeOutfit` |
| 0x0C | `ChatLink_DecodeWvwObjective` | `ChatLink_EncodeWvwObjective` |
| 0x0D | `ChatLink_DecodeBuildTemplate` | `ChatLink_EncodeBuildTemplate` |
| 0x0E | `ChatLink_DecodeHdr0E` | *(none found)* |
| 0x0F | `ChatLink_DecodeHdr0F` | `ChatLink_EncodeHdr0F` |
| 0x10 | `ChatLink_DecodeHdr10` | `ChatLink_EncodeHdr10` |

**Headers 0x0E, 0x0F and 0x10 are named by number, not by meaning.** Only the header
byte and the payload field widths are verified; which of them is "wardrobe" versus
"travel" template is *not*. 0x0E has a decoder but no encoder on the Base64 path, so it
is probably produced through MsgConn instead — consistent with the "System 2" note in
[[gw2-chat-links]].

The encoders were identified by reading the header byte each one writes as the first
payload byte, then base64-encoding — not by matching against the wiki. The two
independently agree, which is the useful part.

### Build template layout, read off `ChatLink_EncodeBuildTemplate`

```
0D [profession:1]
   [3 x (specId:1, traitBits:1)]        6 bytes
   [10 x skillPalette:u16]             20 bytes
   [16 x 1 byte]                       pet / aquatic slots
   [count:1][count x u16]              weapons
   [count:1][count x u32]              skill overrides
```

### Base64 substrate

Encode and decode cores both take the **alphabet / LUT as an argument**, which is why one
core serves the standard and URL-safe variants. Each exists in 8-bit and UTF-16 flavours;
chat links use the wide ones because the client holds chat text as UTF-16.

| addr | name |
| --- | --- |
| `0x140D91CA0` / `0x140D91E50` | `Base64_EncodeCore` / `Base64_EncodeCoreWide` |
| `0x140D919C0` / `0x140D91B30` | `Base64_DecodeCore` / `Base64_DecodeCoreWide` |
| `0x140D92180` / `0x140D921A0` | `Base64_Encode` / `Base64_EncodeWide` (std alphabet) |
| `0x140D921C0` | `Base64_EncodeToWideArray` — every `ChatLink_Encode*` ends here |
| `0x140D92000` / `0x140D92020` / `0x140D920D0` | `Base64_DecodeWide` / `…ToByteArray` / `…WideToByteArray` |
| `0x1420A0950` / `0x1420A09A0` / `0x1420A09F0` | `Base64_AlphabetStd` / `Base64_AlphabetUrl` / `Base64_DecodeLut` |

`Base64EncodeSize(n) = 4*ceil(n/3)+1`, `Base64DecodeSize(n) = 3*(n/4)`. In the decode LUT,
**127 is the `=` padding sentinel** and ends the stream early.

## Token — there are TWO schemes, and neither is a hash

This is the single most important correction here. [[gw2-uniform-hash]] documented the
32-bit base-23 scheme; the binary has a **second, unrelated 64-bit scheme** in the same
file. Confusing them will produce garbage.

### `Token_Decode32` (`0x140E46CF0`, Token.cpp:30) — 32-bit, base 23

```c
v = (token - 0x30000000) mod 2^32;
while (v) { *out++ = "abcdefghiklmnopvrstuwxy"[v % 23]; v /= 23; }
```

Alphabet `Token_Base23Alphabet` @ `0x1420B5738` — 23 chars, **no j, q or z**, and note the
order `...o, p, v, r, s, t, u, w, x, y`. Fully reversible. Because 23^7 just exceeds
2^32, every name is at most **7 lowercase characters**. This is the MODL material-constant
and engine-global-param namespace (`gloover`, `grblcol`, …).

### `Token_Decode64` (`0x140E46E50`, Token.cpp:99) — 64-bit, 5 bits per char

```
low 60 bits : 5 bits per character, 1..26 -> 'a'..'z', 0 -> space
top  4 bits : emitted as two decimal digits  (v/10, then v%10)
```

Up to **12 lowercase characters plus an optional 2-digit numeric suffix** — a completely
different encoding and a much larger namespace than the base-23 one. `Token_Decode64Wide`
(`0x140E46BE0`) is the UTF-16 output variant.

This is the `token64` carried by `ModelTextureDataV65` and `AmatSamplerConstantV1`.
[[gw2-uniform-hash]] recorded that field as "per-texture asset id, NOT a role name" and
left it there — **it is decodable after all**, just with this scheme rather than base-23.
Worth re-testing those fields; not done here.

## CRC and hashing

| addr | name | notes |
| --- | --- | --- |
| `0x140DAAA10` | `Crc32` | Crc.cpp:192, reflected CRC-32, poly 0xEDB88320, table `Crc32_Table` |
| `0x140DAAB40` | `Crc32C` | Crc.cpp:260, **Castagnoli**, poly 0x82F63B78; SSE4.2 `crc32` when `g_HasSse42Crc`, else `Crc32C_Table` |
| `0x140C1A2F0` | `HashMurmur2A_Add` | streaming update, `m = 0x5BD1E995`, `r = 24` |
| `0x140C1C880` | `HashMurmur2A_End` | tail + size mix, then `h^=h>>13; h*=m; h^=h>>15` |

Both CRCs pre- and post-invert and process 8 bytes per iteration with a byte tail. **The
two polynomials are different — results are not interchangeable.**

Murmur2A state is `{u32 hash, u32 size, u32 tail, u32 count}`; callers zero it, so the
**seed is 0**. All 13 callers of the finalizer sit in the bgfx renderer range, matching
[[gw2-uniform-hash]]: this one pair computes both the uniform-name hash and the DXBC
shader content hash.

**Do not reach for these when chasing a Token id.** Hashing a uniform name will never
match a material token, because the token is packed text, not a digest. That dead end is
already recorded in [[gw2-uniform-hash]]; the naming here makes it visible in the IDB.

## Not done

- Which template kind 0x0E / 0x0F / 0x10 each are.
- The MsgConn "System 2" template codecs from [[gw2-chat-links]] were not touched.
- `Token_Decode64` has not been run against real `ModelTextureDataV65.token` values —
  the decode scheme is read off the binary, the *meaning* of the resulting strings is
  unverified.
- `ChatLink_Decode*` bodies past the coin and item cases were named from the dispatch
  table position, not from reading each body in full.
