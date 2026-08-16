---
name: gw2-method0-degenerate-huff
description: "Method0 huffman: an all-zero code-length table decodes to symbol totalSymbolCount-1 in ZERO bits (CmpHuff.cpp) — fixed the 462 'huffman decode failed' entries"
metadata: 
  node_type: memory
  type: project
  originSessionId: 78598ac3-af9b-40bc-8a0b-a33f223cc985
  modified: 2026-07-26T04:11:33.017Z
---

> **Addresses in this note are from an older client build and no longer resolve
> (checked 2026-08-16).** The finding below is correct and was re-confirmed in the
> current binary; only the `sub_14…` labels rotted. In particular `sub_140D9F8C0` is
> *not* Method 0 inflate — it is inside a **compressor**. Current map:
> [[gw2-cmp-img-symbol-map]] (`Cmp_DecompressMethod0` @ `0x140DA27F0`,
> `CmpHuff_BuildDecodeTable` @ `0x140DA9730`).

**Root cause of the "huffman decode failed (no matching code)" entries (fixed 2026-07-26).**

In `cmp_decompress_method0.hpp`, a Huffman table whose RLE code-length block decodes to **all zeros** produced an empty decode table, and `HuffTable::decode()` threw. That is legal input, not corruption: `CmpHuff.cpp` (`sub_140DA6800` in Gw2-64) patches it explicitly —

```c
if (totalSymbolCount && !assigned) {   // nothing got a code
    next[total-1] = head[0];
    head[0]       = total - 1;         // <-- LAST symbol, not symbol 0
    count[0]      = 1;
}
```

so the alphabet collapses to symbol **`totalSymbolCount - 1`**, carried in **ZERO bits**. Fix = `if (entries_.empty()) return total_symbols_ - 1;` before the bit-reading loop (throw only when `total_symbols_ == 0`).

**Substituting symbol 0 is a trap**: it "works" on entries whose degenerate table is the distance table of a pure previous-byte run (`DIST_EXTRA[0] == 0`), yielding the right output SIZE and a valid magic — but the bytes are wrong (strs 4252 decoded to 1 record instead of 1024), and it still desyncs wherever `DIST_EXTRA[total-1] != 0` (4 of the 462 died mid-stream). The symbol choice matters because it also selects how many EXTRA bits follow.

Almost always the **distance** table, usually the last block, on highly repetitive payloads.

**Impact:** all **462** affected entries of the retail Gw2.dat now decode — 277 textures, 55 ABNK, 47 strs, 39 dds, 35 MODL, 1 mapc (26 MB), ABIX, AMSP, AMAT, ASND. e.g. baseId 625979 / fileId 2351337 = a 1024x1024 3DCX ATEX. `idx_stats` is now 0 errors / 0 `flagged_compressed_but_not` over 808,155 entries.

**gw2index's `flagged-compressed-but-not` label was a MISDIAGNOSIS** for these — it try/catches the Method0 decode and falls back to raw on failure. The MFT compression flag was correct all along; they really are compressed. Don't add a "trust the data over the flag" fallback in castlemist — fix the decoder instead.

The header exists in **three copies**, all patched: `castlemist/include/`, `gw2mcp/native/include/` (used by gw2mcp CLI *and* gw2index via `-I $NAT/include`), and `gw2app/gw2_decompress_data.hpp`. Rebuild all three consumers, then force a re-index (`DELETE FROM entries WHERE error!=''` + resumable gw2index run) of both `gw2app/gw2_index.db` and `castlemist/gw2_index.db`.

NB `sub_140D9D720` is Method **1** (delta vs a reference buffer, 3 tables/block + a signed-delta match class), NOT Method 0 — Method 0 inflate is `sub_140D9F8C0` (2 tables per block). See [[castlemist-app]], [[gw2index-tool]], [[gw2mcp-server]].
