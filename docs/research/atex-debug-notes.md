# GW2 ATEX/ATEP/ATEU decoder — debugging handoff

## Context

`gw2_atex.hpp` is a header-only C++20 reverse-engineered decoder for ArenaNet's
ATEX-family texture containers (magics `ATEX`, `ATTX`, `ATEC`, `ATEP`, `ATEU`,
`ATET`), ported from `Gw2-64.exe : sub_140B83040`. Supported block formats:
DXT1/2/3/4/5, DXTA, DXTL, DXTN, 3DCX, BC5, BC7.

Source files used for the RE work (paths as uploaded):
- `gw2_atex.hpp` — the decoder itself
- `disassembly.txt`, `pseudocode.txt` — IDA output for the relevant game binary
- `decompressed_242312.atep`, `decompressed_2871.atex`, `decompressed_27.ateu` —
  real sample files extracted from the game

## The original question

Different container magics (`ATEX` vs `ATEP` vs `ATEU` etc.) seemed to decode
with different degrees of correctness — `.atep` files in particular came out
with wrong colors / blotchy garbage in large regions. Question: does each
container type need its own decode path?

## What's confirmed (verified against real samples, not just theory)

### 1. Container magic *does* carry an extra flag, but it's probably not the bug

`sub_140B83040` derives a flag from the raw magic bytes right after reading the
header (`pseudocode.txt` ~line 47592–47623):

```c
v20 = v19 & 0xFFFFFFF9;               // masked magic, membership check only
v22 = (v19 & 4) ? ((4*(v19&2)) | 0x10) : (4*(v19&2));   // always 0 for "AT.." family
switch (v20) {
    case 'ATEC': v22 |= 0x01; break;
    case 'ATEP': v22 |= 0x02; break;
    case 'ATEU': v22 |= 0x04; break;
    case 'ATET': v22 |= 0x20; break;
}
*((DWORD*)a1 + 19) = v22;              // stored on the texture object
```

This is real — confirmed against the actual sample headers:
- `decompressed_242312.atep` → magic `ATEP`, `container_flags = 0x02`
- `decompressed_2871.atex` → magic `ATEX`, `container_flags = 0x00`
- `decompressed_27.ateu` → magic `ATEU`, `container_flags = 0x04`

**BUT**: the field is only *written* in this function in the disassembly we
have — never *read* again inside `sub_140B83040`. The actual consumer (likely
in the material/render layer, e.g. treating `ATEP` as premultiplied alpha) is
outside the excerpts we have. This was implemented as a hypothesis
(`castlemist::atex::Premult` enum, auto-applied for `ATEP`) but **should be treated as
untested speculation**, not a confirmed fix. Initial numeric testing on the
`.atep` sample showed un-premultiplying raised mean RGB brightness in
partial-alpha regions from 51.5 → 73.7 and reduced near-black pixel count, which
is *suggestive* but not conclusive, especially in light of finding #2 below.

### 2. The real bug is very likely in the RLE constant-fill decoder, not the container flag

Per-mip `flags` byte (read from the mip header, independent of container magic)
controls whether the "constant-fill" RLE compression pass runs:

```
decompressed_242312.atep: level 0 flags=0x0C, level 1 flags=0x0C, ... level 5 flags=0x04, level 6-7 flags=0x00 (raw)
decompressed_2871.atex:   level 0 flags=0x0C
decompressed_27.ateu:     level 0 flags=0x00 (raw copy only, RLE path never runs)
```

The `.ateu` sample (flags=0x00, RLE path skipped) decoded as a clean,
recognizable icon. The `.atex` and `.atep` samples (flags=0x0C, RLE path
heavily used) both decoded as blotchy, incorrect-looking noise — **including
the plain `ATEX` sample**, which has no special container flag at all. That
strongly implicates the RLE constant-fill decode itself, shared by all
container types, rather than anything ATEP/container-flag specific.

**Given it's a map texture** (per the user), the blotchy/cloud output is
almost certainly wrong — real map tiles should have structured content
(terrain colors, roads, borders), not organic noise. (Earlier in this
conversation it was speculated that the blotch pattern might be a legitimate
smoke/particle-effect texture — that reasoning doesn't hold once we know the
source is a map.)

### 3. The `huff_tables()` port is confirmed wrong, but the fix attempt failed verification

The real table data (`byte_141C86390`, dumped in `pseudocode.txt` around line
51050) is:

```
0000000141C86390 : 0D060E060F061006
0000000141C86398 : 09060A060B060C06
0000000141C863A0 : 0506060607060806
0000000141C863A8 : 0106020603060406
0000000141C863B0..3C8 : 1102 repeated (16 entries)
0000000141C863D0..onward : 0001 repeated (32 entries)
```

Read as `(val, len)` byte pairs, the first 16 entries are:

```
idx:   0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
val:  13  14  15  16   9  10  11  12   5   6   7   8   1   2   3   4
len:   6   6   6   6   6   6   6   6   6   6   6   6   6   6   6   6
```

The current `huff_tables()` in `gw2_atex.hpp` instead does `val[k] = 16 - k`
(a simple countdown: `16,15,14,...,1`), which does **not** match — the real
table is grouped in blocks of 4 (`13-16, 9-12, 5-8, 1-4`), not monotonic.

**However**: when this corrected table was substituted in and tested against
the real `.atep` sample, it made things measurably *worse*:

| | boundary jump* | within-block variation* |
|---|---|---|
| original (wrong-looking) table | 16.2 | 16.1 |
| "corrected" table | 51.2 | 23.5 |

*(mean abs alpha difference across a 4×4 block edge vs. across the same
distance inside a block — a big gap between these two numbers indicates block
misalignment)

This means either:
- the byte-pair order is actually `(len, val)` not `(val, val)` as read above
  (though the `0x06` length byte position seems consistent — worth
  re-checking against how `bs.get()`/consumption works elsewhere), or
- the 6-bit index into the table isn't `head >> 58` in the naive MSB-first
  sense assumed by the current `BitReader`, or
- there's a bit-reversal / different addressing scheme between the raw table
  bytes and how the game code actually indexes into them.

**The `huff_tables()` fix was reverted** in the current `gw2_atex.hpp` — do not
re-apply the naive block-of-4 fix without re-deriving the correct bit-order.

## What's needed to actually resolve this

Pull wider excerpts of `pseudocode.txt` around these four call sites (the
actual run-length/bit-consumption logic, not just the table reference):

```
~line 48260–48290   (first: v85 = byte_141C86390[2*(v67>>58)])
~line 48470–48480
~line 48670–48690
~line 49140–49150
```

Get roughly 60-80 lines before and after each to see:
1. How `v67` (the bit-shift register) is built/refilled — this determines the
   actual bit order/endianness the `BitReader` needs to replicate.
2. What happens to the looked-up byte immediately after (is it split into
   nibbles? shifted? used as a direct index?) — this will settle the
   `(val,len)` vs `(len,val)` ordering question definitively.
3. Whether there's a bit-reversal step applied to the raw 6-bit window before
   indexing the table (would explain why the "corrected" table looked right
   in isolation but tested worse in practice).

Once that's nailed down, re-run the same test harness
(`test_decode.cpp` in this same output set) against
`decompressed_242312.atep` and check:
- Block-boundary vs within-block alpha variation (should be close, not 2x+ apart)
- Whether the decoded image actually looks like map content (terrain/roads)
  rather than noise

## Files to bring into Claude Code

- `gw2_atex.hpp` — current state, with `container_flags` added and the
  Premult hypothesis wired in, but `huff_tables()` reverted to the
  known-wrong-but-at-least-self-consistent original.
- `test_decode.cpp` — decodes all three sample files, dumps raw RGBA +
  metadata, prints `container_flags` and per-mip `flags`. Useful scaffold for
  re-testing any table fix:
  ```
  g++ -std=c++20 -O2 test_decode.cpp -o test_decode && ./test_decode
  ```
  then convert `.rgba` dumps to PNG for viewing:
  ```python
  from PIL import Image
  with open('atep_meta.txt') as f: w,h = map(int, f.read().split())
  im = Image.frombytes('RGBA', (w,h), open('atep_raw.rgba','rb').read())
  im.save('atep_raw.png')
  ```
- `disassembly.txt`, `pseudocode.txt` — original IDA output, needed for
  further RE around the line ranges above.
- The three sample files (`decompressed_242312.atep`, `decompressed_2871.atex`,
  `decompressed_27.ateu`).

## Open questions / things not yet investigated

- What do `ATEC` and `ATET` container flags actually do downstream? No sample
  files with those magics were available to test.
- Whether the premultiplied-alpha handling for `ATEP` is real or coincidental
  — can't be separated from the RLE bug until the RLE decode is fixed, since
  the current `.atep` sample's "garbage" output makes any alpha-based
  conclusion unreliable.
- The per-mip `flags` bits 0x01/0x02/0x04/0x08 semantics in `inflate_mip()`
  were ported from pseudocode but not independently re-verified bit-by-bit
  against disassembly the way the huffman table was.
