# gw2emu — client-only GW2 server emulator (scaffold)

Goal: run the GW2 client against a **local emulator** with no connection to the
real ArenaNet servers (no live traffic → nothing to ban). Approach chosen:
**patch the crypto out of the client** so the AuthSrv MsgConn channel is
plaintext, then speak that plaintext protocol from a small static-linked C++
server. Milestone ladder: reach the **login screen** offline first, then grow
message handling from there.

This is a scaffold: the transport + framing are real and match the
reverse-engineering; the server-authoritative game logic is intentionally a
set of stubs to grow into.

## Reverse-engineering basis (Gw2-64-disable-aslr.exe, imagebase 0x140000000)

Two stacked layers, established by RE:

| Layer | Detail |
|---|---|
| Socket TLS/RSA + pinned **StartCom** cert | The **NCPlatform HTTPS portal**; issues a 20-byte session key. *Not* on the AuthSrv message path. |
| MsgConn RC4 | AuthSrv transport. `seed = PRF(sessionKey20, bakedConst @ 0x1421117C0)`; textbook RC4 (`sub_140FEB950`); mode `conn+264` 2→3. |

Receive dispatcher `sub_140FE6FE0`: mode 1 = plaintext stream, mode 2 =
raw `[type][len]` connect, mode 3 = RC4 → `DispatchStream` (`sub_140FE62B0`).
Wire message = `[msgId u16 LE][fields per def]`, no length prefix.

## Patches (`gw2patch`)

| # | Patch | Status |
|---|---|---|
| 2 | **RC4 → identity** — `0x140feb9ac` `32→8A` (`xor dl,[..]`→`mov dl,[..]`). Whole channel becomes plaintext. | ✅ concrete, verified bytes |
| 1 | Endpoint strings → `127.0.0.1` (`ArenaNetworks.com`, `NCPlatform.net`) | ⚠ best-effort string replace |
| 3 | Force MsgConn mode 1 (alt to #2) | 🔧 documented, needs call-site disasm |
| 4 | Portal→AuthSrv open trigger (run fully offline w/o portal) | ⛔ TODO — next RE step |

See `src/patcher/patches.h`. Only patch **a copy** of the client:

```
bin\gw2patch.exe "Gw2-64-disable-aslr.exe" "Gw2-emu.exe"
```

## Emulator (`gw2emu`)

- Listens on `127.0.0.1:<port>` (default 6112), plaintext MsgConn.
- On connect: sends `[00][16][20×00]` to push the client mode 2→3.
- Splits the stream into messages using the registered `MsgDef`s
  (`src/emu/protocol.cpp`) and logs them; unknown ids are hex-dumped.
- `src/emu/msgconn.cpp` implements the def-driven field codec matching the
  client's `MsgUnpack` type codes (`src/emu/msgdef.h`).

```
bin\gw2emu.exe 6112
```

## Build

MinGW-w64 `g++` on PATH, then:

```
build.bat
```

Produces static binaries in `bin\` (`-static -static-libgcc -static-libstdc++`,
`-lws2_32`) — no runtime DLL deps.

## Next steps

1. **Patch #4**: find the producer of the GcSrv "open AuthSrv" command
   (callers into `sub_14023F920` case 2) to trigger the connection offline with
   a loopback address + dummy key — closes the last gap to a login screen.
2. **Capture pre-login defs**: instrument `DispatchStream` on a real auth login
   to record the server→client message defs, add them to `protocol.cpp`, and
   craft responses until the Coherent login HTML renders.
3. Grow handlers upward (character select → world enter).
