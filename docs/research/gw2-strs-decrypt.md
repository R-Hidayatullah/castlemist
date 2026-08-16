---
name: gw2-strs-decrypt
description: "How GW2 encrypts \"packed\" strs strings — RC4 (CptRc4) keyed by a per-stringId 8-byte key from a runtime map; full TextDecode.cpp pipeline + IDA addresses"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 6f515566-5d5a-4a00-b35a-d3b38336c11f
---

> **Addresses here are from an older build and no longer resolve (checked 2026-08-16).**
> Everything else in this note was re-verified against the current client and stands.
> Current symbols: [[gw2-strs-crypt-symbol-map]] — `TextDecode_DecodeRecord` @ `0x1410DBA40`,
> `CptRc4_Crypt` @ `0x140DAB0F0`, `Cpt_ExpandKeyTo20` @ `0x140DAAE60`,
> `TextKey_Insert` @ `0x1410D79A0`.
>
> **Good news for `gw2app/hook/`: both byte signatures below still match this build, one
> hit each.** They did not need re-deriving.

GW2 "packed"/password strs decryption, reversed from Gw2-64 (IDB `Gw2-64-disable-aslr.exe.i64`, imagebase 0x140000000). Answers why gw2mcp marks packed records `confirmed=false`.

**cntc text chunks**: fourccs `txtm`, `txtp`, `txtv`, `txtV`, `vari`. Registered/torn down in `sub_141901000` (atexit, via `sub_140206D90`) on 4 manager globals (txtm=`14256E9F8`, txtp=`14256EA00`, vari=`14256EA08`, txtv=`14256EA10`). `txtp` fourcc only appears at `0x141901044` (reg fn) + descriptor tree at `0x1420f3be0`; NOT in gw2index (idx_find_chunk txtp=0 hits — index covers only ARMF/locl). Lazy getters on singleton `off_14256E990`: txtm=`sub_1410CAA40`(+72), txtV=`sub_1410CAAF0`(+88), txtv=`sub_1410CABA0`(+96); loader `sub_1410CBC80` (Language.cpp:501, driver `sub_1413C3730`).

**Decode pipeline** (`Engine\Text\TextDecode.cpp`):
- `sub_1410CFF60` entry(stringId). Validate `sub_1410D5F40`; source scan `sub_1410D4500`→`sub_1410D4610` (TextParser.cpp) yields key v37; else map lookup `sub_1404336D0(ctx+512,&stringId)`.
- `sub_1410CFC50` = core decoder. Record header = `uint16 a4[]`: `[0]`=count(≥6), `[1]`=**baseChar**, byte`[4]`=**rangeBits**, payload=`a4+3` (len `a4[0]-6`).
  - If key `a5`: `sub_140D9F630(8,&key,ctx+552)` expand 8B→20B, then RC4.
  - Bit-unpack loop: pull rangeBits-wide symbol LSB-first; symbol≥0x20 → `char=symbol+baseChar-32`; 1..31 → table `a0123456strnum` @0x1420f3100; 0 → NUL.

**Cipher = RC4** (`Arena\Services\Crypt\CptRc4.cpp`):
- `sub_140D9F4E0(keylen,keyptr)` = RC4 KSA. 272-byte obj: vtable `off_142062AE0` + i/j + 256B S-box@+16. vtable[0]=PRGA.
- `sub_140D9F630` = key expansion, 8-byte seed → 20-byte digest (SHA-1-like rounds, ROL5/30, magic 0x59D148C0).

**The "password"** = per-stringId 8-byte key in a runtime open-addressing hashmap at `context+512` (`ctx = *(sub_1409B1940()+80)`). `sub_1404336D0` hashes stringId via `dword_141B82750`, 24-byte entries `{id, ?, key8@+8, occupied@+16}`. Map is populated at text-DB load from txtV/txtp/vari — absent from the standalone strs entry, so gw2mcp can't decrypt.

**Key-map population** (traced): map ctx+512 = open-addr hashtable, struct `{u32 mask/size@0, u32 count@4, entriesPtr@8}`, 24-byte entries `{u32 id@0, ?, u64 key8@8, u32 occupied@16}`.
- `sub_1410CBBB0(stringId, key8)` = single insert.
- `sub_1410D6A10(?, chunk)` = **bulk loader**, referenced only from DATA (registered chunk-read callback; descriptor at `0x1420f3b60`, paired writer `sub_1410D6B40`). Reads chunk `{u8 count@2, ptr@3}` → N×**12-byte records `{u32 stringId@0, u64 key8@4}`** → inserts each.
- `sub_1413026F0` = streamed/manifest registrar; parses a msg with flag bits in `*a1` (0x10000000/0x8000000/0x4000000/0x2000000/0x1000000) each pulling a stringId (file rec +128) + 8-byte key → `sub_1410CBBB0`.
- The keys live in **TextPack** chunks — schema field names at `0x1420f3b40`: `TextPackManifest`, `TextPackLanguage`, `stringsPerFile`, `languages`. So the per-stringId 8-byte password table is in the TextPack manifest/language packfiles (the cntc content set), NOT the strs entry.

**strs entry format** (verified on Gw2.dat fileId 2440959): after `strs` magic, each entry = `u16 length, u16 baseChar, u16 rangeBits, payload[length-6]`. Bit-unpack (LSB-first, refill acc to >24 bits): sym=acc&((1<<rangeBits)-1); sym 0=terminator; 1..31→table `"0123456strnum()[]<>%#/:-'\" ,.!\n"` (@0x1420f3100); ≥0x20→`chr(sym+baseChar-32)`.

**EMPIRICALLY VERIFIED (2026)**: reimplemented the exact bit-unpack on real packed records → produces the SAME garbage as gw2mcp. So bit-unpack is correct and packed payloads ARE RC4-encrypted (not a decoder bug). File 2440959: 1024 records = 334 raw-utf16 (plaintext, readable now) + 690 packed/RC4.

**Where the dat data lives** (Gw2.dat, index `gw2index/gw2_index.db`, 807k entries): TextPackManifest = container `txtm` (1 file, base 58570) = `{stringsPerFile, filenames[]→strs fileIds, languages[]}`. Variants=`txtV`/chunk`vari`/TextPackVariants (base156894). Voices=`txtv` (base69220). Content=`cntc` (32 files, each a `Main` chunk = nested packfile). The actual strings are `strs` files referenced by the manifest. **No `txtp` file/chunk exists in Gw2.dat** (no txtp container; cntc Main has no txtp; manifest has no passwords[]).

**Passwords come at RUNTIME** into map ctx+512 via: (a) cross-ref control codes in *parent* strings (`sub_1410D4610`); (b) msg/command dispatch `sub_1408F5810`→`sub_1410CBBB0` (vtable @0x141b64130) + streaming `sub_1413026F0`. But a logged-in client DOES receive MANY keys (for released content): a live capture yielded ~770 valid textId→key8 pairs just from login + a few maps.

**CAPTURE (verified working)**: run Gw2-64-disable-aslr.exe under IDA dbg (base 0x140000000). BP `sub_1410CBBB0` (0x1410CBBB0), read RCX=textId, RDX=key8 (registers only — clean). Sentinel textId 0xFFFFFFFF/key 0xFFFF... = skip. `sub_1410D6A10` is a GENERIC map loader (many callers) — noisy, prefer the insert BP or dump map ctx+512 directly (read via ida_dbg.read_dbg_memory, NOT ida_bytes). Script: gw2app/gw2_capture_textkeys.py → gw2app/textkeys.csv.

**FULLY DECRYPTED & VERIFIED (2026)** — real plaintext out. e.g. textId 1601→"The Cost of Victory", 611643→"Second Birthday Gift", 241299→"Survival Survivor". Recipe:
1. Manifest txtm base 58570: stringsPerFile=1024; **5 language blocks × 1169 files each** (lang0..4), each block = filenames[0..1168] in traversal order. Per-lang filenames[] are NOT fully consecutive (early indices run 2440724+i, then jump to a higher fileId range ~2467xxx for later-added content). So build map from the ACTUAL lang0 filenames[] array (traversal order, split blocks where idx resets to 0): `fileId = lang0_filenames[textId//1024]`, `record = textId%1024`. Keys are per-textId (language-independent): same key8 decrypts every language's payload; pick file from the desired lang block. lang0 = English (verified). Do NOT assume consecutive fileIds and do NOT flat-sort all filenames[] (languages interleave). Map dumped to gw2app/lang0_files.txt.
2. Extract that strs file (gw2_extract). strs entry = `u16 length, u16 baseChar, u16 rangeBits, payload[length-6]`. Walk entries to `record`.
3. key20 = expand(key8): bytes = struct.pack("<Q",key8); buf[i]=kb[i%8] for i in 0..19; then one custom mixing round (port of `sub_140D9F630`, SHA1-like, consts -1615554381/1722862861/0x22222222/0x7BF36AE2/-214083945/0x59D148C0/-696916869/-1269579175, ROL5/30). Full python in this session.
4. payload2 = RC4(key20).decrypt(payload)  (CptRc4 KSA `sub_140D9F4E0`, standard PRGA).
5. bit-unpack payload2 with baseChar+rangeBits (table @0x1420f3100 = "0123456strnum()[]<>%#/:-'\" ,.!\n").
Raw-utf16 records (baseChar==0 && rangeBits==16) are plaintext, need no key. Decryptor: gw2app/gw2_decrypt_strs.py (+ gw2app/lang0_files.txt, gw2app/textkeys.csv).

**Keys are NOT derivable/RE-able.** Analysis of 773 captured keys: all unique, uniform ~45-bit values (bits 45-63 always 0), no correlation with textId, and NO public hash matches (md5/sha1/crc32(textId) truncated = 0/773). Client has NO key generator — every path only READS keys as data (sub_1413026F0 from server msg; sub_1410D6A10 from txtp chunk; sub_1410D4610 embedded in parent strings). Generation is server/build-side (random per-string, or secret-keyed hash — indistinguishable, unreproducible). So keys can only be CAPTURED, not computed. They're stable per-textId across patches, so textkeys.csv stays reusable.

**Retail hook DLL (ASLR on, patch-resilient)**: gw2app/hook/ — signature-scans the two funcs (relative to GetModuleHandle(NULL) base), MinHook-hooks them, logs textId,key8 to textkeys_hook.csv. Sigs (each unique, verified 1 match):
- sub_1410CBBB0 (insert, RCX=textId RDX=key8): `85 C9 0F 84 ?? ?? ?? ?? 48 89 74 24 20 89 4C 24 08 57 48 83 EC 20 48 8B F2 48 89 5C 24 38 8B F9 E8 ?? ?? ?? ?? 4C 8D 44 24 40 48 8D 54 24 30 48 8B 58 50 48 81 C3 00 02 00 00`
- sub_1410D6A10 (bulk txtp, RDX=chunk {u8 count@2,u64 arr@3}, rec 12B {u32 id,u64 key@4}): `40 53 55 57 48 83 EC 20 48 8B EA E8 ?? ?? ?? ?? 0F B6 55 02 48 8B 48 50 03 91 04 02 00 00 48 8D 99 00 02 00 00 83 FA 08`
If a patch recompiles these, re-derive sigs in IDA. See [[gw2mcp-server]] [[gw2index-tool]].
