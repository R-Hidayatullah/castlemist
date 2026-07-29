---
name: gw2-granny-64bit
description: GW2 embeds granny animation blobs as 32- OR 64-bit; reader must switch pointer width or duration reads 0.0s
metadata: 
  node_type: memory
  type: project
  originSessionId: 49266c47-c071-46be-89fa-1f293d3ccae0
  modified: 2026-07-25T05:21:48.984Z
---

GW2 MODL ANIM chunks embed a serialized `granny_animation` whose internal
pointer width matches the **packfile's** width, not a fixed 32-bit. The packfile
flag `pfv & 4` selects it (gw2model exposes it as `ptr_`: 4 or 8). Older content
(e.g. springer fileId 1762011, Marjory&Kasmeer 904350) is 32-bit; newer content
(e.g. fileId 3442664) is **64-bit**.

The blob is **4-byte packed with p-byte pointers** (a 64-bit pointer sits at a
4-byte boundary, no 8-align padding). Key offsets vs pointer size `p`:
- animation: Name@0, Duration@p, TimeStep@p+4, Oversampling@p+8, TrackGroupCount@p+12, TrackGroups@p+16
- track_group: Name@0, VectorTrackCount@p, VectorTracks@p+4, TransformTrackCount@2p+4, TransformTracks@2p+8
- transform_track stride = **7p+4** (32 for 32-bit, 60 for 64-bit); Ori/Pos/Sca curve Object @ 2p+4 / 4p+4 / 6p+4
- Only two curve_data formats shift with p (scalar after an embedded pointer):
  DaK16uC16u/DaK8uC8u (KCC@8+p, KnotsControls@12+p) and DaK32fC32f (CC@8+p, Controls@12+p).
  All others (D4n, D3K, D9*, D3I1, constants, keyframes) have their sole pointer
  at a p-independent offset. Pointer VALUES read fine with u32() for both widths.

Symptom when read as 32-bit but actually 64-bit: Duration reads the Name
pointer's high dword (=0) → **"0.0s" / animation won't play**, and the track
stride is wrong → 0 tracks bound → pose stays at bind / "berantakan". This was
THE bug behind [[gw2-skeleton]]'s animation playback. Fixed 2026-07-12:
`castlemist::granny::parse(b,n,ptrSize)` in granny_anim.hpp threads `ptr_` from gw2model.

**Version discriminator (verified 2026-07-25):** the leaked full Granny 3D SDK
source (C:\Users\...\Documents\Granny-3D-SDK-main) is **v2.9.12.0**
(`granny_version.h` ProductVersion, branch //jeffr/granny_29/, CurrentGRNStandardTag
= 0x80000000+55 = **0x80000037**). GW2's statically-linked Granny checks type tag
**0x80000039** (0x80000000+57, seen as `-2147483591` in `GetFileInfo`/sub_140F9FE90
= granny_file_info.cpp) → GW2 Granny is 2 layout-revisions NEWER than the source.
Same codebase (GetFileInfo matches 1:1, source-path strings ..\source\granny_*.cpp
in exe), so 2.9.12.0 source is a valid RE map; verify offsets in IDA where tag differs.
Note: skeleton/bone/track_group struct layouts are UNCHANGED between tag 55↔57.

**Skeleton/bone offsets (64-bit, pack-4, verified in GW2 sub_140F9E3F0 =
SkeletonCountLODBones @ granny_skeleton.cpp):**
- skeleton (40B): Name@0, BoneCount@8, Bones(ptr8)@12, LODType@20, ExtendedData@24
- bone stride = **164B (41 floats)**: Name@0, ParentIndex@8, LocalTransform(transform,68B)@12,
  InverseWorld4x4(16f)@80, LODError@144, ExtendedData(variant16)@148
- transform (68B): Flags(u32)@0, Position(3f)@4, Orientation(quat 4f)@16, ScaleShear(3x3=9f)@32.
  ScaleShear is a FULL 3x3 matrix, not a scale vec3; Flags bits 1/2/4 = Has Pos/Ori/ScaleShear.
Likely cause of "weird skeleton": reader assuming natural 8-byte align (Bones would be @16) —
Granny 64-bit pointers sit on 4-byte boundaries.

Curve sampling MATH itself is a verified-faithful port of Granny 2.9 (SDK under
SDK/Granny/.../source); GW2 calls the real granny2_x64.dll so values match.

**Audit 2026-07-25 (granny_anim.hpp vs full 2.9.12.0 source):** every curve-format
offset (DaK16u/K8u, D3K, D4n, D9I1/I3, D3I1, constants) matches the source
`data_type_definition` incl. the p-shift for 64-bit; ScaleOffsetTable, D4n missing-
component reconstruct, B-spline coeffs (deg1/2/3), FindKnot(upper_bound+clamp),
quat-continuity — all faithful. `InitialPlacement` is correctly IGNORED: the source
never applies it in the pose-sampling path (only GetTrackGroupInitialPlacement4x4 API
+ basis_conversion), so castlemist must not apply it either. Two real fixes made:
(1) DaKeyframes32f had no TimeStep, so it sampled seconds as a frame index -> now
parse() materializes it into a degree-0 spline with knots at i*TimeStep.
(2) degree-0 hold was off-by-one (held controls[ub] = next keyframe); fixed to
controls[ub-1] (segment start), matching the degree>=1 endpoint. Both verified with
unit probes. Keyframes are rare in shipped GW2 content (quantized formats dominate),
so this mostly hardens edge cases, not the common path.

**Skeleton/bind-pose reader audit (2026-07-25): CLEAN, no fix needed.**
gw2model.hpp parseSkeleton reads bone fields via DYNAMIC type-def offsets (no
hardcoded packing), LocalTransform Pos/Ori(xyzw)/ScaleShear(3x3) + InverseWorld4x4.
computeBonePositions inverts InverseWorld (row-vector affine: worldPos = -t*inv(R))
for the bind origin. model_renderer compute_world composes parent-first (assumes
parent<i) and the skin palette = InverseWorld * AnimatedWorld (row-vector) -> bind
resolves to identity. All verified faithful to Granny BuildWorldPoseNoCompositeLOD
(granny_world_pose.cpp:271: linear iter reading WorldBuffer[ParentIndex], so Granny
itself REQUIRES parent<child ordering) and NoParentBone==-1 (granny_skeleton.cpp:21).
External skeleton ref (Skeleton.externalRef / fileReference) resolution IMPLEMENTED
2026-07-25 in castlemist build_model_preview (entry_extractor.cpp): when a .modl has
no inline rig but externalRef!=0, load_modl_bytes_by_fileid(dat, externalRef) ->
Extractor(rigBytes, tpl).extract() -> use its skeleton.bones for the joints AND
re-map every mesh.boneBindingSkelIndex via castlemist::model::tokenizeBoneName (gw2model's own
resolveBoneBindings ran against the absent inline skeleton, so all -1 -> mesh would
stay unskinned). Done up front (before the mesh loop) so both skin bindings and joints
use the external rig. Full `ninja -C build` links castlemist.exe clean. Also fixed pre-existing build
drift in castlemist/CMakeLists.txt (2026-07-25): added include dirs ../minieditor and
../minieditor/external/glm-1.0.3 (for cloth_sim.h -> GrCloth.h -> glm) AND added
../minieditor/GrCloth.cpp to the sources (cloth_sim.cpp calls gw2::GrCloth::init/update;
it was declared-but-unlinked). GrCloth.cpp only needs GrCloth.h + glm.
Bone tracks are named identically to skeleton bones ("bone:COG"); matching is
exact-string. Engine indexes bones by token64 (tokenizeBoneName == engine
sub_140E3B5E0: 5-bit pack a-z/A-Z, 12 chars, 2-digit suffix in top nibble).
