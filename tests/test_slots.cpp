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
  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
