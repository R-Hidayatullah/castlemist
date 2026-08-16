# tools/

Everything that dumps, converts or probes. None of it is needed to run
castlemist -- the application reads a `.dat` on its own -- but most of what is
in [`dumps/`](../dumps) and most of what is written up in
[`docs/research/`](../docs/research) came out of these.

Four of them build with the project. The rest have their own instructions
below, because they need a running game process, IDA, or a Python interpreter.

## Built by CMake

Switch the whole set off with `-DCASTLEMIST_BUILD_TOOLS=OFF`.

| tool | what it does |
| ---- | ------------ |
| `gw2index`    | scans a `Gw2.dat` once into a queryable SQLite index |
| `gw2dat_cli`  | extract / decompress / decode / re-compress single entries; JSON on stdout |
| `gw2viewer`   | prototype model + texture viewer, hand-written HLSL |
| `gw2gsviewer` | the same viewer, but driving the game's own DXBC shaders |

### gw2index

```bash
./build/debug/bin/gw2index.exe --dat "C:/Program Files (x86)/Steam/steamapps/common/Guild Wars 2/Gw2.dat" --out dumps/index/gw2_index.db --template dumps/packfile/gw2_packfile.json --threads 16
```

Records, per MFT entry and without storing any payload: the baseId and every
fileId that resolves to it, the MFT header, the sniffed type and packfile
container fourcc, the chunk list with the struct-template variant that applies,
and three sizes -- on disk, after the CRC32 strip, after decompression.

It also records *verified* compression truth. The dat contains entries flagged
compressed that are not, so the indexer actually attempts the method-0
decompress and stores the answer rather than trusting the flag.

Resumable: a re-run skips entries whose `offset|size|crc|flag` fingerprint is
unchanged, so re-indexing after a game patch costs seconds rather than a full
pass. `--full` forces everything. Interrupting it is safe.

### gw2dat_cli

```bash
./build/debug/bin/gw2dat_cli.exe lookup --dat <Gw2.dat> --file-id 1247272
```

Subcommands: `info`, `list`, `lookup`, `resolve`, `extract`, `texture`,
`parse`, `sniff`, `compress`, `decompress`, `encode-texture`. Output is JSON,
which is what makes it the thing the MCP servers under [`mcp/`](../mcp) shell
out to.

### The viewers

They predate `src/render` and are kept deliberately. When a rendering question
comes up -- is this material picking the wrong pass, is this vertex format
being read right -- a 1500-line single file is somewhere you can bisect. The
full application is not.

## Python: dump and convert

Run with any Python 3.10+; no packages beyond the standard library unless the
script says otherwise.

| directory  | script | what it does |
| ---------- | ------ | ------------ |
| `structs/` | `dump_gw2_structs.py` | pulls the packfile chunk struct definitions out of the client via IDA |
| `structs/` | `gen_gw2_json.py`     | turns that dump into `dumps/packfile/gw2_packfile.json` |
| `shaders/` | `extract_exe_shaders.py` | carves the ~2275 bgfx shader blobs embedded in `Gw2-64.exe` |
| `shaders/` | `parse_bgfx_shaders.py`  | decodes those blobs into DXBC plus their uniform tables |
| `strs/`    | `gw2_capture_textkeys.py` | captures per-stringId RC4 keys from a running client |
| `strs/`    | `gw2_decrypt_strs.py`     | decrypts packed `strs` entries with the captured keys |
| `strs/`    | `strs_decode.py`          | the bit-unpacking half: baseChar / rangeBits |

`structs/dump_gw2_structs.py` is the one with a prerequisite: it runs inside
IDA against the client binary. Everything downstream of it -- named packfile
fields in the info panel, model extraction, the indexer's chunk variants --
depends on its output, which is why `dumps/packfile/gw2_packfile.json` is worth
regenerating after a game patch.

See [`shaders/SHADER_PIPELINE.md`](shaders/SHADER_PIPELINE.md) for how the
shader dump fits together.

## The IDA symbol backup

| script | what it does |
| ------ | ------------ |
| `ida_restore_symbols.py` | replays the whole symbol set into an IDB: enums, struct headers, function names and comments, data symbols, inline comments, **local variable names, pseudocode labels, and both folder trees** |
| `ida_apply_cmp_img_names.py` | the address-free path — re-derives the Compress/image function map from the binary's embedded Perforce source paths after a patch |
| `gw2_ida_symbols.json` | the data: 324 functions, 219 comments, 863 locals, 85 labels, 29 data symbols |
| `structs/gw2_ida_types.h` | bgfx, GrFvf, DDI texture, ATEX and AMAT-shader-chunk types |
| `structs/gw2_ida_types_granny.h` | the granny 2.9.12 structs, all `#pragma pack(1)` |
| `structs/gw2_ida_types_subsystems.h` | Archive3 MFT rows, PF packfile headers, model load stages, scene chatter lines |

`restore_folders()` rebuilds the Functions-window and Local-Types folder trees
from prefix rules in the script. Those rules key off **names**, not addresses, so
that one piece survives a client patch — run it on its own after re-anchoring.

Local variable names and label names live **only** inside the `.i64` — nothing else
in the repo can regenerate them, which is the whole reason the JSON exists.

```
File > Script file... > ida_restore_symbols.py
idat -A -S"ida_restore_symbols.py" Gw2-64.exe.i64      # headless
```

After doing more RE, dump it back out from the IDA console:

```python
import ida_restore_symbols as s; s.export_symbols()
```

`export_symbols()` finds functions by name prefix rather than from a hand-kept
address list, so newly named ones are picked up automatically — but a name outside
`NAME_PREFIXES` is invisible to it and will be lost on the next restore. Add the
prefix when you add the name.

The JSON is keyed by absolute address and therefore belongs to **one build**. After a
client patch use `ida_apply_cmp_img_names.py`'s `rebuild_from_source_paths()` to
re-anchor, then re-export. The two `.h` files are the exception: they are keyed by
field offset, not address, so they survive a patch as long as the structs do.

## Probes: code that runs inside the game

These attach to a live client, so read what they do before running them.

| directory | what it is |
| --------- | ---------- |
| `hook/`   | MinHook-based in-process probes: `gw2_maploader_probe.cpp` traces the map-loader state machine, `gw2_textkey_hook.cpp` captures string-table keys, `inject.cpp` is the loader |
| `host/`   | `gw2host.cpp` hosts the client's own decode routines out of process, so our decoders can be diffed against the real ones |
| `emu/`    | a client-only server emulator scaffold: patch the crypto out of the client, then speak the plaintext MsgConn protocol locally. See `emu/README.md` |

Build the hook probes with MinHook from `external/`:

```bash
g++ -shared -O2 -m64 -static -static-libgcc -static-libstdc++ -I ../../external/minhook-master/include tools/hook/gw2_maploader_probe.cpp ../../external/minhook-master/src/*.c ../../external/minhook-master/src/hde/hde64.c -o gw2_maploader_probe.dll -lpsapi
```

## Reverse-engineering aids

`templates/` holds the things that are loaded into other programs rather than
run: `gw2_pf_32bit_pointer.bt` and `gw2_pf_64bit_pointer.bt` are 010 Editor
templates for packfiles at either pointer width, and `parseANStructs.idc` is
the IDA script that walks ArenaNet's struct definition tables.
