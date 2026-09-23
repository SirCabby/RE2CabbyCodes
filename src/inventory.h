#pragma once

#include <cstddef>

#include "game.h"

// The inventory editor and the item box, on the pause menu's panel. Both are
// the game's own data: the inventory of the character in play
// (InventoryManager.CurrentInventory: a list of slots, each holding a
// StockItem) and the item box every box in the game opens
// (ItemLockerManager's save data: a list of ItemData). Every change is made
// between frames while the pause menu has the game stopped, and re-checked
// against what the panel was showing - acting on the wrong item is worse than
// not acting.
//
// The item box moves anything, the way the game's own item box screen does:
// Take and Store run the game's inventory functions (call.h), so a weapon gets
// or loses its model and is unequipped first, and an item that takes two slots
// lands where the game would put it. Without those functions (not found, or one
// failed) they fall back to field writes, which cannot arm a weapon: weapons
// are then refused. The editor is field writes only: it changes a weapon's
// loaded rounds but never adds or removes one, and leaves two-slot items to the
// item box. Store stacks as the game's box does - ammo and the other items the
// game stacks top up the box's stacks of the same item, up to the game's own
// maximum (game::stack_max), and the rest becomes a new stack or stays in the
// slot; an item whose rule cannot be read moves whole. Take moves a stack whole.
namespace re2cc::inventory {

constexpr int kBagMax = game::kMaxSlots;
constexpr int kBoxMax = game::kMaxBox;
constexpr int kWeaponIds = 384;  // EquipmentDefine.WeaponType goes to WP9990 (381) in this build

struct Entry {
  game::Item item;
  uintptr_t obj = 0;   // the StockItem / ItemData
  uintptr_t prim = 0;  // its DefaultItem
  bool force_blank = false;  // the game counts the slot as blank (Slot.IsForceBlank)
  bool dead = false;         // the second half of the two-slot item to its left
  bool fat = false;          // the item takes two slots
};

// A copy of everything the panel draws, taken under the lock.
struct View {
  bool known = false;     // the inventory was found in the game
  bool open = false;      // the pause menu is up: the only time anything changes
  int size = 0;           // the slots the player has (8, up to 20 with hip pouches)
  int slots = 0;
  int columns = 4;        // slots per row
  int free_slots = 0;     // where Take can put an item
  int free_pairs = 0;     // where Take can put a two-slot item
  Entry bag[kBagMax];
  bool box_known = false;
  int box_count = 0;
  int box_blank = 0;      // entries the game keeps empty (a store can fill one)
  Entry box[kBoxMax];
  int stack_room[kBagMax] = {};  // how many of slot i's item the box's stacks of it can still take
  // The rounds each weapon the editor offers holds when it is full, by
  // WeaponType with no parts (game::weapon_full_count: 0 unknown, -1
  // unlimited) - the count a weapon picked in the editor starts at. The panel
  // never reads the game itself, so the tick brings them.
  int weapon_full[kWeaponIds] = {};
  bool calls = false;     // the game's inventory functions are available (weapons can move)
  char calls_status[160] = {};
  char note[200] = {};    // what the last change came to (logged; the panel shows a refusal)
  bool note_error = false;
};
View view();  // any thread

// The name the panel gives an item: "Spare Key (Spare Part)" for one the game
// renames once examined, the weapon's name for a weapon, "(empty)".
const char* item_name(const game::Item& it, char* buf, size_t n);
bool editable(const Entry& e);  // a slot the editor can change (not half of a two-slot item, not one the game keeps blank)

// Requests from the panel, carried out on the next game tick while the pause
// menu is up. `seen` is what the panel showed: an entry that holds something
// else by then is left alone.
void request_set(int slot, const game::Item& want, const Entry& seen);  // the editor
void request_take(int box_index, const Entry& seen);                     // box -> the first place it fits
void request_store(int slot, const Entry& seen);                         // slot -> the box's stacks, then an empty entry
void request_drop(int box_index, const Entry& seen);                     // throw a box item away

void init();                           // from DllMain: the lock
void tick(bool in_game, bool paused);  // the game tick

}  // namespace re2cc::inventory
