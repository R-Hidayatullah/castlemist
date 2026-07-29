---
name: gw2-engine-uniform-values
description: "Measured values of GW2's engine-global shader uniforms (ScreenDims, LightBuffer, SH rig) — component ORDER matters and getting it wrong renders dark and flat"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 8359fe52-6089-4655-b01c-f68be06b878e
  modified: 2026-07-29T04:30:41.618Z
---

Measured from `sample1.rdc` (1344x756 frame). These are the engine-filled bgfx
uniforms every material cbuffer expects; the component order is load-bearing.

- **`ScreenDims` = (1/w, 1/h, w, h)** — reciprocals FIRST. eid 1650 cb0[3] =
  `(0.000744, 0.001323, 1344, 756)`. Deferred materials fetch their lighting with
  `uv = (screenPos.xy + TexelOffset.x) * ScreenDims.xy`, so `.xy` must be the
  reciprocals. Passing `(w, h, 1/w, 1/h)` makes every pixel sample one clamped corner
  texel of the light buffer — the model renders dark and FLAT (no directional
  shading at all). This was a real bug in castlemist.
- **`TexelOffset` = 0** on D3D11 (pixel centres already at the half-texel; it is the
  D3D9-era half-pixel fixup).
- **`LightBuffer` = (0.25, 4.0, 1/128, 128)** — a VECTOR, not a scalar to broadcast.
  `.x` light-buffer encode, `.y` decode, `.z` = 1/`.w`, **`.w` = specular power scale**
  (`pow(NdotH, gloss * .w)`). Broadcasting one value wrecks the specular exponent.
  castlemist stores its light buffer display-referred instead of pre-divided by 4, so
  only `.y` becomes 1.0 there; the other three keep the game's values.
- **SH lighting rig** (same model in the deferred light pass, sample1 ps 8641
  cb0[50..54], and in forward materials, AMAT 135429 ps 4 cb0[1..6]):
  ```
  colour = dot(float4(N,1), sh{Red,Green,Blue})            // directional SH ambient
         + saturate(dot(float4(N,1), shSun)) * shSunColor  // sun, x shadow
  alpha  = max(shadow,0.5) * pow(dot(N, normalize(V+L)), gloss*128) * 0.5
  ```
  Calibrate against the game's own decoded light buffer (RT 6826, x4):
  **p1 0.32 / p50 0.98 / p90 1.06 / max 1.28** — a lit surface sits near 1.0 and the
  buffer never falls below ~0.32, so shadow sides keep material detail.
- **`LightCount` = 0** offline → no local point/spot lights, lit purely by the global
  env rig. **`WorldToShadowD` = 0** + a WHITE `gSs15` collapses the shadow chain to
  1.0 (fully lit), which is the right answer with no shadow-caster pass.
- The final composite is a **plain copy** — `mul r0, r0, 0.999985`, no tonemap
  operator (eid 6379). Every colour target is 8-bit UNORM; the only float surface is
  the R32_FLOAT linear depth. Dynamic range comes from the 0.25/4.0 light-buffer
  range compression, not from an HDR pipeline.

See [[gw2-amat-shader-roles]], [[gw2-frame-pass-order]], [[gw2-map-lighting]].
