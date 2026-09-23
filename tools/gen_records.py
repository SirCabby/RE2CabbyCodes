#!/usr/bin/env python3
"""The records panel's text, out of the game's own files.

    python3 tools/gen_records.py --game "$GAME_DIR" --out src/records_text.h

RE2 keeps two sets of records: the main game's (RecordManager, 91 records) and The
Ghost Survivors' (RogueRecordManager, 15). The mod reads and writes them in memory by
name (src/records.cpp); what it cannot get from memory is their English text - the
game looks that up in its message files as it draws them. This takes it from the same
places the game does:

  - the main records' names and conditions: the record table (RecordListUserData,
    found by its class's type id in the archives) gives each RecordId a title and a
    condition message GUID; the messages are Mes_Sys_Bouns_RecordNN and their
    conditions. The four HIDDEN records have none (the panel names them by reward).
  - The Ghost Survivors' records: the game picks their messages in code
    (RogueRecordManager.getRogueRecordGuid), by the scheme its message names follow -
    Mes_LostS_Record_{A..D} for a scenario cleared on either difficulty
    (CLEAR_TRAINING), {A..D}1 on normal (CLEAR_NORMAL), {A..D}2 its special condition
    (SPECIAL_CONDITION), Etc1..3 the COUNT_UP ones in RogueRecordId order. The rogue
    table (RogueRecordUserData) gives each record its type and scenario.
  - the rewards: RecordManager fills each RewardNode's NameGuid at run time, so the
    panel looks the GUID up in the table written here - every Mes_Sys_Reward_*,
    Mes_Sys_Costume_Rewards* and WEAPON_NAME_* message, keyed by GUID.
  - the accessories (The Ghost Survivors' rewards): Mes_LostS_Reward_Accessory_NN by
    RogueRewardId.

Text is cleaned for the panel: markup tags dropped, line breaks made spaces, and the
characters ImGui's default font lacks (the infinity sign, typographic quotes) spelled
out. "{0}" stays: the panel puts the record's goal in its place. Pure stdlib.
"""
import argparse, os, re, struct, sys, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb_dump import PE, TDB, F_LITERAL, F_STATIC  # noqa: E402
from pak_msg import open_paks, find_msgs, Msg  # noqa: E402

# Field types as RSZ stores them: (size, alignment).
RSZ_TYPES = {"System.Int32": (4, 4), "System.UInt32": (4, 4), "System.Single": (4, 4), "System.Boolean": (1, 1),
             "System.Guid": (16, 8), "System.Int64": (8, 8), "System.UInt64": (8, 8)}


def clean(t):
    if t is None:
        return None
    t = re.sub(r"</?[A-Z]+[^>]*>", "", t)
    t = t.replace("\r", "").replace("\n", " ").replace("\\n", " ")
    t = t.replace("∞", "infinite").replace("’", "'").replace("‘", "'").replace("“", '"')
    t = t.replace("”", '"').replace("–", "-").replace("—", "-").replace("…", "...")
    t = "".join(c if ord(c) < 0x100 else "?" for c in t)
    return re.sub(r"\s+", " ", t).strip()


def cstr(s):
    if s is None:
        return "nullptr"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def rsz_at(data):
    """Where a user data file's RSZ block starts: the USR header's data offset (+0x20, after
    the resource and user data tables' offsets at +0x10 and +0x18), or wherever the magic
    is in the first 0x100 bytes when that one points elsewhere."""
    r = struct.unpack_from("<Q", data, 0x20)[0] if len(data) >= 0x28 else -1
    if 0 <= r < len(data) - 4 and data[r:r + 4] == b"RSZ\0":
        return r
    return data.find(b"RSZ\0", 0, 0x100)


class Rsz:
    """One user data file's RSZ block: its instances, each parsed field by field
    (the class's instance fields in database order, each aligned to its type)."""

    def __init__(self, data, tdb, by_fqn):
        if data[:4] != b"USR\0":
            raise ValueError("not a user data file")
        r = rsz_at(data)
        magic, _ver, nobj, ninst, _nuser, _res, ioff, doff, _uoff = struct.unpack_from("<4sIiiiiQQQ", data, r)
        if magic != b"RSZ\0":
            raise ValueError("no RSZ block")
        self.types = [struct.unpack_from("<II", data, r + ioff + 8 * i)[0] for i in range(ninst)]
        self.instances = [None] * ninst
        o = r + doff
        for i in range(1, ninst):
            t = by_fqn.get(self.types[i])
            if t is None:
                raise ValueError("instance %d: type id %08X not in the database" % (i, self.types[i]))
            vals = {}
            for f in tdb.fields_of(t):
                if f["flags"] & (F_STATIC | F_LITERAL):
                    continue
                tn = tdb.name(f["type"])
                ft = tdb.typedef(f["type"])
                if ft["obj_type"] == 5 and tdb.name(ft["parent"]) == "System.Enum":
                    tn = "System.Int32"
                if tn.startswith("System.Collections.Generic.List`1<"):
                    o = (o + 3) & ~3
                    n = struct.unpack_from("<I", data, o)[0]
                    o += 4
                    vals[f["name"]] = list(struct.unpack_from("<%dI" % n, data, o))
                    o += 4 * n
                    continue
                if tn not in RSZ_TYPES:
                    raise ValueError("%s.%s: field type %s not handled" % (tdb.name(t), f["name"], tn))
                size, align = RSZ_TYPES[tn]
                o = (o + align - 1) & ~(align - 1)
                raw = data[o:o + size]
                o += size
                if tn == "System.Guid":
                    vals[f["name"]] = raw
                elif tn == "System.Boolean":
                    vals[f["name"]] = raw[0] != 0
                else:
                    vals[f["name"]] = struct.unpack("<" + {4: "i", 8: "q"}[size], raw)[0]
            self.instances[i] = (tdb.name(t), vals)
        self.end = o - r
        self.size = len(data) - r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    tdb = TDB(PE(os.path.join(a.game, "re2.exe")))

    def enum(name):
        t = tdb.find_type(name)
        return {f["name"]: tdb.literal_value(f) for f in tdb.fields_of(t) if f["flags"] & F_LITERAL}

    want = {n: tdb.find_type(n) for n in ("app.ropeway.RecordListUserData", "app.ropeway.RecordSingleData",
                                          "app.ropeway.RogueRecordUserData", "app.ropeway.RogueRecordSingleData")}
    by_fqn = {tdb.typedef(t)["fqn"]: t for t in want.values()}
    record_ids = enum("app.ropeway.gamemastering.RecordManager.RecordId")
    rogue_ids = enum("app.ropeway.gamemastering.RogueRecordManager.RogueRecordId")
    rogue_types = {v: k for k, v in enum("app.ropeway.gamemastering.RogueRecordManager.RogueRecordType").items()}
    n_records, n_rogue = record_ids["MAX"], rogue_ids["MAX"]

    paks, table = open_paks(a.game)
    # The two tables, by the type ids of their instances (no file list needed).
    tables = {}
    for key, (pak, e) in table.items():
        if e[4] > 256 << 10:
            continue
        try:
            data = pak.read(e)
        except Exception:
            continue
        if data[:4] != b"USR\0" or b"RSZ\0" not in data[:0x100]:
            continue
        try:
            head = rsz_at(data)
            ninst, ioff = struct.unpack_from("<i", data, head + 0xC)[0], struct.unpack_from("<Q", data, head + 0x18)[0]
            kinds = {struct.unpack_from("<I", data, head + ioff + 8 * i)[0] for i in range(ninst)}
        except (struct.error, ValueError):
            continue
        for n in ("app.ropeway.RecordListUserData", "app.ropeway.RogueRecordUserData"):
            if tdb.typedef(want[n])["fqn"] in kinds:
                if n in tables:
                    raise SystemExit("two %s files in the archives (%s and %s)" % (n, tables[n][0], key))
                tables[n] = (key, Rsz(data, tdb, by_fqn))
    for n in ("app.ropeway.RecordListUserData", "app.ropeway.RogueRecordUserData"):
        if n not in tables:
            raise SystemExit("%s not found in the archives" % n)
        key, rsz = tables[n]
        if rsz.end != rsz.size:
            raise SystemExit("%s: parsed %d of %d bytes - the field layout is not what this expects" % (n, rsz.end, rsz.size))
        print("%s: pak entry %s, %d instances" % (n, key, len(rsz.instances) - 1))

    # Every message, English, by name and by GUID.
    by_name, by_guid = {}, {}
    for _, pak, e in find_msgs(table):
        try:
            msg = Msg(pak.read(e))
        except Exception:
            continue
        for ent in msg.entries:
            t = msg.text(ent, 1)
            by_name.setdefault(ent[1], (ent[0], t))
            by_guid.setdefault(ent[0], (ent[1], t))

    def text(guid):
        hit = by_guid.get(guid)
        return clean(hit[1]) if hit and hit[1] and "#Rejected#" not in hit[1] else None

    # The main records, by RecordId.
    records = [(None, None)] * n_records
    for kind, vals in (i for i in tables["app.ropeway.RecordListUserData"][1].instances[1:] if i):
        if kind != "app.ropeway.RecordSingleData":
            continue
        rid = vals["RecordId"]
        if 0 <= rid < n_records:
            records[rid] = (text(vals["RecordTitleId"]), text(vals["RecordConditionId"]))
    unnamed = [rid + 1 for rid in range(n_records) if not records[rid][0]]

    # The Ghost Survivors' records, by RogueRecordId.
    rogue = [(None, None)] * n_rogue
    counted = []
    rows = [vals for kind, vals in (i for i in tables["app.ropeway.RogueRecordUserData"][1].instances[1:] if i)
            if kind == "app.ropeway.RogueRecordSingleData"]
    for vals in sorted(rows, key=lambda v: v["RecordId"]):
        rid, kind, lost = vals["RecordId"], rogue_types.get(vals["RecordType"]), vals["LostType"]
        if not 0 <= rid < n_rogue or not 0 <= lost < 4:
            continue
        letter = "ABCD"[lost]
        if kind == "CLEAR_TRAINING":
            key = letter
        elif kind == "CLEAR_NORMAL":
            key = letter + "1"
        elif kind == "SPECIAL_CONDITION":
            key = letter + "2"
        elif kind == "COUNT_UP":
            counted.append(rid)
            key = "Etc%d" % len(counted)
        else:
            continue
        name = by_name.get("Mes_LostS_Record_" + key)
        cond = by_name.get("Mes_LostS_Record_" + key + "_Guide")
        rogue[rid] = (clean(name[1]) if name else None, clean(cond[1]) if cond else None)
    accessories = []
    for i in range(n_rogue):
        hit = by_name.get("Mes_LostS_Reward_Accessory_%02d" % i)
        accessories.append(clean(hit[1]) if hit else None)

    # The rewards' names, by the GUID the game gives each RewardNode.
    texts = {}
    for name, (guid, t) in by_name.items():
        if not t or "#Rejected#" in t or name.endswith(("Dummy", "Guide")):
            continue
        if name.startswith(("Mes_Sys_Reward_", "Mes_Sys_Costume_Rewards", "WEAPON_NAME_")):
            texts[guid] = clean(t)

    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "// Generated by tools/gen_records.py from the game's record tables and its English text - do not",
        "// edit by hand. The records panel's names: the main records by RecordId, The Ghost Survivors'",
        "// records by RogueRecordId and their accessories by RogueRewardId, and the rewards' names by the",
        "// message GUID RecordManager gives each RewardNode. \"{0}\" in a condition is the record's goal.",
        "namespace re2cc::records_text {",
        "",
        "struct Record {",
        "  const char* name;       // nullptr: the game has none (a hidden record)",
        "  const char* condition;",
        "};",
        "",
        "inline constexpr int kRecords = %d;  // RecordManager.RecordId.MAX" % n_records,
        "inline constexpr Record kRecord[kRecords] = {",
    ]
    for rid, (n, c) in enumerate(records):
        lines.append("    {%s, %s},  // RECORD_%03d" % (cstr(n), cstr(c), rid + 1))
    lines += ["};", "", "inline constexpr int kRogueRecords = %d;  // RogueRecordManager.RogueRecordId.MAX" % n_rogue,
              "inline constexpr Record kRogueRecord[kRogueRecords] = {"]
    for rid, (n, c) in enumerate(rogue):
        lines.append("    {%s, %s},  // ROGUE_RECORD_%02d" % (cstr(n), cstr(c), rid))
    lines += ["};", "", "inline constexpr const char* kAccessory[kRogueRecords] = {"]
    for i, n in enumerate(accessories):
        lines.append("    %s,  // ACCESSORY_%02d" % (cstr(n), i))
    lines += ["};", "", "// A message GUID as the game stores a System.Guid: its 16 bytes, read as two little-endian words.",
              "struct Text {", "  uint64_t lo, hi;", "  const char* text;", "};", "",
              "inline constexpr Text kRewardText[] = {  // sorted by (hi, lo)"]
    for guid in sorted(texts, key=lambda g: (struct.unpack_from("<Q", g, 8)[0], struct.unpack_from("<Q", g, 0)[0])):
        lo, hi = struct.unpack_from("<QQ", guid)
        lines.append("    {0x%016XULL, 0x%016XULL, %s},  // %s" % (lo, hi, cstr(texts[guid]), uuid.UUID(bytes_le=guid)))
    lines += ["};", "", "}  // namespace re2cc::records_text"]
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d records (%d without text: %s), %d Ghost Survivors records, %d accessories, %d reward texts" % (
        a.out, n_records, len(unnamed), ", ".join("RECORD_%03d" % r for r in unnamed), n_rogue,
        sum(1 for x in accessories if x), len(texts)))
    missing = [i for i, (n, c) in enumerate(rogue) if not n]
    if missing:
        print("warning: Ghost Survivors records without text: %s" % missing)


if __name__ == "__main__":
    main()
