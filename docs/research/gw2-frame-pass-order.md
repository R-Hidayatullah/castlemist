---
name: gw2-frame-pass-order
description: "GW2 renders each model TWICE (normal prepass + material pass) linked by a screen-space light buffer at t14; opaque materials are alpha-CUTOUT at 0.25, never alpha-blended"
metadata: 
  node_type: memory
  type: project
  originSessionId: 30331272-20cd-4f25-844f-a6e181e4069f
  modified: 2026-07-28T22:40:58.675Z
---

Reverse-engineered from `sample1.rdc` (D3D11 capture, 550 actions, 161 shaders)
replayed via `qrenderdoc.exe --python` (see [[renderdoc-python-access]]).

**Frame pass order** (1344x756 internal, 1920x1080 output):

1. **Normal G-buffer prepass** — 156 draws. PS normalizes the interpolated vertex
   normal and writes `normal*0.5+0.5` into a B8G8R8A8 RT + depth. No normal map is
   sampled anywhere; per-pixel normals are vertex normals only.
2. **Linearize depth** — 1 fullscreen draw, depth -> R32_FLOAT.
3. **SSAO / light accumulation** — 2 fullscreen draws; reads normal buffer +
   linear depth + a 4x4 noise + 1024x1024 R32G32; writes the screen-space light buffer.
4. **Material pass** — 207 draws. Each model is drawn **again** with its real
   material shader, which samples albedo, then modulates by the light buffer bound
   at **t14**: `uv = (SV_Position.xy + cb0[4].x) * cb0[2].xy`, `color *= light * cb0[0].y`,
   then vertex fog `o0.rgb = color*(1-v2.w) + v2.rgb`.
5. Post-process/upscale to 1920x1080, then UI (512x512), then final composite.

So prepass and material pass draw the same geometry (verified: all 156 prepass
index-counts appear in the material pass) and are linked only by that light buffer.

**Alpha rule — the important one.** Opaque GW2 materials are **cutout, never
alpha-blended**:

- 50 of 87 pixel shaders contain `discard`. The idiom is always
  `add_sat a,a,a ; add a,-0.5 ; lt a,0 ; discard_nz` == `clip(saturate(2a) - 0.5)`,
  i.e. **discard where diffuse alpha < 0.25**. The prepass applies the SAME test
  (it samples `t0.wxyz` -> alpha only) so cutouts punch real holes in the normal buffer.
- Output alpha is a **constant**: 32 shaders `mov o0.w, cb0[..]`, 12 a literal; only
  8 compute a real opacity. Measured cb value is ~0 or 5/255 — it is a submodel/stencil
  **id** channel, not coverage.
- Of 207 material draws only 38 blend, and **every** blended draw uses
  writeMask 7 (RGB only) so it cannot touch that id channel.

Measured material cbuffer (constant across shaders): `cb0[0] = (0.25, 4.0, 1/128, 128)`
— 0.25 is the alpha-test ref, 4.0 multiplies the light buffer; `cb0[2].xy = (1/w, 1/h)`.

**Applied to castlemist** (see [[castlemist-app]]): added the cutout to both `kModel`
and `kNormalGBuffer`, gated by a CPU-side `hasCutout` flag (fraction of texels with
a<64 in 0.005..0.95) — required, because plenty of opaque materials ship a diffuse
whose alpha is uniformly 0 and would otherwise be erased. Verified against the six
t0 textures the game actually alpha-tests in the capture: 3 are real masks
(5.9%/43.8%/75.5% below threshold), 3 are fully opaque (0.000%) — the gate agrees
with the game's effective result on all six. Also set `AlphaRef` to 0.25 (was 0) and
made blend states RGB-only.

NOT changed: `LightBuffer` uniform is 0.8 in castlemist vs the game's 4.0, because
castlemist's reconstructed light buffer has a different scale than the game's HDR one.
