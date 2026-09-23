#!/usr/bin/env python3
"""Generate src/items.h - RE2's items and weapons with the game's own English names.

    python3 tools/gen_items.py --game "$GAME_DIR" --out src/items.h

The ids are the values of the type database's Item.ID (sm70_000 ...) and
EquipmentDefine.WeaponType (WP0000 ...) enums, read from re2.exe; the names are
the game's own text (ITEM_NAME_70_000, WEAPON_NAME_WP0000, and for an item the
game renames once it is examined, the name it takes - see changed_name()), read
from its archives. The bank an item is filed under in the item box listing is the
game's own category (see banks()). Nothing here is a hand-written list except
which weapon types are offered in the pickers (kListedWeapons - the rest are the
game's development placeholders and variants) and the two items the panel files
elsewhere than the game does (kBankOverrides).

Some items read exactly the same in the game's own text (two sets of herbs, the
two Power Panel Parts, the wristbands of both scenarios, ...): every item after
the first with a given label gets its code appended, as weapons do, so each
entry in a picker is told apart - by the player and by ImGui.
"""
import argparse, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tdb_dump import PE, TDB, F_LITERAL  # noqa: E402
from pak_msg import open_paks, find_msgs, path_key, Msg  # noqa: E402
from gen_records import Rsz  # noqa: E402

# Weapon types that are real, obtainable weapons (everything else in the enum
# is unused, a development placeholder or an internal variant). The 8xxx ones
# are the unlockable infinite versions.
kListedWeapons = ["WP0000", "WP0100", "WP0200", "WP0300", "WP0600", "WP0700", "WP0800", "WP1000", "WP2000",
                  "WP2200", "WP3000", "WP4100", "WP4200", "WP4300", "WP4400", "WP4500", "WP4510", "WP4600",
                  "WP4700", "WP6200", "WP6300", "WP7000", "WP7010", "WP7020", "WP7030", "WP8400", "WP8600",
                  "WP8700"]
kInfinite = {"WP8400", "WP8600", "WP8700"}
kSubWeapons = {"WP4500", "WP4510", "WP6200", "WP6300"}  # knives and grenades: no magazine

# The game files each item under one of its own categories (Item.Category: Weapon,
# Custom, Material, Bullet, Heal, Support, Key, Etc) - one user file of ids per
# category, the seven ItemManager.ItemOrderUserDataList holds, which is what sorts
# its inventory. The five item ones (the other two are weapons, which the panel
# banks by the stock itself) are found here by name and give each item its bank:
# the weapon parts (Custom Part) go under the key items, as the player asked, and
# the gunpowders (Resource) under Other.
kOrderPath = "natives/stm/SectionRoot/UserData/ItemList/Order/%sOrder.user.2"
kOrderBanks = {"KeyItem": "kBankKey", "Custom": "kBankKey", "Heal": "kBankHeal",
               "Bullet": "kBankAmmo", "Material": "kBankOther"}
# The two the panel files against the game's own table: the ink ribbons and the
# wooden boards are stocked up on like the gunpowders, not carried like a key.
kBankOverrides = {"sm70_201": "kBankOther", "sm70_202": "kBankOther"}


def banks(tdb, table):
    """Which bank each item goes under, by Item.ID, out of the game's order tables."""
    types = {n: tdb.find_type(n) for n in ("app.ropeway.ItemOrderUserData", "app.ropeway.ItemOrderSingleData")}
    by_fqn = {tdb.typedef(t)["fqn"]: t for t in types.values()}
    out, where = {}, {}
    for name, bank in kOrderBanks.items():
        path = kOrderPath % name
        hit = table.get(path_key(path))
        if hit is None:
            raise SystemExit("%s is not in the archives" % path)
        pak, e = hit
        rsz = Rsz(pak.read(e), tdb, by_fqn)
        if rsz.end != rsz.size:
            raise SystemExit("%s: parsed %d of %d bytes - the field layout is not what this expects"
                             % (path, rsz.end, rsz.size))
        for kind, vals in (i for i in rsz.instances[1:] if i):
            if kind != "app.ropeway.ItemOrderSingleData":
                continue
            item = vals["ItemId"]
            if item in where:
                raise SystemExit("item %d is in both %sOrder and %sOrder" % (item, where[item], name))
            out[item], where[item] = bank, name
    return out


def fallback_bank(code):
    """An item no order table lists - the maps, the Hip Pouch, the unused Laser Sight
    and the development placeholders - goes by the family of its id."""
    fam, num = code[2:4], int(code[5:8])
    if fam == "70" and num < 100:
        return "kBankHeal"
    if fam == "70" and num < 200:
        return "kBankAmmo"
    if fam == "71":  # a weapon part, where the tables' Custom ones go
        return "kBankKey"
    if fam in ("72", "73", "77") or (fam == "74" and num < 200):
        return "kBankKey"
    return "kBankOther"


def changed_name(text, code):
    """The name an item takes once examined. The game spells the key three ways."""
    fam, num = code[2:4], code[5:8]
    for key in ("ITEM_NAME_%s_CHANGED_%s" % (fam, num),  # ITEM_NAME_73_CHANGED_133
                "ITEM_NAME_CHANGED_%s_%s" % (fam, num),  # ITEM_NAME_CHANGED_72_200
                "ITEM_NAME_CAHNGED_%s_%s" % (fam, num)):  # ITEM_NAME_CAHNGED_77_004 (sic)
        if key in text:
            return text[key]
    return None


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"' if s is not None else "nullptr"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    tdb = TDB(PE(os.path.join(a.game, "re2.exe")))

    def enum(name):
        t = tdb.find_type(name)
        return [(f["name"], tdb.literal_value(f)) for f in tdb.fields_of(t) if f["flags"] & F_LITERAL]

    items = [(n, v) for n, v in enum("app.ropeway.gamemastering.Item.ID") if re.fullmatch(r"sm\d\d_\d\d\d", n)]
    weapons = [(n, v) for n, v in enum("app.ropeway.EquipmentDefine.WeaponType") if re.fullmatch(r"WP\d\d\d\d", n)]

    # Every ITEM_NAME_*/WEAPON_NAME_* entry across the message files, English.
    _, table = open_paks(a.game)
    item_banks = banks(tdb, table)
    text = {}
    for _, pak, e in find_msgs(table):
        try:
            msg = Msg(pak.read(e))
        except Exception:
            continue
        for ent in msg.entries:
            name = ent[1]
            if name.startswith(("ITEM_NAME_", "WEAPON_NAME_")) and name not in text:
                t = msg.text(ent, 1)
                if t and "#Rejected#" not in t:
                    text[name] = t.strip()

    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// Generated by tools/gen_items.py from re2.exe's type database (the Item.ID and")
    lines.append("// WeaponType enums), the game's own English text and its item order tables (the")
    lines.append("// bank each item is filed under) - do not edit by hand.")
    lines.append("namespace re2cc::items {")
    lines.append("")
    lines.append("enum Bank : int { kBankKey = 0, kBankWeapon, kBankAmmo, kBankHeal, kBankOther, kBankCount };")
    lines.append("")
    lines.append("struct Def {")
    lines.append("  int id;               // Item.ID")
    lines.append("  const char* code;     // its enum name")
    lines.append("  const char* name;     // what the game calls it")
    lines.append("  const char* examined; // what the game calls it once examined (nullptr: the same)")
    lines.append("  const char* label;    // what the panel shows: \"examined (name)\", with the code added")
    lines.append("                        // when an earlier item reads the same")
    lines.append("  int bank;")
    lines.append("  bool listed;          // offered in the item pickers")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr Def kItems[] = {")
    n_items, n_banked, n_moved = 0, 0, 0
    labels_seen = {}
    shared = []
    for code, value in items:
        name = text.get("ITEM_NAME_" + code[2:])
        changed = changed_name(text, code)
        listed = bool(name)
        label = "%s (%s)" % (changed, name) if changed and name else (name or code)
        if listed:
            if label in labels_seen:
                shared.append("%s = %s" % (code, labels_seen[label]))
                label += " (%s)" % code
            labels_seen[label] = code
        bank = kBankOverrides.get(code)
        n_moved += bank is not None and listed
        if bank is None:
            bank = item_banks.get(value)
            n_banked += bank is not None and listed
        lines.append("    {%d, %s, %s, %s, %s, %s, %s}," % (value, cstr(code), cstr(name or code), cstr(changed), cstr(label),
                                                       bank or fallback_bank(code), "true" if listed else "false"))
        n_items += listed
    lines.append("};")
    lines.append("")
    lines.append("struct WeaponDef {")
    lines.append("  int id;            // WeaponType")
    lines.append("  const char* code;  // its enum name")
    lines.append("  const char* name;  // what the game calls it")
    lines.append("  bool listed;       // a real, obtainable weapon")
    lines.append("  bool infinite;     // the game's own infinite-ammo variant")
    lines.append("  bool sub;          // a knife or a grenade: no magazine")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr WeaponDef kWeapons[] = {")
    n_weapons = 0
    names_seen = {}
    for code, value in weapons:
        name = text.get("WEAPON_NAME_" + code)
        listed = code in kListedWeapons and bool(name)
        if listed:
            if code in kInfinite:
                name += " (infinite)"
            if name in names_seen:
                name += " (%s)" % code
            names_seen[name] = code
        lines.append("    {%d, %s, %s, %s, %s, %s}," % (value, cstr(code), cstr(name or code), "true" if listed else "false",
                                                   "true" if code in kInfinite else "false",
                                                   "true" if code in kSubWeapons else "false"))
        n_weapons += listed
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace re2cc::items")
    with open(a.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d items (%d listed), %d weapon types (%d listed)" % (a.out, len(items), n_items, len(weapons), n_weapons))
    print("banks: %d of the %d listed items from the game's own order tables, %d filed elsewhere by hand, %d by family"
          % (n_banked, n_items, n_moved, n_items - n_banked - n_moved))
    if shared:
        print("items that read the same as an earlier one (code appended): %s" % ", ".join(shared))
    missing = [c for c in kListedWeapons if "WEAPON_NAME_" + c not in text]
    if missing:
        print("warning: no name for listed weapons %s" % missing)


if __name__ == "__main__":
    main()
