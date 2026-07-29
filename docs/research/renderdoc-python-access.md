---
name: renderdoc-python-access
description: "RenderDoc replay scripting works via `qrenderdoc.exe --python`, NOT via importing renderdoc.pyd; wrapped as the local \"renderdoc\" MCP server"
metadata: 
  node_type: memory
  type: project
  originSessionId: dfb894a2-41a8-45c1-b7f2-fc1819de29ea
  modified: 2026-07-28T22:54:45.105Z
---

**Use the local `renderdoc` MCP server first** — `gw2app/renderdoc-mcp/server.py`,
registered in `.mcp.json`, defaults to `sample1.rdc`. It wraps everything below so
you rarely need raw scripts: `rd_passes` (frame skeleton), `rd_pipeline`,
`rd_shader`, `rd_shaders_dump` (disassemble all + regex grep), `rd_cbuffer` (real
constant values), `rd_find_draws`, `rd_save_texture`, `rd_exec` (escape hatch).
Results are disk-cached by capture mtime, so repeat queries are instant.
NOTE: this is a hand-written server, NOT the `renderdoc-mcp` PyPI package (which
cannot run here — see below). The old stub config is `renderdoc-mcp.stale-config.json`.

To script RenderDoc replay of `.rdc` captures directly, run:
`"C:\Program Files\RenderDoc\qrenderdoc.exe" --python <script.py>`

That executes the script in RenderDoc's **embedded** interpreter, where the
`renderdoc` module is built into the binary. Script should write results to a
JSON file (GUI app stdout is unreliable) and end with `os._exit(0)` to stop the
main UI from opening. Scripts must be **Python 3.6** compatible.

**Why the obvious route fails:** `import renderdoc` from system Python does not
work here, and cannot be made to work without building RenderDoc from source:
- The Windows MSI install of RenderDoc 1.45 ships **no** `renderdoc.pyd` and no
  `pymodules/` dir. Upstream: the module "isn't included by default in
  distributed builds". The `renderdoc-mcp` README claiming it "ships with every
  RenderDoc installation" is wrong.
- Even with the pyd, RenderDoc 1.45 bundles **Python 3.6.4** (`python36.dll`)
  and the module is ABI-locked to it; system Python here is 3.14.6.
- the `renderdoc-mcp` PyPI package therefore cannot run: it needs `>=3.10` (the
  `mcp` SDK), so no single interpreter satisfies both. The local server solves
  this by running in modern Python and *shelling out* to `qrenderdoc --python`.
- `renderdoccmd.exe` has **no** `python` subcommand (1.45).

Also note: this is always a fresh headless replay of the file on disk — there is
no way to attach to a capture already open in the qrenderdoc GUI.

Gotchas (all handled inside the MCP server; needed for raw scripts and `rd_exec`):
- `qrenderdoc --python` can return **before** the script finishes — poll for a
  sentinel file, don't trust process exit. Scripts must end `os._exit(0)` or the
  GUI opens, and must be Python 3.6 compatible.
- Read state via the API-agnostic `ctrl.GetPipelineState()`. The D3D11-specific
  object has **no** `.srvs` any more; bound resources come from the descriptor API
  (`GetReadOnlyResources`/`GetConstantBlocks`), whose elements wrap the real
  descriptor in `.descriptor` and the slot in `.access.index`.
- Field sets differ between agnostic and per-API state objects (the agnostic
  `RasterState` has no `depthBias`) — probe with `hasattr`.
- `APIProperties` has no `driverName` in 1.45 (use `vendor`, `pipelineType`,
  `localRenderer`). Result objects are `ResultDetails` — test with `res.OK()`,
  not `== 0`.
