---
name: gw2-ida-naming-coverage
description: "Exactly how far the IDB naming pass has got — what is finished, what is deliberately left, and the prioritised worklist for the remaining register temporaries"
metadata:
  node_type: memory
  type: project
  modified: 2026-08-16
---

# IDB naming coverage

State of `gw2decomp/Gw2-64.exe.i64` as of 2026-08-16. Written so the next session does not
have to re-measure. Numbers come from a scan over the 180 functions the backup tracks.

## Finished

| item | state |
| --- | --- |
| Function names | **180 / 180 named**, 0 still carrying an address suffix |
| Function comments | 154 |
| Data symbols | 22 |
| Local variables named | 792 |
| Pseudocode labels named | 85 |
| Enums declared and wired in | 6 |
| Fully clean functions (no `vNN`, no `LABEL_nn`) | 30 |

### The six enums

`GR_FORMAT` (38 members, from `ImgFmt_Names`), `CHAT_LINK_TYPE`, `IMG_FILE_TYPE`,
`CMP_METHOD`, `BGFX_ATTRIB`, `BGFX_ATTRIB_TYPE`. All are declared in
`tools/ida_restore_symbols.py` so they survive a restore.

The payoff is visible in the decompiler:

```c
BgfxVertexLayout_Add(layout, BGFX_ATTRIB_WEIGHT, 4u, BGFX_ATTRIB_TYPE_UINT8, 1, 0);
if ( method == CMP_METHOD0_STANDALONE ) ...
case IMG_FILE_TYPE_DDS:
```

`GrFvf_BuildVertexLayout` now reads essentially as the original C++.

## Two findings from this pass

**`ImgAtexCommon_DecodeRleBlocks` @ `0x140C213F0`** — the ATEX RLE constant-fill *decoder*,
which [[gw2-cmp-img-symbol-map]] had listed as an open question. Identified from its
asserts, which name the fields outright: `rleCode < 0xff`, `(rleCode & RLE_COMMON_MASK) == 0`,
`detail->rleLength <= detail->blockCount`. It is a far better place to study the
constant-fill behaviour than `ImgAtex_Decode`, where the same logic is inlined and tangled
with the streaming state machine.

**`CmpHuff_BootSymTbl` is 768 bytes, not 128** — three 256-byte sections sharing one index
`(runLength << 5) | codeLength`: `[0..255]` decode packed byte, `[256..511]` encode bit
count, `[512..767]` encode value. It is a bidirectional codec table. The earlier note
described only the decode third.

## Deliberately not done

**3543 generic `vNN` locals and 199 `LABEL_nn` remain**, concentrated in a few large
functions. This was a choice, not an oversight: most of those are single-use arithmetic
scratch inside decode loops, and naming them mechanically (`temp1`, `temp2`, …) would make
the code *look* annotated while conveying nothing — worse than leaving `v5`, because it
hides which names were actually derived.

Where a variable carries real meaning it has been named. What is left is the tail.

### Prioritised worklist

Ranked by how likely someone is to read the function, not by count:

| function | generic | labels | why it matters |
| --- | --- | --- | --- |
| `BgfxShaderD3D11_Create` | 133 | 2 | every shader passes through it; blob layout is documented in its comment |
| `ImgAtex_Decode` | 235 | 0 | already has the field-map legend; the fill passes are the gap |
| `ImgDds_Decode` | 221 | 72 | **72 labels** — the worst control-flow readability in the set |
| `Cmp_CompressMethod0/1` | 158 / 207 | 7 / 7 | compressor side; decompressors are already done |
| `GrFvf_ConvertVertices` | 117 | 10 | dispatcher over ~50 per-format converters |
| `ImgDxt_DecodeBlocksSse` | 121 | 1 | SSE, hard to name well without care |
| `ChatLink_DecodeBuildTemplate` | 77 | 5 | the most complex chat link |
| `ImgAtexCommon_DecodeRleBlocks` | 40 | 1 | small and high value — good next target |

`ImgDds_Decode`'s 72 labels are the single highest-value cheap win: labels are control
flow, so they can be named from context far more reliably than register temporaries.

### Named from call-site guard, not from the body

Four functions are named after the format check that selects them in `ImgAtex_Encode`,
because the guard is verified but the bodies were not read line by line. Their comments say
so explicitly:

`ImgAtex_EncodePlane_Dxt2Dxt3`, `ImgAtex_EncodePlane_Dxt4ToDxtl`,
`ImgAtex_EncodePlane_Color`, `ImgAtex_EncodePlane_Secondary`.

Treat those names as a hypothesis with good evidence, not as established fact.

## Backup

`tools/gw2_ida_symbols.json` + `tools/ida_restore_symbols.py` carry 180 functions,
154 comments, 792 locals, 85 labels, 22 data symbols, 4 inline comments and all six enums.
Round trip verified. Re-export with `export_symbols()` after any further work — it
discovers by name prefix, so newly named functions are picked up automatically.
