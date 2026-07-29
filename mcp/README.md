# mcp/

Model Context Protocol servers that put the dat, its index and RenderDoc
captures in front of an agent. Three of them, each a thin wrapper over
something that already exists rather than a second implementation:

| server     | wraps                              | needs |
| ---------- | ---------------------------------- | ----- |
| `gw2dat`   | `tools/gw2dat_cli`                 | the CLI built, a `Gw2.dat` |
| `gw2index` | `dumps/index/gw2_index.db`         | the index built |
| `renderdoc`| `qrenderdoc.exe --python`          | RenderDoc installed, a `.rdc` |

`gw2dat` shelling out to the same binary the application uses is the point: the
agent and castlemist can never disagree about what an entry contains, because
there is one decoder.

## Setup

```bash
pip install -r gw2dat/requirements.txt
```

```bash
cmake --build --preset debug --target gw2dat_cli gw2index
```

Then copy `mcp.example.json` to `.mcp.json` at the repository root and fix the
paths for your machine. `.mcp.json` is gitignored -- it holds absolute paths
that are true only on the machine that wrote it, which is exactly what should
not be shared.

```bash
cp mcp/mcp.example.json .mcp.json
```

## gw2dat

Finds `gw2dat_cli.exe` in whichever `build/<preset>/bin/` has one, preferring
`debug`. `CASTLEMIST_CLI` overrides. The archive defaults come from
`GW2_DAT_PATH` and `GW2_LOCAL_DAT_PATH`, and any tool takes `dat_path=` to
override per call.

Tools: `gw2_info`, `gw2_list_entries`, `gw2_lookup`, `gw2_resolve`,
`gw2_extract`, `gw2_sniff`, `gw2_decode_texture`, `gw2_encode_texture`,
`gw2_decode_strs`, `gw2_parse_packfile`, `gw2_compress`, `gw2_decompress`.

```bash
python mcp/gw2dat/server.py --selftest
```

## gw2index

Answers "what is fileId X, what type, which chunks, what is mis-flagged"
straight from SQLite -- no dat re-scan, no extract, no decompress. Defaults to
`dumps/index/gw2_index.db`; `GW2_INDEX_DB` or a `db=` argument overrides.

Tools: `idx_stats`, `idx_lookup`, `idx_query`, `idx_find_chunk`,
`idx_chunk_variants`.

## renderdoc

Replays a frame capture through RenderDoc's own Python API and reports draws,
passes, pipeline state, constant buffers, shaders and textures. This is how the
frame-pass order and the material/shader questions in `docs/research/` were
settled: against a real capture rather than by reading disassembly.

Scripts run via `qrenderdoc.exe --python` and its embedded interpreter, because
`renderdoc.pyd` is not in the MSI and cannot be imported from a normal Python.
Set `RDMCP_QRENDERDOC` to `qrenderdoc.exe` and `RDMCP_CAPTURE` to a default
`.rdc` under `dumps/captures/`.

Tools: `rd_info`, `rd_actions`, `rd_passes`, `rd_find_draws`, `rd_pipeline`,
`rd_shader`, `rd_shaders_dump`, `rd_cbuffer`, `rd_textures`, `rd_save_texture`,
`rd_exec`.
