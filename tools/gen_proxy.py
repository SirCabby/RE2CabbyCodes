#!/usr/bin/env python3
"""Generate a proxy-DLL export list from the original DLL's export table.

    python3 tools/gen_proxy.py --exe "$GAME_DIR/re2.exe" --dll steam_api64.dll \
        --from-dll "$GAME_DIR/steam_api64_orig.dll" --def steam_api64.def \
        --inc src/proxy_exports.inc --prefix steam

Emits a .def aliasing every export name onto a plain C symbol <prefix>_<n>, and
an X-macro include listing (index, exported name) for src/proxy.cpp, which
builds one jmp thunk per entry. --from-dll takes the complete export table of
the original DLL (a superset of what the exe imports, so anything the game
resolves through GetProcAddress at run time is covered too). Exports that live
in a non-executable section are data and cannot be jump thunks; they become PE
forwarders to <lib>_orig.NAME. Handles PE32 and PE32+, and the exe's delay-load
table as well as its import table (re2.exe delay-loads steam_api64.dll). Pure
stdlib.
"""
import argparse, struct


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
        self.pe32plus = struct.unpack_from("<H", d, opt)[0] == 0x20B
        self.dirs_at = opt + (112 if self.pe32plus else 96)
        self.secs = []
        for i in range(nsec):
            o = opt + opt_sz + i * 40
            vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.secs.append((va, max(vsz, rsz), rptr, chars))

    def dir(self, i):
        return struct.unpack_from("<II", self.d, self.dirs_at + 8 * i)

    def off(self, rva):
        for va, sz, rptr, _ in self.secs:
            if va <= rva < va + sz:
                return rptr + (rva - va)
        raise ValueError(f"rva {rva:#x} not in any section")

    def executable(self, rva):
        for va, sz, _, chars in self.secs:
            if va <= rva < va + sz:
                return bool(chars & 0x20000000)
        return False

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.find(b"\0", o)].decode("ascii")

    def thunk_names(self, rva):
        size, ordflag = (8, 1 << 63) if self.pe32plus else (4, 1 << 31)
        fmt = "<Q" if self.pe32plus else "<I"
        out, o = [], self.off(rva)
        while True:
            t = struct.unpack_from(fmt, self.d, o)[0]
            if not t:
                return out
            out.append(f"#{t & 0xFFFF}" if t & ordflag else self.cstr((t & 0x7FFFFFFF) + 2))
            o += size


def imports_of(exe_path, dll_name):
    """Names the exe imports from dll_name, through its import table or its delay-load table."""
    pe = PE(exe_path)
    names = []
    rva, _ = pe.dir(1)
    if rva:
        o = pe.off(rva)
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", pe.d, o)
            if not name_rva:
                break
            if pe.cstr(name_rva).lower() == dll_name.lower():
                names += pe.thunk_names(oft or ft)
            o += 20
    rva, _ = pe.dir(13)
    if rva:
        o = pe.off(rva)
        while True:
            _, name_rva, _, _, int_rva, _, _, _ = struct.unpack_from("<8I", pe.d, o)
            if not name_rva:
                break
            if pe.cstr(name_rva).lower() == dll_name.lower():
                names += pe.thunk_names(int_rva)
            o += 32
    return names


def exports_of(dll_path):
    """Every named export of a PE DLL, in export-table order, with whether it is data."""
    pe = PE(dll_path)
    rva, _ = pe.dir(0)
    if not rva:
        return []
    e = pe.off(rva)
    n_names = struct.unpack_from("<I", pe.d, e + 24)[0]
    funcs_rva, names_rva, ords_rva = struct.unpack_from("<III", pe.d, e + 28)
    out = []
    for i in range(n_names):
        name = pe.cstr(struct.unpack_from("<I", pe.d, pe.off(names_rva) + i * 4)[0])
        ordinal = struct.unpack_from("<H", pe.d, pe.off(ords_rva) + i * 2)[0]
        frva = struct.unpack_from("<I", pe.d, pe.off(funcs_rva) + ordinal * 4)[0]
        out.append((name, not pe.executable(frva)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dll", required=True)
    ap.add_argument("--from-dll", required=True, help="the original DLL, whose whole export table is proxied")
    ap.add_argument("--def", dest="def_path", required=True)
    ap.add_argument("--inc", required=True)
    ap.add_argument("--prefix", default="proxy")
    a = ap.parse_args()
    imported = imports_of(a.exe, a.dll)
    if not imported:
        raise SystemExit(f"{a.dll}: {a.exe} neither imports nor delay-loads it")
    if [n for n in imported if n.startswith("#")]:
        raise SystemExit(f"{a.dll}: ordinal imports are not supported")
    exports = exports_of(a.from_dll)
    names = [n for n, is_data in exports if not is_data]
    data = [n for n, is_data in exports if is_data]
    missing = [n for n in imported if n not in names]
    if missing:
        raise SystemExit(f"{a.from_dll} lacks functions the exe needs: {missing}")
    lib = a.dll.rsplit(".", 1)[0]
    src = a.from_dll.replace("\\", "/").rsplit("/", 1)[-1]
    with open(a.def_path, "w") as f:
        f.write(f"; Generated by tools/gen_proxy.py from the export table of {src} - do not edit by hand.\n")
        f.write(f"; {len(names)} functions of {a.dll} ({len(imported)} of them used by the exe), each aliased onto\n")
        f.write(f"; a jmp thunk in src/proxy.cpp that jumps into {lib}_orig.dll; {len(data)} data export(s)\n")
        f.write(f"; forwarded there directly.\n")
        f.write(f"LIBRARY {lib}\nEXPORTS\n")
        for i, n in enumerate(names):
            f.write(f"    {n} = {a.prefix}_{i}\n")
        for n in data:
            f.write(f"    {n} = {lib}_orig.{n} DATA\n")
    with open(a.inc, "w") as f:
        f.write("// Generated by tools/gen_proxy.py - do not edit by hand.\n")
        f.write(f"// X(index, exported name) for every function {src} exports.\n")
        f.write("#define PROXY_EXPORTS(X) \\\n")
        for i, n in enumerate(names):
            f.write(f'  X({i}, "{n}") \\\n')
        f.write("\n")
        f.write(f"#define PROXY_EXPORT_COUNT {len(names)}\n")
    print(f"{a.dll}: {len(names)} function exports + {len(data)} data -> {a.def_path}, {a.inc} "
          f"(the exe uses {len(imported)})")


if __name__ == "__main__":
    main()
