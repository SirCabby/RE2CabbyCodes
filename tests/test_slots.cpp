// Offline test of game::file_slots - where the panel files each inventory slot.
// The game numbers a slot by its stock's Index and its inventory screen moves
// items by rewriting those numbers (Inventory.exchangeSlot), so the _Slots
// list's order is not the slot order once the player has rearranged anything.
// And of game::fat_by and game::dead_slots - which slots two-slot items take
// (InventoryManager.isFatStock, isDeadSlot).
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_slots.cpp -o tests/build/test_slots.exe
//   wine tests/build/test_slots.exe
#include <cstdio>

#include "../src/game.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

re2cc::game::Item item(int item_id, int count = 1) {
  re2cc::game::Item it;
  it.item_id = item_id;
  it.count = count;
  return it;  // weapon -1, parts 0: what the game's own item stocks hold
}
re2cc::game::Item weapon(int weapon_id, int parts, int bullet_id, int count) {
  re2cc::game::Item it;
  it.weapon_id = weapon_id;
  it.parts = parts;
  it.bullet_id = bullet_id;
  it.count = count;
  return it;
}

// dead[] of an inventory of `n` slots, `size` of them the player's, 4 a row.
void expect_dead(const char* what, const re2cc::game::FatRule& rule, const re2cc::game::Item* slot, int n, int size,
                 const bool* want) {
  bool fat[24] = {}, dead[24] = {};
  for (int i = 0; i < n; ++i) fat[i] = re2cc::game::fat_by(rule, slot[i]);
  re2cc::game::dead_slots(fat, n, size, 4, dead);
  bool ok = true;
  for (int i = 0; i < n; ++i) ok = ok && dead[i] == want[i];
  check(ok, what);
  if (!ok) {
    std::printf("      dead:");
    for (int i = 0; i < n; ++i) std::printf(" %d", dead[i]);
    std::printf("\n");
  }
}

void expect(const char* what, const int* number, int n, int size, bool numbered, const int* want) {
  int pos[64];
  const bool got = re2cc::game::file_slots(number, n, size, pos);
  bool ok = got == numbered;
  for (int i = 0; i < n; ++i) ok = ok && pos[i] == want[i];
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    ++g_fail;
    std::printf("      numbered %d, positions:", got);
    for (int i = 0; i < n; ++i) std::printf(" %d", pos[i]);
    std::printf("\n");
  }
}

}  // namespace

int main() {
  {
    const int number[] = {0, 1, 2, 3, 4, 5};
    const int want[] = {0, 1, 2, 3, 4, 5};
    expect("a fresh inventory: the list's order is the slot order", number, 6, 4, true, want);
  }
  {
    // The Samurai Edge in list position 4 moved to slot 6 on the game's screen:
    // its stock and the blank one at list position 6 swapped numbers.
    const int number[] = {0, 1, 2, 5, 4, 3, 6, 7};
    const int want[] = {0, 1, 2, 5, 4, 3, 6, 7};
    expect("an item moved onto a blank slot is filed under its new number", number, 8, 8, true, want);
  }
  {
    const int number[] = {1, 0, 2, 3};
    const int want[] = {1, 0, 2, 3};
    expect("two items swapped", number, 4, 4, true, want);
  }
  {
    // 8 slots in play, 12 not yet: the ones beyond keep their list order.
    const int number[] = {3, 1, 2, 0, 4, 5, 6, 7, -1, 9, 8, -1};
    const int want[] = {3, 1, 2, 0, 4, 5, 6, 7, 8, 9, 10, 11};
    expect("the slots not in play follow in list order", number, 12, 8, true, want);
  }
  {
    const int number[] = {0, 1, 1, 3};
    const int want[] = {0, 1, 2, 3};
    expect("a number used twice: list order, not numbered", number, 4, 4, false, want);
  }
  {
    const int number[] = {0, 1, -1, 3};
    const int want[] = {0, 1, 2, 3};
    expect("a slot in play without a number: list order, not numbered", number, 4, 4, false, want);
  }
  {
    const int number[] = {0, 1, 2};
    const int want[] = {0, 1, 2};
    expect("more slots in play than the list holds: not numbered", number, 3, 4, false, want);
  }

  // Two-slot items. The lists are the game's data (FatItemUserData), made up
  // here; the parts are what InventoryManager.isFatWeapon tests in build 11636119
  // (WeaponParts.A). Items and weapons as the log showed them on 2026-09-16.
  using re2cc::game::fat_by;
  re2cc::game::FatRule rule;
  rule.items[rule.n_items++] = 70;
  rule.weapons[rule.n_weapons++] = 11;
  rule.parts = 1;
  const re2cc::game::Item blank = weapon(-1, 0, 0, 1);  // item 0, weapon -1: an empty slot
  const re2cc::game::Item matilda = weapon(1, 6, 15, 24), matilda_stock = weapon(1, 7, 15, 24);
  check(!fat_by(rule, matilda), "the Matilda with parts B and C takes one slot");
  check(fat_by(rule, matilda_stock), "the Matilda with its stock (parts A) takes two, though it is not on the weapon list");
  check(fat_by(rule, weapon(11, 0, 16, 4)), "a weapon on the weapon list takes two, with no parts");
  check(fat_by(rule, item(70)), "an item on the item list takes two");
  check(!fat_by(rule, item(80)) && !fat_by(rule, blank), "other items, and an empty slot, take one");
  re2cc::game::FatRule unread;  // the lists could not be read
  unread.parts = 1;
  check(fat_by(unread, matilda_stock) && !fat_by(unread, weapon(11, 0, 16, 4)), "the parts count without the lists");
  {
    // 21:39:13: Take put the Square Crank into slot 2, behind the Matilda with its stock in slot 1.
    const re2cc::game::Item slot[] = {matilda_stock, item(80), item(119), blank, blank, blank, blank, blank};
    const bool want[] = {false, true, false, false, false, false, false, false};
    expect_dead("the slot to the right of the Matilda with its stock is dead, whatever it holds", rule, slot, 8, 8, want);
  }
  {
    const re2cc::game::Item slot[] = {matilda, blank, item(119), blank, blank, blank, blank, blank};
    const bool want[] = {false, false, false, false, false, false, false, false};
    expect_dead("without the stock nothing is dead", rule, slot, 8, 8, want);
  }
  {
    const re2cc::game::Item slot[] = {blank, blank, blank, matilda_stock, blank, blank, blank, blank};
    const bool want[] = {false, false, false, false, false, false, false, false};
    expect_dead("a two-slot item in the last column: the next row's first slot is not dead", rule, slot, 8, 8, want);
  }
  {
    const re2cc::game::Item slot[] = {blank, blank, blank, blank, blank, item(70), blank, blank};
    const bool want[] = {false, false, false, false, false, false, false, false};
    expect_dead("a slot the player does not have yet is not dead", rule, slot, 8, 6, want);
  }

  // game::pick_combo - the game's rule for a weapon's magazine and ammo kind
  // (WeaponLoadingpartsCombination.getNumber / getKind: two passes each), on
  // the entries the game's own WeaponBulletUserData holds (build 11636119, read
  // out of the archive 2026-09-25).
  {
    using re2cc::game::ComboEntry;
    using re2cc::game::ComboPick;
    using re2cc::game::pick_combo;
    // The Matilda (WP1000): a base entry of 12 Handgun rounds that overwrites
    // both figures, and the high-capacity magazine's (parts 0x4) entry of 12
    // more that overwrites neither.
    const ComboEntry matilda[] = {{9999, 0, true, false, 12, true, 1}, {1000, 4, false, false, 12, false, 0}};
    ComboPick p = pick_combo(matilda, 2, 0);
    check(p.count == 12 && p.kind == 1, "the Matilda with no parts: 12 rounds of Handgun ammo (kind 1)");
    p = pick_combo(matilda, 2, 7);
    check(p.count == 24 && p.kind == 1, "the Matilda with every part: the magazine adds 12, the kind stays Handgun");
    p = pick_combo(matilda, 2, 3);
    check(p.count == 12 && p.kind == 1, "the Matilda with parts A and B only: the magazine's entry (parts 0x4) does not apply");
    // The same two entries in the other order: the priorities decide, not the list.
    const ComboEntry reversed[] = {matilda[1], matilda[0]};
    p = pick_combo(reversed, 2, 7);
    check(p.count == 24 && p.kind == 1, "the entries' order in the list does not matter");
    // The Lightning Hawk (WP4000 = 9): a base entry of 5 Magnum rounds, and a part
    // whose entry adds a second kind (Magnum2, 0x400) without overwriting.
    const ComboEntry hawk[] = {{9999, 0, true, false, 5, true, 8}, {1000, 2, false, false, 0, false, 0x400}};
    p = pick_combo(hawk, 2, 0);
    check(p.count == 5 && p.kind == 8, "the Lightning Hawk with no parts: 5 rounds, Magnum");
    p = pick_combo(hawk, 2, 2);
    check(p.count == 5 && p.kind == 0x408, "with its part: the second kind is ORed in (0x408), the count unchanged");
    // WP8300-8500: one entry of priority 0 that overwrites the number but not the
    // kind - the kind comes through the second pass alone (the first pass leaves
    // its best priority at INT32_MAX, so every non-overwriting entry counts).
    const ComboEntry ghost[] = {{0, 0, true, false, 15, false, 1}};
    p = pick_combo(ghost, 1, 0);
    check(p.count == 15 && p.kind == 1, "an entry that does not overwrite the kind still names it (WP8300)");
    // The infinite weapons: _Infinity makes the count -1.
    const ComboEntry infinite[] = {{9999, 0, true, true, 1000, true, 0}};
    p = pick_combo(infinite, 1, 0);
    check(p.count == -1 && p.kind == 0, "an infinite weapon is full at -1 and names no ammo");
    // A knife, a grenade: a count and no kind.
    const ComboEntry knife[] = {{9999, 0, true, false, 1000, true, 0}};
    p = pick_combo(knife, 1, 7);
    check(p.count == 1000 && p.kind == 0, "a knife: its durability, no ammo");
    // The GM 79 (WP6000 = 42): one entry naming both grenade kinds at once (0x30).
    const ComboEntry gm79[] = {{9999, 0, true, false, 1, true, 0x30}};
    p = pick_combo(gm79, 1, 0);
    check(p.count == 1 && p.kind == 0x30, "the GM 79 takes two kinds of rounds: both flags, one round");
    // No entry at all, and no entry for the parts fitted.
    p = pick_combo(nullptr, 0, 0);
    check(p.count == 0 && p.kind == 0, "no entries: nothing");
    const ComboEntry only_parts[] = {{9999, 1, true, false, 8, true, 1}};
    p = pick_combo(only_parts, 1, 0);
    check(p.count == 0 && p.kind == 0, "an entry for a part not fitted does not apply");
    // Two overwriting entries: the lower priority wins; a non-overwriting one
    // above it (a higher priority number) is not added.
    const ComboEntry layered[] = {{9999, 0, true, false, 12, true, 1}, {500, 1, true, false, 30, true, 2}, {2000, 1, false, false, 5, false, 4}};
    p = pick_combo(layered, 3, 1);
    check(p.count == 30 && p.kind == 2, "the lowest-priority overwriting entry sets the figure; entries above it add nothing");
    const ComboEntry layered2[] = {{9999, 0, true, false, 12, true, 1}, {500, 1, true, false, 30, true, 2}, {100, 1, false, false, 5, false, 4}};
    p = pick_combo(layered2, 3, 1);
    check(p.count == 35 && p.kind == 6, "a non-overwriting entry below the winner adds to it");
  }

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
