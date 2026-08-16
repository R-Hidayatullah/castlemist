"""Re-apply the Compress / image-decode symbol map to a Gw2-64.exe IDB.

Run inside IDA (File > Script file) or via ida-pro-mcp's py_exec_file.

Addresses below are for the build analysed on 2026-08-16, imagebase 0x140000000.
They WILL rot across client patches. Before trusting a run, check the report this
prints: every entry is verified against an expected anchor (a source-path string
xref, or a distinctive string/constant) and skipped if the anchor does not match.

If most entries fail, do not hand-patch the addresses. Rebuild the map from the
embedded Perforce source paths instead -- see rebuild_from_source_paths() at the
bottom, and docs/research/gw2-cmp-img-symbol-map.md.
"""

import ida_bytes
import ida_dirtree
import ida_funcs
import ida_name
import idautils
import idc

IMAGEBASE = 0x140000000

# --- source-path anchor strings: addr -> substring that must appear there ------
ANCHORS = {
    0x1420A1E20: r"Arena\Services\Compress\CmpApi.cpp",
    0x1420A2450: r"Arena\Services\Compress\CmpDict.cpp",
    0x1420A2C40: r"Arena\Services\Compress\CmpHuff.cpp",
    0x141C92BF0: r"Arena\Engine\Gr\Img\ImgAtex.cpp",
    0x141C7F0E0: r"Arena\Engine\Gr\Img\ImgDecode.cpp",
    0x141C80750: r"Arena\Engine\Gr\Img\ImgFmt.cpp",
}

# --- func addr -> (name, required source-path anchor or None) -----------------
FUNCS = {
    # Arena\Services\Compress
    0x140D9D7B0: ("Cmp_Compress", 0x1420A1E20),
    0x140D9D9F0: ("Cmp_Decompress", 0x1420A1E20),
    0x140D9E970: ("Cmp_CompressMethod1", 0x1420A2450),
    0x140DA1220: ("Cmp_CompressMethod0", 0x1420A2450),
    0x140DA0650: ("Cmp_DecompressMethod1", None),
    0x140DA27F0: ("Cmp_DecompressMethod0", None),
    0x140D9E8F0: ("Cmp_EncSymLutLookup", None),
    0x140DA9730: ("CmpHuff_BuildDecodeTable", 0x1420A2C40),
    0x140DA9FC0: ("CmpHuff_BuildCanonicalCodes", None),
    0x140DA9DA0: ("CmpHuff_Enc_140DA9DA0", 0x1420A2C40),
    0x140DAA430: ("CmpHuff_Enc_140DAA430", 0x1420A2C40),
    0x140DAA550: ("CmpHuff_Enc_140DAA550", None),
    0x140DAA7A0: ("CmpHuff_Enc_140DAA7A0", 0x1420A2C40),
    # Arena\Engine\Gr\Img\ImgAtex.cpp
    0x140B86B70: ("ImgAtex_Decode", 0x141C92BF0),
    0x140B892D0: ("ImgAtex_Encode", 0x141C92BF0),
    0x140B86A90: ("ImgAtex_DecodeCleanup", 0x141C92BF0),
    0x140B86620: ("ImgAtex_MirrorTerrainBorders", 0x141C92BF0),
    0x140B84C20: ("ImgAtex_Enc_140B84C20", 0x141C92BF0),
    0x140B861B0: ("ImgAtex_Enc_140B861B0", 0x141C92BF0),
    0x140B85280: ("ImgAtex_Enc_140B85280", None),
    0x140B85990: ("ImgAtex_Enc_140B85990", None),
    0x140B84B60: ("ImgAtex_Enc_140B84B60", None),
    0x140C213F0: ("ImgAtexCommon_140C213F0", None),
    # ImgDecode.cpp
    0x140B187E0: ("ImgDecode_Run", 0x141C7F0E0),
    0x140B18B40: ("ImgDecode_ProbeFormat", 0x141C7F0E0),
    0x140B18640: ("ImgDecode_140B18640", 0x141C7F0E0),
    0x140B18750: ("ImgDecode_140B18750", 0x141C7F0E0),
    0x140B18730: ("ImgDecode_140B18730", None),
    # header sniffers + per-decoder init, reached from ImgDecode_Run
    0x140B89290: ("ImgAtex_IsAtexMagic", None),
    0x140B8EB90: ("ImgDds_IsDdsMagic", None),
    0x140B86AF0: ("ImgAtex_DecodeBegin", None),
    0x140B8BC50: ("ImgDds_DecodeBegin", None),
    # ImgFmt.cpp / ImgCalc.cpp
    0x140B3FC80: ("ImgFmt_GetFlags", 0x141C80750),
    0x140B40740: ("ImgFmt_GetBitCount", 0x141C80750),
    0x140B40890: ("ImgFmt_GetBlockDims", 0x141C80750),
    0x140B3F730: ("ImgCalc_LevelSize", None),
    # probe / decode pairs, in ImgDecode_ProbeFormat order
    0x140B8AD50: ("ImgBmp_Probe", None),
    0x140B8AAE0: ("ImgBmp_Decode", None),
    0x140B8B650: ("ImgDcx_Probe", None),
    0x140B8B500: ("ImgDcx_Decode", None),
    0x140B96480: ("ImgGif_Probe", None),
    0x140B961D0: ("ImgGif_Decode", None),
    0x140B94300: ("ImgJpeg_Probe", None),
    0x140B941A0: ("ImgJpeg_Decode", None),
    0x140B971A0: ("ImgPcx_Probe", None),
    0x140B97050: ("ImgPcx_Decode", None),
    0x140B97A70: ("ImgPng_Probe", None),
    0x140B979A0: ("ImgPng_Decode", None),
    0x140B986A0: ("ImgPsd_Probe", None),
    0x140B983A0: ("ImgPsd_Decode", None),
    0x140B9A760: ("ImgTiff_Probe", None),
    0x140B9A630: ("ImgTiff_Decode", None),
    0x140B9B150: ("ImgTga_Probe", None),
    0x140B9AE50: ("ImgTga_Decode", None),
    # ImgDds.cpp / ImgDxt.cpp
    0x140B8BCC0: ("ImgDds_Decode", None),
    0x140B8E240: ("ImgDds_140B8E240", None),
    0x140BFA2E0: ("ImgDxt_140BFA2E0", None),
    0x140BF75A0: ("ImgDxt_140BF75A0", None),
    0x140BFAF60: ("ImgDxt_MakeConstantAlphaBlock", None),
    0x140BFAFC0: ("ImgDxt_140BFAFC0", None),
    # ImgDecode lifecycle + format naming
    0x140B18750: ("ImgDecode_Init", 0x141C7F0E0),
    0x140B18640: ("ImgDecode_Cleanup", 0x141C7F0E0),
    0x140B18730: ("ImgDecode_OffsetIfBuffered", None),
    0x140B8BC00: ("ImgDds_DecodeCleanup", None),
    0x140B40900: ("ImgFmt_GetName", 0x141C80750),
    0x140A7E100: ("ImgFmt_NeedsEndianSwap", None),
    # ---- Arena engine runtime primitives (used binary-wide, not just here) ----
    0x1409DDA80: ("Arena_AssertFailed", None),        # ~46,000 call sites
    0x1409DDC30: ("Arena_FatalFormatted", None),
    0x1409BDC00: ("Arena_Log", None),
    0x140235910: ("Arena_ReportError2Args", None),
    0x140239260: ("Arena_ReportError3Args", None),
    0x1409CFEF0: ("Arena_Alloc", None),
    0x1409CF780: ("Arena_Free", None),
    0x140E4ED0C: ("j_Arena_Free", None),
    0x1409D0170: ("Arena_TryResize", None),
    0x1409CF760: ("Arena_TagAlloc", None),
    0x1409D0280: ("Arena_GetAllocSize", None),
    0x1409B9DC0: ("Arena_MemZero", None),
    0x1409B9D90: ("Arena_MemSet", None),
    0x1409B9840: ("Arena_MemCategory_Uncategorized", None),
    0x1409B6E20: ("Arena_MemCategory_Array", None),
    0x140A6E8F0: ("Arena_MemCategory_GrImg", None),
    0x140A1FF20: ("Arena_ArraySetCount", None),
    0x140B84B60: ("Arena_AllocatorSetSize", None),
}

# --- data addr -> (name, expected leading bytes as a fingerprint) -------------
DATA = {
    0x1420A2250: ("Cmp_LenBase", [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14]),
    0x1420A2390: ("Cmp_DistExtraBits", [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4]),
    0x1420A2430: ("Cmp_LenExtraBits", [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1]),
    0x1420A2270: ("Cmp_EncSymLut", [0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6]),
    0x1420A2940: ("CmpHuff_BootSymTbl", [8, 9, 10, 0, 7, 11, 12, 6]),
    # word / dword tables: fingerprint checked separately below
    0x1420A2210: ("Cmp_DistBase", None),
    0x1420A28D0: ("CmpHuff_BootRangeTbl", None),
    0x141C80410: ("ImgFmt_Flags", None),
    0x141C801A0: ("ImgFmt_InfoTable", None),
    0x142581B20: ("ImgFmt_Names", None),
}

# split point: Cmp_DistExtraBits lives inside the array that starts at Cmp_EncSymLut
SPLIT = (0x1420A2270, 288, 0x1420A2390, 32)


def _anchor_ok(func_ea, anchor_ea):
    """True if func_ea references the given source-path string."""
    if anchor_ea is None:
        return True
    if not idc.get_func_name(func_ea):
        return False
    for ea in idautils.FuncItems(func_ea):
        for xr in idautils.XrefsFrom(ea, 0):
            if xr.to == anchor_ea:
                return True
    return False


def verify_anchors():
    bad = []
    for ea, want in ANCHORS.items():
        got = idc.get_strlit_contents(ea, -1, idc.STRTYPE_C)
        got = got.decode("latin-1", "replace") if got else ""
        if want not in got:
            bad.append((hex(ea), want, got[:60]))
    return bad


def apply_names(dry_run=False):
    bad = verify_anchors()
    if bad:
        print("!! source-path anchors do not match this binary -- addresses have rotted.")
        for a, want, got in bad:
            print(f"   {a}: expected {want!r}, found {got!r}")
        print("   Rebuild the map: see rebuild_from_source_paths().")
        return False

    # split the merged Cmp_EncSymLut / Cmp_DistExtraBits array
    base, base_n, tail, tail_n = SPLIT
    if not dry_run and ida_bytes.get_item_head(tail) != tail:
        ida_bytes.del_items(base, ida_bytes.DELIT_SIMPLE, base_n + tail_n)
        ida_bytes.create_byte(base, base_n)
        ida_bytes.create_byte(tail, tail_n)

    ok = skipped = failed = 0
    for ea, (name, anchor) in sorted(FUNCS.items()):
        f = ida_funcs.get_func(ea)
        if f is None or f.start_ea != ea:
            print(f"  SKIP {ea:#x} -> {name}: not a function entry")
            skipped += 1
            continue
        if not _anchor_ok(ea, anchor):
            print(f"  SKIP {ea:#x} -> {name}: source-path anchor mismatch")
            skipped += 1
            continue
        if dry_run:
            ok += 1
            continue
        if ida_name.set_name(ea, name, ida_name.SN_NOWARN | ida_name.SN_FORCE):
            ok += 1
        else:
            print(f"  FAIL {ea:#x} -> {name}")
            failed += 1

    for ea, (name, fp) in sorted(DATA.items()):
        if fp is not None:
            got = [ida_bytes.get_byte(ea + i) for i in range(len(fp))]
            if got != fp:
                print(f"  SKIP {ea:#x} -> {name}: fingerprint mismatch {got}")
                skipped += 1
                continue
        if dry_run:
            ok += 1
            continue
        if ida_name.set_name(ea, name, ida_name.SN_NOWARN | ida_name.SN_FORCE):
            ok += 1
        else:
            print(f"  FAIL {ea:#x} -> {name}")
            failed += 1

    verb = "would rename" if dry_run else "renamed"
    print(f"\n{verb} {ok}, skipped {skipped}, failed {failed}")
    return failed == 0


# --- folders in the Functions / Names windows, numbered in data-flow order -----
FOLDERS = {
    "/GW2 Decompression/1 Entry points": [0x140D9D9F0, 0x140D9D7B0],
    "/GW2 Decompression/2 Method 0 (standalone)": [0x140DA27F0, 0x140DA1220],
    "/GW2 Decompression/3 Method 1 (delta vs older copy)": [0x140DA0650, 0x140D9E970],
    "/GW2 Decompression/4 Huffman code tables": [
        0x140DA9730, 0x140DA9FC0, 0x140DA9DA0, 0x140DAA430,
        0x140DAA550, 0x140DAA7A0, 0x140D9E8F0,
    ],
    "/GW2 Image decode/1 Entry points": [
        0x140B187E0, 0x140B18B40, 0x140B18640, 0x140B18750, 0x140B18730,
        0x140B89290, 0x140B8EB90,
    ],
    "/Arena runtime/Errors and logging": [
        0x1409DDA80, 0x1409DDC30, 0x1409BDC00, 0x140235910, 0x140239260,
    ],
    "/Arena runtime/Memory": [
        0x1409CFEF0, 0x1409CF780, 0x140E4ED0C, 0x1409D0170, 0x1409CF760,
        0x1409D0280, 0x1409B9DC0, 0x1409B9D90,
        0x1409B9840, 0x1409B6E20, 0x140A6E8F0,
    ],
    "/Arena runtime/Collections": [0x140A1FF20, 0x140B84B60],
    "/GW2 Image decode/2 ATEX game textures": [
        0x140B86B70, 0x140B892D0, 0x140B86620, 0x140B86A90, 0x140B86AF0,
        0x140B84C20, 0x140B861B0, 0x140B85280, 0x140B85990,
        0x140C213F0,
        # NB 0x140B84B60 (Arena_AllocatorSetSize) is generic Allocator.h code and
        # lives under /Arena runtime/Collections -- do not also list it here, or the
        # two groups will move it back and forth on every run.
    ],
    "/GW2 Image decode/3 Pixel format info": [
        0x140B3FC80, 0x140B40740, 0x140B40890, 0x140B40900,
        0x140B3F730, 0x140A7E100,
    ],
    "/GW2 Image decode/4 Standard file loaders": [
        0x140B8AD50, 0x140B8AAE0, 0x140B8B650, 0x140B8B500,
        0x140B96480, 0x140B961D0, 0x140B94300, 0x140B941A0,
        0x140B971A0, 0x140B97050, 0x140B97A70, 0x140B979A0,
        0x140B986A0, 0x140B983A0, 0x140B9A760, 0x140B9A630,
        0x140B9B150, 0x140B9AE50,
    ],
    "/GW2 Image decode/5 DDS and DXT blocks": [
        0x140B8BCC0, 0x140B8BC50, 0x140B8BC00, 0x140B8E240,
        0x140BFA2E0, 0x140BF75A0, 0x140BFAF60, 0x140BFAFC0,
    ],
}

DATA_FOLDERS = {
    "/GW2 Decompression tables": [
        0x1420A2250, 0x1420A2430, 0x1420A2210, 0x1420A2390,
        0x1420A28D0, 0x1420A2940, 0x1420A2270,
    ],
    "/GW2 Image format tables": [0x141C80410, 0x141C801A0, 0x142581B20],
}


def apply_folders():
    """File the renamed symbols into folders. Run after apply_names()."""
    filed = skipped = 0
    for tree_id, groups, namer in (
        (ida_dirtree.DIRTREE_FUNCS, FOLDERS, idc.get_func_name),
        (ida_dirtree.DIRTREE_NAMES, DATA_FOLDERS, idc.get_name),
    ):
        tree = ida_dirtree.get_std_dirtree(tree_id)
        for path in groups:
            parts = path.strip("/").split("/")
            for i in range(len(parts)):
                tree.mkdir("/" + "/".join(parts[: i + 1]))
        # a symbol may already sit in another managed folder (e.g. it was filed
        # under an older grouping before being renamed) -- search those too, not
        # just the root, so entries can be relocated instead of silently skipped
        candidates = ["/"] + [p + "/" for p in groups]
        for path, eas in groups.items():
            for ea in eas:
                name = namer(ea)
                if not name:
                    skipped += 1
                    continue
                dest = path + "/" + name
                if tree.isfile(dest):               # already in the right place
                    filed += 1
                    continue
                src = next((c + name for c in candidates if tree.isfile(c + name)), None)
                if src and tree.rename(src, dest) == ida_dirtree.DTE_OK:
                    filed += 1
                else:
                    skipped += 1
                    print(f"  could not file {name} under {path}"
                          + ("" if src else " (entry not found in any managed folder)"))
    print(f"folders: {filed} filed, {skipped} skipped")


def rebuild_from_source_paths(needle=r"Arena\Services\Compress"):
    """Enumerate functions belonging to translation units matching `needle`.

    This is the technique to fall back on after a client patch: the retail binary
    keeps each .cpp's full Perforce path as an assert argument, so xrefs to that
    string enumerate the functions compiled from it.
    """
    groups = {}
    for s in idautils.Strings():
        text = str(s)
        if needle not in text:
            continue
        for xr in idautils.XrefsTo(s.ea):
            f = ida_funcs.get_func(xr.frm)
            if f:
                groups.setdefault(text, set()).add(f.start_ea)
    for path, eas in sorted(groups.items()):
        print(f"\n=== {path}  ({len(eas)} funcs) ===")
        for ea in sorted(eas):
            f = ida_funcs.get_func(ea)
            print(f"  {ea:#x} size={f.end_ea - f.start_ea:6d} {idc.get_func_name(ea)}")
    return groups


if __name__ == "__main__":
    if apply_names():
        apply_folders()
