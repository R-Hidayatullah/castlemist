---
name: castlemist-transparency-bug
description: "castlemist's transparency failures and their two distinct root causes — the dead sortLayer gate (fixed) and the depth-test-less ground grid + StencilId-alpha blending (fixed 2026-07-29)"
metadata:
  node_type: memory
  type: project
  originSessionId: c3f2e769-8382-45c5-a657-2ccf20d77fbd
  modified: 2026-07-29T13:32:50.982Z
---

Two separate bugs, both diagnosed on model **771205** / **1925381**, both fixed.

## 1. The sortLayer gate stripped every blend (fixed earlier, 2026-07-29)

`game_shader.cpp` had `useTrans = set.hasTrans && m.sortLayer > 0`, then
`if (!useTrans) out.renderState &= ~0x0FFFF000ull`. **MODL v65 has no `sortLayer`
field — it is `sortOrder`** — so the gate was always false and every material drew
opaque. The engine never consults it either: the draw path ORs
`effectData->renderState` in unconditionally (sub_140BFAD20). Gate deleted; the
effect's own bgfx word is authoritative. See [[gw2-amat-effect-selection]].

## 2. "Everything looks see-through" (fixed 2026-07-29, verified by screenshot)

Two independent causes, both since fixed:

**(a) The ground grid was drawn with the depth test OFF.** `draw_gizmo` bound
`g_dssNoDepth` for the whole overlay, so faint grid lines painted straight across
solid geometry — which reads as the model being transparent. This was the visible
symptom the user reported. Fix: `build_gizmo_verts` emits the grid FIRST and
reports its vertex count; `draw_gizmo` draws the grid with `g_dssNoWrite`
(depth-tested, no depth write) and only the gizmo HANDLES with `g_dssNoDepth`.

That only works because the grid now shares the model's projection. It didn't:
`render()`/`compute_giz_mvp` used `0.02r..40r` while `render_game` /
`render_light_prepass` used a tight bracket, so grid depth and model depth were
not comparable. All four now call one **`main_camera()`** in `detail/state.h`
(far plane widened 3r -> 6r so the grid, which reaches ~4.3r diagonally, still
clears it).

**(b) Blending a pixel shader whose alpha is a StencilId.** An AMAT effect can
declare SRC_ALPHA/INV_SRC_ALPHA (`renderState 0x…6565000`) over a shader that ends
`mov o0.w, cb0[k].x` — the engine's submodel **id**, not an opacity. Blending that
multiplies the whole surface by a near-zero fraction. Ground truth from
`sample1.rdc` material pass, PS 17726 at eid 1650: instruction 33 is exactly
`mov o0.w, cb0[1].x` and the game draws it **blend disabled, mask 15**.
Fix: `gw2model.hpp::dxbcAlphaIsConstant()` walks the SHEX token stream — every
write to `o#.w` must be a `mov` from a constant buffer or an immediate — and
`extractAmat` strips the blend nibbles (`kBgfxBlendMask`) when such a shader is
paired with a blend that reads source alpha (`blendReadsSrcAlpha`: factors 5, 6,
11). Premultiplied blends like `0x4242000` (ONE/INV_SRC_COLOR) are untouched —
they do not read alpha.

### Calibration target
The real material pass in `sample1.rdc` (207 draws) is
**82% blend-disabled**: `One/Zero mask15` x156, `mask7` x6, `mask8` x7, and 38
blended. A 156-AMAT sample of the extractor now gives **124 opaque / 31 blended /
1 with no shader** — 20% blended vs the game's 18%. Use these numbers to check any
future change to effect selection.

### Related renderer fixes in the same pass
- Opaque game draws now bind a per-material `opaqueBlend`
  (`make_opaque_blend_state`) carrying that effect's own `game_write_mask`,
  instead of one shared mask-15 state — matching the game's mask 15/7/8 mix.
- A submesh whose material yields **no game shader** is no longer skipped (it used
  to render as a hole / invisible model). `render_game` draws it with the
  reconstruction VS/PS + `MaterialGPU` first, with `hasSkin = 0` because `vbUse`
  is already CPU-skinned.

### Template coverage (checked via `idx_chunk_variants`)
Every model/shader chunk version resolves to a struct: MODL v65–v70, GEOM v1,
ANIM v24/25, COLL, PRPS, SKEL v1, AMAT BGFX v1/v3, GRMT v6, DX9S v11. 156/156
sampled AMATs parse. The only empty variants are `mapc` `env.`/`trn.` (hand-rolled
parsers) and a handful of SKEL/GAME entries.

### Build note
`castlemist/build/` is stuck in a ninja "manifest still dirty after 100 tries"
regeneration loop (clock skew). **Build in `castlemist/build2/`** and copy the exe
up; that is what the verified binary came from.
