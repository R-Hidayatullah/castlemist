---
name: gw2-animation-banks
description: "The geometry<->animation link: a rigged MODL carries a zeropose and IMPORTS its locomotion from animation-only MODLs. ModelFileAnimationBank.imports, the inline-Granny shape of an animation-only file, the bank chain, and the rigid attach behind 'only part of the model animates'"
metadata:
  node_type: memory
  type: project
  modified: 2026-08-19
---

# Animation banks

[[gw2-skeleton]] decodes the `ANIM` chunk of *one* file and stops there. That is
why a character loaded with a rig and nothing to play: **the clips are not in the
model.** A geometry MODL ships its meshes, its bindings and usually a single
static `zeropose`, and then *names the files* that hold its real locomotion.

The two halves are joined by one array, `ModelFileAnimationBank.imports`. This
note is that array, the files it points at, and what the client does with both.

Addresses are `Gw2-64.exe`, imagebase `0x140000000`, and were re-resolved
against the live IDB while this note was written (see
[[gw2-ida-source-path-anchors]] for why that matters).

## Why some MODLs are "geometry only" and others "animation only"

Same container, different chunks:

| shape | `GEOM` | `ANIM` | what it is |
| ----- | ------ | ------ | ---------- |
| geometry + bank | yes | bank with `animations[]` + `imports[]` | a character, a weapon, an armour piece |
| animation only | no | **no bank** — one Granny animation inline | one clip, the thing an import points at |

The animation-only shape is the common one, and it is unreachable from the model
side unless the import list is followed — which is exactly what castlemist was
not doing. `gw2dat_cli scananim` counts both shapes over a DAT range.

## `ModelFileAnimationBank.imports`

Each entry names one animation-only file plus the clips it is *declared* to
provide:

```
ModelAnimationImportDataV*  {
    filename       : fileref -> MODL fileId of the animation-only file
    sequenceTokens : token64[]                           // <= V24
    sequences      : {name: token64, duration: float}[]  // >= V25
}
```

**Field drift at V25.** Up to `V24` an import lists bare clip name hashes; from
`V25` it lists name+duration pairs. A live client ships `ANIM` v25, so v25 is the
path that matters in practice — but older DATs hit the first, and reading one
layout as the other yields a plausible-looking array of garbage tokens rather
than an error. Both are handled in `Extractor::parseAnimImports`.

`ModelFileAnimationBankV19..V21` additionally carry `modelReference`, the model
an animation file says it animates. Later versions dropped it; it is decoded
where present and reported as `animation.modelReference`.

## The engine side

| function | address | what it does |
| -------- | ------- | ------------ |
| `Model_CreateAnimationBanksFromImports` | `0x140CDB750` | walks `imports[]` |
| `Model_AddAnimationBank` | `0x140CDA760` | loads one entry as another ModelFile, chains it onto `m_animationBanks` |
| `ModelAnimBank_FindAnimation` | `0x140D327A0` | clip lookup, walks the chain |
| `ModelFile_LoadAnimations` | `0x140CFF8A0` | the loader, including the no-bank branch |
| `ModelAnimBankDir_SetImportSequences` | `0x140CF3E70` | records the declared sequence list |

Three properties of that chain were copied deliberately into
`resolveAnimImports()` (`gw2model.hpp`), because guessing differently changes
which clip a name resolves to:

1. **Root bank first.** Lookup walks `m_animationBanks` from the head and stops
   at the first bank that answers, so an import never overwrites a clip token the
   model already has.
2. **No recursion.** `Model_CreateAnimationBanksFromImports` reads the *root*
   bank's imports only. An imported bank's own imports are ignored — in the
   client too.
3. **`sequences` is a hint, not a filter.** It is what
   `ModelAnimBankDir_SetImportSequences` records for early-out; the bank still
   contributes every clip it contains.

## The animation-only file: no bank, inline Granny

`ModelFileAnimation*.bank` being null is not a malformed file. It means the
single Granny animation sits in the chunk itself, as
`PackGrannyAnimationType { animation: byte[], pointers: dword[] }` — the same
blob+fixup pair a banked clip keeps under `ModelAnimationData.data`.
`ModelFile_LoadAnimations` rebases it in place on that branch. Without this
branch every animation-only file parses to **zero clips**, silently.

Consequence for anything that keys clips by name: **an animation-only file has
no `token64`.** The token lives in the *bank entry of whoever imports it*; the
clip's only name is the one inside the Granny blob. So token identity cannot be
used to dedupe these — a clip without a token neither shadows nor is shadowed,
or dozens of distinct banks collapse into one.

## Rigid attach: the other half of "only part of the model animates"

A geoset can carry bone bindings and **no per-vertex weights/indices**. That is
GW2's rigid attach — a blade, a pauldron, a helmet — and it is not a rare case:
on model 291977, four of nine geosets are rigid and five carry a skin feed.

The client draws these with vertex-shader **variant 0**, which declares no
`grbones` at all: `BgfxDraw_MeshDrawLoop` (`0x140AB2CA0`) selects variant 1 only
when the mesh flags carry **both** `0x2` and `0x4` (weights *and* indices). They
still animate, because every *surface* carries its own transform — the draw
context takes `surface->transform` from `surface+8`, and for a rigid piece the
engine has already folded the attach bone's animated world matrix into it.

So the bone folds into that draw's `World`/`WorldView`, not into a palette. Get
this wrong and the rigid pieces stay at bind pose while the smooth-skinned
geosets move around them: the sword hangs in the air, the pauldron slides off the
shoulder.

Two consequences that cost time here:

- bindings must be resolved for **every** geoset, not only the ones with a skin
  feed. Resolving only skinned geosets is what left the rigid pieces frozen.
- the 255-slot cap belongs to the *vertex-indexed* path only (a `uint8` index
  cannot address past it, which is the same limit the engine enforces by
  dropping `GROUP`). Applying it before binding resolution throws away rigid
  attaches that were never going to index a palette.

The D3D preview path reached the same conclusion earlier from the other
direction — see the rigid-attach entry in [[castlemist-app]], which fixes it by
writing `bidx[0]=rigidBone, bwt[0]=1`. The bgfx 1:1 surface cannot do that,
because it uploads the archive's vertex bytes verbatim; it folds the matrix into
the draw instead. Same rule, two implementations, and both surfaces now agree on
which pieces follow the rig.

## Tooling

- `gw2dat_cli scananim --dat <Gw2.dat> --template <json> [--ids f | --offset/--count/--step]`
  counts the three shapes (`withClips`, `animationOnly`, `withImports`) over a
  range and lists which models import what. Loads the DAT and template once.
- `gw2dat_cli skel ... --imports` follows the banks and merges their clips, and
  reports `modelReference`, `importCount`, `mergedImportClips` and the
  unresolved import list. Off by default: a character can name dozens of banks.
  The clip listing now decodes each Granny header, so an imported clip shows a
  name and duration even though it has no token.
- `gw2bgfx_probe --fileid <id>` takes the number the archive browser and the
  MODL references use, not just a raw MFT row, and its geometry table gained
  `binds` (resolved/total bindings) and `skin` (`vtx` / `rigid` / `-`).

## Open

- Clip *blending* and the state machine that picks a clip. Only the loading and
  the bank chain were traced.
- `ModelAnimBankDir_*` beyond the import-sequence setter.
- Whether anything reads `modelReference` at runtime, or whether it is authoring
  metadata that V22+ dropped for that reason.

See also: [[gw2-skeleton]], [[gw2-granny-64bit]], [[gw2-render-asset-pipeline]],
[[castlemist-app]].
