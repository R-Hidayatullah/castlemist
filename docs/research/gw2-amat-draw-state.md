---
name: gw2-amat-draw-state
description: "Re-anchored effect selection and draw-state composition (BgfxShader.cpp / BgfxDraw.cpp) — the token chain, the quality walk, the uniform<->constants pairing rule, and four corrections to the older notes"
metadata:
  node_type: memory
  type: reference
  modified: 2026-08-16
---

# AMAT effect selection and draw state, re-anchored

Applied to `gw2decomp/Gw2-64.exe.i64` on 2026-08-16. Supersedes the addresses in
[[gw2-amat-effect-selection]] and the `shaderPassFlags` table in
[[gw2-amat-shader-roles]]. Method as in [[gw2-render-asset-pipeline]]: resolve by
the Perforce path anchor, never by remembered address.

Anchors used: `BgfxShader.cpp` @ `0x141D12310`, `BgfxDraw.cpp` @ `0x141C327A0`.

| addr | name (now in the IDB) | was |
| --- | --- | --- |
| `0x140BFDC40` | `BgfxShader_SelectEffect` | `sub_140BFAD20` |
| `0x140BFE990` | `BgfxShader_FindVsVariant` | — |
| `0x140BFEBB0` | `BgfxShader_BindUniforms` | — |
| `0x140BFFD20` | `BgfxShader_BuildPassEffects` | — |
| `0x140BF7550` | `BgfxShader_FindEffectByToken` | `sub_140BF4630` |
| `0x140BFFB20` | `BgfxShader_BuildPasses` | (unchanged) |
| `0x140AB2CA0` | `BgfxDraw_MeshDrawLoop` | `sub_140AAFDB0` |
| `0x140AB6D10` | `BgfxDraw_ComputeDepthState` | `sub_140AB3E10` |
| `0x140AB3530` | `BgfxDraw_SubmitDraw` | — |
| `0x140B69650` | `bgfx_getShaderUniforms` | — |
| `0x140A6B020` | `GrGetGlobalParamIndex` | — |

## Four corrections

**1. `shaderPassFlags` bits 0 and 1 are the cull mode, not "opaque".**
`BgfxShader_BuildPassEffects` folds them into the runtime effect's cull word:
bit 0 → `CULL_CCW` (`0x2000000000`), bit 1 → `CULL_CW` (`0x1000000000`).
`BgfxShader::m_flags` overrides them first (bit 2 → CCW, bit 3 → CW, bit 4 →
none). [[gw2-amat-effect-selection]] read `0x1` as an opaque marker; it never
was one, which is part of why it "disagreed with shader content 48 times out
of 80".

**2. Quality has four tiers, and selection is first-affordable-in-file-order.**
`BgfxShader_BuildPasses` decodes each technique's quality token and switches on
its **first character only**: `u`ltra=4, `h`igh=3, `m`edium=2, `l`ow=1, anything
else 0 plus a log line. The loop then breaks on the first technique whose
quality is `<= maxQuality` **in file order**, and if none qualifies it keeps the
last one examined. It is not "pick the best affordable tier" — the answer
depends on how the file orders its techniques. `GR_SHADER_QUALITIES = 5`
(asserted at :860).

**3. The second shader binary is `osxShader`, not `pbrShader`.**
The packfile schema names `AmatShaderV1 { isPixelShader, dx11Shader, osxShader }`,
and the runtime picks the blob at +52 over the one at +4 only when a context
flag is set. It is a platform switch (Metal), not a shading-model switch, and it
is always the `dx11Shader` on this path.

**4. The pass-0 rule on the default token.**
The default token `183330` is accepted **only on pass 0**. On any later pass
`BgfxShader_SelectEffect` returns failure and the mesh is simply not drawn for
that pass — that is how a material opts out of a frame pass. Substituting the
default there adds geometry the game never renders.

## The selection token is a RENDER MODE, not a material id

The `materialToken64` argument is the single most misread thing in this path,
including by an earlier draft of this note. It is **not** the MODL material's
`token` field. The client reads it from the *runtime* McMaterial
(`*(mesh->material + 64)`), and it names **which render mode the calling pass
wants** — opaque, faded, decal, and so on.

Two observations settle it, both from `gw2bgfx_probe` on jade-tech 291977:

- All six of that model's materials carry the same MODL token `0xB58860F`, and
  it appears in none of their AMATs.
- Every AMAT offers the *same menu* of 15 effect tokens regardless of material,
  and that menu contains all three remap-chain sources and both of the
  paper-doll's material-constant override tokens:

```
0x2CC22 (=183330, default)   0x1081              0x471582
0x52B0A55E8A40A4             0xC29608A40A4       0x5DE40A268A40A4
0x1481311EC                  0x2C26C0B64F711EC   0x1779028991EC
0x18011ED                    0x914C6A8A883B1EE   0x6584D816C9EE
0xA20E963532                 0x51C59061370654    0x1128A40A4
```

`0x914C6A8A883B1EE` is **opaque** and `0x51C59061370654` is **faded** — exactly
the pair [[gw2-preview-render]] recorded the paper-doll widget writing through
`SetMaterialConstant` when its fade is 1.0 and below 1.0. The same two tokens
serve both jobs.

It matters. Selecting 291977 with the opaque token instead of falling through to
the default changes the pixel shader on five of six materials:

| material | default token | opaque token |
| --- | --- | --- |
| 0 | ps 1 | **ps 24** |
| 1 | ps 10 | **ps 39** |
| 2 | ps 1 | **ps 28** |
| 3 | ps 1 | **ps 29** |
| 4 | ps 3 | **ps 30** |
| 5 | ps 25 | ps 25 |

So an offline renderer must decide which mode it is emulating and pass that
token. For a lone model on a neutral background — the paper-doll case — that is
`0x914C6A8A883B1EE`.

## Effect selection — `BgfxShader_SelectEffect` (BgfxShader.cpp:1069-1200)

```
select(shader, ctx, passIdx, materialToken64, meshFlags, vsVariantId, out)
```

1. technique = `techniques[m_techniqueIndex]` (quality, per above);
   assert `passIdx < technique.passCount`.
2. effect looked up **by token64** in `pass.effects[]`, in this order:
   - the material's own token
   - one remap, for exactly three tokens and one attempt each:
     `0x1481311EC → 0x51C59061370654`,
     `0x1779028991EC → 0x5DE40A268A40A4`,
     `0x2C26C0B64F711EC → 0x6584D816C9EE`
   - the default `183330` — pass 0 only
3. vertex variant: match `vertexShaderVariants[i].variant == vsVariantId`, else
   retry once with `variant == 4 ? 2 : 0` (`BgfxShader_FindVsVariant`, asserted
   at :1037). `AMAT_VERTEX_SHADER_VARIANTS = 5`.
4. asserts `vertexShaderIndex < shaderCount` (:1172),
   `pixelShaderIndex < shaderCount` (:1179).

`AmatEffectV1` is 36 bytes and the runtime stride confirms the schema exactly:
`{ u64 token, u64 renderState, u32 shaderPassFlags, u32 pixelShaderIndex,
u32 variantCount + u64 variants }`. `AmatVertexShaderVariantV1` is
`{ u32 variant, u32 vertexShaderIndex }` — `variant` is the small integer 0..4,
not a Token.

### The variant ids, from the draw loop

```
if ((meshFlags & 4) && (meshFlags & 2) && !(surfaceFlags & 2))  -> 1
else if (!(meshFlags & 0x80))   -> instanced ? 3 : 0
else                            -> instanced ? 4 : 2
```

`0x80` is skinned. Variant 1 has no established name.

## Uniform binding — `BgfxShader_BindUniforms` (BgfxShader.cpp:316-343)

This is the mechanism the material-constant work had been reaching for. The
engine calls `bgfx::getShaderUniforms` and walks the returned handles **in
order**, keeping two independent cursors:

- a uniform whose type is **not** Sampler consumes the next entry of
  `AmatShaderBinary.constants[]` (an array of bare token32), and the pair is
  stored with `GrGetGlobalParamIndex(token)` — so each shader constant is
  wired to an engine global param **by token**, and filled per draw from the
  token-keyed registry.
- a uniform whose type **is** Sampler consumes the next entry of `samplers[]`.

The count assert at :336 is the rule that makes this exact:

```
bgfxUniformsCount == constantCount + samplerCount
                     - (samplers whose textureIndex == -1)
```

So a sampler with `textureIndex == 0xFFFFFFFF` consumes no uniform. Any
leftover samplers past the uniform list get synthetic uniforms named
`fakesampler{N}`. `MAX_UNIFORMS` here is **128**, not bgfx's default 512
(:316).

This is why positional pairing looked unreliable before: it is positional, but
over the *whole* uniform list with the sampler/non-sampler split, not over a
filtered "material uniforms only" subset.

## Draw state — `BgfxDraw_MeshDrawLoop` (BgfxDraw.cpp)

```
state = depthState                       // BgfxDraw_ComputeDepthState
      | cull | effect.renderState        // cull skipped when materialFlags & 0x4000
      | (passFlags & 0x2000 ? BLEND_EQUATION(REVSUB) : 0)
      | (instancing && (passFlags & 0x40000) ? 0x2000000000000 : 0)
      | writeMask
```

The `0x2000` term is the literal `0x120000000` in the binary, which is exactly
`BGFX_STATE_BLEND_EQUATION(REVSUB)` — the macro ORs the value in at shift 28
and again at shift 31.

**Write mask** (unchanged from the older note, restated with its inputs):

```
noRGB   = (passFlags & 0x0008) || (materialFlags & 0x800)
noAlpha = (passFlags & 0x4000) || (materialFlags & 0x400)
mask = (passFlags & 0x4) ? 0 : !noRGB ? (noAlpha ? RGB : RGBA) : !noAlpha ? A : 0
```

### Depth — `BgfxDraw_ComputeDepthState`

Two different flag words, which is easy to conflate and produces exactly the
wrong-looking result if you do: `materialFlags` = `*(mesh->material + 28)`,
`surfaceFlags` = `*(surface[2] + 12)`.

```
if (drawCtx[+8] == 0) { test = ALWAYS(0x80); write = false; }
else {
    test  = (passIdx == 0 || (passFlags & 0x10) || (materialFlags & 0x2000))
          ? LEQUAL(0x20) : EQUAL(0x30);
    write = !(surfaceFlags & 0x4000)
         || ((surfaceFlags & 0x2000) && !(materialFlags & 0x3800000));
}
if ((passFlags & 0x40) || (materialFlags & 0x2000)) write = false;
if ((passFlags & 0x20) || (materialFlags & 0x1000)) test  = 0;
if (materialFlags & 0x100) test = EQUAL(0x30);
if (materialFlags & 0x200) test = GREATER(0x50);
state = test | (write << 38);
```

New against the older note: the `ALWAYS` branch, the `0x10` LEQUAL escape, and
the `0x100` / `0x200` surface overrides.

**Depth bias** comes out through a separate float parameter, not the state word:
`+65` for material tokens `0x6584D816C9EE`, `0x2C26C0B64F711EC`, `0x48A635447`;
`-65` when `passFlags & 0x8000` or `materialFlags & 0x80`; otherwise a per-frame
value off the draw context — and **only that last case** ORs bit 59
(`0x0800000000000000`) into the state. So bit 59 is not a general "bias active"
flag. Bit 59 is unused in upstream bgfx (56/57/58 are MSAA / LINEAA /
CONSERVATIVE_RASTER), so it is a fork addition whose meaning is still open.

## `shaderPassFlags`, consolidated

| bit | effect |
| --- | --- |
| `0x0001` | cull CCW |
| `0x0002` | cull CW |
| `0x0004` | colour write mask = 0 (depth-only / prepass) |
| `0x0008` | no RGB write |
| `0x0010` | force LEQUAL even on a later pass |
| `0x0020` | disable depth test |
| `0x0040` | disable depth write |
| `0x2000` | blend equation REVSUB, both channels |
| `0x4000` | no ALPHA write |
| `0x8000` | depth bias -65 |
| `0x40000` | with instancing, ORs `0x2000000000000` |

## Consequence: the vertex buffer needs no repacking

`GrFvf_CreateVertexLayout` (`0x140B9D110`) asserts at BgfxBuffer.cpp:755 that
`DDI_STRIDE(fvf) == vertexLayout->CalcStride()`. That assert is live in the
shipping client, so for every fvf the game loads, **the MODL's vertex buffer is
already in GPU layout**. Checked on jade-tech 291977's real fvfs (`0x30081`,
`0x70081`) in `tools/viewer/gw2bgfx/selftest.cpp`: 32 and 36 bytes both ways.

This removes the largest remaining guess in `src/render` — the fat 144-byte
`GVertex`, the `game_sem_off` semantic-to-offset table, and inferring an element
format from `D3D11_SIGNATURE_PARAMETER_DESC::Mask` (which cannot distinguish a
4-component uint8 from a 2-component float, and gets `BLENDINDICES` and packed
tangent frames wrong).

Three fvf bits would break the equality if they appeared — `0x01000000` (+48
on disk, no GPU element), `0x02000000` (+4, none) and `0x10000000` (6 on disk
against a 4-byte Normal slot). The live assert is the evidence they do not occur
in shipped content. `grFvfStrideMatches` checks rather than assumes.

See [[gw2-bgfx-vendored-version]], [[vertex-fvf]], [[gw2-uniform-hash]],
[[gw2-render-asset-pipeline]].
