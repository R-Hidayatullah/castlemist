# renderdoc-mcp

An MCP server for querying RenderDoc `.rdc` frame captures: passes, draw calls,
pipeline state, shader disassembly, constant-buffer values, textures.

Built for GW2 shader RE, but nothing in it is GW2-specific.

## Why it shells out instead of importing `renderdoc`

The obvious design — `import renderdoc` in the MCP process — cannot work on a
stock Windows install, and no amount of pip fixes it:

* The RenderDoc MSI ships **no** `renderdoc.pyd` and no `pymodules/` dir. Upstream:
  the module "isn't included by default in distributed builds". The third-party
  `renderdoc-mcp` PyPI package claims it ships with every install; that is wrong.
* Even with the `.pyd`, RenderDoc bundles **Python 3.6** (`python36.dll`) and the
  module is ABI-locked to it. The `mcp` SDK needs >= 3.10. No single interpreter
  satisfies both.
* `renderdoccmd.exe` has no `python` subcommand.

So the server runs in modern Python and dispatches each query to RenderDoc's
**embedded** interpreter via `qrenderdoc.exe --python <script>`, which does have
the module built in. Every tool generates a small Python-3.6-compatible replay
script, runs it, and reads back a JSON file.

Two behaviours are handled internally so callers never hit them:

* `qrenderdoc --python` can return **before** the script finishes, so the server
  polls for a `.done` sentinel instead of trusting process exit.
* The generated script ends with `os._exit(0)`; without it the main GUI opens.

Each replay is a fresh headless replay of the file on disk. There is no way to
attach to a capture already open in the qrenderdoc GUI.

## Install

Needs `mcp` in the host Python (same one that runs your other MCP servers) and a
RenderDoc install. Register in `.mcp.json`:

```json
"renderdoc": {
  "type": "stdio",
  "command": "python",
  "args": ["C:\\Users\\...\\gw2app\\renderdoc-mcp\\server.py"],
  "env": {
    "RDMCP_QRENDERDOC": "C:\\Program Files\\RenderDoc\\qrenderdoc.exe",
    "RDMCP_CAPTURE": "C:\\Users\\...\\gw2app\\sample1.rdc"
  }
}
```

| env | meaning |
| --- | --- |
| `RDMCP_QRENDERDOC` | path to `qrenderdoc.exe` |
| `RDMCP_CAPTURE` | default `.rdc`, so tools can be called without `capture=` |
| `RDMCP_WORKDIR` | scratch dir for generated scripts / results / cache |
| `RDMCP_TIMEOUT` | per-replay timeout in seconds (default 300) |

`rd_config` reports all of the above and whether the paths exist — check it first
when something fails.

## Tools

| tool | what it answers |
| --- | --- |
| `rd_config` | is the server wired up correctly |
| `rd_info` | API, vendor, draw/texture/buffer counts |
| `rd_passes` | **start here** — the frame's render passes |
| `rd_actions` | flat paginated event list, filter by name |
| `rd_pipeline` | complete state at one event |
| `rd_shader` | disassembly + reflection for one shader |
| `rd_shaders_dump` | disassemble *every* shader, optionally regex-grep them |
| `rd_cbuffer` | actual constant-buffer values as float4/int4 rows |
| `rd_textures` | list/filter textures |
| `rd_save_texture` | write a texture to PNG (alpha preserved) |
| `rd_find_draws` | draws using a shader / binding a texture / blending |
| `rd_disassembly_targets` | available ISA flavours (DXBC, AMDIL, GCN, RDNA…) |
| `rd_exec` | escape hatch: arbitrary replay Python |
| `rd_clear_cache` | drop cached results |

### Typical flow

`rd_passes` to get the frame skeleton → pick an interesting event → `rd_pipeline`
on it → `rd_shader` for the disassembly → `rd_cbuffer` for the values the engine
actually fed it. `rd_shaders_dump` with a pattern answers frame-wide questions
("how many pixel shaders `discard`?"). `rd_find_draws` answers "where is this
shader/texture used".

## Caching

Results are cached on disk under `RDMCP_WORKDIR/cache`, keyed by capture path +
mtime + size + the exact query, so re-capturing invalidates automatically. A
repeat `rd_passes` drops from ~15 s to ~0 s. `rd_save_texture` and `rd_exec` are
never cached. `rd_clear_cache` forces a re-run.

## Writing `rd_exec` code

Must be **Python 3.6** compatible and fill the dict `RESULT` (JSON-serialisable
values only). Available: `rd`, `ctrl` (ReplayController), `ACTIONS`, `DRAWS`,
`TEXINFO`, `BUFINFO`, `STAGES`, `ARGS`, and helpers `texdesc(id)`, `_desc(u)`,
`_slot(u)`, `_rid(x)`.

```python
import collections
c = collections.Counter()
for a in DRAWS:
    ctrl.SetFrameEvent(a.eventId, True)
    p = ctrl.GetPipelineState()
    c[_rid(p.GetShader(STAGES['ps']))] += 1
RESULT['psHistogram'] = dict(c)
```

## API notes (current RenderDoc builds)

Things that cost time to discover, worth knowing before writing `rd_exec` code:

* Read pipeline state through the **API-agnostic** `ctrl.GetPipelineState()`. The
  D3D11-specific object no longer has `.srvs`; bound resources come from the
  descriptor API (`GetReadOnlyResources` / `GetConstantBlocks`), whose elements
  wrap the real descriptor in `.descriptor` and the slot in `.access.index`.
* Result objects are `ResultDetails` — test with `res.OK()`, not `== 0`.
* `APIProperties` has no `driverName`; use `vendor` / `pipelineType` /
  `localRenderer`.
* Field sets differ between the agnostic and per-API state objects (the agnostic
  `RasterState` has no `depthBias`, for instance) — probe with `hasattr`.
