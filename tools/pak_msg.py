#!/usr/bin/env python3
"""Read RE2's text (.msg files) straight out of its .pak archives.

    python3 tools/pak_msg.py --game "$GAME_DIR" --scan              # every message file: hash, entries, sample names
    python3 tools/pak_msg.py --game "$GAME_DIR" --dump 0x1234abcd   # one file's entries (name = English text)
    python3 tools/pak_msg.py --game "$GAME_DIR" --grep 'sm7._'      # entry names / English text matching a regex

The mod needs the game's own item and weapon names: the type database only has
internal codes (Item.ID sm70_000, WeaponType WP0000). The archives (KPKA v4) keep
a plain table of 48-byte entries keyed by murmur3 hashes of the path, so the
message files are found by content instead: every small entry is partially
inflated and checked for the GMSG magic. Later patch archives override earlier
ones entry by entry. Message strings are UTF-16 in a pool obfuscated with RE
Engine's rolling XOR. Pure stdlib (Python 3.14 for compression.zstd).
"""
import argparse, os, re, struct, sys, zlib

try:
    from compression import zstd
except ImportError:  # pragma: no cover
    zstd = None

MSG_KEY = bytes([0xCF, 0xCE, 0xFB, 0xF8, 0xEC, 0x0A, 0x33, 0x66, 0x93, 0xA9, 0x1D, 0x93, 0x50, 0x39, 0x5F, 0x09])
LANGS = ["Japanese", "English", "French", "Italian", "German", "Spanish", "Russian", "Polish", "Dutch", "Portuguese",
         "PortugueseBr", "Korean", "TransitionalChinese", "SimplifiedChinese", "Finnish", "Swedish", "Danish",
         "Norwegian", "Czech", "Hungarian", "Slovak", "Arabic", "Turkish", "Bulgarian", "Greek", "Romanian", "Thai",
         "Ukrainian", "Vietnamese", "Indonesian", "Fiction", "Hindi", "LatinAmericanSpanish"]


class Pak:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(16)
        magic, major, minor, flags, count, _ = struct.unpack("<4sBBHII", hdr)
        if magic != b"KPKA" or major != 4:
            raise ValueError("%s: not a KPKA v4 archive" % path)
        if flags:
            raise ValueError("%s: feature flags 0x%X (encrypted table?) not supported" % (path, flags))
        raw = self.f.read(count * 48)
        self.entries = []
        for i in range(count):
            lo, up, off, csz, dsz, attr, _crc = struct.unpack_from("<IIQQQQQ", raw, i * 48)
            self.entries.append((lo, up, off, csz, dsz, attr))

    def read(self, entry, limit=None):
        lo, up, off, csz, dsz, attr = entry
        self.f.seek(off)
        data = self.f.read(csz if limit is None else min(csz, limit))
        kind = attr & 0xF
        if kind == 0:
            return data if limit is None else data[:limit]
        if kind == 1:
            d = zlib.decompressobj(-15)
            return d.decompress(data) if limit is None else d.decompress(data, 4096)
        if kind == 2:
            if limit is None:
                return zstd.decompress(data)
            d = zstd.ZstdDecompressor()
            try:
                return d.decompress(data, max_length=4096)
            except zstd.ZstdError:
                return b""
        raise ValueError("compression %d" % kind)


def open_paks(game):
    """The base archive and its patches, later ones overriding earlier ones."""
    names = ["re_chunk_000.pak"] + ["re_chunk_000.pak.patch_%03d.pak" % i for i in range(1, 100)]
    table = {}
    paks = []
    for n in names:
        p = os.path.join(game, n)
        if not os.path.exists(p) or os.path.getsize(p) < 16:
            continue
        pak = Pak(p)
        paks.append(pak)
        for e in pak.entries:
            table[(e[0], e[1])] = (pak, e)
    return paks, table


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
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    return h ^ (h >> 16)


def path_key(path):
    """A file's key in the entry table: murmur3 of its lower- and upper-case path,
    platform folder included (natives/stm/...). So a file can be asked for by name."""
    p = path.replace("\\", "/")
    return murmur3(p.lower().encode("utf-16-le")), murmur3(p.upper().encode("utf-16-le"))


def decrypt(data):
    out = bytearray(len(data))
    prev = 0
    for i, cur in enumerate(data):
        out[i] = cur ^ prev ^ MSG_KEY[i & 0xF]
        prev = cur
    return bytes(out)


def wstr(buf, off):
    if off < 0 or off >= len(buf):
        return None
    end = off
    while end + 1 < len(buf) and (buf[end] or buf[end + 1]):
        end += 2
    return buf[off:end].decode("utf-16-le", "replace")


class Msg:
    """A GMSG file. The header layout moved between versions, so both known shapes
    are tried and the one whose entry table and names make sense is kept."""

    def __init__(self, data):
        self.version, magic = struct.unpack_from("<I4s", data, 0)
        if magic != b"GMSG":
            raise ValueError("no GMSG")
        n_entries, n_attrs, n_langs = struct.unpack_from("<III", data, 0x10)
        self.n_langs = n_langs
        last_err = None
        for has_unk in (True, False):
            try:
                o = 0x20
                data_off = struct.unpack_from("<Q", data, o)[0]; o += 8
                if has_unk:
                    o += 8
                lang_off, attr_off, attr_name_off = struct.unpack_from("<QQQ", data, o); o += 24
                ent_offs = struct.unpack_from("<%dQ" % n_entries, data, o)
                if data_off > len(data) or any(e >= data_off or e < o for e in ent_offs):
                    raise ValueError("entry table out of range")
                langs = struct.unpack_from("<%dI" % n_langs, data, lang_off)
                if any(l > 64 for l in langs):
                    raise ValueError("language ids out of range")
                pool = data[:data_off] + decrypt(data[data_off:])
                entries = []
                # entry: guid, crc, hash/index, name, attributes, one string per language
                for eo in ent_offs:
                    guid = data[eo:eo + 16]
                    name_off = struct.unpack_from("<Q", data, eo + 0x18)[0]
                    strs = struct.unpack_from("<%dQ" % n_langs, data, eo + 0x28)
                    name = wstr(pool, name_off)
                    if name is None:
                        raise ValueError("bad name offset")
                    entries.append((guid, name, [wstr(pool, s) for s in strs]))
                self.langs = list(langs)
                self.entries = entries
                return
            except (ValueError, struct.error) as ex:
                last_err = ex
        raise ValueError("unparsed GMSG v%d: %s" % (self.version, last_err))

    def text(self, entry, lang=1):
        guid, name, strs = entry
        if lang in self.langs:
            return strs[self.langs.index(lang)]
        return strs[lang] if lang < len(strs) else None


def find_msgs(table):
    out = []
    for key, (pak, e) in table.items():
        if e[4] > 16 << 20:
            continue
        try:
            head = pak.read(e, limit=65536)
            # A zstd frame yields nothing until its first block is in whole, and
            # a big message file's can be longer than 64 KB (the records' text,
            # the options, the tutorials - 42 files): read further for those.
            if len(head) < 8 and e[3] > 65536:
                head = pak.read(e, limit=256 << 10)
        except Exception:
            continue
        if len(head) >= 8 and head[4:8] == b"GMSG":
            out.append((key, pak, e))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="the game folder (with re_chunk_000.pak)")
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--dump", help="lower-case path hash of one message file (from --scan)")
    ap.add_argument("--grep", help="regex over entry names and English text")
    ap.add_argument("--lang", type=int, default=1, help="language id (1 = English)")
    a = ap.parse_args()
    if zstd is None:
        raise SystemExit("needs Python 3.14+ (compression.zstd)")
    paks, table = open_paks(a.game)
    print("%d archives, %d distinct entries" % (len(paks), len(table)), file=sys.stderr)
    msgs = find_msgs(table)
    print("%d message files" % len(msgs), file=sys.stderr)
    want = int(a.dump, 16) if a.dump else None
    rx = re.compile(a.grep) if a.grep else None
    for (lo, up), pak, e in sorted(msgs, key=lambda x: x[0]):
        if want is not None and lo != want:
            continue
        try:
            msg = Msg(pak.read(e))
        except Exception as ex:
            if a.scan:
                print("0x%08X  (%s)" % (lo, ex))
            continue
        if a.scan:
            sample = ", ".join(n for _, n, _ in msg.entries[:4])
            print("0x%08X v%d %5d entries, langs %s: %s" % (lo, msg.version, len(msg.entries),
                                                          ",".join(map(str, msg.langs[:3])), sample))
        if want is not None:
            for ent in msg.entries:
                print("%s\t%s" % (ent[1], (msg.text(ent, a.lang) or "").replace("\n", "\\n")))
        if rx:
            for ent in msg.entries:
                t = msg.text(ent, a.lang) or ""
                if rx.search(ent[1]) or rx.search(t):
                    print("0x%08X %s\t%s" % (lo, ent[1], t.replace("\n", "\\n")))


if __name__ == "__main__":
    main()
