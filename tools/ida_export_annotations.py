"""Export IDB annotations to JSON so they can be restored into a fresh database.

Captures function names + signatures, Hex-Rays local variable names, stack frame
member names, in-function labels, global/data names, comments, the function
folder tree, and the local type library.

Run inside IDA:  File > Script file...  (or py_exec_file over MCP)

    SCOPE = None                      -> whole IDB
    SCOPE = (0x140f95000, 0x140fd3000)-> address range only

Addresses are stored as RVAs (relative to the image base) so the dump survives
rebasing.  Each function also carries a hash of its first bytes; the restore
script uses it to skip functions whose code moved or changed, which is what
happens after a game patch.
"""
import json, hashlib, time, os

import idaapi, idautils, idc
import ida_funcs, ida_bytes, ida_name, ida_segment, ida_nalt
import ida_typeinf, ida_frame, ida_dirtree

try:
    import ida_hexrays
    HAVE_HR = ida_hexrays.init_hexrays_plugin()
except Exception:
    HAVE_HR = False

SCOPE = None                     # or (start_ea, end_ea)
COMMENTS_ALL = False             # True keeps PE-loader and IDA auto comments too
# IDA plants its own comments in code -- "StackCookie", "switch jump", and
# argument-name hints derived from callee prototypes ("Size", "lpCriticalSection").
# They are regenerated on load, and in this IDB they outnumber real notes 20:1.
# Any comment text occurring more than this many times is treated as auto.
AUTO_CMT_THRESHOLD = 20
OUT = os.path.join(os.path.dirname(idc.get_idb_path()), "gw2_annotations.json")

BASE = idaapi.get_imagebase()
HASH_BYTES = 32


def rva(ea):
    return ea - BASE


def in_scope(ea):
    return SCOPE is None or (SCOPE[0] <= ea < SCOPE[1])


def is_default_name(ea, name):
    """True for IDA's auto-generated names, which regenerate on load anyway.

    has_user_name() is the discriminator: it is set only for names something
    explicitly assigned.  Auto names (sub_*, aSomeString, off_*) leave it clear.
    """
    if not name:
        return True
    return not ida_bytes.has_user_name(ida_bytes.get_flags(ea))


def func_hash(f):
    n = min(HASH_BYTES, f.end_ea - f.start_ea)
    b = ida_bytes.get_bytes(f.start_ea, n) or b''
    return hashlib.sha1(b).hexdigest()[:16]


def type_of(ea):
    tif = ida_typeinf.tinfo_t()
    if ida_nalt.get_tinfo(tif, ea):
        return tif._print(None, ida_typeinf.PRTYPE_1LINE | ida_typeinf.PRTYPE_SEMI) or None
    return None


# ---------------------------------------------------------------- folders
def folder_map():
    """{func_ea: '/folder/path'} for every function filed outside the root.

    Walks the tree rather than querying per function -- the IDB has ~87k
    functions but only a few hundred are filed.
    """
    out = {}
    try:
        dt = ida_dirtree.get_std_dirtree(ida_dirtree.DIRTREE_FUNCS)
    except Exception:
        return out

    def walk(path):
        it = ida_dirtree.dirtree_iterator_t()
        ok = dt.findfirst(it, path.rstrip('/') + '/*')
        subdirs = []
        while ok:
            de = dt.resolve_cursor(it.cursor)
            nm = dt.get_entry_name(de)
            if de.isdir:
                subdirs.append((path.rstrip('/') + '/' + nm))
            elif in_scope(de.idx):
                out[de.idx] = path
            ok = dt.findnext(it)
        for d in subdirs:
            walk(d)

    for top in ('/',):
        it = ida_dirtree.dirtree_iterator_t()
        ok = dt.findfirst(it, top + '*')
        roots = []
        while ok:
            de = dt.resolve_cursor(it.cursor)
            if de.isdir:
                roots.append('/' + dt.get_entry_name(de))
            ok = dt.findnext(it)
        for r in roots:
            walk(r)
    return out


# ---------------------------------------------------------------- lvars
def user_lvars(ea):
    """Only variables the user explicitly renamed/retyped -- no decompilation."""
    if not HAVE_HR:
        return []
    lsi = ida_hexrays.lvar_uservec_t()
    if not ida_hexrays.restore_user_lvar_settings(lsi, ea):
        return []
    out = []
    for lv in lsi.lvvec:
        item = {}
        if lv.name:
            item['name'] = lv.name
        if lv.type and not lv.type.empty():
            item['type'] = lv.type._print(None, ida_typeinf.PRTYPE_1LINE)
        if not item:
            continue
        item['ll_off'] = lv.ll.defea - BASE if lv.ll.defea != idc.BADADDR else None
        item['is_stk'] = bool(lv.ll.is_stk_var())
        item['stkoff'] = lv.ll.get_stkoff() if lv.ll.is_stk_var() else None
        item['reg'] = lv.ll.get_reg1() if not lv.ll.is_stk_var() else None
        if lv.cmt:
            item['cmt'] = lv.cmt
        out.append(item)
    return out


# ---------------------------------------------------------------- frame
def frame_members(f):
    out = []
    try:
        tif = ida_typeinf.tinfo_t()
        if not ida_frame.get_func_frame(tif, f):
            return out
        udt = ida_typeinf.udt_type_data_t()
        if not tif.get_udt_details(udt):
            return out
        for m in udt:
            nm = m.name or ''
            if not nm or nm.startswith((' s', ' r')) or nm in (' s', ' r'):
                continue
            if nm.startswith(('var_', 'arg_', 'anonymous')):
                continue
            out.append({'off': m.offset // 8, 'name': nm,
                        'type': m.type._print(None, ida_typeinf.PRTYPE_1LINE)})
    except Exception:
        pass
    return out


# ---------------------------------------------------------------- main sweep
def collect(folders):
    funcs, labels_by_func = [], {}

    # named addresses that are not function starts -> labels / data
    data, orphan_labels = [], []
    for ea, name in idautils.Names():
        if not in_scope(ea):
            continue
        f = ida_funcs.get_func(ea)
        if f and f.start_ea == ea:
            continue
        if is_default_name(ea, name):
            continue          # auto label / auto string name -- regenerates on load
        rec = {'rva': rva(ea), 'name': name, 'type': type_of(ea)}
        if f:
            labels_by_func.setdefault(f.start_ea, []).append(rec)
        elif ida_bytes.is_code(ida_bytes.get_flags(ea)):
            orphan_labels.append(rec)
        else:
            data.append(rec)

    # cheap first pass: only functions worth the expensive per-function work
    candidates = []
    for fea in idautils.Functions():
        if not in_scope(fea):
            continue
        f = ida_funcs.get_func(fea)
        if (not is_default_name(fea, ida_funcs.get_func_name(fea))) \
                or fea in labels_by_func or fea in folders \
                or ida_nalt.get_tinfo(ida_typeinf.tinfo_t(), fea) \
                or ida_funcs.get_func_cmt(f, False) or ida_funcs.get_func_cmt(f, True):
            candidates.append(fea)

    for fea in candidates:
        f = ida_funcs.get_func(fea)
        name = ida_funcs.get_func_name(fea)
        lv = user_lvars(fea)
        fr = frame_members(f)
        lb = labels_by_func.get(fea, [])
        cmt = ida_funcs.get_func_cmt(f, False)
        rcmt = ida_funcs.get_func_cmt(f, True)
        ty = type_of(fea)
        rec = {'rva': rva(fea), 'name': name, 'size': f.end_ea - f.start_ea,
               'hash': func_hash(f)}
        if ty:
            rec['type'] = ty
        if cmt:
            rec['cmt'] = cmt
        if rcmt:
            rec['rcmt'] = rcmt
        if lv:
            rec['lvars'] = lv
        if fr:
            rec['frame'] = fr
        if lb:
            rec['labels'] = lb
        funcs.append(rec)

    return funcs, data, orphan_labels


def scan_comments(budget_sec=None, resume=None):
    """Walk every commented address.  Returns (records, resume_rva_or_None).

    The scan is the slow part of a full-IDB export, so it can be run under a
    time budget and picked up again from the returned cursor.
    """
    t0 = time.time()
    out = []
    segs = [(ida_segment.getseg(s).start_ea, ida_segment.getseg(s).end_ea)
            for s in idautils.Segments()]
    if SCOPE:
        segs = [(max(a, SCOPE[0]), min(b, SCOPE[1])) for a, b in segs]
    segs = [(a, b) for a, b in segs if a < b]
    start = BASE + resume if resume is not None else None
    for lo, hi in segs:
        if start is not None and hi <= start:
            continue
        ea = max(lo, start) if start is not None else lo
        while ea < hi:
            ea = ida_bytes.next_that(ea, hi, ida_bytes.has_cmt)
            if ea == idc.BADADDR:
                break
            # The PE loader plants thousands of header comments ("PE magic
            # number", "Pages in file", ...).  They are regenerated on load and
            # would dominate the dump, so keep only comments that sit inside a
            # function or on an explicitly named address.
            keep = COMMENTS_ALL or ida_funcs.get_func(ea) is not None \
                or ida_bytes.has_user_name(ida_bytes.get_flags(ea))
            if keep:
                c = ida_bytes.get_cmt(ea, False)
                r = ida_bytes.get_cmt(ea, True)
                if c or r:
                    rec = {'rva': rva(ea)}
                    if c:
                        rec['cmt'] = c
                    if r:
                        rec['rcmt'] = r
                    out.append(rec)
            if budget_sec and (time.time() - t0) > budget_sec:
                return out, rva(ea) + 1
    return out, None


def local_types():
    """The whole local type library as a compilable header."""
    ti = ida_typeinf.get_idati()
    n = ida_typeinf.get_ordinal_count(ti)
    try:
        txt = ida_typeinf.print_decls(
            ti, list(range(1, n + 1)),
            ida_typeinf.PDF_INCL_DEPS | ida_typeinf.PDF_DEF_FWD | ida_typeinf.PDF_DEF_BASE)
        if txt:
            return txt
    except Exception:
        pass
    parts = []
    for o in range(1, n + 1):
        nm = ida_typeinf.get_numbered_type_name(ti, o)
        if not nm:
            continue
        tif = ida_typeinf.tinfo_t()
        if tif.get_numbered_type(ti, o):
            parts.append(tif._print(nm, ida_typeinf.PRTYPE_MULTI | ida_typeinf.PRTYPE_TYPE |
                                    ida_typeinf.PRTYPE_SEMI) or '')
    return '\n'.join(p for p in parts if p)


def main(budget_sec=None):
    """budget_sec limits the comment scan per invocation.  When it runs out the
    partial state is checkpointed to OUT + '.part'; re-run to continue."""
    t0 = time.time()
    part = OUT + '.part'
    state = None
    if os.path.exists(part):
        with open(part, encoding='utf-8') as fp:
            state = json.load(fp)
        print("resuming from checkpoint at rva 0x%x" % state['resume'])

    def drop_auto(cmts):
        if COMMENTS_ALL:
            return cmts, 0
        freq = {}
        for r in cmts:
            for k in ('cmt', 'rcmt'):
                if k in r:
                    freq[r[k]] = freq.get(r[k], 0) + 1
        kept = []
        for r in cmts:
            keep = {k: v for k, v in r.items()
                    if k == 'rva' or freq.get(v, 0) <= AUTO_CMT_THRESHOLD}
            if len(keep) > 1:
                kept.append(keep)
        return kept, len(cmts) - len(kept)

    folders = folder_map()

    if state:
        funcs, data, orphan = state['functions'], state['data'], state['labels']
        comments = state['comments']
        more, resume = scan_comments(budget_sec, state['resume'])
        comments.extend(more)
    else:
        funcs, data, orphan = collect(folders)
        comments, resume = scan_comments(budget_sec)
    raw_cmts = len(comments)

    if resume is not None:
        with open(part, 'w', encoding='utf-8') as fp:
            json.dump({'functions': funcs, 'data': data, 'labels': orphan,
                       'comments': comments, 'resume': resume}, fp)
        print("checkpoint: %d comments so far, resume at rva 0x%x -- re-run to continue"
              % (len(comments), resume))
        return
    comments, dropped = drop_auto(comments)
    for r in funcs:
        d = folders.get(BASE + r['rva'])
        if d and d != '/':
            r['folder'] = d
    doc = {
        'meta': {
            'tool': 'ida_export_annotations',
            'version': 1,
            'module': idaapi.get_root_filename(),
            'input_md5': ida_nalt.retrieve_input_file_md5().hex(),
            'imagebase': BASE,
            'scope': list(SCOPE) if SCOPE else None,
            'exported_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        },
        'local_types': local_types(),
        'functions': funcs,
        'data': data,
        'labels': orphan,
        'comments': comments,
    }
    with open(OUT, 'w', encoding='utf-8') as fp:
        json.dump(doc, fp, indent=1, ensure_ascii=False)
    print("wrote %s" % OUT)
    print("  functions %d (with lvars %d, frame %d, labels %d, types %d)" % (
        len(funcs),
        sum(1 for f in funcs if 'lvars' in f), sum(1 for f in funcs if 'frame' in f),
        sum(1 for f in funcs if 'labels' in f), sum(1 for f in funcs if 'type' in f)))
    print("  data %d, orphan labels %d, local_types %d chars" % (
        len(data), len(orphan), len(doc['local_types'])))
    print("  comments %d kept of %d scanned (%d dropped as IDA auto), %.1fs" % (
        len(comments), raw_cmts, dropped, time.time() - t0))
    if os.path.exists(OUT + '.part'):
        os.remove(OUT + '.part')


# Runs in one pass by default, which is what you want from File > Script file...
# Set IDA_EXPORT_BUDGET to a number of seconds when driving this over a
# transport with a call deadline (the MCP server times out around 30s); the run
# then checkpoints to gw2_annotations.json.part and continues where it left off.
BUDGET = int(os.environ.get('IDA_EXPORT_BUDGET', '0')) or None
main(BUDGET)
