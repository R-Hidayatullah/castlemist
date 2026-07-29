---
name: gw2-terrain-heightmap
description: GW2 mapc trn heightmap is CHUNK-TILED (not flat) — de-tile fix for castlemist terrain that rendered as a squeezed wavy mat
metadata: 
  node_type: memory
  type: project
  originSessionId: 2f6f1674-8f99-4cd3-88f9-9dd7abdb5770
---

**GW2 map terrain heightmap layout (mapc `trn` chunk, v14) — de-tiled, verified.** The `trn` heightMapArray is stored **chunk-tiled**, NOT one flat row-major grid. Fields: `dims`(dwordX,dwordY = tiles, e.g. 128×192), `swapDistance`, `heightMapArray`, `tileFlagArray`, `chunkArray` (count = number of terrain chunks), `materials`.

Layout (proven on 2 maps + zero overlap conflicts):
- `TILES_PER_CHUNK = 32` constant (chunk = 3072 world units = 32 tiles × 96 units/tile).
- chunk grid: `cX = dimX/32`, `cY = dimY/32`; `cX*cY == chunkArray.count`. (128×192→4×6=24; 160×160→5×5=25.)
- verts-per-chunk-side `vps = 35` (constant; `= round(sqrt(heightCount/chunkCount))`; heightCount = chunks·35²).
- Storage order: **chunk-major** (row-major over cY×cX chunk grid), then **row-major within a chunk** (ly outer, lx inner): `heights[(cy*cX+cx)*vps*vps + ly*vps + lx]`.
- Chunks **OVERLAP with stride 32**: chunk origin advances 32 verts but each spans 35, so a chunk's last 3 cols/rows are bit-identical to the next chunk's first 3 (verified: chunk0 idx32,33,34 == chunk1 idx0,1,2 for all edge pairs). De-tiled grid = `Gx=(cX-1)*32+35`, `Gy=(cY-1)*32+35` (e.g. 131×195); world vertex spacing = one tile = `(rect_x1-rect_x0)/dimX` (=96); far-edge overhang from the last chunk extends ~2 tiles past the `parm.rect`, harmless.

**Bug fixed (2026-07-17):** `entry_extractor.cpp` `build_terrain_model` assumed a flat perfect-square grid `G=round(sqrt(heights.size()))` — for a real map (29400 samples, sqrt≈171.5, non-square/non-flat) the rows wrapped at the wrong stride → terrain rendered as a **diagonally-corrugated "wavy mat", horizontally squeezed** (exactly the user's complaint). Rewrote it to de-tile per the layout above (keeps a flat-square fallback for unrecognised layouts). Verified headless (`GW2_AUTOLOAD=182586` mapc → shot_map.bmp): coherent valley landscape, correct proportions, props placed right. `parseTerrain` (gw2model.hpp) already exposes dimX/dimY; no parser change needed. See [[gw2-map-scene]] [[castlemist-app]].
