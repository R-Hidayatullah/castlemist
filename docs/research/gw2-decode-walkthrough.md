---
name: gw2-decode-walkthrough
description: "Plain-English walkthrough of how GW2 unpacks and decodes a texture — written for readers who are not reverse engineers; pairs with the IDA folders and renamed symbols"
metadata:
  node_type: memory
  type: reference
  modified: 2026-08-16
---

# How GW2 gets from a file in Gw2.dat to pixels on screen

Written for someone who can read code but does not do reverse engineering. No IDA
knowledge assumed. Every name below is the name you will actually see in
`Gw2-64.exe.i64` — the functions have been renamed and sorted into folders, so you can
follow along by opening the Functions window and expanding **GW2 Decompression** or
**GW2 Image decode**.

The symbol tables, offsets and evidence are in [[gw2-cmp-img-symbol-map]]. This file is
the story; that one is the reference.

## The two stages

A texture in the game archive is squeezed twice over, so getting a picture out takes two
separate passes:

```
bytes in Gw2.dat
      |
      |  stage 1 : general-purpose decompression   (folder: GW2 Decompression)
      v
an ATEX file  ("ATEX" + a format tag + width/height + mip levels)
      |
      |  stage 2 : image decoding                  (folder: GW2 Image decode)
      v
pixels
```

Stage 1 knows nothing about images — it would unpack a sound file the same way. Stage 2
knows nothing about the archive. They are genuinely independent, which is why they live
in different folders.

## Stage 1 — decompression

### Where to start reading

`Cmp_Decompress`. Every compressed entry in the archive goes through this one function.
Its job is small: look at the first few bits, decide which of two algorithms was used,
and hand off.

```
Cmp_Decompress
   |
   +-- method 0 --> Cmp_DecompressMethod0     (the common case)
   +-- method 1 --> Cmp_DecompressMethod1     (rare; needs an older copy of the file)
   +-- anything else --> print "corrupt data, Method = %d" and give up
```

The method number is the top 4 bits of the very first 32-bit word. That is it — there is
no header, no magic number, no length field.

### Method 0, the one you will actually meet

This is a close relative of the algorithm in ZIP files. The idea is that most data
repeats itself, so instead of writing bytes out again you write *"go back N bytes and
copy M of them"*.

The decoder reads a stream of **symbols**. Each symbol is one of two things, and this
single decision is the heart of the whole thing (it is commented in the binary at
`0x140DA2A98`):

- **symbol below 0x100** — a plain byte. Write it out. Done.
- **symbol 0x100 or above** — a repeat instruction. Now read two more things:
  - **how many bytes to repeat** (the *length*)
  - **how far back to start** (the *distance*)

Neither the length nor the distance is stored directly, because that would waste space.
Each is stored as a small symbol that indexes a table of starting values, plus a few
raw "extra" bits to pin down the exact number:

```
length   = Cmp_LenBase[symbol]  + <read Cmp_LenExtraBits[symbol] more bits>  + minMatchLen
distance = Cmp_DistBase[symbol] + <read Cmp_DistExtraBits[symbol] more bits>
```

All four of those tables are in the **GW2 Decompression tables** folder, with their
contents written out in the comments.

Then the copy happens, and there is one detail worth pausing on. The copy is byte by
byte, moving forward, from `outPtr - (distance + 1)`. If the distance is small — say 1 —
the loop immediately starts re-reading bytes it has only just written this same loop.
That is not a bug; it is the trick that lets 5000 identical bytes compress to a handful
of bits. This is commented in the binary at `0x140DA2DA0`.

### Why there is a Huffman step at all

Symbols are not fixed-width. Common symbols get short bit patterns, rare ones get long
patterns. The mapping is rebuilt **for every block** of up to 4096 symbols, and it is
stored at the front of the block, so the decoder has to build a lookup table before it
can read anything.

That is `CmpHuff_BuildDecodeTable`. Method 0 calls it **twice** per block (one table for
literals/lengths, one for distances). Method 1 calls it three times. Counting those calls
is, in fact, the cleanest way to tell the two methods apart.

### The one genuinely surprising thing

A block is allowed to say *"every symbol in this table has length zero"*. That sounds
like corruption, and a naive decoder throws an error — this is what used to break 462
entries in the retail archive.

It is legal. It means the alphabet has collapsed to exactly one symbol, and that symbol
costs **zero bits** to encode: you just know what it is. The game handles it explicitly:

```c
if (totalSymbolCount && !assignedCount) {
    headByCodeLen[0]  = totalSymbolCount - 1;   // the LAST symbol
    countByCodeLen[0] = 1;
}
```

Note **`totalSymbolCount - 1`**, the last symbol — not symbol 0. Using symbol 0 appears
to work on some files and then quietly corrupts others, because the symbol you pick also
decides how many extra bits get read next. Getting that wrong desynchronises the whole
remaining stream. Full detail in [[gw2-method0-degenerate-huff]].

## Stage 2 — image decoding

### Where to start reading

`ImgDecode_Run`. It sniffs the first four bytes and picks one of three routes:

```
ImgDecode_Run
   |
   +-- ImgAtex_IsAtexMagic? --> ImgAtex_Decode        the game's own texture format
   +-- ImgDds_IsDdsMagic?  --> ImgDds_Decode          plain DirectDraw Surface
   +-- otherwise           --> ImgDecode_ProbeFormat  ordinary image files
```

`ImgDecode_ProbeFormat` just tries nine ordinary formats in order until one accepts the
header — Bmp, Dcx, Gif, Jpeg, Pcx, Png, Psd, Tiff, Tga — and says
`"Unknown image file format"` if none do. These are all paired up as `*_Probe` /
`*_Decode` in the **4 Standard file loaders** folder. Nothing exotic here.

### ImgAtex_Decode, and why it looks so intimidating

This is the big one (about 9,800 bytes) and it is worth knowing *why* before you open it.

It is **not** a straight-line decoder. The game streams textures off disk, so this
function has to work on whatever bytes have arrived so far, stop cleanly, and pick up
exactly where it left off next time. That shape is called a resumable state machine, and
it is why the body is one huge `switch (state)` with cases 0 to 13.

Practical consequences when reading it:

- `savedState`, `savedMipLevel`, `savedBlockIndex` are the bookmark.
- `spillWord`, `spillBits`, `spillByteCount` hold a partly-read value that straddled the
  end of a chunk.
- Control flow jumps around a lot. That is the streaming, not obfuscation.

The header parse itself is simple once you filter the streaming out:

1. read the 4-byte container magic → `containerMagic`
2. read the 4-byte pixel format tag → `formatFourCC` → `grFormat`
3. read width and height
4. for each mip level: read its flags, then run the fill passes

There is a full field map for the `decoder` struct in the function's comment in IDA, so
you can decode expressions like `*((_DWORD *)decoder + 17)` by looking at the legend
rather than counting offsets yourself.

### The finding that matters most

There are **six** container magics: `ATEX`, `ATTX`, `ATEC`, `ATEP`, `ATEU`, `ATET`.
It is natural to assume they decode differently. They do not.

The decoder turns the magic into a small tag (`ATEC`=1, `ATEP`=2, `ATEU`=4, `ATET`=0x20)
and stores it at `decoder+0x4C`. That store, at address `0x140B86D97`, is the **only**
access to that field in the entire function — and it is a write. Nothing ever reads it
back, here or anywhere downstream.

So an `.atep` file decodes byte-for-byte identically to an `.atex` file. The magic is a
label for the engine's own bookkeeping, nothing more. There is no premultiplied-alpha
step and no other container-driven transform, and adding one will corrupt your output.

**If an ATEP looks wrong, suspect your viewer, not the decoder.** This exact trap has
been hit before: GW2 textures routinely store garbage colour in fully transparent
pixels, so a viewer that shows RGB and ignores alpha will render psychedelic red and
green blobs from a perfectly correct decode. See [[gw2-atex-atep-decode]].

### What actually changes the pixels

Only two inputs:

1. **The pixel format tag** (the second fourCC), which maps to a `GR_FORMAT` number:

   ```
   DXT1=22 DXT2=23 DXT3=24 DXT4=25 DXT5=26 DXTA=27
   DXTL=28 DXTN=29 3DCX=30 BC5X=31 BC7X=32
   ```

2. **The per-mip flags word**:

   | flag | meaning |
   | --- | --- |
   | `0x1` `0x2` `0x4` `0x8` | switch on the run-length "constant fill" passes |
   | `0x10` | mirror the border blocks — terrain tiles only, see below |
   | `0x200` | no block bitmap, this mip is stored raw |

The `0x10` case has its own function, `ImgAtex_MirrorTerrainBorders`, and it is a nice
small one to read if you want to warm up on something tractable. Terrain tiles are
256×256, which is 64×64 blocks of 4×4 pixels. The mask `0xC0000003` picks out the
outermost two rows and columns, and those blocks get mirrored so adjacent terrain tiles
line up seamlessly at the seam.

## Suggested reading order

If you are new to this, open the **GW2 Decompression** folder and go in this order —
each one is bigger than the last, and each depends only on what came before:

1. `Cmp_Decompress` — 475 bytes, just a dispatcher. Read the whole thing.
2. `ImgFmt_GetFlags` / `ImgFmt_GetBitCount` — a few lines each, pure table lookups.
3. `ImgAtex_IsAtexMagic` — six comparisons, nothing else.
4. `ImgAtex_MirrorTerrainBorders` — a real algorithm, but small and self-contained.
5. `CmpHuff_BuildDecodeTable` — the first genuinely dense one.
6. `Cmp_DecompressMethod0` — now you have everything you need for it.
7. `ImgAtex_Decode` — read the comment legend first, then only the case you care about.

## A caution about addresses

Addresses in these notes are for the client build analysed on 2026-08-16. **They change
every time the game patches**, and they change silently — a stale address still lands
inside *some* function, so it looks fine while being wrong.

The stable way to find things again is the Perforce source paths ArenaNet left in the
binary (`D:\Perforce\Live\NAEU\v2\Code\Arena\...`). Each `.cpp` passes its own path to
the assert helper, so the cross-references to that string list exactly the functions
from that file. `castlemist/tools/ida_apply_cmp_img_names.py` re-applies this whole map
and checks those anchors first, so it refuses to run rather than mislabel a new build.
