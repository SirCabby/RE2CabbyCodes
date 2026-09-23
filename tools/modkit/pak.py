#!/usr/bin/env python3
"""Look inside RE2's pak archives by path - no file list needed.

    pak.py probe   PATH...            does this path exist?  (paths may also come on stdin)
    pak.py extract PATH... -o DIR     write the retail file(s) out
    pak.py refs    TEXT [--max N]     which files mention TEXT (UTF-16)?  -> what loads a model
    pak.py recover SIZE... -o DIR     retail files whose entry a mod manager zeroed, found by size

The entry table is keyed by murmur3 (seed 0xFFFFFFFF) of the UTF-16-LE lower- and upper-case path,
platform folder included (natives/stm/...).  Fluffy's "invalidate" install zeroes that pair in place,
so an installed mod hides the retail file from `probe`/`extract`; `recover` still finds the data, by
its decompressed size (note sizes with `probe` BEFORE installing a mod).  Python 3.14 (compression.zstd).
"""
import argparse, os, re, struct, sys, zlib
from compression import zstd


def game_dir(arg):
    if arg:
        return arg
    mk = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "config.mk")
    if os.path.exists(mk):
        m = re.search(r"^GAME_DIR\s*:?=\s*(.+)$", open(mk).read(), re.M)
        if m:
            return m.group(1).strip()
    sys.exit("pass --game (or set GAME_DIR in config.mk)")


def murmur3(data, seed=0xFFFFFFFF):
    c1, c2, h = 0xCC9E2D51, 0x1B873593, seed
    n = len(data) // 4 * 4
    for i in range(0, n, 4):
        k = (struct.unpack_from("<I", data, i)[0] * c1) & 0xFFFFFFFF
        k = (((k << 15) | (k >> 17)) * c2) & 0xFFFFFFFF
        h ^= k
        h = ((((h << 13) | (h >> 19)) & 0xFFFFFFFF) * 5 + 0xE6546B64) & 0xFFFFFFFF
    k = 0
    for i, b in enumerate(data[n:]):
        k |= b << (8 * i)
    if len(data) > n:
        k = (k * c1) & 0xFFFFFFFF
        h ^= (((k << 15) | (k >> 17)) * c2) & 0xFFFFFFFF
    h ^= len(data)
    h ^= h >> 16; h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13; h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    return h ^ (h >> 16)


def key(path):
    p = path.replace("\\", "/")
    return murmur3(p.lower().encode("utf-16-le")), murmur3(p.upper().encode("utf-16-le"))


def paks(game):
    names = ["re_chunk_000.pak"] + ["re_chunk_000.pak.patch_%03d.pak" % i for i in range(1, 100)]
    for n in names:
        p = os.path.join(game, n)
        if os.path.exists(p) and os.path.getsize(p) >= 16:
            yield p


def entries(pak):
    with open(pak, "rb") as f:
        magic, _maj, _min, _flags, count, _ = struct.unpack("<4sBBHII", f.read(16))
        raw = f.read(count * 48)
    for i in range(count):
        lo, up, off, csz, dsz, attr, _crc = struct.unpack_from("<IIQQQQQ", raw, i * 48)
        yield lo, up, off, csz, dsz, attr


def read(pak, off, csz, attr):
    with open(pak, "rb") as f:
        f.seek(off)
        data = f.read(csz)
    kind = attr & 0xF
    return zlib.decompressobj(-15).decompress(data) if kind == 1 else zstd.decompress(data) if kind == 2 else data


def table(game):
    t = {}
    for pak in paks(game):                       # later archives override earlier ones
        for lo, up, off, csz, dsz, attr in entries(pak):
            t[(lo, up)] = (pak, off, csz, dsz, attr)
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["probe", "extract", "refs", "recover"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--game"); ap.add_argument("-o", "--out", default="."); ap.add_argument("--max", type=int, default=400000)
    a = ap.parse_args()
    game = game_dir(a.game)
    if a.cmd in ("probe", "extract"):
        t = table(game)
        for path in (a.args or [l.strip() for l in sys.stdin if l.strip()]):
            hit = t.get(key(path))
            if not hit:
                if a.cmd == "extract": print("missing  %s" % path)
                continue
            pak, off, csz, dsz, attr = hit
            print("%10d  %s" % (dsz, path))
            if a.cmd == "extract":
                os.makedirs(a.out, exist_ok=True)
                open(os.path.join(a.out, os.path.basename(path)), "wb").write(read(pak, off, csz, attr))
    elif a.cmd == "refs":
        needle = a.args[0].encode("utf-16-le").lower()
        for pak in paks(game):
            for lo, up, off, csz, dsz, attr in entries(pak):
                if 64 <= dsz <= a.max:
                    try: data = read(pak, off, csz, attr)
                    except Exception: continue
                    if needle in data.lower():
                        os.makedirs(a.out, exist_ok=True)
                        name = "ref_%08X_%08X_%d.bin" % (lo, up, dsz)
                        open(os.path.join(a.out, name), "wb").write(data)
                        print("%10d  %s   (strings -el to read it)" % (dsz, name))
    else:
        want = {int(s) for s in a.args}
        for pak in paks(game):
            for lo, up, off, csz, dsz, attr in entries(pak):
                if dsz in want and lo == 0 and up == 0:
                    os.makedirs(a.out, exist_ok=True)
                    data = read(pak, off, csz, attr)
                    name = "recovered_%d_%s" % (dsz, data[:4].decode("ascii", "replace").strip("\0"))
                    open(os.path.join(a.out, name), "wb").write(data)
                    print("%10d  %s" % (dsz, name))


if __name__ == "__main__":
    main()
