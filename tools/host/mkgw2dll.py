#!/usr/bin/env python3
"""mkgw2dll.py -- turn Gw2-64.exe into a LoadLibrary-able DLL (Gw2-64.dll).

The retail client is an ordinary unpacked MSVC PE32+ image: it already carries
.reloc, an export directory and a TLS directory, so nothing has to be unpacked
or rebuilt.  Four things have to change:

  1. COFF Characteristics    |= IMAGE_FILE_DLL
  2. AddressOfEntryPoint     -> a 6-byte `mov eax,1 / ret` stub, so the loader
                                does NOT run the CRT + WinMain under loader lock
  3. Export directory        -> rebuilt so the host can GetProcAddress the
                                ORIGINAL entry point ("Gw2RealEntry") and call
                                it later, from a normal thread
  4. SECURITY data directory -> zeroed (the Authenticode blob is void anyway)

All of that lives in one appended section (.gw2hst).  The 8th section header
fits inside the existing SizeOfHeaders (table ends at 0x3a0, headers span
0x400), so no header relayout is needed and every original RVA -- and therefore
every address in the IDA database -- stays exactly where it was.

Usage:  python mkgw2dll.py <Gw2-64.exe> <out.dll>
"""

import struct
import sys

IMAGE_FILE_DLL = 0x2000
DIR_EXPORT, DIR_SECURITY = 0, 4
SCN_CODE_RX = 0x60000020  # CNT_CODE | MEM_EXECUTE | MEM_READ

# Data exports the original image carries for the GPU-switching drivers. They
# are meaningless in a DLL but cost 2 slots to keep, so keep them.
LEGACY_EXPORTS = ("AmdPowerXpressRequestHighPerformance", "NvOptimusEnablement")

DLL_STUB = bytes((0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3))  # mov eax,1 ; ret


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


class Pe:
    def __init__(self, data):
        self.d = bytearray(data)
        if self.d[:2] != b"MZ":
            raise ValueError("not an MZ image")
        self.pe = struct.unpack_from("<I", self.d, 0x3C)[0]
        if self.d[self.pe:self.pe + 4] != b"PE\0\0":
            raise ValueError("bad PE signature")
        self.nsec = struct.unpack_from("<H", self.d, self.pe + 6)[0]
        self.ohsz = struct.unpack_from("<H", self.d, self.pe + 20)[0]
        self.oh = self.pe + 24
        if struct.unpack_from("<H", self.d, self.oh)[0] != 0x20B:
            raise ValueError("not PE32+")
        self.sec_tab = self.oh + self.ohsz
        self.sect_align, self.file_align = struct.unpack_from("<II", self.d, self.oh + 32)
        self.size_of_headers = struct.unpack_from("<I", self.d, self.oh + 60)[0]
        self.imagebase = struct.unpack_from("<Q", self.d, self.oh + 24)[0]
        self.ddir = self.oh + 112

    def sections(self):
        out = []
        for i in range(self.nsec):
            o = self.sec_tab + i * 40
            name = self.d[o:o + 8].rstrip(b"\0").decode("latin1")
            vs, va, rs, ra = struct.unpack_from("<IIII", self.d, o + 8)
            out.append((name, va, vs, ra, rs))
        return out

    def rva_to_off(self, rva):
        for _, va, vs, ra, rs in self.sections():
            if va <= rva < va + max(vs, rs):
                return ra + (rva - va)
        return None

    def dd(self, i):
        return struct.unpack_from("<II", self.d, self.ddir + i * 8)

    def set_dd(self, i, rva, size):
        struct.pack_into("<II", self.d, self.ddir + i * 8, rva, size)


def read_exports(pe):
    """Return {name: rva} for the image's current export directory."""
    rva, size = pe.dd(DIR_EXPORT)
    if not rva or not size:
        return {}
    o = pe.rva_to_off(rva)
    n_names = struct.unpack_from("<I", pe.d, o + 24)[0]
    a_func, a_name, a_ord = struct.unpack_from("<III", pe.d, o + 28)
    of, on, oo = pe.rva_to_off(a_func), pe.rva_to_off(a_name), pe.rva_to_off(a_ord)
    out = {}
    for i in range(n_names):
        nrva = struct.unpack_from("<I", pe.d, on + i * 4)[0]
        idx = struct.unpack_from("<H", pe.d, oo + i * 2)[0]
        frva = struct.unpack_from("<I", pe.d, of + idx * 4)[0]
        no = pe.rva_to_off(nrva)
        name = pe.d[no:pe.d.index(b"\0", no)].decode("latin1")
        out[name] = frva
    return out


def build_section(sec_rva, exports, dll_name):
    """Lay out the .gw2hst payload: stub + export directory.

    GetProcAddress binary-searches AddressOfNames, so the name table MUST be
    sorted by string. `exports` is a dict name -> rva; the stub is placed first
    and its RVA returned alongside the blob.
    """
    stub_rva = sec_rva
    body = bytearray(DLL_STUB)
    body += b"\0" * (0x10 - len(body))  # export dir starts 16-byte aligned

    names = sorted(exports)  # ASCII ascending -- required by GetProcAddress
    n = len(names)
    edir_off = len(body)
    a_func_off = edir_off + 40
    a_name_off = a_func_off + n * 4
    a_ord_off = a_name_off + n * 4
    strings_off = a_ord_off + n * 2

    strings = bytearray()
    name_rvas = []
    for nm in names:
        name_rvas.append(sec_rva + strings_off + len(strings))
        strings += nm.encode("latin1") + b"\0"
    dll_name_rva = sec_rva + strings_off + len(strings)
    strings += dll_name.encode("latin1") + b"\0"

    body += b"\0" * (strings_off - len(body))
    body += strings

    struct.pack_into("<IIHHIIIIIII", body, edir_off,
                     0, 0, 0, 0,              # Characteristics, TimeDateStamp, Major/Minor
                     dll_name_rva,            # Name
                     1,                       # Base (ordinal base)
                     n, n,                    # NumberOfFunctions, NumberOfNames
                     sec_rva + a_func_off,
                     sec_rva + a_name_off,
                     sec_rva + a_ord_off)
    for i, nm in enumerate(names):
        struct.pack_into("<I", body, a_func_off + i * 4, exports[nm])
        struct.pack_into("<I", body, a_name_off + i * 4, name_rvas[i])
        struct.pack_into("<H", body, a_ord_off + i * 2, i)

    return bytes(body), stub_rva, sec_rva + edir_off, len(body) - edir_off


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]

    pe = Pe(open(src, "rb").read())
    secs = pe.sections()

    # An 8th section header must fit inside the space the headers already claim.
    hdr_end = pe.sec_tab + pe.nsec * 40
    if hdr_end + 40 > pe.size_of_headers:
        raise SystemExit("no room for another section header (%#x + 40 > %#x)"
                         % (hdr_end, pe.size_of_headers))

    orig_ep = struct.unpack_from("<I", pe.d, pe.oh + 16)[0]
    if orig_ep == 0:
        raise SystemExit("image has no entry point")

    last_rva = max(va + vs for _, va, vs, _, _ in secs)
    sec_rva = align_up(last_rva, pe.sect_align)
    sec_raw = align_up(len(pe.d), pe.file_align)

    exports = read_exports(pe)
    keep = {k: v for k, v in exports.items() if k in LEGACY_EXPORTS}

    # First pass with a placeholder so the stub RVA is known, then final.
    keep["Gw2RealEntry"] = orig_ep       # the untouched CRT entry -> WinMain
    keep["Gw2HostEntry"] = sec_rva       # the DllMain stub (fixed at sec start)
    blob, stub_rva, edir_rva, edir_size = build_section(sec_rva, keep, "Gw2-64.dll")

    raw_size = align_up(len(blob), pe.file_align)
    blob = blob + b"\0" * (raw_size - len(blob))

    # --- patch headers ------------------------------------------------------
    struct.pack_into("<H", pe.d, pe.pe + 6, pe.nsec + 1)
    chars = struct.unpack_from("<H", pe.d, pe.pe + 22)[0]
    struct.pack_into("<H", pe.d, pe.pe + 22, chars | IMAGE_FILE_DLL)
    struct.pack_into("<I", pe.d, pe.oh + 16, stub_rva)                    # AddressOfEntryPoint
    struct.pack_into("<I", pe.d, pe.oh + 56,                              # SizeOfImage
                     align_up(sec_rva + len(blob), pe.sect_align))
    struct.pack_into("<I", pe.d, pe.oh + 64, 0)                           # CheckSum
    pe.set_dd(DIR_EXPORT, edir_rva, edir_size)
    pe.set_dd(DIR_SECURITY, 0, 0)                                         # signature is void

    hdr = struct.pack("<8sIIII", b".gw2hst", len(blob), sec_rva, raw_size, sec_raw)
    hdr += struct.pack("<IIHHI", 0, 0, 0, 0, SCN_CODE_RX)
    assert len(hdr) == 40
    pe.d[hdr_end:hdr_end + 40] = hdr

    out = bytes(pe.d) + b"\0" * (sec_raw - len(pe.d)) + blob
    open(dst, "wb").write(out)

    print("  source            %s (%d bytes)" % (src, len(pe.d)))
    print("  image base        %#x" % pe.imagebase)
    print("  original entry    %#x  (VA %#x)" % (orig_ep, pe.imagebase + orig_ep))
    print("  .gw2hst           rva %#x  raw %#x  size %#x" % (sec_rva, sec_raw, len(blob)))
    print("  DllMain stub      %#x" % stub_rva)
    print("  exports           %s" % ", ".join(sorted(keep)))
    print("  wrote             %s (%d bytes)" % (dst, len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
