---
name: gw2-amat-shader-roles
description: An AMAT holds every frame-pass shader (normal prepass / depth / colour); picking the prepass is what renders models flat green — how to tell them apart
metadata: 
  node_type: memory
  type: project
  originSessionId: 8359fe52-6089-4655-b01c-f68be06b878e
  modified: 2026-07-29T04:30:13.861Z
---

> **The heuristics below are a fallback, not the mechanism ([[gw2-amat-draw-state]],
> 2026-08-16).** The engine picks its effect by **token64** with a fixed remap
> chain and a pass-0-only default — no content inspection at all. Detecting a
> prepass by its `0.5` immediates or by the SH uniform block is still useful for
> surveying an archive, and remains the right answer when a material's token is
> unknown, but a renderer that has the MODL material's token should use it.

An AMAT's `shaders[]` holds **every shader the engine binds for that material across
the whole frame**, not just the one that paints it: the deferred normal G-buffer
prepass, depth-only and depth-peel passes, and the real colour pass. Choosing the
wrong one is the root cause of "model renders flat green/yellow" — the prepass
outputs `normal*0.5+0.5` as RGB.

**How to tell them apart** (validated over 40 AMATs / 999 pixel shaders):

- **Normal prepass**: its final instruction is
  `mad o0.xyz, r, l(0.5,0.5,0.5,0), l(0.5,0.5,0.5,0)`. Detect from raw DXBC without a
  disassembler: two copies of the 16-byte immediate `{0.5,0.5,0.5,0.0}` exactly **20
  bytes apart** in the SHEX chunk. 91 true positives, 908 true negatives, 0 misses.
  Can false-positive mid-body on long shaders, so let the SH test below override it.
- **Real colour pass**: declares the SH rig — `shRed/shGreen/shBlue/shSun/shSunColor/
  shSunData`. 291 of 291 such shaders are colour passes; none is a prepass.
- **Depth/util**: `AlphaRef` only, or `ScreenDims+DepthPeelPush+TexelOffset`, or no
  slot-0 sampler. Note one trap: a shader can sample slot 0 and not be a prepass and
  still be a depth-moment pass writing `(depth, depth*depth, 0, 1)` (AMAT 135429 ps 3).

**`shaderPassFlags` is NOT a usable discriminator** — bit 0x4000 marks the colour
effects in some AMATs (135429: colour 0xc041, prepass 0x8041) and is absent from them
in others (643276: colour 0x1). It disagreed with shader content 48 times out of 80.
Split opaque vs blended on the effect's bgfx `renderState` blend bits instead.

Two shader families exist per material and BOTH are legitimate:
- **deferred** (what GW2 runs for ordinary world geometry): samples the light
  accumulation buffer at `gSs14`, uniforms `LightBuffer/StencilId/ScreenDims/gSs2/gSs12/gSs14`
- **forward**: computes SH ambient + sun + shadow itself, uniforms include `gSs15` (shadow map)

See [[gw2-frame-pass-order]], [[gw2-engine-uniform-values]], [[castlemist-app]].
