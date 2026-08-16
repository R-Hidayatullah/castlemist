---
name: gw2-strs-crypt-symbol-map
description: "Named IDA symbols for the strs text pipeline — RC4 (CptRc4), the 8→20 byte key expansion, the bit-unpack record decoder and the runtime key map. Both retail hook signatures still match this build."
metadata:
  node_type: memory
  type: reference
  modified: 2026-08-16
---

# strs decrypt / encrypt — symbol map

Applied to `gw2decomp/Gw2-64.exe.i64` on 2026-08-16. Confirms and re-anchors
[[gw2-strs-decrypt]], whose addresses are from an older build. Same method as
[[gw2-cmp-img-symbol-map]] and [[gw2-chatlink-token-hash-map]].

Source-path anchors: `0x1420A3DD0` `Arena\Services\Crypt\CptRc4.cpp`,
`0x1421346C0` `Gw2\Engine\Text\TextDecode.cpp`, `0x142134E50` `…\TextParser.cpp`.

## First: there is no separate "encrypt" function

RC4 is symmetric. `CptRc4_Crypt` is the whole cipher, and the CptRc4 vtable holds the
**same pointer in slot 0 (encrypt) and slot 1 (decrypt)**. Encrypting a strs record and
decrypting one are the identical call. Nothing else to find on that side.

What the client genuinely lacks is a *key generator* — see the bottom of this note.

## The pipeline

```
TextDecode_DecodeString (0x1410DBD50)          TextDecode.cpp:217   <- start here
  |
  |-- TextParser_Validate            check the source buffer parses
  |-- TextParser_FindEmbeddedKey     a parent string may carry the child's key inline
  |-- Arena_OpenAddrFindSlot         else look up the runtime key map at textContext+512
  |                                    24-byte entries { u32 id@0, u64 key8@8, u32 occupied@16 }
  v
TextDecode_DecodeRecord (0x1410DBA40)
  |-- Cpt_ExpandKeyTo20    8-byte password -> 20-byte RC4 key
  |-- CptRc4_Create        KSA
  |-- CptRc4_Crypt         PRGA over the payload
  v
bit-unpack -> wide string
```

| addr | name |
| --- | --- |
| `0x1410DBD50` | `TextDecode_DecodeString` — entry, resolves the key |
| `0x1410DBA40` | `TextDecode_DecodeRecord` — decrypt + bit-unpack |
| `0x1410DB630` | `TextDecode_Finish` |
| `0x140DAB0F0` | `CptRc4_Crypt` — **encrypt and decrypt** |
| `0x140DAAD10` | `CptRc4_Create` — alloc 272-byte object + KSA |
| `0x140DAB1D0` | `CptRc4_SetKey` — KSA in place |
| `0x140DAAE60` | `Cpt_ExpandKeyTo20` — 8B → 20B |
| `0x1420A3D90` | `CptRc4_Vtable` |
| `0x1410E1D30` | `TextParser_Validate` (`CParser::Validate`) |
| `0x1410E02F0` | `TextParser_FindEmbeddedKey` |
| `0x142134680` | `TextDecode_SymbolTable` |

### Record layout, re-read off `TextDecode_DecodeRecord`

```
u16 length      total, must be >= 6
u16 baseChar
u16 rangeBits   only the low byte is used
payload[length - 6]
```

Decrypt first if a key is present, then:

- `baseChar == 0 && rangeBits == 16 && length even` → the payload **is** raw UTF-16, passed
  straight through, no unpacking and no key needed.
- otherwise bit-unpack LSB-first, refilling while the accumulator holds ≤ 0x18 bits:

```
sym = acc & ((1 << rangeBits) - 1)
sym == 0      -> NUL, end of string
sym in 1..31  -> TextDecode_SymbolTable[sym - 1]
sym >= 0x20   -> (wchar)(sym + baseChar - 32)
```

`TextDecode_SymbolTable` = `"0123456strnum()[]<>%#/:-'\" ,.!\n"` (31 chars).

### `Cpt_ExpandKeyTo20` is not SHA-1

It fills 20 bytes cyclically (`dst[i] = key[i % keyBytes]`), XORs any key bytes past 20
(never happens for the 8-byte strs keys), then runs **one** SHA-1-*shaped* round over the
five dwords — ROL5/ROL30 with constants `-1615554381, 1722862861, 0x22222222, 0x7BF36AE2,
-214083945, 0x59D148C0, -696916869, -1269579175`. One round, not eighty, and the constants
are not SHA-1's. It has to be ported literally; no crypto library will reproduce it.

## The runtime key map

| addr | name | notes |
| --- | --- | --- |
| `0x1410D79A0` | `TextKey_Insert` | `(textId in RCX, key8 in RDX)` — **the capture hook point** |
| `0x1410E2800` | `TextKey_LoadFromChunk` | bulk txtp path: `{u8 count@2, ptr@3}` → 12-byte records `{u32 id@0, u64 key8@4}` |
| `0x1410D6A40` | `TextKey_GrowMap` | rehash when `3*(count+1) >= 2*size` |
| `0x140432910` | `Arena_OpenAddrFindSlot` | generic open-addressing lookup, not text-specific |

Map lives at `textContext + 512`, header `{u32 size@0, u32 count@4, entries@8}`.

**Every key the client ever learns passes through `TextKey_Insert`** — server messages,
TextPack chunks, and control codes embedded in parent strings all converge there. That is
what makes it the right hook.

There is also a web-UI path: `GameWeb::CBrowserBindings::BindSetTextEncryptionKey`
(`GwBindings.cpp:743`, handler `0x14095FEE0` → `0x140962C50`), which takes a textId and a
key string from JavaScript. It reaches the map through a virtual call, so it was not
traced to the insert statically — but it ends in the same place.

## Both retail hook signatures still match

The two byte signatures in [[gw2-strs-decrypt]] were tested against this build with
`find_bytes`, and each returned **exactly one hit**:

| signature from the note | resolves to |
| --- | --- |
| `85 C9 0F 84 ?? ?? ?? ?? 48 89 74 24 20 89 4C 24 08 57 …` | `0x1410D79A0` `TextKey_Insert` |
| `40 53 55 57 48 83 EC 20 48 8B EA E8 ?? ?? ?? ?? 0F B6 55 02 …` | `0x1410E2800` `TextKey_LoadFromChunk` |

So `gw2app/hook/` did **not** need re-deriving for this build — a useful data point on how
stable these two functions are across patches, and a reminder that byte signatures survive
where absolute addresses do not.

## Unchanged conclusion: keys cannot be computed

Nothing here weakens the finding in [[gw2-strs-decrypt]] that the keys are not derivable.
Every path in the client only ever *reads* a key as data — `TextKey_Insert` from a message,
`TextKey_LoadFromChunk` from a chunk, `TextParser_FindEmbeddedKey` from a parent string.
There is no key-derivation function anywhere in the pipeline, because generation happens
server/build-side. Keys can only be captured.

## Not done

- `TextDecode_1410DB5B0` and `TextDecode_1410DC440` are named by module only.
- `CptRc4_140DAACF0` (24 bytes, one xref) — role not established.
- The web-UI binding's virtual call to the map was not resolved.
- The TextPack manifest readers (`0x1410D8EE0`, `0x1410D9130`, the
  `m_stringsPerFile` / `index < m_stringsPerFile * m_fileArray[language].Count()` asserts)
  were located but not named.
