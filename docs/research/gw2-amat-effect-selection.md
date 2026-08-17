---
name: gw2-amat-effect-selection
description: "RE'd AMAT effect selection + render-state composition (BgfxShader.cpp/BgfxDraw.cpp) — technique=quality, effect keyed by token64 (fallback 183330), shaderPassFlags = write-mask/depth bits, blend comes ONLY from effect.renderState"
metadata: 
  node_type: memory
  type: project
  originSessionId: c3f2e769-8382-45c5-a657-2ccf20d77fbd
  modified: 2026-07-29T06:57:26.812Z
---

> **Superseded in part by [[gw2-amat-draw-state]] (2026-08-16).** Every address
> below has rotted; the re-anchored ones are in that note. Two findings here are
> also wrong: `shaderPassFlags` bit 0 is the **cull mode**, not an opaque marker,
> and the default token `183330` is accepted on **pass 0 only**. The token chain,
> the write-mask rule and the "blend comes only from `effect.renderState`"
> conclusion all still hold.

How GW2 turns an AMAT into a draw (IDA, Gw2-64-disabled-aslr.exe):

**Effect selection — `sub_140BFAD20` (BgfxShader.cpp ~1069-1200)**
`select(program, ctx, passIdx, materialToken64, meshFlags16, vsVariantId, out)`
- `techniques[]` are **quality levels**, not passes: `quality` token32 decodes
  (base-23, `-0x30000000`) to `high` / `medium` / `low`. Picked by
  `m_techniqueIndex` on the material instance.
- Inside `techniques[q].passes[p]`, the effect is looked up **by its `token`**
  (`sub_140BF4630(pass, token, &idx)`), NOT by sampler count / SH-uniform
  heuristics. Hardcoded fallback chain:
  `0x1481311EC -> 0x51C59061370654`, `0x1779028991EC -> 0x5DE40A268A40A4`,
  `0x2C26C0B64F711EC -> 0x6584D816C9EE`, then **default token `183330`**.
- vertex variant id must match `vertexShaderVariants[i].variantId`; caller
  computes it from mesh flags: 0=static, 1, 2=skinned(0x80), 3=instanced,
  4=skinned+instanced. Fallback `sub_140BFBA70(variant==4?2:0, ...)`.

**Render state — `sub_140AAFDB0` (BgfxDraw.cpp, mesh draw loop)**
`state = depthState(sub_140AB3E10) | ptBits | effect.renderState | eqBits | writeMask`
- `out+464 |= effectData->renderState` **unconditionally** — the AMAT effect's
  bgfx word is the ONLY source of blend + ALPHA_REF (bits 40-47). MODL
  `sortOrder`/`sortLayer` never gates blending; it only orders draws.
- `out+464 |= effectRef.cullState` (0x1000000000 / 0x2000000000 = CULL_CW/CCW),
  **skipped when materialFlags & 0x4000** (two-sided). It is the *material* word
  `*(mesh->material + 28)`, not the mesh's: `BgfxDraw_MeshDrawLoop` passes it to
  `BgfxShader_SelectEffect` as `a5`, which guards the OR with `(a5 & 0x4000) == 0`.
  An earlier draft of this line said `meshFlags`; that is wrong, and it matters,
  because the two words are read at different offsets and neither is the MODL
  file field of the same name. Under that guard the client also swaps CW/CCW when
  the mirrored-view global at `+1372836` is set.
- one draw per pass index in `[surface+84, surface+88)`.

**`shaderPassFlags` (effectData+16, mirrored to out+452) — the real meaning**
| bit | effect |
|---|---|
| 0x0004 | colour write mask = 0 (depth-only / prepass) — never use as colour shader |
| 0x0008 | no RGB write |
| 0x0020 | disable depth test |
| 0x0040 | **disable depth write** (translucent) |
| 0x4000 | **no ALPHA write** -> D3D write mask 7 |
| 0x8000 | depth bias -65 (decal push) |
| 0x40000 | with instancing, ORs 0x2000000000000 |

write mask = `(flags&4) ? 0 : ((noRGB?0:7) | (noA?0:8))`, i.e. plain 15 when
flags is just 0x1. Mesh flags 0x800/0x400 do the same as 0x8/0x4000.

**Depth — `sub_140AB3E10`**: pass 0 => DEPTH_TEST_LEQUAL (32); later passes =>
DEPTH_TEST_EQUAL (48) against the prepass depth; surface flag 0x200 => GREATER
(80); WRITE_Z = bit 38.

**`grltbf` (LightBuffer) literal**: `xmmword_14209D9A0` in
ColorDeferredRenderPass = **(0.25, 4.0, 1/128, 128)** — confirms
[[gw2-engine-uniform-values]].

`sub_140B76180` = `bgfx::Encoder::setState`; mask in the D3D11 blend-state
builder `sub_140B744E0` is `0xFFFFFF00F` (write RGBA bits 0-3 + blend/equation
bits 12-35), so write mask really is `state & 0xF`.

See [[gw2-amat-shader-roles]], [[gw2-frame-pass-order]], [[castlemist-transparency-bug]].
