# ATEX Decoder Fix Specification — `gw2_atex.hpp`

**Target file:** `gw2_atex.hpp`
**Target function:** `inline void inflate_mip(...)` (per-mip inflate / fill-pass dispatcher)
**Reference:** ArenaNet `ImgAtex_Decode()` (Gw2-64.exe `sub_140B83040`, IDA 9.0 RE)

## 1. The bug

`inflate_mip()` decides which RLE constant-fill passes to run using a mix of
`FORMAT_FLAGS[fmt_enum]` (memory-layout capability bits) and, in two of the
four passes, explicit `fmt_enum` checks. The two passes that are **not**
gated by `fmt_enum` are Pass 1 and Pass 4, and this lets `GR_FORMAT_BC7`
(`fmt_enum == 32`) slip into fill logic that only makes sense for formats
whose second plane is a genuine BC1-style 565 color block.

`FORMAT_FLAGS[32]` (BC7) is `0xB1` — bit-identical to DXT2/DXT3/DXT4/DXT5.
That is **correct** and must not change: it correctly describes BC7's
physical layout (2 planes, 16-byte block, block-compressed). The bug is
that `0xB1` also implies `has_BC == true` in the current layout calc, and
`has_BC` alone is used as the gate for Pass 4, so BC7 is treated as if its
second 8-byte plane were a quantized 565 BC1 palette. It is not — it's an
opaque chunk of raw BC7 bitstream (mode/partition/endpoints/indices) that
must never be touched by BC1 constant-color reconstruction.

## 2. Current code (the four fill passes)

```cpp
// Pass 1 — BC-only constant fill (alpha-less formats)
if ((flags & 0x01) && (ff & 0x210) && !(ff & 0x280) && fmt_enum != 28) {
    uint8_t val[8]; std::memset(val, 0xFF, 8);
    rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offB, val, 8, bmB, &bmA, &bmB, false);
}

// Pass 2 — DXT2/DXT3 constant alpha nibble
if ((flags & 0x02) && (fmt_enum == 23 || fmt_enum == 24)) {
    int nib = (int)br.read(4);
    uint8_t b = (uint8_t)((nib | (nib << 4)) & 0xFF);
    uint8_t val[8]; std::memset(val, b, 8);
    rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offA, val, 8, bmB, &bmA, nullptr, true);
}

// Pass 3 — DXT4/DXT5/DXTA/DXTL constant alpha byte
if ((flags & 0x04) && (fmt_enum >= 25 && fmt_enum <= 28)) {
    uint8_t a = (uint8_t)br.read(8);
    uint8_t val[8] = { a, a, 0, 0, 0, 0, 0, 0 };
    rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offA, val, 8, bmB, &bmA, nullptr, true);
}

// Pass 4 — BC1-style constant color block  <-- BUG: no fmt_enum gate
if ((flags & 0x08) && has_BC) {
    uint32_t rgb = (uint32_t)br.read(24);
    auto val = encode_bc1_color(rgb, fmt_enum == 22);
    rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offB, val.data(), 8, bmB, &bmB, nullptr, false);
}
```

`has_BC` is computed purely from flags:

```cpp
bool has_BC = (ff & 0x210) != 0;
```

For BC7, `ff == 0xB1`, and `0xB1 & 0x210 == 0x10 != 0`, so `has_BC` is
`true` for BC7 — same as it is for DXT1/2/3/4/5. That's the crux: Pass 4
currently cannot distinguish "this format's plane B is a BC1 palette" from
"this format's plane B happens to be 8 bytes for layout purposes."

Pass 1 already avoids the problem for BC7 as a side effect (BC7 also has
`ff & 0x280 != 0`, i.e. `has_A == true`, so the `!(ff & 0x280)` guard
excludes it) — but that's incidental, not an explicit BC7 exclusion, and
it's worth stating explicitly rather than relying on the coincidence.
Pass 2 and Pass 3 already use explicit `fmt_enum` ranges that don't include
32, so they're already correct.

**Net effect of the bug:** any BC7 mip encoded with `flags & 0x08` set will
have `encode_bc1_color()` output written into bytes that are actually part
of the raw BC7 block payload (endpoints/partition/index bits), corrupting
those blocks even though the Huffman bitstream itself decoded correctly.

## 3. Required fix

Add an explicit BC7 exclusion to Pass 4 (and make the Pass 1 exclusion
explicit rather than incidental). Introduce clearly named booleans near the
top of `inflate_mip()`, alongside the existing `has_A` / `has_BC`:

```cpp
const bool isBC7 = (fmt_enum == 32);
```

Then:

```cpp
// Pass 1 — unchanged behaviour, BC7 exclusion now explicit
if ((flags & 0x01) && (ff & 0x210) && !(ff & 0x280) && fmt_enum != 28 && !isBC7) {
    ...
}

// Pass 4 — must not run for BC7
if ((flags & 0x08) && has_BC && !isBC7) {
    uint32_t rgb = (uint32_t)br.read(24);
    auto val = encode_bc1_color(rgb, fmt_enum == 22);
    rle_fill(br, hlen, hval, surf, nbBlocks, bpb, offB, val.data(), 8, bmB, &bmB, nullptr, false);
}
```

Pass 2 and Pass 3 need no change — verify only that their range checks
(`fmt_enum == 23 || 24` and `25 <= fmt_enum <= 28`) still exclude 32, which
they do.

Do **not** touch `FORMAT_FLAGS[32] = 0xB1`, and do **not** touch the plane
layout math (`a2`, `a210`, `dxtl`, `bpb`, `offA/offB/offC`) — those are
correct; BC7 really is a 2×8-byte / 16-byte block and must stay that way
for the byte-aligned plane copy at the bottom of `inflate_mip()` to place
the raw 16-byte BC7 payload correctly.

## 4. Why plane layout stays shared but fill passes must not

`FORMAT_FLAGS` only encodes *physical* block shape (byte count, plane
count) — it's how the Huffman bitmap sizing and the raw byte-aligned
`copy()` calls at the end of `inflate_mip()` know how many bytes to place
per block. It says nothing about the *semantic content* of those bytes.

DXT2–5 and BC7 all happen to be 16-byte, 2-plane formats (`FORMAT_FLAGS ==
0xB1`), so they share the same Huffman bitmap layout and the same raw
`copy()` logic — that part of the code is already correct and shared. But
DXT2–5's plane B genuinely is a BC1 565 color block (which is why constant
color fill via `encode_bc1_color()` is valid for them), while BC7's plane
B is opaque BC7 bitstream data that must be preserved byte-for-byte from
the container and never synthesized.

| `fmt_enum` | Format | `FORMAT_FLAGS` | Pass 4 (`flags&0x08`) valid? |
|---|---|---|---|
| 22 | DXT1 | 0x71 | ✅ |
| 23 | DXT2 | 0xB1 | ✅ |
| 24 | DXT3 | 0xB1 | ✅ |
| 25 | DXT4 | 0xB1 | ✅ |
| 26 | DXT5 | 0xB1 | ✅ |
| 27 | DXTA | 0xA1 | n/a (`has_BC` already false) |
| 28 | DXTL | 0x11 | ✅ (unaffected by this fix; not BC7) |
| 29 | DXTN | 0x201 | ✅ (unaffected; not BC7) |
| 30 | 3DCX | 0x201 | ✅ (unaffected; not BC7) |
| 31 | BC5 | 0x201 | ✅ (unaffected; not BC7) |
| **32** | **BC7** | **0xB1** | **❌ must be excluded — this fix** |

(Only BC7 needs the new guard; the other formats sharing `has_BC == true`
genuinely do carry BC1-shaped or channel-shaped constant-fill semantics in
that plane, which is out of scope for this fix.)

## 5. Verification checklist

After applying the fix:

- [ ] `FORMAT_FLAGS[32]` is still `0xB1` (unchanged).
- [ ] Pass 1 and Pass 4 both skip when `fmt_enum == 32`.
- [ ] Pass 2 and Pass 3 unchanged (already correctly gated).
- [ ] Block size / plane-offset math for BC7 unchanged (`bpb == 16`,
      `offA`/`offB` as before) — only the fill-pass *dispatch* changes,
      not the layout.
- [ ] A BC7 `.atex` mip with `flags & 0x08` set now decodes with its raw
      16-byte blocks intact (byte-for-byte copy from the container),
      instead of having `encode_bc1_color()` output written into plane B.
- [ ] DXT1/2/3/4/5 mips decode identically to before (regression check —
      this fix must be a no-op for every `fmt_enum` other than 32).

## 6. Expected result

BC7 mips no longer run the DXT/BC1 constant-color fill pass. Huffman
bitmap reconstruction and the raw byte-aligned block copy are unchanged.
BC7 output becomes byte-identical to ArenaNet's `ImgAtex_Decode()`, while
`FORMAT_FLAGS` and the shared 2-plane/16-byte layout used by
DXT2/3/4/5/BC7 remain exactly as in the original binary.
