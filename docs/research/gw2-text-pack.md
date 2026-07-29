---
name: gw2-text-pack
description: GW2 text/localization subsystem — how cntc content names/voices resolve through the txtm/txtv/txtV packs
metadata: 
  node_type: memory
  type: reference
  originSessionId: 56a747ce-ba42-4311-a680-fd5e2bdb9c40
  modified: 2026-07-25T15:14:59.925Z
---

GW2 localization is a family of PF packfiles that [[gw2-strs-decrypt]] string tables plug into, and that [[castlemist-app]] cntc content objects point into by textId.

- **txtm** container / chunk `txtm` v0 = `TextPackManifest` `{dword stringsPerFile; array_ptr languages -> TextPackLanguage{ array_ptr filenames -> filename }}`. THE index: a global textId maps to strs file `textId / stringsPerFile` and string slot `textId % stringsPerFile`, per language. `filenames[]` are the fileIds of the strs entries. (1 entry: base 5133.)
- **txtv** container / chunk `txtv` v0 = `TextPackVoices` `{array_ptr voices -> {dword textId; dword voiceId}}`. Maps a spoken textId → its voice audio id (resolves to ASND/ABNK). (1 entry: base 69220.)
- **txtV** container / chunk `vari` v0 = `TextPackVariants` `{array_ptr variants -> {dword textId; array_ptr variantTextIds(dword)}}`. Maps a textId → gender/race/profession variant textIds. (1 entry: base 156894.)
- **txtp** / `TextPackPasswords`; **eula** / `PackEulaV0` `{array_ptr Language->{byte Language; wchar_ptr Text}; byte Version}` (URLs to the user agreement, 3 entries).

Relationship: txtm/txtv/txtV are all keyed by the same global **textId** (txtm resolves textId→localized string in a strs file — VERIFIED, e.g. English file 0 = fileId 2440724 yields "Wildflame Caverns"; txtv→voice audio id; txtV→gendered variant textIds).

**CORRECTION (2026-07-25): the cntc→txt* link is NOT established.** An earlier note here claimed cntc content objects "store textIds resolved through txtm" — that was inference, and testing contradicted the specific mechanism assumed. `PackContent.stringIndices` (the obvious candidate) is a **`PackContentStringIndexFixup`** — confirmed by the client's own reflection table at `0x142360f20` — and dereferencing it over a live cntc yields values **0..21996, exactly matching the file's own `strings` count of 21997**, i.e. an index into the cntc's INTERNAL codename table (`vl8Av.4gynM`), not a global textId. All 32 cntc in this build have `typeInfos`/`namespaces`/`fileRefs` count **0** (schema lives compiled into the client, not reflected as pack structs), so the per-content-type field holding a name textId has not been located. Reaching localized cntc names needs IDA work on the individual content-type C++ structs. See [[castlemist-app]] for what the browser shows instead (codenames). Wire format = self-relative pointers, ptr width from `pfVersion & 4` (v5=64-bit, v1=32-bit); array_ptr = {u32 count, ptr rel}, tightly packed (no alignment padding — e.g. PackEulaLanguageV0 stride = 1+ptr). filename decode = fileId = 0xFF00*hi + lo - 0xFF00FF.

Also: **ABIX** container / chunk `BIDX` v0 = `BankIndexDataV0` (audio bank index, per-language bankFileName filerefs, PF **v1/32-bit**); **CSCN** = cinematic `SceneDataV0..V36`. castlemist parses eula/ABIX/CSCN/txt* into readable text-preview summaries.
