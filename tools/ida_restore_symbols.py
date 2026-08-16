"""Restore the full GW2 symbol set into an IDB from gw2_ida_symbols.json.

This is the backup that `ida_apply_cmp_img_names.py` cannot be: it carries the
LOCAL VARIABLE names too, which live only inside the .i64 and are lost if that
file is lost or reverted.

Run inside IDA:  File > Script file... > ida_restore_symbols.py
or headless:     idat -A -S"ida_restore_symbols.py" Gw2-64.exe.i64

Restores, in order:
  * the enums below, then the struct headers in structs/
  * function names + function comments
  * data symbol names + comments
  * inline (address) comments
  * local variable names, per function
  * pseudocode label names (the `goto` targets Hex-Rays calls LABEL_nn)

Local variable names AND label names exist only inside the .i64 -- neither is
recoverable from anywhere else, which is the whole reason this sidecar exists.

SCOPE: the JSON is keyed by absolute address, so it belongs to ONE build of
Gw2-64.exe (the one recorded in "imagebase" plus the addresses themselves).
After a client patch the addresses move and this file is worthless -- use
ida_apply_cmp_img_names.py's rebuild_from_source_paths() to re-derive the map,
then export a fresh JSON with export_symbols() below.

Local variables are matched BY INDEX into the decompiler's lvar list. That is
stable for a given binary, which is all this is meant to cover. If Hex-Rays is
upgraded and re-orders variables, re-export rather than trusting a stale file.
"""

import json
import os
import re

import ida_dirtree
import ida_funcs
import ida_hexrays
import ida_ida
import ida_name
import ida_typeinf
import idautils
import idc


def _imagebase():
    """_imagebase() was removed in IDA 9.0."""
    for getter in (getattr(ida_ida, "inf_get_min_ea", None),
                   getattr(ida_ida, "get_imagebase", None)):
        if getter:
            try:
                return getter()
            except Exception:
                pass
    import ida_nalt
    return ida_nalt.get_imagebase()

HERE = os.path.dirname(os.path.abspath(__file__))
JSON_PATH = os.path.join(HERE, "gw2_ida_symbols.json")

# Struct/typedef headers, parsed in this order after the enum blob below.
# Unlike the enums these are large enough to be worth keeping as real C files:
# they double as the reference anyone reading the notes actually wants, and they
# carry the evidence for every offset in their comments.
TYPE_HEADERS = (
    os.path.join(HERE, "structs", "gw2_ida_types.h"),
    os.path.join(HERE, "structs", "gw2_ida_types_granny.h"),
    os.path.join(HERE, "structs", "gw2_ida_types_subsystems.h"),
)


# All enums the IDB relies on. Declared as one blob so restore() can re-parse them
# in a single call; without these, switches and Add() calls fall back to bare integers.
GR_FORMAT_ENUM = """enum GR_FORMAT : unsigned int
{
    GR_FORMAT_ARGB_32323232F = 0, GR_FORMAT_ARGB_16161616F = 1,
    GR_FORMAT_ARGB_2101010 = 2,   GR_FORMAT_ARGB_8888 = 3,
    GR_FORMAT_XRGB_8888 = 4,      GR_FORMAT_ARGB_4444 = 5,
    GR_FORMAT_ARGB_1555 = 6,      GR_FORMAT_RGB_888 = 7,
    GR_FORMAT_RGB_565 = 8,        GR_FORMAT_RGB_555 = 9,
    GR_FORMAT_RG_1616 = 10,       GR_FORMAT_RG_1616F = 11,
    GR_FORMAT_RG_3232F = 12,      GR_FORMAT_R_16F = 13,
    GR_FORMAT_R_32F = 14,         GR_FORMAT_AL_88 = 15,
    GR_FORMAT_AL_44 = 16,         GR_FORMAT_AL_8 = 17,
    GR_FORMAT_L_8 = 18,           GR_FORMAT_A_8 = 19,
    GR_FORMAT_P_8 = 20,           GR_FORMAT_VU_88 = 21,
    GR_FORMAT_DXT1 = 22,          GR_FORMAT_DXT2 = 23,
    GR_FORMAT_DXT3 = 24,          GR_FORMAT_DXT4 = 25,
    GR_FORMAT_DXT5 = 26,          GR_FORMAT_DXTA = 27,
    GR_FORMAT_DXTL = 28,          GR_FORMAT_DXTN = 29,
    GR_FORMAT_3DC = 30,           GR_FORMAT_BC5 = 31,
    GR_FORMAT_BC7 = 32,           GR_FORMAT_D24 = 33,
    GR_FORMAT_SHADOWMAP = 34,     GR_FORMAT_ABGR_8888 = 35,
    GR_FORMAT_R_32UINT = 36,      GR_FORMAT_RGBE_9995 = 37,
    GR_FORMATS = 38,
};

enum CHAT_LINK_TYPE : unsigned __int8
{
    CHAT_LINK_RESERVED00 = 0,  CHAT_LINK_COIN = 1,       CHAT_LINK_ITEM = 2,
    CHAT_LINK_NPC_TEXT = 3,    CHAT_LINK_MAP_POI = 4,    CHAT_LINK_PVP_GAME = 5,
    CHAT_LINK_SKILL = 6,       CHAT_LINK_TRAIT = 7,      CHAT_LINK_USER = 8,
    CHAT_LINK_RECIPE = 9,      CHAT_LINK_SKIN = 10,      CHAT_LINK_OUTFIT = 11,
    CHAT_LINK_WVW_OBJECTIVE = 12, CHAT_LINK_BUILD_TEMPLATE = 13,
    CHAT_LINK_HDR0E = 14,      CHAT_LINK_HDR0F = 15,     CHAT_LINK_HDR10 = 16,
    CHAT_LINK_INVALID = 17,
};

enum IMG_FILE_TYPE : int
{
    IMG_FILE_TYPE_ATEX = 0, IMG_FILE_TYPE_DDS = 1,
    IMG_FILE_TYPE_STANDARD = 2, IMG_FILE_TYPE_UNKNOWN = -1,
};

enum CMP_METHOD : unsigned int
{
    CMP_METHOD0_STANDALONE = 0, CMP_METHOD1_DELTA = 1,
};

enum BGFX_ATTRIB : unsigned int
{
    BGFX_ATTRIB_POSITION = 0,  BGFX_ATTRIB_NORMAL = 1,
    BGFX_ATTRIB_TANGENT = 2,   BGFX_ATTRIB_BITANGENT = 3,
    BGFX_ATTRIB_COLOR0 = 4,    BGFX_ATTRIB_INDICES = 8,
    BGFX_ATTRIB_WEIGHT = 9,
    BGFX_ATTRIB_TEXCOORD0 = 10, BGFX_ATTRIB_TEXCOORD1 = 11,
    BGFX_ATTRIB_TEXCOORD2 = 12, BGFX_ATTRIB_TEXCOORD3 = 13,
    BGFX_ATTRIB_TEXCOORD4 = 14, BGFX_ATTRIB_TEXCOORD5 = 15,
    BGFX_ATTRIB_TEXCOORD6 = 16, BGFX_ATTRIB_TEXCOORD7 = 17,
};

enum BGFX_ATTRIB_TYPE : unsigned int
{
    BGFX_ATTRIB_TYPE_UINT8 = 0, BGFX_ATTRIB_TYPE_UINT10 = 1,
    BGFX_ATTRIB_TYPE_INT16 = 2, BGFX_ATTRIB_TYPE_HALF = 3,
    BGFX_ATTRIB_TYPE_FLOAT = 4,
};"""

GENERIC = re.compile(r"(v|a)\d+|i\d*|j|k|result")


def restore_types():
    """Declare the enums, then the struct headers.

    Order matters: the headers may name an enum, never the other way round.
    parse_decls returns the number of declarations it could NOT parse, so a
    non-zero result is a real failure and is reported rather than swallowed.
    """
    til = ida_typeinf.get_idati()
    errs = ida_typeinf.parse_decls(til, GR_FORMAT_ENUM, None, ida_typeinf.HTI_DCL)
    if errs:
        print(f"!! {errs} enum declaration(s) failed to parse")
    n_hdr = 0
    for path in TYPE_HEADERS:
        if not os.path.exists(path):
            print(f"   missing type header, skipped: {path}")
            continue
        with open(path, encoding="utf-8") as fh:
            src = fh.read()
        errs = ida_typeinf.parse_decls(til, src, None, ida_typeinf.HTI_DCL)
        if errs:
            print(f"!! {errs} declaration(s) failed in {os.path.basename(path)}")
        else:
            n_hdr += 1
    print(f"types: enums + {n_hdr}/{len(TYPE_HEADERS)} struct headers declared")


def _sanity_check(doc):
    """Refuse to run against a binary the JSON was not taken from."""
    base = doc.get("imagebase")
    if base is not None and _imagebase() != base:
        print(f"!! imagebase mismatch: json={base:#x} idb={_imagebase():#x}")
        return False
    bad = 0
    for ea_s in doc["functions"]:
        ea = int(ea_s, 16)
        f = ida_funcs.get_func(ea)
        if f is None or f.start_ea != ea:
            bad += 1
    if bad > len(doc["functions"]) // 4:
        print(f"!! {bad}/{len(doc['functions'])} recorded addresses are not function entries.")
        print("   This JSON is for a different build. Do not restore it.")
        return False
    if bad:
        print(f"   note: {bad} recorded addresses are no longer function entries; they are skipped.")
    return True


def restore(path=JSON_PATH, do_lvars=True):
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    if not _sanity_check(doc):
        return False

    restore_types()

    n_fn = n_cmt = n_lv = n_data = n_inl = n_lab = 0
    for ea_s, rec in doc["functions"].items():
        ea = int(ea_s, 16)
        f = ida_funcs.get_func(ea)
        if f is None or f.start_ea != ea:
            continue
        if ida_name.set_name(ea, rec["name"], ida_name.SN_NOWARN | ida_name.SN_FORCE):
            n_fn += 1
        if rec.get("comment"):
            ida_funcs.set_func_cmt(f, rec["comment"], True)
            ida_funcs.set_func_cmt(f, rec["comment"], False)
            n_cmt += 1
        # labels and lvars are independent: a function may have one and not the
        # other, so never let a missing "lvars" entry skip the label restore
        if not do_lvars or not (rec.get("lvars") or rec.get("labels")):
            continue
        try:
            cf = ida_hexrays.decompile(ea)
        except Exception as exc:
            print(f"   cannot decompile {rec['name']}: {exc}")
            continue
        if rec.get("labels"):
            labels = ida_hexrays.restore_user_labels(ea) or ida_hexrays.user_labels_new()
            for num_s, lname in rec["labels"].items():
                ida_hexrays.user_labels_insert(labels, int(num_s), lname)
            ida_hexrays.save_user_labels(ea, labels)
            ida_hexrays.mark_cfunc_dirty(ea)
            n_lab += len(rec["labels"])
            cf = ida_hexrays.decompile(ea)   # re-read after the label edit
        lvars = list(cf.get_lvars())
        for idx_s, name in rec["lvars"].items():
            idx = int(idx_s)
            if idx >= len(lvars):
                continue
            v = lvars[idx]
            if v.name == name:
                n_lv += 1
                continue
            # only overwrite names that still look auto-generated
            if v.name and not GENERIC.fullmatch(v.name):
                continue
            lsi = ida_hexrays.lvar_saved_info_t()
            lsi.ll = ida_hexrays.lvar_locator_t(v.location, v.defea)
            lsi.name = name
            if ida_hexrays.modify_user_lvar_info(ea, ida_hexrays.MLI_NAME, lsi):
                n_lv += 1

    for ea_s, rec in doc.get("data", {}).items():
        ea = int(ea_s, 16)
        if ida_name.set_name(ea, rec["name"], ida_name.SN_NOWARN | ida_name.SN_FORCE):
            n_data += 1
        if rec.get("comment"):
            idc.set_cmt(ea, rec["comment"], 1)
            idc.set_cmt(ea, rec["comment"], 0)

    for ea_s, cmt in doc.get("inline_comments", {}).items():
        if cmt:
            ea = int(ea_s, 16)
            idc.set_cmt(ea, cmt, 1)
            idc.set_cmt(ea, cmt, 0)
            n_inl += 1

    print(f"restored: {n_fn} functions, {n_cmt} function comments, {n_lv} local variables, "
          f"{n_lab} labels, {n_data} data symbols, {n_inl} inline comments")
    # folders last: they are keyed by name, so every rename above must land first
    restore_folders()
    print("Remember to save the database (Ctrl+W).")
    return True


# Every function we have named carries one of these prefixes. export_symbols()
# discovers by prefix rather than from a hand-kept address list, so newly named
# functions are picked up automatically and cannot be silently left out of the backup.
NAME_PREFIXES = (
    "Cmp_", "CmpHuff_",
    "Img", "GrImg",
    "Arena_", "j_Arena_",
    "Base64_", "Token_", "Crc32", "HashMurmur2A_",
    "ChatLink_",
    "CptRc4_", "Cpt_",
    "TextDecode_", "TextKey_", "TextParser_",
    "GrFvf_", "Bgfx", "GrGeoset_", "GrMat_", "GrResource_",
    "McMaterial_", "ModelFile_", "ModelGranny_", "Model_", "GrannyFvf_",
    "Math_",
    # lowercase bgfx_ is the vendored upstream API (bgfx::createVertexBuffer and
    # friends); "Bgfx" above is case-sensitive and does NOT cover it
    "bgfx_",
    "Bink_",
    "ModelAnim", "ModelFileFormat", "ModelUtil",
    # dat container + asset containers
    "Archive_", "Packfile_",
    # map, video, cinematics, scene subtitles
    "Map_", "MapData_", "Video_", "BinkAssetIo_", "CinVideo_", "ScnCli_",
)


# ---------------------------------------------------------------- folders --
# The Functions-window and Local-Types folder trees live only inside the .i64,
# exactly like local variable names. Both are keyed by NAME (function name /
# type name) rather than by address, so unlike the rest of this file they
# survive a client patch.
#
# Layout mirrors the engine's own source tree, the one baked into the binary as
# D:\Perforce\Live\NAEU\v2\Code\Arena\... -- that is the same anchor the naming
# pass navigates by, so the two agree by construction.
#
# Rules are matched MOST SPECIFIC FIRST: "ModelFileFormat" must be tested
# before "ModelFile", which must be tested before "Model_".
FUNC_FOLDERS = [
    ("Archive_",               "/Arena/Services/Archive3"),
    ("Packfile_",              "/Arena/Services/Packfile"),
    ("CmpHuff_",               "/Arena/Services/Compress"),
    ("Cmp_",                   "/Arena/Services/Compress"),
    ("CptRc4_",                "/Arena/Services/Crypt"),
    ("Cpt_",                   "/Arena/Services/Crypt"),
    ("Base64_",                "/Arena/Services/Encoding"),
    ("Token_",                 "/Arena/Services/Encoding"),
    ("Crc32",                  "/Arena/Services/Encoding"),
    ("HashMurmur2A_",          "/Arena/Services/Encoding"),
    ("j_Arena_",               "/Arena/Core"),
    ("Arena_",                 "/Arena/Core"),
    ("Math_",                  "/Arena/Core"),
    ("TextDecode_",            "/Arena/Engine/Text"),
    ("TextKey_",               "/Arena/Engine/Text"),
    ("TextParser_",            "/Arena/Engine/Text"),
    ("bgfx_",                  "/External/bgfx"),
    ("BgfxVertexLayout_",      "/Arena/Engine/Gr/Bgfx"),
    ("BgfxTexture_",           "/Arena/Engine/Gr/Bgfx"),
    ("BgfxBuffer_",            "/Arena/Engine/Gr/Bgfx"),
    ("BgfxShader",             "/Arena/Engine/Gr/Bgfx"),
    ("Bgfx",                   "/Arena/Engine/Gr/Bgfx"),
    ("GrannyFvf_",             "/Arena/Engine/Gr/Fvf"),
    ("GrFvf_",                 "/Arena/Engine/Gr/Fvf"),
    ("ImgAtexCommon_",         "/Arena/Engine/Gr/Img"),
    ("ImgAtex_",               "/Arena/Engine/Gr/Img"),
    ("Img",                    "/Arena/Engine/Gr/Img"),
    ("GrImg",                  "/Arena/Engine/Gr/Img"),
    ("McMaterial_",            "/Arena/Engine/Gr/Material"),
    ("GrMat_",                 "/Arena/Engine/Gr/Material"),
    ("GrGeoset_",              "/Arena/Engine/Gr"),
    ("GrResource_",            "/Arena/Engine/Gr"),
    ("ModelFileFormatGranny_", "/Arena/Engine/ModelFileFormat"),
    ("ModelFileFormat_",       "/Arena/Engine/ModelFileFormat"),
    ("ModelFile_",             "/Arena/Engine/Model"),
    ("ModelGranny_",           "/Arena/Engine/Model"),
    ("ModelAnim",              "/Arena/Engine/Model"),
    ("ModelUtil",              "/Arena/Engine/Model"),
    ("Model_",                 "/Arena/Engine/Model"),
    ("MapData_",               "/Arena/Engine/Map"),
    ("Map_",                   "/Arena/Engine/Map"),
    ("BinkAssetIo_",           "/Arena/Engine/Video"),
    ("Video_",                 "/Arena/Engine/Video"),
    ("CinVideo_",              "/Arena/Engine/Cinema"),
    ("ScnCli_",                "/Gw2/Game/Scene"),
    ("ChatLink_",              "/Gw2/Game/ChatLink"),
]

TYPE_FOLDERS = [
    ("bgfx_",              "/External/bgfx"),
    ("BGFX_ATTRIB",        "/External/bgfx"),
    ("GrFvfVertexLayout",  "/Gr/Fvf"),
    ("GR_FVF",             "/Gr/Fvf"),
    ("GrannyAttribToFvf",  "/Granny"),
    ("granny_",            "/Granny"),
    ("GR_FORMAT",          "/Gr/Texture"),
    ("DDI_TEXTURE",        "/Gr/Texture"),
    ("DdiTexture",         "/Gr/Texture"),
    ("ATEX_MAGIC",         "/Gr/Img"),
    ("AtexHeader",         "/Gr/Img"),
    ("IMG_FILE_TYPE",      "/Gr/Img"),
    ("Amat",               "/Gr/Shader"),
    ("BgfxShader",         "/Gr/Shader"),
    ("GR_SHADER",          "/Gr/Shader"),
    ("ArchiveAllocEntry",  "/Services/Archive3"),
    ("ArchiveDescriptor",  "/Services/Archive3"),
    ("ARCHIVE_",           "/Services/Archive3"),
    ("Packfile",           "/Services/Packfile"),
    ("PACKFILE_",          "/Services/Packfile"),
    ("CMP_METHOD",         "/Services/Compress"),
    ("CHAT_LINK",          "/Gw2/ChatLink"),
    ("MODEL_LOAD",         "/Model"),
    ("SCN_CHATTER",        "/Gw2/Scene"),
]


def _folder_for(name, rules):
    for prefix, folder in rules:
        if name.startswith(prefix):
            return folder
    return None


def _move_into_folder(dt, inode, name, folder):
    """Move one dirtree entry, tolerating an entry that is already in place."""
    cursor = dt.find_entry(ida_dirtree.direntry_t(inode, False))
    if cursor is None or not cursor.valid():
        return False
    src = dt.get_abspath(cursor)
    dst = folder + "/" + name
    if src == dst:
        return True
    return dt.rename(src, dst) == 0


def restore_folders():
    """Rebuild both folder trees from the prefix rules above.

    Only entries whose name matches a rule are touched; everything else --
    Windows/CRT types, the 87k unnamed subs -- is left at the root where it
    belongs. Safe to re-run.
    """
    fdt = ida_dirtree.get_std_dirtree(ida_dirtree.DIRTREE_FUNCS)
    for _, folder in FUNC_FOLDERS:
        fdt.mkdir(folder)
    n_fn = 0
    for ea in idautils.Functions():
        name = idc.get_func_name(ea)
        if not name:
            continue
        folder = _folder_for(name, FUNC_FOLDERS)
        if folder and _move_into_folder(fdt, ea, name, folder):
            n_fn += 1

    til = ida_typeinf.get_idati()
    tdt = ida_dirtree.get_std_dirtree(ida_dirtree.DIRTREE_LOCAL_TYPES)
    for _, folder in TYPE_FOLDERS:
        tdt.mkdir(folder)
    n_ty = 0
    for ordinal in range(1, ida_typeinf.get_ordinal_count(til) + 1):
        tif = ida_typeinf.tinfo_t()
        if not tif.get_numbered_type(til, ordinal):
            continue
        name = tif.get_type_name()
        if not name:
            continue
        folder = _folder_for(name, TYPE_FOLDERS)
        if folder and _move_into_folder(tdt, ordinal, name, folder):
            n_ty += 1
    print(f"folders: {n_fn} functions, {n_ty} local types filed")


def _discover():
    """Every function whose name matches one of NAME_PREFIXES."""
    out = []
    for ea in idautils.Functions():
        name = idc.get_func_name(ea)
        if name and name.startswith(NAME_PREFIXES):
            out.append(ea)
    return out


def export_symbols(path=JSON_PATH):
    """Re-dump the current IDB state over the JSON. Run after doing more RE work.

    Discovers functions by name prefix, so anything newly named is added rather
    than quietly skipped. Existing entries whose function has vanished are kept
    (they may belong to a build you still care about) but are not refreshed.
    """
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    doc["imagebase"] = _imagebase()
    added = 0
    for ea in _discover():
        key = hex(ea)
        if key not in doc["functions"]:
            doc["functions"][key] = {"name": "", "comment": "", "lvars": {}, "labels": {}}
            added += 1
    n_lv = n_lab = 0
    for ea_s, rec in doc["functions"].items():
        ea = int(ea_s, 16)
        f = ida_funcs.get_func(ea)
        if f is None or f.start_ea != ea:
            continue
        rec["name"] = idc.get_func_name(ea)
        rec["comment"] = ida_funcs.get_func_cmt(f, True) or ""
        rec["lvars"] = {}
        rec["labels"] = {}
        try:
            cf = ida_hexrays.decompile(ea)
        except Exception:
            continue
        for i, v in enumerate(cf.get_lvars()):
            if v.name and not GENERIC.fullmatch(v.name):
                rec["lvars"][str(i)] = v.name
                n_lv += 1
        # user labels are keyed by Hex-Rays label number; walk the stored map
        stored = ida_hexrays.restore_user_labels(ea)
        if stored:
            it = ida_hexrays.user_labels_begin(stored)
            while it != ida_hexrays.user_labels_end(stored):
                num = ida_hexrays.user_labels_first(it)
                name = ida_hexrays.user_labels_second(it)
                rec["labels"][str(num)] = name
                n_lab += 1
                it = ida_hexrays.user_labels_next(it)
    for ea_s, rec in doc.get("data", {}).items():
        ea = int(ea_s, 16)
        rec["name"] = idc.get_name(ea)
        rec["comment"] = idc.get_cmt(ea, 1) or ""
    for ea_s in doc.get("inline_comments", {}):
        ea = int(ea_s, 16)
        doc["inline_comments"][ea_s] = idc.get_cmt(ea, 1) or idc.get_cmt(ea, 0) or ""
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, ensure_ascii=False)
    print(f"exported {len(doc['functions'])} functions ({added} newly discovered), "
          f"{n_lv} local variable names, {n_lab} labels -> {path}")


if __name__ == "__main__":
    restore()
