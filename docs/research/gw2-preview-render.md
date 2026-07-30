---
name: gw2-preview-render
description: "Equipment Preview / paper-doll is a SEPARATE renderer — own GrScene+GrCamera+ShLight+render target, a hardcoded 8-light studio rig in .rdata, off-centre projection into the widget rect, and ColorForwardRenderPass (not the world's deferred path)"
metadata:
  node_type: memory
  type: project
  modified: 2026-07-31
---

RE'd in IDA (`Gw2-64-disabled-aslr.exe`, imagebase 0x140000000) via the `ida_mcp`
HTTP server on 127.0.0.1:13337 (added to `.mcp.json` as server `ida`).

## Widget chain

`Ui\Widgets\ItemPreview\IpvDialog.cpp` (the "Equipment Preview" window) owns a
paper-doll — proven by its own assert `composite == paperDoll->GetPaperDollComposite()`
@0x141b31170 and `IS_TRUE(paperDoll)` @0x141b18750
(`Ui\Widgets\HeroDialog\Equipment\EqpPaperDoll.cpp`). The class is
`Ui::HeroDialog::CPaperDoll` (RTTI @0x14265c1c0), TU
`Ui\Widgets\HeroDialog\HdPaperDoll.cpp` = **0x14056E870 … 0x140580100**.

Five widgets share this machinery (all reference the `lightSh` assert @0x141ac1668):

| widget | init | render |
|---|---|---|
| `HdPaperDoll.cpp` (hero panel / **equipment preview**) | `0x140573840` | **`0x140575DC0`** |
| `BtHeroPaperDoll.cpp` | `0x1403f3820` | `0x1403F3C70` |
| `CaPaperDoll.cpp` (change appearance) | `0x14040aa20` | `0x14040B3B0` |
| `EmdPreview.cpp` (guild emblem) | `0x1404fc020` | `0x1404FCF80` |
| `PortraitManager.cpp` | — | `0x1402F7840` |

## Init — `sub_140573840`: it builds its own world

```
this+632 = sub_140AB79F0()            // GrScene create (also called by Map.cpp, CinModel, VdfLoad)
this+584 = sub_140AADD80()            // GrCamera create   -> SetScene(this+632)
this+592 = sub_140AB7430()            // ShLight create    -> vtbl+88(this+632) = SetScene
this+264 = sub_140ABF6D0(...)         // its own render target texture
```

So each preview widget is a **self-contained scene graph**: separate scene,
camera, SH light and colour target. Nothing from the map scene reaches it.

## Render — `sub_140575DC0` (0x1164 bytes), in order

1. `sub_140D5BE60(20480)` then `sub_140A6A500()` (GrDev push state) and
   `sub_140CF4540()` = **PushModelView** (`Model\ModelBillboarding.cpp`,
   `s_modelView` stack, max 10 — assert `s_modelView.Count() < 10`).
2. GrTrans stacks 0/1/2 reset (`sub_140AA1170`), then the model transform is
   built on stack 2: translate `(0, y, z*0.85 - 10000.0)` — **the preview is
   parked 10 000 units away from the map** — rotate about `(0,0,-1)` then
   `(1,0,0)`, translate back. Legs/boots clipping at the window bottom is this
   transform plus the scissor in step 6, not geometry culling.
3. **The studio light rig.** `lightSh->Clear()`, then a loop over a hardcoded
   8-entry table in `.rdata` (28 bytes/entry, HdPaperDoll @ **0x141B10520**):

   ```c
   struct PaperDollLight {   // 28 bytes
       float azimuthDeg;     // +0
       float elevationDeg;   // +4
       float r, g, b;        // +8, +12, +16   <- &entry+8 is what AddLight() gets
       float intensity;      // +20
       int   enabled;        // +24
   };
   ```

   direction = `-(cos el·cos az, sin el·cos az, sin el)` after `×π/180`, fed to
   `ShLight::AddLight` (`sub_140AB70F0`).

   **The first enabled light is NOT accumulated into SH — it becomes the sun.**
   `sub_140AB70F0` opens with `if (!*(a1+120)) return sub_140AB6ED0(a1, dir, a1+88);`
   — the first light is stored in the dedicated directional slot at `+88`
   (`shSun` = direction, `shSunColor` = `rgb·I`, `.w` = `I`). Only lights 2..n
   accumulate into the SH bands: `L1 += dir·0.488603·I·rgb`,
   `L0 += 0.28209499·I·rgb` → `shRed/shGreen/shBlue`
   (see [[gw2-engine-uniform-values]]).

   **The rig (identical in HdPaperDoll @0x141B10520, BtHeroPaperDoll
   @0x141AE3170, EmdPreview @0x141AFC260):**

   | # | az° | el° | rgb | I | on |
   |---|---|---|---|---|---|
   | 0 | 52.26 | 40.00 | 0.906, 0.949, 1.000 | 1.0500 | ✔ key, cool |
   | 1 | −161.42 | −40.06 | 0.725, 0.804, 1.000 | 1.0000 | ✔ blue back-fill |
   | 2 | 180.00 | −90.00 | 1.000, 1.000, 1.000 | 0.3226 | ✔ bounce |
   | 3 | 180.00 | 90.00 | 0.784, 0.996, 0.980 | 0.2500 | ✘ |
   | 4 | 121.94 | 20.32 | 1.000, 0.910, 0.867 | 0.6000 | ✔ warm rim |
   | 5 | −47.61 | 25.00 | 1.000, 0.910, 0.867 | 0.7000 | ✔ warm rim |
   | 6 | 0.00 | −45.00 | 1.000, 1.000, 1.000 | 0.1300 | ✘ |
   | 7 | −90.00 | 0.00 | 0.510, 0.482, 1.000 | 1.3032 | ✘ |

   5 of 8 enabled. `CaPaperDoll` (@~0x141AE4C08) and `PortraitManager`
   (@~0x141AC0F9C) use a *different* struct layout — not decoded yet.

4. **The backlight is overridden with fixed values.** `sub_140AB13E0(token32,
   float4)` = `GrSetGlobalParam` — it writes into a token-keyed engine-param
   registry in `BgfxDdi.cpp` (`qword_1426A6100 + 1363456`). The keys are **base-23
   Tokens**, not hashes — same `Token::Decode` as [[gw2-uniform-hash]]
   (`(t − 0x30000000)`, ALPHA `abcdefghiklmnopvrstuwxy`), so every engine-param
   name is ≤7 lowercase chars and lives in a *different* namespace from the
   shader uniform names:

   | token32 | decodes to | paper-doll writes |
   |---|---|---|
   | `0x8D43A284` | **`grblcol`** (backlight colour) | `(1.5, 1.5, 1.5, 1.5)` (`xmmword_141AE3640`) |
   | `0xC04DF0F8` | **`grbldir`** (backlight direction) | fixed az 30° / el −40° → `(0.6634, 0.3830, −0.6428)`, thread-safe-static init into `dword_142676098` (Bt/Ca use `..26C0`/`..2800`) |
   | `0x3482C79B` | `grblsm` | — (zeroed by GrDeviceCreate) |
   | `0x3D55A0CC` | `grblsmb` | — |
   | `0x368C09EC` | `grsmbs` | — (CinModel.cpp) |

   **These are the map's backlight.** `sub_140C407C0` (`Map\Environment\EnvContext.cpp`)
   writes the same `grbldir` token @0x140c41990 from `sin/cos` of two angles and
   scales a colour by an RGB triple at `[r12], [r12+4], [r12+8]` — i.e. from
   `PackMapEnvDataLightingV75.backlightColor/backlightIntensity`
   ([[gw2-map-lighting]]). The AMAT uniform table exposes them to materials as
   `BacklightColor` / `BacklightDirection` (present in AMAT 19896). So the preview
   does not merely ignore map lighting — it **actively replaces the map's backlight
   term** with a constant one. `CinModel.cpp` does the same for cinematics.
   `GrDeviceCreate` (`sub_140A69030`, GrDev.cpp) zeroes them at startup.

5. **Material-constant override for the fade** — `sub_140578F30(this, model,
   token64, float4)` calls `model->vtbl+1976(token, value, recurse=1)` on the
   root model *and every attached equipment sub-model*:
   - opaque (`this+664 == 1.0`): token `0x914C6A8A883B1EE`, value `(1,1,1,1)`
   - faded: token `0x51C59061370654`, value `(1,1,1,alpha)` — this token is the
     2nd link of the AMAT effect fallback chain in [[gw2-amat-effect-selection]].
6. Camera: `tanf(fov*0.5)` with fov @this+644, near @this+640, then
   `sub_140A9F4C0(0, {l,b,r,t})` = **off-centre projection** derived from the
   widget's screen rect, plus `sub_140AB1450(rect/uiScale)` = scissor. That is how
   a normal perspective model lands exactly inside the dialog frame.
7. **The pass list** — `sub_140D665C0` (called *only* by the three paper-dolls)
   builds a render-graph array:
   - `sub_140D689F0` ×2 → vtable `off_14209D9B0` = **`ColorForwardRenderPass`**
     (Execute `0x140d68a80`), mode flag 0 then 1
   - `sub_140D68F30` ×1 → vtable `off_14209DA40` (a pass class from
     `LightingRenderPass.cpp`, Execute `0x140d68f60`) — **only when alpha < 1.0**

   `ColorDeferredRenderPass` (vtable `0x14209D8F8`, Execute `0x140d688b0`) is
   **not** in the list. So the preview uses the **forward** colour path, while the
   world uses deferred + the screen-space light buffer at t14
   ([[gw2-frame-pass-order]]). `sub_140D67CD0()` gates render-graph vs a legacy
   `GrDev` path (`sub_140A6A140(camera, …, 0x2000/0x4000)`).
8. `sub_140A6A140` blits the widget's render target into the UI, then
   `PopModelView` + `sub_140A6ACA0` restore state.

## Light groups (separate mechanism, same conclusion)

Submodels and lights both carry an 8-bit mask in `GrSubmodelInfo::LIGHT_FLAG_MASK`
(= `0xFF00`, shift 8). `Model::SetLightFlags` = `sub_140CF0850` (Model.cpp:6496) →
`sub_140A82600` (GrModel.cpp:3396). Every call site (virtual slot `vtbl+0x808`):

| value | TU |
|---|---|
| 0x0B | **`Ui\Controls\CtlModel.cpp`** (`sub_1414A2720`) — the generic UI 3-D model control |
| 0x0D | `Map\Zones\ZnModel.cpp` |
| 0x07 | `AvCharEquipment.cpp` — **world characters** |
| 0x05 | `AvTest.cpp`, `EfCliEffectModel.cpp` |
| 0x02 | `CpsComposite.cpp` |
| 0x08 | `sub_140C582F0` |

UI models sit in group `0b1011`, world characters in `0b0111` — a UI model can
never be lit by a map light and vice versa. (`CtlModel` is a *sibling* control of
`CPaperDoll`, not the one the equipment preview uses; the preview gets its
isolation from the private GrScene instead.)

## APPLIED to castlemist (2026-07-31)

`src/render/detail/preview_rig.h` — the client's 8-row table verbatim plus
`eval_preview_rig()`, a faithful `ShLight::AddLight` (including the first-light →
sun-slot rule) returning `shRed/shGreen/shBlue/shSun/shSunColor/shSunData` +
`BacklightColor/BacklightDirection`.

`apply_env_uniforms()` (`game_material.cpp`) uses it when
`g_preview_rig && !g_env_rig_active && !g_scene_mode` — i.e. exactly the case the
game special-cases, a lone model/skin with no map. Map scenes are untouched and
keep their `env`-chunk rig. Vectors go in **unconverted**: these uniforms are GW2
world space (Z-up), the same space the map path already feeds `r.sunDir` in, and
castlemist keeps GW2's Z-up throughout (`test_render_math: world_matrix_treats_z_as_up`).

`BacklightColor/BacklightDirection` are now written on **both** paths — previously
neither was set, so any material sampling them read an unwritten cbuffer slot. The
map path writes an explicit zero because `MapEnvRig` does not yet carry the env
chunk's `backlightColor/backlightIntensity` (a real gap: parsing them would let map
mode drive the backlight properly).

`lighting.cpp`: the SH upload was duplicated verbatim in the single-model and
map-scene prepasses, each with its own stale hardcoded fallbacks. Now one
`upload_sh_light_cb()` that falls back to the rig, so the deferred light buffer and
the forward materials cannot drift apart.

Toggle: `set_preview_rig(bool)` / UI push-button **"GW2 rig"** next to "Follow cam"
(single-model toolbar), on by default. Headlight follow still overrides the key
direction; the SH fills/rims stay as the rig built them.

Regression test `tests/test_render_preview_rig.cpp` (5 cases) asserts the evaluated
uniforms against the **captured GPU constants**, not against themselves — plus the
first-light-excluded-from-SH rule, unit-length directions, and scale linearity.

## Answering "can this be extracted / turned into a model viewer"

Yes — everything the preview needs is CPU-side and hardcoded:

- The rig is 224 bytes of `.rdata` at `0x141B10520` (file offset = RVA
  `0x1B10520`). Patching those floats changes the preview lighting directly, with
  no shader work. Same for the `(1.5,1.5,1.5,1.5)` at `0x141AE3640`.
- The camera is fully parameterised on the widget: fov `this+644`, near
  `this+640`, distance/pan `this+648/652`, yaw/pitch feeding step 2.
- The material shaders are unchanged — models still go through the normal AMAT
  effect selection ([[gw2-amat-effect-selection]]); only the *pass* (forward) and
  the *light source* (SH from the static rig) differ. So castlemist's existing
  GameShader path already renders what the preview renders, provided it feeds
  `shRed/shGreen/shBlue` from this rig instead of `MapEnvRig`
  ([[gw2-map-lighting]] `clear_environment_rig()` case) — this table is a
  ready-made, game-authentic default for the skin viewer.

## Which shaders — corroborated by the exe shader dump

`dumps/shaders/exe_shaders/manifest.json` (2244 blobs: engine 1114 / material 1089
/ compute 41, 308 unique uniform names) contains exactly **20 full forward SH-lit
pixel shaders** — uniform signature `shRed, shGreen, shBlue, shSun, shSunColor,
shSunData, WorldToShadowD` + shadow sampler `gSs15`. They come in **opaque /
faded pairs**, which lines up 1:1 with the paper-doll's two `ColorForwardRenderPass`
instances (mode 0 / mode 1) and its two material-constant override tokens:

| variant | extra uniforms | csо indices |
|---|---|---|
| opaque | `StencilId` | 1428, 1742, 1785, 1828, 1850, 1882, 1907, 1939, 1965, 1998 |
| faded/cutout | `fxclr`, `AlphaRef` | 1437, 1751, 1794, 1834, 1856, 1893, 1910, 1949, 1968, 2008 |

Sampler counts vary (`gSs15` only → `+ss0` → `+ss0,ss1`), i.e. 0/1/2 material
textures. This is the shader class whose inputs the paper-doll's `ShLight` fills,
and it is the same set [[gw2-exe-shaders]] flagged as "the AUTHENTIC replacement
for castlemist's hand-written `kLightHLSL`".

## VERIFIED against a capture (`dumps/captures/weaponpreview2.rdc`)

Frame: 39 passes, 1335 draws, 1920x1080. The world runs its normal deferred path —
**pass 20** (eid 8674-11473, 228 draws, RT 26778) has `cb0[0] = (0.25, 4.0, 1/128, 128)`
= the `LightBuffer` constant, exactly as [[gw2-frame-pass-order]] describes.

**The preview character is pass 36** (eid 14603-15597, 89 draws, RT **20993** — a
low resource id = widget-owned, created long before the 26xxx scene targets).
Its character draws are eid **15113-15212**, binding 1024x1024 BC3 (albedo) +
1024x1024 BC5 (normal) armour textures, `nIdx` 8754 / 8652 / 2484 / 2118.
Sampler signature is **`ss0, ss1, gSs13, gSs15`** — the forward-lit family, not the
deferred one.

**Every lighting constant matches the `.rdata` rig byte-exactly.** Predicted from
the table at `0x141B10520` *before* opening the capture, then compared to the PS
cbuffer at eid 15194 (worst error over all 28 components: **2.0e-05**, i.e. the
5-decimal rounding in the dump):

| cb0 reg | uniform | predicted from rig | actual on GPU |
|---|---|---|---|
| 2 | `shRed` | ( 0.19360, 0.08209, 0.13942, 0.66239) | ( 0.19360, 0.08209, 0.13941, 0.66238) |
| 3 | `shGreen` | ( 0.22712, 0.09183, 0.18630, 0.65143) | ( 0.22713, 0.09183, 0.18630, 0.65143) |
| 4 | `shBlue` | ( 0.29937, 0.11538, 0.25859, 0.69094) | ( 0.29937, 0.11538, 0.25859, 0.69092) |
| 5 | `shSun` | (−0.46890, −0.60577, −0.64279, 0) | (−0.46890, −0.60577, −0.64279, 0) |
| 6 | `shSunColor` | ( 0.95120, 0.99645, 1.05000, 1.05) | ( 0.95118, 0.99647, 1.05000, 1.05) |
| 8 | `grblcol` | ( 1.5, 1.5, 1.5, 1.5) | ( 1.5, 1.5, 1.5, 1.5) |
| 9 | `grbldir` | ( 0.66341, 0.38302, −0.64279, 0) | ( 0.66341, 0.38302, −0.64279, 0) |

`shSun`/`shSunColor` are rig light 0 (az 52.26°, el 40°, `rgb·I` = (0.906,0.949,1.0)·1.05)
verbatim; `shRed/Green/Blue` are exactly lights 1, 2, 4, 5 accumulated. `cb0[7]`
(`shSunData`) repeats the sun direction with `.w = −1`.

**Map lighting is provably absent.** The map's rig is a warm sun `(1.0, 0.914, 0.753)`
at intensity 1.3, direction `(0.707, 0, 0.707)` ([[gw2-map-lighting]]). The preview's
sun is cool `(0.951, 0.996, 1.05)` pointing `(−0.469, −0.606, −0.643)`. Different
values, different pass, different render target.

**One caveat on `grblcol`:** `(1.5, 1.5, 1.5, 1.5)` also appears in *world*
material draws (eids 9476-10587, RT 26778), so that value alone does not identify a
preview draw — this map's own backlight is 1.5 too. The SH/sun values are the
discriminator.

Reproduce with `scratchpad/prev_zoom.py` (see [[renderdoc-python-access]] — pass no
extra argv).

See [[gw2-frame-pass-order]], [[gw2-map-lighting]], [[gw2-engine-uniform-values]],
[[gw2-amat-effect-selection]], [[castlemist-app]].
