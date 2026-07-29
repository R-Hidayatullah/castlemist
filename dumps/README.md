# dumps/

Output directory for everything the tools under [`tools/`](../tools) extract
from a local Guild Wars 2 installation. **Nothing in here is tracked in git** --
only the directory layout and this file are. The contents are derived data:
they belong to your copy of `Gw2.dat`, they run to hundreds of megabytes, and
regenerating them is a single command.

Point the tools at your own install and fill these in:

| directory   | contents                                            | produced by |
| ----------- | --------------------------------------------------- | ----------- |
| `index/`    | `gw2_index.db`, `local_index.db` -- the SQLite index of every MFT entry (baseId, fileIds, header, compression truth, sizes, chunk list) | `tools/gw2index` |
| `packfile/` | `gw2_packfile.json` -- packfile chunk struct definitions recovered from the client | `tools/structs` |
| `shaders/`  | DXBC blobs and disassembly pulled out of AMAT entries and out of `Gw2-64.exe` | `tools/shaders` |
| `strs/`     | decrypted string tables, `textkeys.csv`, `strs_textbase.csv` | `tools/strs` |
| `textures/` | decoded ATEX/ATEP/ATEU/DDS exports                  | castlemist, `tools/gw2dat_cli` |
| `models/`   | exported MODL geometry, skeletons and animations     | castlemist |
| `captures/` | RenderDoc `.rdc` frame captures used by the renderdoc MCP server | RenderDoc |

## Generating them

Full instructions, including where each format's parser lives and how the
results were validated, are in [`docs/generating-data.md`](../docs/generating-data.md).
The short version:

```bash
cmake --build build/debug --target gw2index
./build/debug/bin/gw2index.exe --dat "C:/Program Files (x86)/Steam/steamapps/common/Guild Wars 2/Gw2.dat" --out dumps/index/gw2_index.db
```

castlemist itself reads `dumps/index/gw2_index.db` if it is there and works
without it -- the index only makes searching by baseId and filtering by type
fast.

## Why none of this is committed

`gw2_index.db` alone is ~227 MB and a single RenderDoc capture is 50-70 MB.
They are also install-specific: a different game build produces a different
MFT, so a committed index would be wrong for everyone but the person who
committed it.
