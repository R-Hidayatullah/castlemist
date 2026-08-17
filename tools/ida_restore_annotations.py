"""Restore annotations exported by ida_export_annotations.py into an IDB.

Run inside IDA:  File > Script file...

Everything is keyed by RVA, so the dump survives rebasing but not a patch that
moves code.  Each function carries a hash of its first bytes; when that hash no
longer matches, the function is reported as drifted.  STRICT decides whether
drifted functions are skipped (safe, for restoring onto a new game build) or
renamed anyway (right when you are restoring into the same build).

    STRICT = True   -> skip functions whose bytes changed
    STRICT = False  -> apply anyway, just report the count
"""
import json, hashlib, os, time

import idaapi, idautils, idc
import ida_funcs, ida_bytes, ida_name, ida_nalt, ida_typeinf, ida_frame, ida_dirtree

try:
    import ida_hexrays
    HAVE_HR = ida_hexrays.init_hexrays_plugin()
except Exception:
    HAVE_HR = False

IN = os.path.join(os.path.dirname(idc.get_idb_path()), "gw2_annotations.json")
STRICT = False
DO_COMMENTS = True
DO_FOLDERS = True
DO_TYPES = True

BASE = idaapi.get_imagebase()
HASH_BYTES = 32

stats = {}


def bump(k, n=1):
    stats[k] = stats.get(k, 0) + n


def ea_of(rec):
    return BASE + rec['rva']


def set_name(ea, name):
    if idc.get_name(ea) == name:
        return True
    return bool(ida_name.set_name(ea, name, ida_name.SN_NOCHECK | ida_name.SN_FORCE))


def apply_decl(ea, decl):
    """decl is a one-line C declaration as printed by the exporter."""
    if not decl:
        return False
    d = decl.strip()
    if not d.endswith(';'):
        d += ';'
    tif = ida_typeinf.tinfo_t()
    # the exporter prints anonymous declarations ("int __fastcall(int)"); give
    # the parser a name to hang them on
    for cand in (d, d.replace('(', ' __restore_dummy(', 1)):
        if ida_typeinf.parse_decl(tif, ida_typeinf.get_idati(), cand,
                                  ida_typeinf.PT_SIL | ida_typeinf.PT_TYP) is not None:
            return bool(ida_typeinf.apply_tinfo(ea, tif, ida_typeinf.TINFO_DEFINITE))
    return False


def restore_types(text):
    if not text or not DO_TYPES:
        return
    ti = ida_typeinf.get_idati()
    before = ida_typeinf.get_ordinal_count(ti)
    errs = ida_typeinf.parse_decls(ti, text, None, ida_typeinf.HTI_DCL)
    bump('types_added', ida_typeinf.get_ordinal_count(ti) - before)
    if errs:
        bump('type_parse_errors', errs)


def restore_lvars(ea, lvars):
    if not HAVE_HR or not lvars:
        return
    lsi = ida_hexrays.lvar_uservec_t()
    ida_hexrays.restore_user_lvar_settings(lsi, ea)   # start from whatever exists
    for item in lvars:
        si = ida_hexrays.lvar_saved_info_t()
        loc = ida_hexrays.lvar_locator_t()
        if item.get('is_stk') and item.get('stkoff') is not None:
            loc.location.set_stkoff(item['stkoff'])
        elif item.get('reg') is not None:
            loc.location.set_reg1(item['reg'])
        else:
            continue
        if item.get('ll_off') is not None:
            loc.defea = BASE + item['ll_off']
        si.ll = loc
        if item.get('name'):
            si.name = item['name']
        if item.get('cmt'):
            si.cmt = item['cmt']
        if item.get('type'):
            t = ida_typeinf.tinfo_t()
            if ida_typeinf.parse_decl(t, ida_typeinf.get_idati(),
                                      item['type'] + ' x;', ida_typeinf.PT_SIL) is not None:
                si.type = t
        lsi.lvvec.push_back(si)
    ida_hexrays.save_user_lvar_settings(ea, lsi)
    bump('lvars', len(lvars))


def restore_frame(f, members):
    if not members:
        return
    try:
        tif = ida_typeinf.tinfo_t()
        if not ida_frame.get_func_frame(tif, f):
            return
        for m in members:
            idx = tif.find_udm(m['off'] * 8, ida_typeinf.STRMEM_OFFSET)
            if idx >= 0 and tif.rename_udm(idx, m['name']) == 0:
                bump('frame_members')
    except Exception:
        bump('frame_errors')


def restore_folders(pairs):
    """pairs: [(ea, '/path/to/folder')]"""
    if not pairs or not DO_FOLDERS:
        return
    dt = ida_dirtree.get_std_dirtree(ida_dirtree.DIRTREE_FUNCS)
    made = set()
    for ea, path in pairs:
        if path not in made:
            acc = ''
            for part in [p for p in path.split('/') if p]:
                acc += '/' + part
                dt.mkdir(acc)
            made.add(path)
        try:
            de = ida_dirtree.direntry_t(ea, False)
            cur = dt.get_abspath(dt.find_entry(de))
            nm = idc.get_name(ea)
            tgt = path + '/' + nm
            if cur != tgt and dt.rename(cur, tgt) == 0:
                bump('foldered')
        except Exception:
            bump('folder_errors')


def main():
    t0 = time.time()
    with open(IN, encoding='utf-8') as fp:
        doc = json.load(fp)
    meta = doc['meta']
    print("restoring %s exported %s" % (meta['module'], meta['exported_utc']))
    if meta['module'] != idaapi.get_root_filename():
        print("  !! module differs: dump is %s, IDB is %s"
              % (meta['module'], idaapi.get_root_filename()))
    same_build = meta.get('input_md5') == ida_nalt.retrieve_input_file_md5().hex()
    print("  build match: %s%s" % (same_build,
                                   "" if same_build else "  (expect drift)"))

    restore_types(doc.get('local_types'))

    folder_pairs = []
    for r in doc['functions']:
        ea = ea_of(r)
        f = ida_funcs.get_func(ea)
        if not f or f.start_ea != ea:
            bump('no_function_at_rva')
            continue
        n = min(HASH_BYTES, f.end_ea - f.start_ea)
        h = hashlib.sha1(ida_bytes.get_bytes(ea, n) or b'').hexdigest()[:16]
        if h != r.get('hash'):
            bump('drifted')
            if STRICT:
                continue
        if set_name(ea, r['name']):
            bump('functions')
        if r.get('type') and apply_decl(ea, r['type']):
            bump('signatures')
        if r.get('cmt'):
            ida_funcs.set_func_cmt(f, r['cmt'], False)
            bump('func_comments')
        if r.get('rcmt'):
            ida_funcs.set_func_cmt(f, r['rcmt'], True)
        restore_lvars(ea, r.get('lvars'))
        restore_frame(f, r.get('frame'))
        for lb in r.get('labels', []):
            if set_name(ea_of(lb), lb['name']):
                bump('labels')
        if r.get('folder'):
            folder_pairs.append((ea, r['folder']))

    for rec in doc.get('data', []) + doc.get('labels', []):
        ea = ea_of(rec)
        if not ida_bytes.is_loaded(ea):
            bump('data_out_of_range')
            continue
        if set_name(ea, rec['name']):
            bump('data_names')
        if rec.get('type'):
            apply_decl(ea, rec['type'])

    if DO_COMMENTS:
        for rec in doc.get('comments', []):
            ea = ea_of(rec)
            if not ida_bytes.is_loaded(ea):
                continue
            if rec.get('cmt'):
                ida_bytes.set_cmt(ea, rec['cmt'], False)
                bump('comments')
            if rec.get('rcmt'):
                ida_bytes.set_cmt(ea, rec['rcmt'], True)
                bump('comments')

    restore_folders(folder_pairs)

    print("done in %.1fs" % (time.time() - t0))
    for k in sorted(stats):
        print("  %-22s %d" % (k, stats[k]))
    print("\nSave the database (Ctrl+W) to keep this.")


main()
