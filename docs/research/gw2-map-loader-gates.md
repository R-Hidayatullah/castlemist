---
name: gw2-map-loader-gates
description: "CMapLoader (VdfLoad.cpp) state machine RE'd: 13 states, which ones are server-gated, and the exact patch points to force an offline map load without login"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 1aa4cf46-3c9a-41e8-8004-2c436cfa135b
  modified: 2026-07-26T05:12:48.929Z
---

CMapLoader state machine, RE'd from Gw2-64-disabled-aslr.exe (base 0x140000000), source `Code\Gw2\Game\View\Default\VdfLoad.cpp`. This is what actually gates "being inside a map" — NOT the login flow. Related: [[gw2-login-flow]], [[gw2-map-scene]].

**Trigger (server msg):** `sub_14140E8E0` = `MsCli OnRecvLoadMap` (MsCliApi.cpp). Msg field layout (a2 = msg buf): **u16 mapId @+30**, ctx blob @+2, __int128 @+14 (spawn pos/orient), qword @+33, dword @+41 (→ sub_1410AD940). Calls `sub_141414A80(missionCli, a2+2, &blob16, ..., 1)` → scene mgr `sub_140948D90()` vtbl+400. **Fabricating this one message is the whole "mockup data" surface** for map entry.
Related: `sub_14140C9C0` = mapType `sub_1412DBBF0()` → gameType `sub_141411E70(mapType)`; GAME_TYPE_CHARCREATE==1, GAME_TYPES==22, MAP_TYPES==20.

**Update fn = `sub_14094FB80(CMapLoader* a1, int dtMs)`**, `m_state` @ **a1+704** (`*((_DWORD*)a1+176)`):
| val | state | impl |
|---|---|---|
|1|STATE_LOAD_CONTENT|inline|
|2|STATE_LOAD_MANIFEST|inline (sets timeout 300000 @a1+740)|
|3|**STATE_SERVER_WAIT**|inline — **server-gated**|
|4|error|—|
|5|STATE_MAP_DOWNLOAD|sub_1409504C0|
|7|STATE_MAP_STREAM|sub_140950CD0|
|9|STATE_MODELS_STREAM|sub_140951150|
|0xA|STATE_MAP_ASSET_STREAM|inline|
|0xB|**STATE_AGENT_STREAM**|sub_1409500F0 — **server-gated**|
|0xC|STATE_READY_WAIT|sub_1409514B0|
|0xD|STATE_READY|—|

**Only TWO states truly need a server:**
1. **SERVER_WAIT (3)**: advances iff flag **a1+708** (`*((_DWORD*)a1+177)`) != 0; else counts down a1+740 (300s) → state 4 (error). Patch = force flag / NOP branch.
2. **AGENT_STREAM (0xB)**: needs `(*(gGame+152))->vtbl+96` truthy, `sub_140951F70(a1+77)` (all agents/chars settled) != 0, and `sub_141107320()->vtbl+64` (CharLoadingCount) == 0.

**Not server-gated:** MAP_DOWNLOAD reads map content **from the local .dat** via a1[67] (vtbl+72 = ready); only *encrypted* maps need key **a1[99]** when `mapDef+152 & 0x20000000` — and `EncryptionKey` @0x14210e768 is a **local MapDef content field**, so the key is likely local too (verify). READY_WAIT (0xC) is a pure countdown on a1+768, no net. MAP_ASSET_STREAM (0xA) also calls `sub_14140CC40()` (mission-cli check) before advancing.

**BEST INJECTION POINT — `sub_140951E70` = `CMapLoader::OnServerReady`** (VdfLoad.cpp:426). Call it instead of byte-patching SERVER_WAIT. Signature `(CMapLoader* a1, a2, mapDef a3, a4, a5, int a6, a7, a8)`; asserts `m_state ∈ {1,2,3}` and `m_mapDef == a3`; writes `a1+528=a8`, **`a1+680`/`a1+688` = 12 bytes from `a5` (spawn pos, 3 floats)**, **`a1+792 = a7` (map decryption key)**, `a1+692=a6`, **`a1+708 = 1` (releases SERVER_WAIT)**, then `sub_1409B7E10(a1+8, sub_140DB8620(a2), 260)` = 260-byte map-server conn blob. This is the entire "mockup data" surface.

**Local (no-server) map resolution — `sub_14094DA60`** (VdfContext.cpp:2577), called from `sub_14094C0E0`: gets fallback mapDef via `sub_1412D7F80(585, 45, ...)` (**content table 45 = MapDef, fallback mapId 585**), then optional WorldState override (`sub_140E459A0`, WorldState.cpp) behind **param FLAG 49** — flags 0x80000 = numeric mapId, 0x100000/0x200000 = by-name lookup; finally calls `sub_14140C9C0(mapId, ...)`. Content lookup primitives (all local): `sub_1412D7F80(id, 45, out)`; content mgr `*(sub_1409B1760()+224)` vtbl+80 = by-name, vtbl+104 = by-string-key, vtbl+112 = `(45, mapId)`. See [[gw2-param-system]] — FLAG 49 is unnamed/dev-only (set `dword @0x14289B934 = 1`), and `-map` is a dead param.

**VALID mapId LIST — `mMet` fileId 198302 (baseId 790338), chunk "Main".** `sub_1412DCEF0(&unk_1428AB600, mapId)` = `MapTypeFromMapId`: lazily `PackFileLoad`s a fileRef at `0x14220DE7C`, takes chunk `'Main'` (0x6E69614D), binary-searches a **sorted array of 3-byte records `{u16 mapId; u8 mapType}`**; returns **20 (MAP_TYPES) = invalid**. Chunk layout on disk: `u32 count @+28`, `s64 self-rel ptr @+32` → array; struct is packed `{u32 count; u64 ptr}` (code reads ptr at **+4**, not +8). Parsed: **1221 maps, mapId 1..1625**. Type histogram (community enum names): 0 AutoRedirect×1, **1 CharacterCreate×1 (= mapId 1)**, 2 PvP×38, 4 Instance×1016, 5 Public×102, 7 Tutorial×7, 9 EternalBattlegrounds×1, 10/11/12 Blue/Green/RedHome×2 each, 14 JumpPuzzle×1, 15 EdgeOfTheMists×1, 16 PublicMini×46, 18 WvWLounge×1. **FileRef decode (`sub_140DB3DA0`, FileArchive.cpp): `fileId = 0xFF00*w[1] + w[0] - 0xFF00FF`, requires w[0],w[1] >= 0x100.**

**CORRECTION — `sub_1412D7F80(585, 45, out)` args are NOT (mapId, ...).** It is `CfgTable.cpp` config lookup: a1=**config id**, a2=contentType (45). Table `unk_1425E4960`, **stride 72**, `{+0 config, +4 contentType, +8 GUID(16)}`, **runtime-populated (static image is zeros)**. Resolved def's **mapId is at def+40**; `+16` = type (45, or 149 = configuration wrapper).

**MapDef encryption (medium confidence — `sub_140FD3E80`, ArtSerializer.cpp, art-catalog path):** MapDef record has fields `Flags` (bool sub-field **`IsEncrypted`**), **`FileMap`** (map file name string) and **`EncryptionKey`** (u64 via vtbl+80). Keys registered into a runtime dict by `sub_140FD0680(catalog, fileMapName, key)`, keyed by map file name. So the key is **content data, not server-sent** — but this reads the `Gw2.Art`/`Gw2.Common.Map` content DB, so confirm it resolves in a retail install before relying on it.

**RUNTIME-VERIFIED (live capture, hook/gw2_maploader_probe.cpp, mapId 23 public map, ASLR-disabled exe → runtime addrs == IDB addrs exactly):**
- Full observed sequence: `1 LOAD_CONTENT → 2 LOAD_MANIFEST → 3 SERVER_WAIT → [OnServerReady] → 5 → 7 → 9 → 10 → 11 → 12 READY_WAIT → 13 READY`. States 4/6/8 never entered.
- **`loader+708` IS the gate, confirmed**: 0 throughout SERVER_WAIT, `OnServerReady` flips it to 1, next tick state 3→5. `+740` starts at 300000 and only decrements in state 3. `+768` READY_WAIT = 1000ms pure local countdown (ran to -22 → READY).
- **Real OnServerReady args** (a1=loader, a3=mapDef): **`a6` is a FLOAT (~0.394), not int** — Hex-Rays mistyped it; `a8` = **`sub_141414B50`** (the MsgSendMapLoaded completion callback); `a5` → 3 floats spawn pos, e.g. `(-37185.941, 20961.457, -3823.928)`; `a4` unreferenced in the body.
- **`a7` (encryption key) = NULL for a normal map**, and `loader+792` stays 0 the whole load. mapFlags for mapId 23 = `0x40000200` (bit `0x20000000` clear = not encrypted). So the key is only ever non-NULL for encrypted maps — unencrypted maps need nothing here.
- Minimal M2 forge is therefore **smaller than calling OnServerReady**: only `+708 = 1` gates progress; everything else the function writes is data. The 260-byte blob (`a2` → `sub_140DB8620` → `loader+8`) can likely be skipped for unencrypted maps — unverified whether anything reads `loader+8` later.
- Still unproven offline: **AGENT_STREAM (11)** passed here only because a live server was streaming agents.

**SECOND LIVE CAPTURE (mapId 15) + probe findings:**
- **The `PeekMessageW` (main message-pump) thread DOES carry the game TLS context** — verified: `gameCtx` and its `+152` world / `+216` content / `+224` contentMgr / `+456` missionCli all non-NULL there. So `PeekMessageW` is a valid game-thread tick that also runs at the login screen, unlike `CMapLoader::Update`.
- **CMapLoader returns to state 0 (IDLE) after READY**, and the **same loader object is reused across map changes** (identical pointer for mapId 23 → 15). It is not per-map.
- **`a6` is NOT a constant**: 0.394 (mapId 23) vs 3.795 (mapId 15) — per-load data, do not hardcode. `a7` = NULL on both loads (second independent confirmation).
- Per-state durations (mapId 15): SERVER_WAIT 2485ms, MAP_DOWNLOAD 125, MAP_STREAM 562, MODELS_STREAM 2532, MAP_ASSET_STREAM 218, AGENT_STREAM 1266, READY_WAIT ~1000. Client-side work after the gate ≈ 5.5 s.
- **Calling `sub_14094DA60` (LocalMapResolve) from the PeekMessageW hook while in-game at IDLE does NOT crash the game** — it throws, the stack unwinds past the caller, the game catches it and continues normally (map changes still work afterwards). Do not read a truncated log as a crash; confirm with a process check.

**THREADING CONSTRAINT for any injected DLL:** the game context accessor `sub_1409B1760` is **TLS-based** (`mov ecx, cs:TlsIndex; mov rax, gs:58h; mov rax,[rax+rcx*8]; mov rax,[rdx+rax]`, rdx=0x10). Anything reaching it (map load, content lookup, `sub_14094DA60`, `sub_14140C9C0`) returns garbage or crashes if called from a DLL-owned thread. **All game calls must run on a game thread** — do them inside a hook such as `CMapLoader::Update`, never from the injector/hotkey thread.

**Consequence:** an offline "game as map viewer" does NOT require the 3-gate login emu — inject a DLL, call the map-load entry with a synthetic mapId/spawn, then neutralize ~2 gates (a1+708, AGENT_STREAM's 3 checks) + supply a free camera in place of a player agent. Far shorter than a server emulator. `sub_140D86830(0/1)` = loading-screen toggle, called at SERVER_WAIT exit and READY_WAIT completion.
