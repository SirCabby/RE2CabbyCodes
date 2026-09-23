#!/usr/bin/env python3
"""Read RE Engine's type database (TDB v70) straight out of re2.exe on disk.

    python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --dump tdb.txt       # every type, one block each
    python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --type app.ropeway.gamemastering.InventoryManager
    python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --grep 'HitPoint'   # type/field/method names matching a regex
    python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --enum app.ropeway.gamemastering.Item.ID
    python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --methods "$GAME_DIR/RE2CabbyCodes.methods.bin" --type ...
                                                                            # with each method's code RVA (DumpMethods = 1)

RE2's executable carries its whole reflection database in .data, uninitialised: the
header's array pointers are offsets from the header itself (the VM turns them into
pointers when it starts, and sets `initialized`). Layout per REFramework's tdb70/tdb69
structs (shared/sdk/RETypeDB.hpp, RETypeDefinition.hpp), confirmed here by every
array ending exactly where the next begins. Method function pointers are absolute
VAs in the file (relocated at load); they are printed as RVAs. Pure stdlib.
"""
import argparse, mmap, re, struct, sys

IMAGE_BASE_DEFAULT = 0x140000000

# via::clr field / method flags (the .NET attribute values)
F_STATIC, F_LITERAL = 0x10, 0x40
M_STATIC, M_VIRTUAL, M_ABSTRACT = 0x10, 0x40, 0x400


class PE:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        m = self.m
        pe = struct.unpack_from("<I", m, 0x3C)[0]
        nsec = struct.unpack_from("<H", m, pe + 6)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from("<Q", m, opt + 24)[0]
        opt_sz = struct.unpack_from("<H", m, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + opt_sz + 40 * i
            name = m[o:o + 8].rstrip(b"\0").decode("latin1")
            vs, va, rs, ro = struct.unpack_from("<IIII", m, o + 8)
            self.secs.append((name, va, vs, ro, rs))

    def section(self, name):
        for s in self.secs:
            if s[0] == name:
                return s
        return None

    def rva_to_off(self, rva):
        for _, va, vs, ro, rs in self.secs:
            if va <= rva < va + max(vs, rs):
                return ro + rva - va if rva - va < rs else None
        return None

    def off_to_rva(self, off):
        for _, va, vs, ro, rs in self.secs:
            if ro <= off < ro + rs:
                return va + off - ro
        return None


class TDB:
    def __init__(self, pe, off=None):
        self.pe = pe
        self.runtime_rvas = {}  # method index -> code RVA, from a DumpMethods file (main's --methods)
        m = pe.m
        if off is None:
            off = self.find(pe)
        self.off = off
        self.rva = pe.off_to_rva(off)
        h = struct.unpack_from("<22I", m, off)
        (magic, self.version, self.initialized, self.n_types, self.n_methods, self.n_fields, self.n_type_impl,
         self.n_field_impl, self.n_method_impl, self.n_prop_impl, self.n_props, self.n_events, self.n_params,
         self.n_attrs, self.n_init_data, _attrs2, self.n_intern, self.n_modules, self.dev_entry, self.app_entry,
         self.n_strings, self.n_bytes) = h
        assert magic == 0x424454, "no TDB magic"
        ptrs = struct.unpack_from("<18Q", m, off + 0x58)
        (self.p_modules, self.p_types, self.p_types_impl, self.p_methods, self.p_methods_impl, self.p_fields,
         self.p_fields_impl, self.p_props, self.p_props_impl, self.p_events, self.p_params, self.p_attrs,
         self.p_init_data, _unk, self.p_attrs2, self.p_strings, self.p_bytes, self.p_intern) = ptrs
        if self.initialized:
            raise SystemExit("this TDB is initialised (absolute pointers) - read it from the exe on disk, not a dump")
        self._names = {}

    @staticmethod
    def find(pe):
        _, va, vs, ro, rs = pe.section(".data")
        m = pe.m
        pos = m.find(b"TDB\0", ro, ro + rs)
        while pos != -1:
            ver = struct.unpack_from("<I", m, pos + 4)[0]
            if ver == 70 and struct.unpack_from("<Q", m, pos + 0x60)[0] == 0x300:
                return pos
            pos = m.find(b"TDB\0", pos + 1, ro + rs)
        raise SystemExit("TDB v70 not found in .data")

    # --- raw access -------------------------------------------------------------------
    def string(self, off):
        if off >= self.n_strings:
            return "?"
        a = self.off + self.p_strings + off
        e = self.pe.m.find(b"\0", a)
        return self.pe.m[a:e].decode("utf-8", "replace")

    def byte_at(self, off):
        return self.off + self.p_bytes + off

    def typedef(self, i):
        """RETypeDefVersion69 (0x50)."""
        o = self.off + self.p_types + i * 0x50
        a, b, flags, size, fqn, crc, ctor, vt, mm, mf, prop, ev, ifaces, gen = struct.unpack_from("<QQIIIIIIIIIIii", self.pe.m, o)
        return dict(index=a & 0x3FFFF, parent=(a >> 18) & 0x3FFFF, declaring=(a >> 36) & 0x3FFFF,
                    underlying=(a >> 54) & 0x7F, obj_type=(a >> 61) & 7, array=b & 0x3FFFF,
                    element=(b >> 18) & 0x3FFFF, impl=(b >> 36) & 0x3FFFF, system=(b >> 54) & 0x3FF,
                    flags=flags, size=size, fqn=fqn, crc=crc, ctor=ctor, vt=vt, member_method=mm,
                    member_field=mf, num_prop=prop & 0xFFF, member_prop=(prop >> 12) & 0x3FFFF,
                    member_event=ev, interfaces=ifaces, generics=gen)

    def typeimpl(self, i):
        """RETypeImpl (0x30)."""
        o = self.off + self.p_types_impl + i * 0x30
        name, ns, fsz, sfsz, mod, rank, nmeth, nfld, iface, nnvt, attr, nvt = struct.unpack_from("<iiiiBBHihHHH", self.pe.m, o)
        return dict(name=name, ns=ns, field_size=fsz, static_size=sfsz, module=mod, rank=rank,
                    num_methods=nmeth, num_fields=nfld, num_native_vt=nnvt, num_vt=nvt)

    def method(self, i):
        o = self.off + self.p_methods + i * 16
        a, fn = struct.unpack_from("<QQ", self.pe.m, o)
        return dict(declaring=a & 0x3FFFF, impl=(a >> 18) & 0xFFFFF, params=(a >> 38) & 0x3FFFFFF, fn=fn)

    def method_impl(self, i):
        o = self.off + self.p_methods_impl + i * 12
        attr, vtidx, flags, iflags, name = struct.unpack_from("<HhHHI", self.pe.m, o)
        return dict(vt_index=vtidx, flags=flags, impl_flags=iflags, name=name & 0x3FFFFFFF)

    def field(self, i):
        o = self.off + self.p_fields + i * 8
        a = struct.unpack_from("<Q", self.pe.m, o)[0]
        return dict(declaring=a & 0x3FFFF, impl=(a >> 18) & 0xFFFFF, offset=(a >> 38) & 0x3FFFFFF)

    def field_impl(self, i):
        o = self.off + self.p_fields_impl + i * 12
        attr, flags, w1, w2 = struct.unpack_from("<HHII", self.pe.m, o)
        return dict(flags=flags, type=w1 & 0x3FFFF, init_lo=w1 >> 18, name=w2 & 0x3FFFFFFF, init_hi=w2 >> 30)

    def param(self, i):
        o = self.off + self.p_params + i * 12
        attr, init, w1, w2 = struct.unpack_from("<HHII", self.pe.m, o)
        return dict(name=w1 & 0x3FFFFFFF, modifier=w1 >> 30, type=w2 & 0x3FFFF, flags=w2 >> 18)

    def prop(self, i):
        o = self.off + self.p_props + i * 8
        a = struct.unpack_from("<Q", self.pe.m, o)[0]
        return dict(impl=a & 0xFFFFF, getter=(a >> 20) & 0x3FFFFF, setter=(a >> 42) & 0x3FFFFF)

    def prop_impl(self, i):
        o = self.off + self.p_props_impl + i * 8
        flags, attr, name = struct.unpack_from("<HHi", self.pe.m, o)
        return dict(flags=flags, name=name)

    def init_data(self, index):
        """Where a literal's value lives (byte pool, or the string pool for a negative offset)."""
        if index >= self.n_init_data:
            return None
        off = struct.unpack_from("<i", self.pe.m, self.off + self.p_init_data + index * 4)[0]
        if off < 0:
            return ("str", self.string(-off))
        return ("bytes", self.byte_at(off))

    # --- names --------------------------------------------------------------------------
    def short_name(self, i):
        t = self.typedef(i)
        ti = self.typeimpl(t["impl"])
        return self.string(ti["ns"]), self.string(ti["name"])

    def name(self, i, depth=0):
        if i in self._names:
            return self._names[i]
        if i == 0 or i >= self.n_types or depth > 16:
            return "?"
        t = self.typedef(i)
        ns, nm = self.short_name(i)
        if t["declaring"] and t["declaring"] != i and t["declaring"] < self.n_types:
            full = self.name(t["declaring"], depth + 1) + "." + nm
        else:
            full = (ns + "." if ns else "") + nm
        g = self.generic_args(i)
        if g:
            full += "<" + ",".join(self.name(x, depth + 1) for x in g) + ">"
        self._names[i] = full
        return full

    def generic_args(self, i):
        t = self.typedef(i)
        if t["generics"] <= 0 or t["generics"] >= self.n_bytes:
            return []
        w = struct.unpack_from("<I", self.pe.m, self.byte_at(t["generics"]))[0]
        definition, num = w & 0x3FFFF, w >> 18
        if definition == i or num == 0 or num > 16:
            return []
        return list(struct.unpack_from("<%dI" % num, self.pe.m, self.byte_at(t["generics"]) + 4))

    # --- members --------------------------------------------------------------------------
    def fields_of(self, i):
        t = self.typedef(i)
        n = self.typeimpl(t["impl"])["num_fields"]
        if not t["member_field"]:
            return []
        out = []
        for k in range(n):
            f = self.field(t["member_field"] + k)
            fi = self.field_impl(f["impl"])
            out.append(dict(index=t["member_field"] + k, name=self.string(fi["name"]), type=fi["type"],
                            offset=f["offset"], flags=fi["flags"], init=fi["init_lo"] | (fi["init_hi"] << 14)))
        return out

    def methods_of(self, i):
        t = self.typedef(i)
        n = self.typeimpl(t["impl"])["num_methods"]
        if not t["member_method"]:
            return []
        out = []
        for k in range(n):
            idx = t["member_method"] + k
            md = self.method(idx)
            mi = self.method_impl(md["impl"])
            ret, params = self.signature(md["params"])
            out.append(dict(index=idx, name=self.string(mi["name"]), fn=md["fn"], flags=mi["flags"],
                            vt_index=mi["vt_index"], ret=ret, params=params))
        return out

    def signature(self, pl):
        """(return type, [(type, name)]) from a ParamList in the byte pool."""
        if pl >= self.n_bytes:
            return 0, []
        num, invoke, ret = struct.unpack_from("<HHI", self.pe.m, self.byte_at(pl))
        if num > 64:
            return 0, []
        ids = struct.unpack_from("<%dI" % num, self.pe.m, self.byte_at(pl) + 8) if num else ()
        rt = self.param(ret)["type"] if ret < self.n_params else 0
        return rt, [(self.param(p)["type"], self.string(self.param(p)["name"])) for p in ids if p < self.n_params]

    def props_of(self, i):
        t = self.typedef(i)
        out = []
        for k in range(t["num_prop"]):
            p = self.prop(t["member_prop"] + k)
            pi = self.prop_impl(p["impl"])
            out.append(dict(name=self.string(pi["name"]), getter=p["getter"], setter=p["setter"]))
        return out

    def literal_value(self, f):
        d = self.init_data(f["init"])
        if not d:
            return None
        if d[0] == "str":
            return repr(d[1])
        size = {"System.Int8": 1, "System.UInt8": 1, "System.Byte": 1, "System.SByte": 1, "System.Int16": 2,
                "System.UInt16": 2, "System.Int64": 8, "System.UInt64": 8, "System.Boolean": 1}.get(self.name(f["type"]), 4)
        # an enum's literals carry the enum type: use its underlying value field
        ft = self.typedef(f["type"]) if f["type"] < self.n_types else None
        if ft and ft["obj_type"] == 5 and self.name(ft["parent"]) == "System.Enum":
            for g in self.fields_of(f["type"]):
                if not g["flags"] & F_STATIC:
                    size = {"System.Int8": 1, "System.UInt8": 1, "System.Byte": 1, "System.SByte": 1, "System.Int16": 2,
                            "System.UInt16": 2, "System.Int64": 8, "System.UInt64": 8}.get(self.name(g["type"]), 4)
                    break
        fmt = {1: "<b", 2: "<h", 4: "<i", 8: "<q"}[size]
        return struct.unpack_from(fmt, self.pe.m, d[1])[0]

    def find_type(self, name):
        for i in range(1, self.n_types):
            if self.name(i) == name:
                return i
        return None

    def rva(self, va):
        return va - self.pe.image_base if va else 0

    # --- printing ---------------------------------------------------------------------------
    def describe(self, i, inherited=True, out=sys.stdout):
        t = self.typedef(i)
        chain = []
        p = t["parent"]
        while p and len(chain) < 24:
            chain.append(self.name(p))
            p = self.typedef(p)["parent"]
        ti = self.typeimpl(t["impl"])
        print("type %d %s  size=0x%X fields=0x%X static=0x%X objtype=%d flags=0x%X fqn=%08X" % (
            i, self.name(i), t["size"], ti["field_size"], ti["static_size"], t["obj_type"], t["flags"], t["fqn"]), file=out)
        if chain:
            print("  : " + " : ".join(chain), file=out)
        levels = [i] + ([x for x in self._parents(i)] if inherited else [])
        for lv in levels:
            fs = self.fields_of(lv)
            if not fs:
                continue
            if lv != i:
                print("  -- fields of %s" % self.name(lv), file=out)
            for f in fs:
                tag = ""
                if f["flags"] & F_LITERAL:
                    tag = " = %s" % self.literal_value(f)
                elif f["flags"] & F_STATIC:
                    tag = " [static]"
                print("  field +0x%-5X %-50s %s%s" % (f["offset"], self.name(f["type"]), f["name"], tag), file=out)
        for p_ in self.props_of(i):
            print("  prop  %s (get %d set %d)" % (p_["name"], p_["getter"], p_["setter"]), file=out)
        for mth in self.methods_of(i):
            args = ", ".join("%s %s" % (self.name(a), n) for a, n in mth["params"])
            kind = ("static " if mth["flags"] & M_STATIC else "") + ("virtual[%d] " % mth["vt_index"] if mth["flags"] & M_VIRTUAL else "")
            code = self.rva(mth["fn"]) if mth["fn"] else self.runtime_rvas.get(mth["index"], 0)
            print("  meth  [%d] %s%s %s(%s)  @ %s" % (mth["index"], kind, self.name(mth["ret"]) if mth["ret"] else "void",
                                                     mth["name"], args, ("0x%X" % code) if code else "-"), file=out)

    def _parents(self, i):
        p = self.typedef(i)["parent"]
        while p and p < self.n_types:
            yield p
            p = self.typedef(p)["parent"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dump", help="write every type to this file")
    ap.add_argument("--type", action="append", default=[], help="describe a type (full name), inherited fields included")
    ap.add_argument("--grep", help="regex over type, field and method names")
    ap.add_argument("--enum", action="append", default=[], help="print an enum's values")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--methods", help="RE2CabbyCodes.methods.bin (DumpMethods = 1): print each method's code RVA")
    a = ap.parse_args()
    pe = PE(a.exe)
    tdb = TDB(pe)
    if a.methods:
        raw = open(a.methods, "rb").read()
        magic, version, count, _ = struct.unpack_from("<4I", raw, 0)
        if magic != 0x4D324552 or count != tdb.n_methods:
            sys.exit("%s is not a methods dump of this re2.exe (magic 0x%X, %d methods)" % (a.methods, magic, count))
        tdb.runtime_rvas = {i: r for i, r in enumerate(struct.unpack_from("<%dI" % count, raw, 16)) if r}
    if a.summary or not (a.dump or a.type or a.grep or a.enum):
        print("TDB v%d at file 0x%X (RVA 0x%X): %d types, %d methods, %d fields, %d params, strings 0x%X, bytes 0x%X" % (
            tdb.version, tdb.off, tdb.rva, tdb.n_types, tdb.n_methods, tdb.n_fields, tdb.n_params, tdb.n_strings, tdb.n_bytes))
        bad = sum(1 for i in range(1, tdb.n_types) if tdb.typedef(i)["index"] != i)
        print("typedef index self-check: %d mismatches" % bad)
    for name in a.type:
        i = tdb.find_type(name)
        if i is None:
            print("no type %s" % name)
            continue
        tdb.describe(i)
    for name in a.enum:
        i = tdb.find_type(name)
        if i is None:
            print("no type %s" % name)
            continue
        for f in tdb.fields_of(i):
            if f["flags"] & F_LITERAL:
                print("%s = %s" % (f["name"], tdb.literal_value(f)))
    if a.grep:
        rx = re.compile(a.grep)
        for i in range(1, tdb.n_types):
            n = tdb.name(i)
            if rx.search(n):
                print("TYPE  %s" % n)
            for f in tdb.fields_of(i):
                if rx.search(f["name"]):
                    print("FIELD %s.%s  (+0x%X %s%s)" % (n, f["name"], f["offset"], tdb.name(f["type"]),
                                                          " static" if f["flags"] & F_STATIC else ""))
            for mth in tdb.methods_of(i):
                if rx.search(mth["name"]):
                    code = tdb.runtime_rvas.get(mth["index"], 0)
                    print("METH  %s.%s  [%d]%s" % (n, mth["name"], mth["index"], "  @ 0x%X" % code if code else ""))
    if a.dump:
        with open(a.dump, "w") as out:
            for i in range(1, tdb.n_types):
                tdb.describe(i, inherited=False, out=out)
        print("wrote %s" % a.dump)


if __name__ == "__main__":
    main()
