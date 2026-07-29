---
name: gw2-param-system
description: "GW2 command-line param table RE'd (Param.cpp): 194 entries, 3 classes, exact storage globals + full name list; -map exists but is never read"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 1aa4cf46-3c9a-41e8-8004-2c436cfa135b
  modified: 2026-07-26T04:34:12.765Z
---

GW2 command-line parameter system, `Code\Gw2\Game\Param\Param.cpp`, from Gw2-64-disabled-aslr.exe (base 0x140000000). Related: [[gw2-map-loader-gates]].

**Table:** `0x14213D030` .. `0x14213E248`, **194 entries, stride 24**: `+0 flags(dword)`, `+8 name(wchar_t*)`, `+16 id(dword)`. Parser = `sub_1410E3000`. Class = `flags & 0xFF00`.

| class | flags | id range | storage | getter |
|---|---|---|---|---|
| FLAG (bool) | 0x0000 | 0..122 | `dword_14289B870[id]` | `sub_1410E1C70(id)` |
| STRING | 0x0300 | 123..179 | `0x14289BA60 + 512*(id-123)` (256 WCHAR) | `sub_1410E1CE0(id)` |
| VALUE (u32) | 0x0400 | 180..195 | `dword_1428A2C60[id-180]` | `sub_1410E1D20(id)` |

**Named FLAGs (id → global):** 1 allowinstall, 2 analyze, 3 analyzeFull, 4 autologin, 5 bmp, 6 yotd, 7 chunkedpatching, 8 copydat, 9 cuda, 14 diag, 15 dvdInstall, 16 dx9single, 17 exit, 19 forwardrenderer, 20 image, 21 isrelaunch, 22 localdat, 23 log, **24 maploadinfo**, 25 modelsnapshotfix, 26 multi, 28 nodelta, 29 nomusic, 30 nopatchui, 31 nopatch, 32 nosound, 33 noui, 34 novoice, 35 "32", 36 perf, 37 prefreset, 38 mce, 39 repair, 40 shareArchive, 41 uispanallmonitors, 42 uninstall, 43 uninstallSilent, 44 usecoherent, 45 useOldFov, 46 verify, 47 webdisablecache, **48 windowed**, **50 dx9**, **51 dx11**, 52 ignorecoherentgpucrash, 53 EpicPortal. Global addr = `0x14289B870 + 4*id`.

**Named STRINGs:** 123 assetsrv, 124 audio, 125 authsrv, 127 defaultcharname, 128 cinema, 129 clientPort, 130 dat, 131 framework, 132 lang, **133 map**, 134 mumble, 135 portal, 136 portalalias, 137 token, 138 tokenpassword, 139 umbra, 140 userid, 141 provider, 142 steamPath, 143 AUTH_LOGIN, 144 AUTH_PASSWORD, 145 AUTH_TYPE, 146-153 epic*, 176 localui, 177 partner. **VALUEs:** 180 fps, 181 mapviewdist, 182 patchconnections.

**Two key negative results (verified, don't re-derive):**
- **`-map` (STR 133, storage 0x14289CE60) is NEVER READ** — zero direct xrefs, and no `ParamGetString(133)` call site exists (swept all 25 callers of sub_1410E1CE0). Vestigial in the release build; passing `-map <name>` does nothing.
- **FLAG 49 has an EMPTY name** (sits between 48 `windowed` and 50 `dx9`) → dev-only, unreachable from the command line. It gates the local WorldState map-override branch in `sub_14094DA60`. To enable it, write `dword @ 0x14289B934 = 1` (== `0x14289B870 + 4*49`) from injected code.
