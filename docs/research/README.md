# Research notes

The reverse-engineering behind castlemist. Nothing here was documented by
ArenaNet: every format below was recovered from the client binary, from a
running process, or from a RenderDoc capture.

These are kept close to how they were written, working notes rather than a
manual -- IDA addresses, dead ends, and the measurement that settled the
question. That is deliberate. Six months later the useful part of a note is
rarely the conclusion; it is the evidence, and which things were tried and did
not work.

Where a note names a source file, it names one in this repository. Where it
names an address, that is `Gw2-64.exe` with ASLR disabled, imagebase
`0x140000000`.

## The archive

| note | what it settles |
| ---- | --------------- |
| [filetypes.md](filetypes.md) | the file types in the dat and which ones castlemist handles |
| [gw2-archive-packfile-runtime.md](gw2-archive-packfile-runtime.md) | the client's own view of the dat. **Three of our `MftData` field names describe the wrong thing** — `compression_flag` is really `extraBytes`, `entry_flag` is two fields, `counter` is a stream-chain link. Also: the packfile signature is the uint16 `"PF"`, and the MFT CRC deliberately skips its own slot |
| [gw2-method0-degenerate-huff.md](gw2-method0-degenerate-huff.md) | an all-zero code-length table decodes to one symbol in **zero bits** -- this fixed 462 entries that had failed to decompress |
| [gw2index-tool.md](gw2index-tool.md) | what the index records and why compression truth has to be verified rather than read off the flag |
| [gw2mcp-server.md](gw2mcp-server.md) | the CLI/MCP split over the dat tooling |
| [curated-test-ids.txt](curated-test-ids.txt) | known-good baseIds per format, the fixtures the `dat` test suite asserts against |
| [gw2-ida-naming-coverage.md](gw2-ida-naming-coverage.md) | how far the IDB naming pass got, what was deliberately left, and the prioritised worklist. Read before starting more RE |
| [gw2-cmp-img-symbol-map.md](gw2-cmp-img-symbol-map.md) | named IDA symbols for the Compress and image-decode clusters, recovered from the binary's embedded Perforce source paths. **The addresses in the older notes are from a previous build and no longer resolve** |
| [gw2-decode-walkthrough.md](gw2-decode-walkthrough.md) | plain-English tour of both stages, dat entry to pixels. Start here if you are not a reverse engineer |
| [gw2-chatlink-token-hash-map.md](gw2-chatlink-token-hash-map.md) | named symbols for chat links (the full 17-entry decoder table), Base64, CRC-32/32C and Murmur2A. **Token has two schemes, not one** — 32-bit base-23 and a 64-bit 5-bit-per-char packing |
| [gw2-strs-crypt-symbol-map.md](gw2-strs-crypt-symbol-map.md) | named symbols for the strs text pipeline: RC4, the 8→20 key expansion, the bit-unpack decoder, the runtime key map. RC4 is symmetric, so there is no separate encrypt function — and both retail hook signatures still match |

## Textures and images

| note | what it settles |
| ---- | --------------- |
| [gw2-atex-atep-decode.md](gw2-atex-atep-decode.md) | the ATEX family. The container magic flag turns out to be write-only, so ATEP decodes identically to ATEX |
| [atex-debug-notes.md](atex-debug-notes.md) | the decoder debugging trail, kept for the failure modes |

## Models, animation and physics

| note | what it settles |
| ---- | --------------- |
| [gw2-render-asset-pipeline.md](gw2-render-asset-pipeline.md) | how shaders, models and animation load. **58 shader packages are baked into the exe; shaderId 58 loads an AMAT from the dat** — both paths traced to the branch. Also the definitive GrFvf → bgfx::VertexLayout builder, and the 28-entry granny-attribute → fvf table |
| [gw2-texture-upload.md](gw2-texture-upload.md) | decoded pixels → GPU texture, the half [gw2-cmp-img-symbol-map](gw2-cmp-img-symbol-map.md) stops short of. **DdiTexture carries two format fields four bytes apart** — GR_FORMAT at +0x0C, bgfx::TextureFormat at +0x10 — and confusing them breaks every pitch calculation. Also: **atex mip chains stop at 4x4**, so a mipped bgfx texture samples two levels the file never shipped |
| [vertex-fvf.md](vertex-fvf.md) | GrFvf, the flexible vertex format, and how a vertex declaration is read |
| [gw2-skeleton.md](gw2-skeleton.md) | bind pose from SKEL (inverse of the inverse-world), embedded animation, GPU skinning |
| [gw2-animation-banks.md](gw2-animation-banks.md) | **a rigged model does not contain its animation** — it keeps a zeropose and *imports* the rest by fileId through `ModelFileAnimationBank.imports`. The imported files have no bank and no token64: one Granny clip sits inline. Also the rigid attach (bindings, no vertex weights) that leaves a sword hanging in the air |
| [gw2-granny-64bit.md](gw2-granny-64bit.md) | granny blobs are 32- **or** 64-bit. Read a 64-bit one as 32-bit and you get zero-length arrays, not an error -- animations silently came out 0.0 s long |
| [gw2-cloth-system.md](gw2-cloth-system.md) | the Verlet/PBD cloth solver, reconstructed |
| [gw2-particle-system.md](gw2-particle-system.md) | baked emitters in MODL cloudData/lightData |

## Maps

| note | what it settles |
| ---- | --------------- |
| [gw2-map-scene.md](gw2-map-scene.md) | prop placement out of mapc/area prp2. GW2 is Z-up |
| [gw2-terrain-heightmap.md](gw2-terrain-heightmap.md) | the heightmap is chunk-**tiled**, 35x35 verts at stride 32 with 3 overlapping. Read flat, terrain renders as a squeezed wavy mat |
| [gw2-map-lighting.md](gw2-map-lighting.md) | the per-map env sun and fill rig, and the tone mapping that stopped props blowing out to white |

## Shaders and the frame

| note | what it settles |
| ---- | --------------- |
| [gw2-bgfx-vendored-version.md](gw2-bgfx-vendored-version.md) | GW2 vendors **upstream bgfx verbatim** at commit `a476c5b9`, rev 8775, API 128. A `bgfx-master` checkout is not a drop-in reference: `Attrib::Count` is 18 there and 26 on master, so `VertexLayout` copied from master is silently wrong. Three offsets in the binary pin the version without trusting the SHA |
| [gw2-shaders-dxbc.md](gw2-shaders-dxbc.md) | the game's DXBC shaders live in AMAT entries and load in D3D11 |
| [gw2-exe-shaders.md](gw2-exe-shaders.md) | ~2275 more bgfx blobs are embedded in the exe; how they are ordered |
| [gw2-amat-shader-roles.md](gw2-amat-shader-roles.md) | an AMAT holds *every* frame-pass shader. Picking the normal pre-pass is what renders models flat green, and `shaderPassFlags` cannot tell you which is which |
| [gw2-amat-effect-selection.md](gw2-amat-effect-selection.md) | technique = quality level, effect keyed by token64, and blend state comes only from `effect.renderState` |
| [gw2-frame-pass-order.md](gw2-frame-pass-order.md) | each model is drawn **twice**, linked by a screen-space light buffer. Opaque materials are alpha-cutout at 0.25 and the output alpha is a stencil id |
| [gw2-engine-uniform-values.md](gw2-engine-uniform-values.md) | measured values of the engine-global uniforms. Component order matters; wrong order renders dark and flat |
| [gw2-uniform-hash.md](gw2-uniform-hash.md) | the uniform-name hash is Murmur2A seed 0. The AMAT constant token is *not* that hash -- still open |
| [transparency-bug.md](transparency-bug.md) | two distinct causes of the same see-through look |
| [renderdoc-python-access.md](renderdoc-python-access.md) | why replay scripting has to go through `qrenderdoc.exe --python` |

## Text, audio and video

| note | what it settles |
| ---- | --------------- |
| [gw2-strs-decrypt.md](gw2-strs-decrypt.md) | packed strs are RC4 with a per-stringId key that exists only in the running client |
| [gw2-text-pack.md](gw2-text-pack.md) | how cntc content names and voice lines resolve through txtm/txtv/txtV |
| [gw2-chat-links.md](gw2-chat-links.md) | `&[base64]` links: two systems, classic header-dispatch and message-pack templates |
| [gw2-bink-video.md](gw2-bink-video.md) | KB2i/KB2j playback through the game's own DLL, and why seeks are expensive |
| [gw2-scene-video-subtitles.md](gw2-scene-video-subtitles.md) | **subtitles are called "chatter lines"** — searching for `subtitle` or `caption` finds nothing. 201 line types, each with a near/far duration picked by whether you are targeting the speaker. Also: Bink movies stream through the asset system, not off disk |

## The client itself

Not used by castlemist. Kept because the questions recur and the answers cost
real time to establish.

| note | what it settles |
| ---- | --------------- |
| [gw2-login-flow.md](gw2-login-flow.md) | the three-gate STS / Portal / AuthSrv login and where credentials are actually validated |
| [gw2-map-loader-gates.md](gw2-map-loader-gates.md) | the 13-state map loader, and which two states are genuinely server-gated |
| [gw2-net-crypto.md](gw2-net-crypto.md) | per-connection RC4 over MsgConn, keyed through the TLS/RSA handshake |
| [gw2-param-system.md](gw2-param-system.md) | the 194-entry command-line table. `-map` exists but is never read |

## About this program

| note | what it settles |
| ---- | --------------- |
| [castlemist-app.md](castlemist-app.md) | the viewer's own feature and module map |
