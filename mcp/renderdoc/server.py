"""renderdoc MCP -- query a .rdc frame capture (draw calls, passes, pipeline
state, shader disassembly, constant-buffer values, textures).

WHY THIS IS SHAPED THE WAY IT IS
--------------------------------
You cannot `import renderdoc` from a normal Python here, and no amount of pip
will fix it:

  * The Windows MSI install of RenderDoc ships **no** `renderdoc.pyd` -- the
    module "isn't included by default in distributed builds". The third-party
    `renderdoc-mcp` package's claim that it ships with every install is wrong.
  * Even with the .pyd, RenderDoc bundles **Python 3.6** (`python36.dll`) and
    the module is ABI-locked to it, while the `mcp` SDK needs >= 3.10. There is
    no single interpreter that satisfies both.
  * `renderdoccmd.exe` has no `python` subcommand.

So this server runs in modern Python and dispatches every query to RenderDoc's
**embedded** interpreter via `qrenderdoc.exe --python <script>`, which does have
the module built in. Each tool generates a small Python-3.6-compatible replay
script, runs it, and reads back a JSON file.

Two gotchas that are handled here so callers never see them:

  * `qrenderdoc --python` can return *before* the script finishes, so we poll for
    a `.done` sentinel rather than trusting process exit.
  * The script must end with `os._exit(0)` or the main GUI window opens.

Configuration (env vars, all optional):
  RDMCP_QRENDERDOC  path to qrenderdoc.exe
  RDMCP_CAPTURE     default .rdc so tools can be called without `capture=`
  RDMCP_WORKDIR     scratch dir for generated scripts / results / cache
  RDMCP_TIMEOUT     per-replay timeout in seconds (default 300)
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("renderdoc")

_DEFAULT_QRD = os.environ.get(
    "RDMCP_QRENDERDOC", r"C:\Program Files\RenderDoc\qrenderdoc.exe"
)
_DEFAULT_CAPTURE = os.environ.get("RDMCP_CAPTURE", "")
_WORKDIR = Path(
    os.environ.get("RDMCP_WORKDIR", str(Path(tempfile.gettempdir()) / "renderdoc-mcp"))
)
_TIMEOUT = float(os.environ.get("RDMCP_TIMEOUT", "300"))

# ---------------------------------------------------------------------------
# The preamble every generated replay script starts with. Python 3.6 compatible
# (RenderDoc's embedded interpreter) -- no walrus, no f-strings in emitted code
# paths we cannot test, no dataclasses.
#
# It opens the capture, flattens the action tree into ACTIONS, and prebuilds
# TEXINFO / BUFINFO so bodies can describe a resource id without re-querying.
# The body fills RESULT; failures land in the .done sentinel as a traceback.
#
# Substitution is done with str.replace on @@TOKEN@@ markers, NOT %-formatting:
# both this preamble and the tool bodies are full of literal '%' (every
# '%02x' % b and '%dx%d'), which %-formatting would try to interpret.
# ---------------------------------------------------------------------------
_PREAMBLE = r'''
import os, sys, json, struct, hashlib, traceback, re
import renderdoc as rd

CAPTURE = @@CAPTURE@@
OUT     = @@OUT@@
DONE    = @@DONE@@
ARGS    = json.loads(@@ARGS@@)

RESULT = {}

def _flat(acts, out):
    for a in acts:
        out.append(a)
        _flat(a.children, out)

def _desc(u):
    # RenderDoc >=1.something returns UsedDescriptor wrapping .descriptor;
    # older builds hand back the Descriptor directly.
    return getattr(u, 'descriptor', u)

def _slot(u):
    acc = getattr(u, 'access', None)
    return getattr(acc, 'index', None) if acc is not None else None

def _rid(x):
    try:
        return int(x)
    except Exception:
        return 0

def texdesc(rid):
    t = TEXINFO.get(rid)
    if t is None:
        b = BUFINFO.get(rid)
        if b is not None:
            return 'buffer[%d]' % b['len']
        return 'res%d' % rid
    s = '%dx%d %s' % (t['w'], t['h'], t['fmt'])
    if t['arr'] > 1:
        s += '[x%d]' % t['arr']
    if t['mips'] > 1:
        s += ' m%d' % t['mips']
    return s

STAGES = {
    'vs': rd.ShaderStage.Vertex,
    'ps': rd.ShaderStage.Pixel,
    'gs': rd.ShaderStage.Geometry,
    'hs': rd.ShaderStage.Hull,
    'ds': rd.ShaderStage.Domain,
    'cs': rd.ShaderStage.Compute,
}

try:
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAPTURE, '', None)
    if not res.OK():
        raise RuntimeError('OpenFile failed: ' + str(res))
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if not res.OK():
        raise RuntimeError('OpenCapture failed: ' + str(res))

    ACTIONS = []
    _flat(ctrl.GetRootActions(), ACTIONS)

    TEXINFO = {}
    for _t in ctrl.GetTextures():
        TEXINFO[int(_t.resourceId)] = {
            'w': _t.width, 'h': _t.height, 'd': _t.depth,
            'fmt': _t.format.Name(), 'mips': _t.mips, 'arr': _t.arraysize,
            'msaa': _t.msSamp, 'flags': str(_t.creationFlags),
        }
    BUFINFO = {}
    for _b in ctrl.GetBuffers():
        BUFINFO[int(_b.resourceId)] = {'len': _b.length, 'flags': str(_b.creationFlags)}

    DRAWS = [a for a in ACTIONS if a.flags & rd.ActionFlags.Drawcall]

@@BODY@@

    json.dump(RESULT, open(OUT, 'w'), indent=1, default=str)
    open(DONE, 'w').write('ok')
    ctrl.Shutdown()
    cap.Shutdown()
except Exception:
    open(DONE, 'w').write('EXC\n' + traceback.format_exc())
os._exit(0)
'''


def _indent(code: str, spaces: int = 4) -> str:
    pad = " " * spaces
    return "\n".join(pad + ln if ln.strip() else ln for ln in code.split("\n"))


def _resolve_capture(capture: Optional[str]) -> str:
    path = capture or _DEFAULT_CAPTURE
    if not path:
        raise ValueError(
            "no capture given. Pass capture='...rdc' or set RDMCP_CAPTURE."
        )
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError("capture not found: %s" % path)
    return str(p.resolve())


def _cache_key(capture: str, body: str, args: dict) -> str:
    st = Path(capture).stat()
    h = hashlib.sha1()
    h.update(capture.encode("utf-8", "replace"))
    h.update(str(st.st_mtime_ns).encode())
    h.update(str(st.st_size).encode())
    h.update(body.encode("utf-8", "replace"))
    h.update(json.dumps(args, sort_keys=True, default=str).encode())
    return h.hexdigest()[:20]


def _replay(
    capture: Optional[str],
    body: str,
    args: Optional[dict] = None,
    cache: bool = True,
    qrenderdoc: Optional[str] = None,
    timeout: Optional[float] = None,
) -> dict[str, Any]:
    """Run `body` inside RenderDoc's embedded interpreter against `capture`.

    `body` is Python 3.6 source that fills the dict `RESULT`. It may use ARGS,
    ctrl, ACTIONS, DRAWS, TEXINFO, BUFINFO, STAGES, texdesc(), _desc(), _slot().
    """
    cap_path = _resolve_capture(capture)
    args = args or {}
    qrd = qrenderdoc or _DEFAULT_QRD
    if not Path(qrd).exists():
        raise FileNotFoundError(
            "qrenderdoc.exe not found: %s. Set RDMCP_QRENDERDOC." % qrd
        )

    _WORKDIR.mkdir(parents=True, exist_ok=True)
    cachedir = _WORKDIR / "cache"
    cachedir.mkdir(exist_ok=True)

    key = _cache_key(cap_path, body, args)
    cached = cachedir / (key + ".json")
    if cache and cached.exists():
        try:
            out = json.loads(cached.read_text(encoding="utf-8"))
            out["_cached"] = True
            return out
        except Exception:
            cached.unlink(missing_ok=True)

    run = _WORKDIR / key
    run.mkdir(exist_ok=True)
    script = run / "replay.py"
    outp = run / "result.json"
    donep = run / "replay.done"
    for f in (outp, donep):
        if f.exists():
            f.unlink()

    src = _PREAMBLE
    for token, value in (
        ("@@CAPTURE@@", repr(cap_path)),
        ("@@OUT@@", repr(str(outp))),
        ("@@DONE@@", repr(str(donep))),
        ("@@ARGS@@", repr(json.dumps(args, default=str))),
        ("@@BODY@@", _indent(body)),
    ):
        src = src.replace(token, value)
    script.write_text(src, encoding="utf-8")

    try:
        subprocess.run([qrd, "--python", str(script)], timeout=timeout or _TIMEOUT,
                       capture_output=True)
    except subprocess.TimeoutExpired:
        raise TimeoutError("qrenderdoc replay timed out after %ss" % (timeout or _TIMEOUT))

    # qrenderdoc can return before the script finishes -- wait for the sentinel.
    deadline = time.time() + (timeout or _TIMEOUT)
    while time.time() < deadline and not donep.exists():
        time.sleep(0.2)
    if not donep.exists():
        raise TimeoutError(
            "replay produced no result (no .done sentinel). Script: %s" % script
        )

    status = donep.read_text(encoding="utf-8", errors="replace")
    if status.startswith("EXC"):
        raise RuntimeError("replay script failed:\n" + status)
    if not outp.exists():
        raise RuntimeError("replay reported ok but wrote no result: %s" % outp)

    out = json.loads(outp.read_text(encoding="utf-8"))
    if cache:
        try:
            cached.write_text(json.dumps(out), encoding="utf-8")
        except Exception:
            pass
    out["_cached"] = False
    return out


# ===========================================================================
# Tools
# ===========================================================================


@mcp.tool()
def rd_info(capture: Optional[str] = None) -> dict[str, Any]:
    """Capture overview: graphics API, GPU vendor, and action/draw/texture/buffer
    counts. Cheapest way to confirm a .rdc opens and replays before asking
    anything expensive of it."""
    body = r'''
props = ctrl.GetAPIProperties()
RESULT['api'] = {
    'pipelineType': str(props.pipelineType),
    'localRenderer': str(props.localRenderer),
    'vendor': str(props.vendor),
    'degraded': bool(getattr(props, 'degraded', False)),
}
RESULT['counts'] = {
    'actions': len(ACTIONS),
    'drawcalls': len(DRAWS),
    'textures': len(TEXINFO),
    'buffers': len(BUFINFO),
}
RESULT['eventRange'] = [ACTIONS[0].eventId, ACTIONS[-1].eventId] if ACTIONS else []
'''
    r = _replay(capture, body)
    r["ok"] = True
    return r


@mcp.tool()
def rd_passes(capture: Optional[str] = None, no_cache: bool = False) -> dict[str, Any]:
    """Reconstruct the frame's RENDER PASSES -- usually the first thing to run.

    Groups consecutive draws/clears by their (render targets, depth target,
    viewport) into passes, and reports for each: event range, draw/clear counts,
    the render targets and depth buffer with formats, a histogram of the vertex
    and pixel shaders used, and the blend/depth/cull/layout of a representative
    draw plus the textures it binds.

    This is what turns a flat list of draw calls into "prepass -> depth ->
    lighting -> material -> post", which is the structure you actually want."""
    body = r'''
rows = []
for a in ACTIONS:
    isdraw = bool(a.flags & rd.ActionFlags.Drawcall)
    isclear = bool(a.flags & rd.ActionFlags.Clear)
    if not (isdraw or isclear):
        continue
    ctrl.SetFrameEvent(a.eventId, True)
    p = ctrl.GetPipelineState()
    d = {'eid': a.eventId, 'kind': 'draw' if isdraw else 'clear',
         'nIdx': a.numIndices, 'nInst': a.numInstances}
    d['rts'] = [_rid(t.resource) for t in p.GetOutputTargets() if _rid(t.resource)]
    d['dsv'] = _rid(p.GetDepthTarget().resource)
    vp = p.GetViewport(0)
    d['vp'] = [round(vp.width, 1), round(vp.height, 1)]
    if isdraw:
        for k in ('vs', 'ps'):
            d[k] = _rid(p.GetShader(STAGES[k]))
        cbl = p.GetColorBlends()
        if len(cbl):
            b0 = cbl[0]
            d['blend'] = {'en': bool(b0.enabled),
                          'src': str(b0.colorBlend.source).split('.')[-1],
                          'dst': str(b0.colorBlend.destination).split('.')[-1],
                          'op': str(b0.colorBlend.operation).split('.')[-1],
                          'mask': b0.writeMask}
        dts = p.GetDepthTestState()
        d['depth'] = {'en': bool(dts.depthEnable), 'w': bool(dts.depthWrites),
                      'f': str(dts.depthFunction).split('.')[-1]}
        d['cull'] = str(p.GetRasterState().cullMode).split('.')[-1]
        d['layout'] = ['%s:%s%s' % (vi.name, vi.format.Name(),
                                    ':INST' if vi.perInstance else '')
                       for vi in p.GetVertexInputs()]
        ro = []
        try:
            for u in p.GetReadOnlyResources(rd.ShaderStage.Pixel):
                rid = _rid(_desc(u).resource)
                if rid:
                    ro.append([_slot(u), rid])
        except Exception:
            pass
        d['psRes'] = ro
    rows.append(d)

passes = []
cur = None
for d in rows:
    key = (tuple(d['rts']), d['dsv'], tuple(d['vp']))
    if cur is None or cur['_key'] != key:
        cur = {'_key': key, 'items': []}
        passes.append(cur)
    cur['items'].append(d)

out = []
for i, p in enumerate(passes):
    items = p['items']
    draws = [x for x in items if x['kind'] == 'draw']
    vsh = {}
    psh = {}
    for x in draws:
        vsh[x['vs']] = vsh.get(x['vs'], 0) + 1
        psh[x['ps']] = psh.get(x['ps'], 0) + 1
    top = lambda h: sorted(h.items(), key=lambda kv: -kv[1])[:8]
    e = {
        'pass': i,
        'eidStart': items[0]['eid'], 'eidEnd': items[-1]['eid'],
        'draws': len(draws), 'clears': len(items) - len(draws),
        'viewport': list(p['_key'][2]),
        'renderTargets': [{'id': r, 'desc': texdesc(r)} for r in p['_key'][0]],
        'depthTarget': ({'id': p['_key'][1], 'desc': texdesc(p['_key'][1])}
                        if p['_key'][1] else None),
        'uniqueVS': len(vsh), 'uniquePS': len(psh),
        'topVS': [{'id': k, 'draws': v} for k, v in top(vsh)],
        'topPS': [{'id': k, 'draws': v} for k, v in top(psh)],
    }
    if draws:
        x = draws[0]
        e['example'] = {
            'eid': x['eid'], 'vs': x['vs'], 'ps': x['ps'],
            'blend': x.get('blend'), 'depth': x.get('depth'), 'cull': x.get('cull'),
            'layout': x.get('layout'),
            'psTextures': [{'slot': s, 'id': r, 'desc': texdesc(r)}
                           for s, r in x.get('psRes', [])],
        }
        bl = {}
        for y in draws:
            b = y.get('blend') or {}
            k = '%s %s/%s mask%s' % ('ON' if b.get('en') else 'off',
                                     b.get('src'), b.get('dst'), b.get('mask'))
            bl[k] = bl.get(k, 0) + 1
        e['blendHistogram'] = bl
    out.append(e)
RESULT['passes'] = out
RESULT['totalDraws'] = sum(1 for r in rows if r['kind'] == 'draw')
'''
    r = _replay(capture, body, cache=not no_cache)
    r["ok"] = True
    return r


@mcp.tool()
def rd_actions(capture: Optional[str] = None, offset: int = 0, limit: int = 200,
               drawcalls_only: bool = True, name_contains: Optional[str] = None) -> dict[str, Any]:
    """Flat, paginated list of the frame's actions: eventId, name, flags, index
    and instance counts, and tree depth. Use `name_contains` to find Dispatch /
    Copy / marker events. Prefer rd_passes for structure; use this to locate a
    specific event id to hand to the other tools."""
    body = r'''
want_draw = ARGS['drawcalls_only']
sub = ARGS.get('name_contains') or ''
sf = ctrl.GetStructuredFile()

def depth_of(a, acts, d=0):
    return d

rows = []
def walk(acts, d):
    for a in acts:
        nm = a.GetName(sf)
        isdraw = bool(a.flags & rd.ActionFlags.Drawcall)
        keep = (isdraw or not want_draw)
        if keep and sub and sub.lower() not in nm.lower():
            keep = False
        if keep:
            rows.append({'eid': a.eventId, 'depth': d, 'name': nm,
                         'flags': str(a.flags), 'nIdx': a.numIndices,
                         'nInst': a.numInstances})
        walk(a.children, d + 1)
walk(ctrl.GetRootActions(), 0)

off = ARGS['offset']; lim = ARGS['limit']
RESULT['total'] = len(rows)
RESULT['offset'] = off
RESULT['actions'] = rows[off:off + lim]
'''
    r = _replay(capture, body, args={"offset": offset, "limit": limit,
                                     "drawcalls_only": drawcalls_only,
                                     "name_contains": name_contains})
    r["ok"] = True
    return r


@mcp.tool()
def rd_pipeline(capture: Optional[str] = None, eid: int = 0) -> dict[str, Any]:
    """Full pipeline state at one event: shader ids per stage, every bound
    read-only resource per stage (with slot + texture description), constant
    buffer bindings, colour blend, depth/stencil, rasterizer, vertex input
    layout, index buffer, viewport and scissor.

    Use this on a draw you care about, then rd_shader / rd_cbuffer to go deeper."""
    body = r'''
eid = ARGS['eid']
ctrl.SetFrameEvent(eid, True)
p = ctrl.GetPipelineState()

RESULT['eid'] = eid
sh = {}
for k in ('vs', 'hs', 'ds', 'gs', 'ps', 'cs'):
    sid = _rid(p.GetShader(STAGES[k]))
    if not sid:
        continue
    ent = {'id': sid, 'entry': p.GetShaderEntryPoint(STAGES[k])}
    refl = p.GetShaderReflection(STAGES[k])
    if refl is not None:
        ent['samplers'] = len(refl.samplers)
        ent['readOnlyResources'] = [rr.name for rr in refl.readOnlyResources]
        ent['inputs'] = ['%s%d' % (s.semanticName, s.semanticIndex)
                         for s in refl.inputSignature]
        ent['outputs'] = ['%s%d' % (s.semanticName, s.semanticIndex)
                          for s in refl.outputSignature]
    res = []
    try:
        for u in p.GetReadOnlyResources(STAGES[k]):
            rid = _rid(_desc(u).resource)
            if rid:
                res.append({'slot': _slot(u), 'id': rid, 'desc': texdesc(rid)})
    except Exception:
        pass
    ent['bound'] = res
    cbs = []
    try:
        for u in p.GetConstantBlocks(STAGES[k]):
            dd = _desc(u)
            rid = _rid(dd.resource)
            if rid:
                cbs.append({'slot': _slot(u), 'id': rid,
                            'byteOffset': getattr(dd, 'byteOffset', 0),
                            'byteSize': getattr(dd, 'byteSize', 0)})
    except Exception:
        pass
    ent['constantBuffers'] = cbs
    sh[k] = ent
RESULT['stages'] = sh

RESULT['outputTargets'] = [{'id': _rid(t.resource), 'desc': texdesc(_rid(t.resource))}
                           for t in p.GetOutputTargets() if _rid(t.resource)]
dt = _rid(p.GetDepthTarget().resource)
RESULT['depthTarget'] = {'id': dt, 'desc': texdesc(dt)} if dt else None

cbl = p.GetColorBlends()
RESULT['blends'] = [{
    'enabled': bool(b.enabled),
    'colorSrc': str(b.colorBlend.source), 'colorDst': str(b.colorBlend.destination),
    'colorOp': str(b.colorBlend.operation),
    'alphaSrc': str(b.alphaBlend.source), 'alphaDst': str(b.alphaBlend.destination),
    'alphaOp': str(b.alphaBlend.operation),
    'writeMask': b.writeMask,
} for b in cbl[:4]]
RESULT['blendFactor'] = list(p.GetBlendFactor())

dts = p.GetDepthTestState()
RESULT['depthState'] = {'enable': bool(dts.depthEnable), 'write': bool(dts.depthWrites),
                        'func': str(dts.depthFunction)}
rs = p.GetRasterState()
# Field set varies between the API-agnostic RasterState and the per-API ones
# (e.g. no depthBias here on current builds), so probe rather than assume.
RESULT['rasterState'] = {'cull': str(rs.cullMode), 'fill': str(rs.fillMode)}
for _f in ('frontCCW', 'depthBias', 'slopeScaledDepthBias', 'depthBiasClamp',
           'depthClip', 'scissorEnable', 'multisampleEnable', 'antialiasedLines',
           'lineRasterMode', 'conservativeRasterization', 'forcedSampleCount'):
    if hasattr(rs, _f):
        _v = getattr(rs, _f)
        RESULT['rasterState'][_f] = _v if isinstance(_v, (int, float)) else str(_v)
RESULT['topology'] = str(p.GetPrimitiveTopology())
RESULT['vertexInputs'] = [{'name': vi.name, 'format': vi.format.Name(),
                           'byteOffset': vi.byteOffset, 'perInstance': bool(vi.perInstance),
                           'used': bool(vi.used)}
                          for vi in p.GetVertexInputs()]
vp = p.GetViewport(0)
RESULT['viewport'] = {'x': vp.x, 'y': vp.y, 'width': vp.width, 'height': vp.height,
                      'minDepth': vp.minDepth, 'maxDepth': vp.maxDepth}
'''
    r = _replay(capture, body, args={"eid": eid})
    r["ok"] = True
    return r


@mcp.tool()
def rd_shader(capture: Optional[str] = None, eid: int = 0, stage: str = "ps",
              target: Optional[str] = None, disassemble: bool = True) -> dict[str, Any]:
    """Disassembly + reflection for the shader bound at `eid` on `stage`
    (vs/hs/ds/gs/ps/cs).

    Returns the disassembly text plus entry point, input/output signatures,
    sampler and read-only resource names, constant-block layout, bytecode
    length, a sha1 of the bytecode and the embedded DXBC hash -- the last two
    let you match a captured shader against shaders extracted from a game's
    files or executable.

    `target` picks the disassembly flavour (default the first available, e.g.
    "DXBC"); rd_disassembly_targets lists them."""
    body = r'''
eid = ARGS['eid']
stg = STAGES[ARGS['stage']]
ctrl.SetFrameEvent(eid, True)
p = ctrl.GetPipelineState()
refl = p.GetShaderReflection(stg)
sid = _rid(p.GetShader(stg))
RESULT['eid'] = eid
RESULT['stage'] = ARGS['stage']
RESULT['shaderId'] = sid
if refl is None:
    RESULT['error'] = 'no shader bound on this stage at this event'
else:
    RESULT['entry'] = refl.entryPoint
    RESULT['inputs'] = ['%s%d' % (s.semanticName, s.semanticIndex)
                        for s in refl.inputSignature]
    RESULT['outputs'] = ['%s%d' % (s.semanticName, s.semanticIndex)
                         for s in refl.outputSignature]
    RESULT['samplers'] = len(refl.samplers)
    RESULT['readOnlyResources'] = [
        {'name': rr.name, 'bind': getattr(rr, 'fixedBindNumber', None)}
        for rr in refl.readOnlyResources]
    RESULT['readWriteResources'] = [rr.name for rr in refl.readWriteResources]
    RESULT['constantBlocks'] = [
        {'name': cb.name, 'bind': getattr(cb, 'fixedBindNumber', None),
         'byteSize': cb.byteSize, 'numVars': len(cb.variables),
         'vars': [v.name for v in cb.variables][:128]}
        for cb in refl.constantBlocks]
    raw = bytes(refl.rawBytes)
    RESULT['bytecodeLen'] = len(raw)
    RESULT['sha1'] = hashlib.sha1(raw).hexdigest()
    if len(raw) >= 20:
        RESULT['dxbcHash'] = ''.join('%02x' % b for b in raw[4:20])
    targets = ctrl.GetDisassemblyTargets(True)
    RESULT['availableTargets'] = [str(t) for t in targets]
    if ARGS['disassemble'] and targets:
        want = ARGS.get('target')
        tgt = targets[0]
        if want:
            for t in targets:
                if str(t) == want:
                    tgt = t
                    break
        RESULT['target'] = str(tgt)
        RESULT['disassembly'] = ctrl.DisassembleShader(
            p.GetGraphicsPipelineObject(), refl, tgt)
'''
    r = _replay(capture, body, args={"eid": eid, "stage": stage, "target": target,
                                     "disassemble": disassemble})
    r["ok"] = True
    return r


@mcp.tool()
def rd_shaders_dump(capture: Optional[str] = None, pattern: Optional[str] = None,
                    stage: Optional[str] = None, out_dir: Optional[str] = None,
                    context: int = 2, no_cache: bool = False) -> dict[str, Any]:
    """Disassemble EVERY unique shader in the frame, optionally grepping them.

    Writes one .txt per shader to `out_dir` (default a folder next to the
    capture) and returns an index of {shaderId, stage, firstEid, bound texture
    names, bytecode sha1}. With `pattern` (a regex) it also returns matching
    lines with surrounding context and a per-shader match count.

    This is how you answer frame-wide questions -- "how many pixel shaders
    discard?", "which shaders reconstruct a normal with sqrt?", "who samples
    slot 14?" -- without opening the GUI. Slow on big frames; cached."""
    body = r'''
import os as _os
outdir = ARGS.get('out_dir') or (_os.path.splitext(CAPTURE)[0] + '_shaders')
if not _os.path.isdir(outdir):
    _os.makedirs(outdir)
pat = ARGS.get('pattern')
rx = re.compile(pat) if pat else None
only = ARGS.get('stage')
ctxn = ARGS.get('context', 2)

targets = ctrl.GetDisassemblyTargets(True)
tgt = targets[0] if targets else None

seen = {}
index = []
matches = []
for a in DRAWS:
    ctrl.SetFrameEvent(a.eventId, True)
    p = ctrl.GetPipelineState()
    for k in ('vs', 'hs', 'ds', 'gs', 'ps'):
        if only and k != only:
            continue
        sid = _rid(p.GetShader(STAGES[k]))
        if not sid or sid in seen:
            continue
        seen[sid] = True
        refl = p.GetShaderReflection(STAGES[k])
        if refl is None or tgt is None:
            continue
        txt = ctrl.DisassembleShader(p.GetGraphicsPipelineObject(), refl, tgt)
        fn = '%s_%d.txt' % (k, sid)
        try:
            open(_os.path.join(outdir, fn), 'w').write(txt)
        except Exception:
            pass
        raw = bytes(refl.rawBytes)
        rec = {'id': sid, 'stage': k, 'firstEid': a.eventId, 'file': fn,
               'entry': refl.entryPoint,
               'textures': [rr.name for rr in refl.readOnlyResources],
               'bytecodeLen': len(raw),
               'sha1': hashlib.sha1(raw).hexdigest(),
               'instrCount': len(re.findall(r'(?m)^\s*\d+:', txt))}
        if rx is not None:
            lines = txt.split('\n')
            hits = []
            for i, ln in enumerate(lines):
                if rx.search(ln):
                    lo = max(0, i - ctxn); hi = min(len(lines), i + ctxn + 1)
                    hits.append({'line': i + 1, 'text': ln.strip(),
                                 'context': [l.rstrip() for l in lines[lo:hi]]})
            rec['matchCount'] = len(hits)
            if hits:
                matches.append({'id': sid, 'stage': k, 'file': fn, 'hits': hits[:20]})
        index.append(rec)

RESULT['outDir'] = outdir
RESULT['shaders'] = index
RESULT['totalShaders'] = len(index)
if rx is not None:
    RESULT['pattern'] = pat
    RESULT['shadersWithMatch'] = len(matches)
    RESULT['matches'] = matches[:80]
'''
    r = _replay(capture, body,
                args={"pattern": pattern, "stage": stage, "out_dir": out_dir,
                      "context": context},
                cache=not no_cache)
    r["ok"] = True
    return r


@mcp.tool()
def rd_cbuffer(capture: Optional[str] = None, eid: int = 0, stage: str = "ps",
               slot: Optional[int] = None, max_rows: int = 64) -> dict[str, Any]:
    """Read back the ACTUAL constant-buffer contents bound at `eid` for `stage`.

    Returns each bound buffer's bytes decoded as float4 rows (cb[0], cb[1], ...)
    plus the same words reinterpreted as int32, and -- when the shader has
    reflection for it -- the variable names in the block.

    Ground truth for "what value is the engine really feeding this shader",
    which is usually the fastest way to settle a question that disassembly alone
    leaves ambiguous."""
    body = r'''
eid = ARGS['eid']
stg = STAGES[ARGS['stage']]
want = ARGS.get('slot')
maxr = ARGS['max_rows']
ctrl.SetFrameEvent(eid, True)
p = ctrl.GetPipelineState()

names = {}
refl = p.GetShaderReflection(stg)
if refl is not None:
    for cb in refl.constantBlocks:
        names[getattr(cb, 'fixedBindNumber', None)] = {
            'name': cb.name, 'byteSize': cb.byteSize,
            'vars': [v.name for v in cb.variables][:128]}

blocks = []
for u in p.GetConstantBlocks(stg):
    dd = _desc(u)
    rid = dd.resource
    if not _rid(rid):
        continue
    sl = _slot(u)
    if want is not None and sl != want:
        continue
    off = getattr(dd, 'byteOffset', 0)
    sz = getattr(dd, 'byteSize', 0)
    data = bytes(ctrl.GetBufferData(rid, off, sz if sz else 0))
    nf = len(data) // 4
    fl = struct.unpack('<%df' % nf, data[:nf * 4]) if nf else ()
    iv = struct.unpack('<%di' % nf, data[:nf * 4]) if nf else ()
    rows = []
    for i in range(0, min(len(fl), maxr * 4), 4):
        rows.append({'reg': i // 4, 'byteOffset': i * 4,
                     'float4': [round(x, 6) for x in fl[i:i + 4]],
                     'int4': list(iv[i:i + 4])})
    blocks.append({'slot': sl, 'bufferId': _rid(rid), 'byteOffset': off,
                   'byteSize': len(data), 'reflection': names.get(sl),
                   'rows': rows})
RESULT['eid'] = eid
RESULT['stage'] = ARGS['stage']
RESULT['blocks'] = blocks
'''
    r = _replay(capture, body, args={"eid": eid, "stage": stage, "slot": slot,
                                     "max_rows": max_rows})
    r["ok"] = True
    return r


@mcp.tool()
def rd_textures(capture: Optional[str] = None, min_width: int = 0,
                format_contains: Optional[str] = None, render_targets_only: bool = False,
                offset: int = 0, limit: int = 100) -> dict[str, Any]:
    """List the capture's textures (id, dimensions, format, mips, array size,
    MSAA, creation flags), with optional filters. Use it to find a render target
    by size/format, or to get a resource id for rd_save_texture."""
    body = r'''
mw = ARGS['min_width']; fc = (ARGS.get('format_contains') or '').lower()
rt_only = ARGS['render_targets_only']
rows = []
for rid, t in TEXINFO.items():
    if t['w'] < mw:
        continue
    if fc and fc not in t['fmt'].lower():
        continue
    if rt_only and 'RenderTarget' not in t['flags'] and 'DepthTarget' not in t['flags']:
        continue
    r = dict(t)
    r['id'] = rid
    rows.append(r)
rows.sort(key=lambda r: (-(r['w'] * r['h']), r['id']))
RESULT['total'] = len(rows)
RESULT['textures'] = rows[ARGS['offset']:ARGS['offset'] + ARGS['limit']]
'''
    r = _replay(capture, body,
                args={"min_width": min_width, "format_contains": format_contains,
                      "render_targets_only": render_targets_only,
                      "offset": offset, "limit": limit})
    r["ok"] = True
    return r


@mcp.tool()
def rd_save_texture(out_path: str, capture: Optional[str] = None,
                    resource_id: Optional[int] = None, eid: Optional[int] = None,
                    stage: str = "ps", slot: int = 0, mip: int = 0,
                    file_type: str = "PNG") -> dict[str, Any]:
    """Save a texture to an image file so it can actually be looked at.

    Either name the texture directly with `resource_id`, or point at what a draw
    had bound: `eid` + `stage` + `slot`. Alpha is preserved, so this is also how
    you inspect a mask/coverage channel. Returns the resolved id and description.
    """
    body = r'''
rid = None
if ARGS.get('resource_id'):
    for t in ctrl.GetTextures():
        if int(t.resourceId) == ARGS['resource_id']:
            rid = t.resourceId
            break
    if rid is None:
        raise RuntimeError('no texture with id %s' % ARGS['resource_id'])
else:
    if ARGS.get('eid') is None:
        raise RuntimeError('give resource_id, or eid (+stage/slot)')
    ctrl.SetFrameEvent(ARGS['eid'], True)
    p = ctrl.GetPipelineState()
    for u in p.GetReadOnlyResources(STAGES[ARGS['stage']]):
        if _slot(u) == ARGS['slot']:
            rid = _desc(u).resource
            break
    if rid is None or not _rid(rid):
        # fall back to positional order when slot indices are unavailable
        ro = [u for u in p.GetReadOnlyResources(STAGES[ARGS['stage']])
              if _rid(_desc(u).resource)]
        if ARGS['slot'] < len(ro):
            rid = _desc(ro[ARGS['slot']]).resource
    if rid is None or not _rid(rid):
        raise RuntimeError('nothing bound at %s slot %d for eid %s'
                           % (ARGS['stage'], ARGS['slot'], ARGS['eid']))

sav = rd.TextureSave()
sav.resourceId = rid
sav.mip = ARGS['mip']
sav.alpha = rd.AlphaMapping.Preserve
ft = ARGS['file_type'].upper()
sav.destType = getattr(rd.FileType, ft, rd.FileType.PNG)
ok = ctrl.SaveTexture(sav, ARGS['out_path'])
RESULT['saved'] = bool(ok)
RESULT['resourceId'] = _rid(rid)
RESULT['desc'] = texdesc(_rid(rid))
RESULT['outPath'] = ARGS['out_path']
'''
    r = _replay(capture, body,
                args={"resource_id": resource_id, "eid": eid, "stage": stage,
                      "slot": slot, "mip": mip, "file_type": file_type,
                      "out_path": str(Path(out_path).resolve())},
                cache=False)
    r["ok"] = bool(r.get("saved"))
    return r


@mcp.tool()
def rd_find_draws(capture: Optional[str] = None, shader_id: Optional[int] = None,
                  texture_id: Optional[int] = None, blend_enabled: Optional[bool] = None,
                  min_indices: int = 0, limit: int = 100) -> dict[str, Any]:
    """Find the draws matching a filter: those using a given shader id, those
    binding a given texture id anywhere in the pixel stage, those with blending
    on/off, and/or those above an index-count threshold.

    Answers "where is this shader actually used" and "who draws with this
    texture", which is tedious to do by hand in the GUI."""
    body = r'''
sid = ARGS.get('shader_id'); tid = ARGS.get('texture_id')
be = ARGS.get('blend_enabled'); mi = ARGS['min_indices']
hits = []
for a in DRAWS:
    if a.numIndices < mi:
        continue
    ctrl.SetFrameEvent(a.eventId, True)
    p = ctrl.GetPipelineState()
    ids = {}
    for k in ('vs', 'hs', 'ds', 'gs', 'ps'):
        ids[k] = _rid(p.GetShader(STAGES[k]))
    if sid is not None and sid not in ids.values():
        continue
    binds = []
    for k in ('vs', 'ps'):
        try:
            for u in p.GetReadOnlyResources(STAGES[k]):
                rr = _rid(_desc(u).resource)
                if rr:
                    binds.append([k, _slot(u), rr])
        except Exception:
            pass
    if tid is not None and not any(b[2] == tid for b in binds):
        continue
    cbl = p.GetColorBlends()
    en = bool(cbl[0].enabled) if len(cbl) else False
    if be is not None and en != be:
        continue
    hits.append({'eid': a.eventId, 'nIdx': a.numIndices, 'nInst': a.numInstances,
                 'shaders': {k: v for k, v in ids.items() if v},
                 'blendEnabled': en,
                 'bound': [{'stage': b[0], 'slot': b[1], 'id': b[2],
                            'desc': texdesc(b[2])} for b in binds]})
    if len(hits) >= ARGS['limit']:
        break
RESULT['matches'] = hits
RESULT['count'] = len(hits)
'''
    r = _replay(capture, body,
                args={"shader_id": shader_id, "texture_id": texture_id,
                      "blend_enabled": blend_enabled, "min_indices": min_indices,
                      "limit": limit})
    r["ok"] = True
    return r


@mcp.tool()
def rd_exec(code: str, capture: Optional[str] = None, args: Optional[dict] = None,
            timeout: Optional[float] = None) -> dict[str, Any]:
    """Escape hatch: run arbitrary Python inside RenderDoc's embedded interpreter
    against the capture, for anything the other tools do not cover.

    The code must be **Python 3.6** compatible and fill the dict `RESULT` (only
    JSON-serialisable values survive). Available to it: `rd` (the renderdoc
    module), `ctrl` (ReplayController), `ACTIONS` / `DRAWS` (flattened action
    lists), `TEXINFO` / `BUFINFO`, `STAGES` (name -> ShaderStage), `ARGS` (your
    `args` dict), and helpers `texdesc(id)`, `_desc(u)`, `_slot(u)`, `_rid(x)`.

    Note the pipeline state must be read through the API-agnostic
    `ctrl.GetPipelineState()`; on current builds the D3D11-specific object has no
    `.srvs` and resources come from the descriptor API instead."""
    r = _replay(capture, code, args=args or {}, cache=False, timeout=timeout)
    r["ok"] = True
    return r


@mcp.tool()
def rd_disassembly_targets(capture: Optional[str] = None) -> dict[str, Any]:
    """List the disassembly flavours this RenderDoc build can produce for the
    capture's API (e.g. DXBC, AMDIL, and the GCN/RDNA ISA variants). Pass one as
    `target` to rd_shader to see real hardware ISA instead of bytecode."""
    body = r'''
RESULT['targets'] = [str(t) for t in ctrl.GetDisassemblyTargets(True)]
'''
    r = _replay(capture, body)
    r["ok"] = True
    return r


@mcp.tool()
def rd_config() -> dict[str, Any]:
    """Show how this server is configured and whether the pieces it needs are
    actually present -- qrenderdoc.exe path, default capture, scratch dir,
    timeout. Check here first if a tool call fails."""
    return {
        "ok": True,
        "qrenderdoc": _DEFAULT_QRD,
        "qrenderdocExists": Path(_DEFAULT_QRD).exists(),
        "defaultCapture": _DEFAULT_CAPTURE or None,
        "defaultCaptureExists": bool(_DEFAULT_CAPTURE) and Path(_DEFAULT_CAPTURE).exists(),
        "workdir": str(_WORKDIR),
        "timeoutSeconds": _TIMEOUT,
        "env": {
            "RDMCP_QRENDERDOC": "path to qrenderdoc.exe",
            "RDMCP_CAPTURE": "default .rdc so tools can omit capture=",
            "RDMCP_WORKDIR": "scratch dir for scripts/results/cache",
            "RDMCP_TIMEOUT": "per-replay timeout in seconds",
        },
    }


@mcp.tool()
def rd_clear_cache() -> dict[str, Any]:
    """Drop cached replay results. Results are keyed by capture path + mtime +
    size + query, so a re-captured .rdc invalidates itself; use this only to
    reclaim disk or to force a re-run."""
    cachedir = _WORKDIR / "cache"
    n = 0
    if cachedir.exists():
        for f in cachedir.glob("*.json"):
            try:
                f.unlink()
                n += 1
            except Exception:
                pass
    return {"ok": True, "removed": n, "cacheDir": str(cachedir)}


if __name__ == "__main__":
    mcp.run()
