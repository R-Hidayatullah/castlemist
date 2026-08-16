---
name: gw2-archive-packfile-runtime
description: "The client's own view of Gw2.dat — Archive3's 24-byte MFT row (which is not what our on-disk MftData names it), and the PF packfile container every asset ships in"
metadata:
  node_type: memory
  type: project
  modified: 2026-08-16
---

# Archive3 and Packfile, from the client side

Our dat reader was written from the file format outward. This note is the other
direction: the names, field meanings and invariants the *client* uses, recovered
from the asserts in `Arena\Services\Archive3\Archive.cpp` (43 functions) and
`Arena\Services\Packfile\Packfile.cpp` (44 functions).

The interesting part is where the two disagree.

## The 24-byte MFT row is not what we call it

`ArchiveAllocEntry` is 24 bytes, the same row `MftData` in
`include/castlemist/native/gw2dat.h` describes. Three of our field names are
wrong about what the field *is*:

| our `MftData` | client `ArchiveAllocEntry` | what it actually holds |
| --- | --- | --- |
| `offset` | `offset` | same |
| `size` | `size` | same |
| `compression_flag` (u16) | `extraBytes` (u16) | tail bytes past the payload — **not** a compression flag |
| `entry_flag` (u16) | `flags` (u8) + `stream` (u8) | **two** fields, not one |
| `counter` | `nextStream` | next mftIndex in a singly-linked stream chain, 0 = end |
| `crc` | `crc` | same |

Evidence: `Archive_ReallocEntry` (`0x14156BF40`) writes offset/size/extraBytes/crc
at `+0`/`+8`/`+0xC`/`+0x14`. `Archive_AllocStreamEntry` (`0x141566E90`) tests
`flags` at `+0x0E`, compares `stream` at `+0x0F` against a byte argument, and
walks `nextStream` at `+0x10` as a chain terminator.

That last one matters most: **an entry can be one link of a multi-entry chain.**
`FLAG_FIRST_STREAM` (`0x02`) marks the head; `FLAG_ENTRY_USED` is `0x01`. A reader
that treats every used MFT row as a standalone file will silently mis-handle
chained entries.

Reserved indices, all read off literals in the asserts:

```
INDEX_DESCRIPTOR  = 0
                    1   IsFixedLocation() -- asserted to keep offset 0 and no CRC
INDEX_MFT         = 3
INDEX_FIRST_FILE  = 16
```

## The MFT CRC skips its own slot

`Archive_WriteMftAndDescriptor` (`0x14156C3A0`) does this, in order:

1. assert `descriptor.signature == SIGNATURE`, which is `0x1A75694D`
2. `Crc32C` over the **72-byte** descriptor (entry slots 0..2)
3. continue that CRC over entries `[4 .. count)` — **skipping index 3**, because
   index 3 is `INDEX_MFT`, the slot the finished CRC gets written into
4. bump `descriptor.sequence`, stepping over `(unsigned)-1`

If you ever verify the MFT CRC yourself, step 3 is the one that will not be
obvious from the file alone.

## Packfile: the signature is a uint16, not a fourcc

`Packfile_Init` (`0x140DE3980`) asserts `hdr->signature == PACKFILE_SIGNATURE`,
and the constant is `18000` = `0x4650` = **`"PF"` as a little-endian uint16**.
The 12-byte header is:

| off | field |
| --- | --- |
| `+0x00` | `u16 signature` (`0x4650`) |
| `+0x02` | `u16 flags` |
| `+0x04` | `u16` unknown |
| `+0x06` | `u16 headerSize` — offset of the first chunk header |
| `+0x08` | `u32 type` — asset fourcc (`MODL`, `mapc`, …) |

Chunk lengths are measured **from `+8`**, not from the start of the chunk:

```c
next   = (byte*)&chunk.version + chunk.nextChunkOffset;
dataSz = chunk.nextChunkOffset - chunk.headerSize + 8;
```

`descriptorOffset` at `+0x0C` only exists when `headerSize > 12`. Zero-length
chunks are dropped with a warning, not an error:
*"PackFile: Discarding zero byte chunk %u"*.

### 32-bit authoring, 64-bit client

Header flag bit 2 says the chunk's pointer fields are 8 bytes. When it is clear,
`Packfile_ConvertPointerSize` (`0x140DE2990`) rewrites every pointer field 4 → 8
on load; it asserts `sizeFrom` and `sizeTo` are each 4 or 8. This is the same
32/64 split [[gw2-granny-64bit]] describes for granny blobs, handled here at the
container level — which is why `Packfile_MatchTypes32`/`64`,
`ConvertField32`/`64` and `MarkOffsets32`/`64` all exist in pairs.

## Named functions

`/Arena/Services/Archive3` (34) and `/Arena/Services/Packfile` (30) in the
Functions window. Highlights:

| addr | name |
| --- | --- |
| `0x14156B590` | `Archive_Open` |
| `0x14156C3A0` | `Archive_WriteMftAndDescriptor` |
| `0x14156BF40` | `Archive_ReallocEntry` |
| `0x141566E90` | `Archive_AllocStreamEntry` |
| `0x14156A170` | `Archive_ScanAndRepair` |
| `0x14156AE30` | `Archive_ValidateDirectoryVsMft` |
| `0x141568470` | `Archive_CoalesceFreeSpace` |
| `0x140DE3980` | `Packfile_Init` |
| `0x140DE5AD0` | `Packfile_LoadById` |
| `0x140DE9100` | `Packfile_FixupPointers` |
| `0x140DE2990` | `Packfile_ConvertPointerSize` |
| `0x140DE4070` | `Packfile_ApplyFilters` |

Structs in `tools/structs/gw2_ida_types_subsystems.h`.

## Not done

- `FileArchive.cpp` (43 funcs) and `ArchiveAsyncStreaming.cpp` (23) are
  enumerated but unnamed — that is the async read path above `Archive3`.
- `MultiManifest.cpp` / `AssetDownloader*` (the patcher side) untouched.
- The chunk *filter* registry — `Packfile_ApplyFilters` runs per-chunk version
  upgrade functions, and the table of those was not dumped. That table is what
  maps a chunk fourcc + version to its reader, so it is the obvious next target
  for anyone extending `tools/structs/dump_gw2_structs.py`.

See also: [[filetypes]], [[gw2index-tool]], [[gw2-granny-64bit]],
[[gw2-ida-naming-coverage]].
