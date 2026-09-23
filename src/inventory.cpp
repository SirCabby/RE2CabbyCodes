#include "inventory.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "call.h"
#include "config.h"
#include "items.h"
#include "log.h"

namespace re2cc::inventory {
namespace {

CRITICAL_SECTION g_cs;
View* g_view = nullptr;  // published (heap: it is large)

enum Kind { kSet, kTake, kStore, kDrop };
struct Request {
  Kind kind = kSet;
  int index = -1;
  game::Item want;
  Entry seen;
};
constexpr int kQueue = 16;
Request g_queue[kQueue];
int g_nq = 0;

char g_note[200] = {};
bool g_note_error = false;

void note(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void note(bool error, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(g_note, sizeof(g_note), fmt, a);
  va_end(a);
  g_note_error = error;
  logf("inventory: %s", g_note);
}

void push(const Request& r) {
  EnterCriticalSection(&g_cs);
  if (g_nq < kQueue) g_queue[g_nq++] = r;
  LeaveCriticalSection(&g_cs);
}

// What an empty slot holds, as the game writes it - taken from an empty slot
// the game made, so the mod's own empty is never a guess.
game::Item blank_like(const game::Bag& b) {
  for (int i = 0; i < b.slots && i < game::kMaxSlots; ++i)
    if (b.slot[i].prim && game::is_blank(b.slot[i].item) && !b.force_blank[i]) return b.slot[i].item;
  game::Item none;
  none.item_id = 0;
  none.weapon_id = -1;
  none.parts = 0;
  none.bullet_id = 0;
  none.count = 0;
  return none;
}

// An item as a non-weapon stock holds it: the empty template's weapon fields.
game::Item as_item(const game::Bag& b, int item_id, int count) {
  game::Item v = blank_like(b);
  v.item_id = item_id;
  v.count = count;
  return v;
}

// The box entry emptied the way the box keeps them: the game's own clear when
// it can be called, else field writes (blank_like, or a removal when the box
// keeps no blanks).
bool empty_box_entry(const game::Bag& b, game::Box& box, int index) {
  const game::Stock& e = box.entry[index];
  if (call::ready() && call::blank_item(e.obj)) {
    game::Stock now;
    if (game::read_stock(e.obj, &now) && game::is_blank(now.item)) return true;
  }
  const bool keeps_blanks = [&] {
    for (int i = 0; i < box.count; ++i)
      if (i != index && box.entry[i].prim && game::is_blank(box.entry[i].item)) return true;
    return box.count >= game::kMaxBox;
  }();
  return keeps_blanks ? game::write_item(e, blank_like(b)) : game::box_remove(index, e.obj);
}

bool seen_ok(const game::Stock& now, const Entry& seen) {
  return now.prim && now.prim == seen.prim && now.obj == seen.obj && game::same_item(now.item, seen.item);
}

bool free_at(const game::Bag& b, int i) {
  return i >= 0 && i < b.size && i < b.slots && b.slot[i].prim && game::is_blank(b.slot[i].item) && !b.force_blank[i] &&
         !b.dead[i];
}

int first_free_slot(const game::Bag& b) {
  for (int i = 0; i < b.size && i < b.slots; ++i)
    if (free_at(b, i)) return i;
  return -1;
}

// A two-slot item's place: a free slot with a free one to its right, in the same row.
int first_free_pair(const game::Bag& b) {
  const int cols = game::inventory_columns();
  for (int i = 0; i + 1 < b.size && i + 1 < b.slots; ++i)
    if (i % cols != cols - 1 && free_at(b, i) && free_at(b, i + 1)) return i;
  return -1;
}

int blank_box_entry(const game::Box& box) {
  for (int i = 0; i < box.count; ++i)
    if (box.entry[i].prim && game::is_blank(box.entry[i].item)) return i;
  return -1;
}

// A box entry an item of `item_id` can be stacked onto: the same item, not a weapon.
bool stacks_onto(const game::Stock& e, int item_id) {
  return e.prim && item_id > 0 && e.item.item_id == item_id && !game::is_weapon(e.item) && e.item.count >= 0;
}

// Where a Store puts what a slot holds, the way the game's item box screen
// stores (NewInventorySlotBehavior.executeCommandItemBoxMode): each entry holding
// the same stackable item takes what fits under the item's maximum, in box
// order (exchangeItemSlotAndBox adds to its count), and an empty entry takes the
// rest as a new stack; what finds no room stays in the slot. The game walks its
// screen's list, which it keeps packed, and stops at the first empty entry; the
// saved list can have empty entries between items (a Take leaves one), so every
// stack with room is filled before an empty entry is used.
struct StorePlan {
  int onto[kBoxMax];  // the entries topped up, in box order
  int add[kBoxMax];   // how many each takes
  int stacks = 0;
  int stacked = 0;    // how many go onto them
  int rest = 0;       // how many do not
  int to = -1;        // the empty entry the rest goes to (-1: none is needed, or none is free)
};

void plan_store(const game::Box& box, const game::Item& it, int max, StorePlan* p) {
  p->stacks = p->stacked = 0;
  p->rest = it.count;
  p->to = -1;
  if (max > 0 && it.count > 0 && !game::is_weapon(it))
    for (int i = 0; i < box.count && p->rest > 0; ++i) {
      const game::Stock& e = box.entry[i];
      if (!stacks_onto(e, it.item_id) || e.item.count >= max) continue;
      const int n = max - e.item.count < p->rest ? max - e.item.count : p->rest;
      p->onto[p->stacks] = i;
      p->add[p->stacks++] = n;
      p->stacked += n;
      p->rest -= n;
    }
  // Nothing stacked: the stock moves whole, whatever its count (an empty gun's is 0).
  if (!p->stacked || p->rest > 0) p->to = blank_box_entry(box);
}

// How many of an item the box's stacks of it can still take (0: it does not stack).
int stack_room(const game::Box& box, const game::Item& it) {
  const int max = game::is_weapon(it) || it.item_id <= 0 ? 0 : game::stack_max(it.item_id);
  int room = 0;
  for (int i = 0; max > 0 && i < box.count; ++i)
    if (stacks_onto(box.entry[i], it.item_id) && box.entry[i].item.count < max) room += max - box.entry[i].item.count;
  return room;
}

const char* name_of(const game::Item& it) {
  static char buf[96];
  return item_name(it, buf, sizeof(buf));
}

// The weapon the editor puts in a slot, filled the way the game fills one
// (ItemData.setWeapon, what ItemLockerManager.AddWeaponToStrage hands it): the
// weapon's id, no parts, the rounds asked for and the ammo that weapon takes,
// which is what a stock's BulletId holds. A weapon that takes none - a knife, a
// grenade - keeps 0, as an empty one does.
game::Item as_weapon(int weapon_id, int count) {
  game::Item v;
  v.item_id = 0;
  v.weapon_id = weapon_id;
  v.parts = 0;
  v.bullet_id = game::weapon_bullet_id(weapon_id, 0);
  v.count = count;
  return v;
}

// A slot that can take a two-slot item: not the right-hand column, and the slot
// beside it free - or the second half of the very item being replaced, which
// frees as it goes.
bool pair_at(const game::Bag& b, int i, bool replacing_fat) {
  const int cols = game::inventory_columns();
  if (cols <= 0 || i % cols == cols - 1 || i + 1 >= b.size || i + 1 >= b.slots) return false;
  if (free_at(b, i + 1)) return true;
  return replacing_fat && b.dead[i + 1] && b.slot[i + 1].prim && game::is_blank(b.slot[i + 1].item) &&
         !b.force_blank[i + 1];
}

// A weapon into a slot, or whatever the slot holds out of it. The game's own
// inventory functions do this, never a field write: a weapon in a slot is also
// the model the Equipment component spawns (Inventory.OnAddSlot ->
// Equipment.onAddWeapon), so a weapon written into a slot would be a slot with
// no gun. The weapon is built in an empty item box entry - the game's own
// ItemData, and the box is data alone - and that entry is what
// InventoryManager.setStock is handed, exactly as a Take from the box. Nothing
// is taken out of the slot before everything the change needs is there.
void set_weapon(const Request& r, game::Bag& b, game::Box* box, const char* before) {
  const game::Stock s = b.slot[r.index];
  const game::Item old = s.item;
  const bool want_weapon = game::is_weapon(r.want);
  const game::Item want = want_weapon ? as_weapon(r.want.weapon_id, r.want.count) : r.want;
  if (!call::ready() || !b.numbered)
    return note(true, "a weapon needs the game's own inventory functions, and they are not available: %s",
                b.numbered ? call::status() : "the inventory's slot numbers do not add up (see the log)");
  // Every call below names the slot by number: it must be this stock's.
  if (s.index != r.index)
    return note(true, "slot %d's item is numbered %d by the game - nothing was changed", r.index + 1, s.index + 1);
  const int scratch = want_weapon && box ? blank_box_entry(*box) : -1;
  if (want_weapon && scratch < 0)
    return note(true,
                box ? "the item box has no empty entry, and a weapon is made in one - throw something away first"
                    : "the item box cannot be read right now, and a weapon is made in it");
  if (want_weapon && game::is_fat(want) && !pair_at(b, r.index, game::is_fat(old)))
    return note(true, "%s takes two slots side by side, and there is no room for it at slot %d", name_of(want),
                r.index + 1);
  if (!want_weapon && want.item_id > 0 && game::is_fat(as_item(b, want.item_id, want.count)))
    return note(true, "%s takes two slots - take it from the item box instead", name_of(want));
  // 1. The weapon, in the box's empty entry: the game's ItemData filled as
  // ItemData.setWeapon fills one. Nothing has left the slot yet.
  if (want_weapon && !game::write_item(box->entry[scratch], want))
    return note(true, "%s could not be made in the item box - nothing was changed", name_of(want));
  // 2. The slot emptied the way the game's item box screen empties one.
  if (!game::is_blank(old)) {
    if (game::is_weapon(old)) {
      call::unequip_slot(b.inventory, r.index);
      call::put_shortcut_weapon(b.manager, r.index);
    }
    call::remove_stock(b.manager, r.index);
    game::Bag after;
    if (!game::read_bag(&after) || !after.numbered || !after.slot[r.index].prim ||
        !game::is_blank(after.slot[r.index].item)) {
      if (want_weapon) empty_box_entry(b, *box, scratch);
      return note(true, "the game did not empty slot %d (%s) - nothing was changed", r.index + 1, call::status());
    }
    b = after;
  }
  // What a failed add puts back: the slot's old stock, made in the same entry.
  auto put_back = [&] {
    if (game::is_blank(old)) return true;
    return game::write_item(box->entry[scratch], old) && call::set_stock(b.manager, r.index, box->entry[scratch].obj) &&
           game::read_bag(&b) && !game::is_blank(b.slot[r.index].item);
  };
  if (!want_weapon) {
    if (want.item_id > 0) {
      const game::Item item = as_item(b, want.item_id, want.count);
      if (!game::write_item(b.slot[r.index], item)) return note(true, "slot %d could not be written", r.index + 1);
      call::update_inventory_flags(b.manager);
      return note(false, "slot %d: %s -> %s x%d", r.index + 1, before, name_of(item), item.count);
    }
    call::update_inventory_flags(b.manager);
    return note(false, "slot %d: %s -> (empty)", r.index + 1, before);
  }
  // 3. The game's own add, and the entry emptied again.
  game::Stock placed;
  if (!call::set_stock(b.manager, r.index, box->entry[scratch].obj) || !game::read_bag(&b) || !b.numbered ||
      !game::read_stock(b.slot[r.index].obj, &placed) || placed.index != r.index ||
      placed.item.weapon_id != want.weapon_id) {
    const bool back = put_back();
    empty_box_entry(b, *box, scratch);
    return note(true, "the game did not put %s into slot %d (%s)%s", name_of(want), r.index + 1, call::status(),
                back ? " - nothing was changed" : ", and what was there could not be put back - the slot is empty");
  }
  if (!empty_box_entry(b, *box, scratch))
    logf("inventory: the item box entry %s was made in could not be emptied - check entry %d", name_of(want), scratch + 1);
  call::update_inventory_flags(b.manager);
  return note(false, "slot %d: %s -> %s x%d", r.index + 1, before, name_of(placed.item), placed.item.count);
}

void apply(const Request& r, game::Bag& b, game::Box* box) {
  char before[96];
  switch (r.kind) {
    case kSet: {
      if (r.index < 0 || r.index >= b.size || r.index >= b.slots) return note(true, "slot %d is not one the player has", r.index + 1);
      const game::Stock& s = b.slot[r.index];
      if (!seen_ok(s, r.seen)) return note(true, "slot %d changed before the edit could be made - nothing was changed", r.index + 1);
      if (b.dead[r.index]) return note(true, "slot %d is the second half of a two-slot item", r.index + 1);
      if (b.force_blank[r.index]) return note(true, "the game keeps slot %d blank right now", r.index + 1);
      std::snprintf(before, sizeof(before), "%s", name_of(s.item));
      game::Item v;
      const bool was_weapon = game::is_weapon(s.item), want_weapon = game::is_weapon(r.want);
      if (was_weapon && want_weapon && r.want.weapon_id == s.item.weapon_id) {
        // The weapon already in the slot: only its rounds change, a field write
        // as before - the game made this one, and nothing about it moves.
        v = s.item;
        v.count = r.want.count;
      } else if (was_weapon || want_weapon) {
        return set_weapon(r, b, box, before);
      } else if (r.want.item_id <= 0) {
        v = blank_like(b);
      } else {
        v = as_item(b, r.want.item_id, r.want.count);
        if (game::is_fat(v)) return note(true, "%s takes two slots - take it from the item box instead", name_of(r.want));
      }
      if (!game::write_item(s, v)) return note(true, "slot %d could not be written", r.index + 1);
      return note(false, "slot %d: %s -> %s x%d", r.index + 1, before, name_of(v), v.count);
    }
    case kTake: {
      if (!box || r.index < 0 || r.index >= box->count) return note(true, "that item box entry is gone");
      const game::Stock e = box->entry[r.index];
      if (!seen_ok(e, r.seen)) return note(true, "the item box changed before the take - nothing was moved");
      if (game::is_blank(e.item)) return note(true, "that item box entry is empty");
      const bool weapon = game::is_weapon(e.item), fat = game::is_fat(e.item);
      const int slot = fat ? first_free_pair(b) : first_free_slot(b);
      if (slot < 0)
        return note(true, fat ? "%s takes two slots side by side - the inventory has no such room"
                              : "the inventory has no free slot for %s",
                    name_of(e.item));
      const game::Item moved = e.item;
      if (call::ready() && b.numbered) {
        // The game's own add: the slot is filled from the box entry, and a
        // weapon's model is made (InventoryManager.setStock). `slot` is the
        // game's own number for it (game.h, Bag).
        if (!call::set_stock(b.manager, slot, e.obj))
          return note(true, "the game's inventory refused %s (%s) - see the log", name_of(moved), call::status());
        game::Bag after;
        game::Stock placed;
        if (!game::read_bag(&after) || !after.numbered || !game::read_stock(after.slot[slot].obj, &placed) ||
            placed.index != slot || placed.item.item_id != moved.item_id || placed.item.weapon_id != moved.weapon_id)
          return note(true, "the game did not put %s into slot %d - nothing was moved", name_of(moved), slot + 1);
        if (!empty_box_entry(after, *box, r.index)) {
          call::remove_stock(b.manager, slot);  // never leave the item in both places
          return note(true, "the item box entry could not be emptied - nothing was moved");
        }
        call::update_inventory_flags(b.manager);
        return note(false, "took %s x%d from the item box into slot %d", name_of(moved), moved.count, slot + 1);
      }
      if (weapon)
        return note(true, "weapons need the game's own inventory functions, and they are not available: %s",
                    b.numbered ? call::status() : "the inventory's slot numbers do not add up (see the log)");
      const game::Item written = as_item(b, moved.item_id, moved.count);
      if (!game::write_item(b.slot[slot], written)) return note(true, "slot %d could not be written", slot + 1);
      if (!empty_box_entry(b, *box, r.index)) {
        game::write_item(b.slot[slot], blank_like(b));  // never leave the item in both places
        return note(true, "the item box entry could not be emptied - nothing was moved");
      }
      return note(false, "took %s x%d from the item box into slot %d", name_of(written), written.count, slot + 1);
    }
    case kStore: {
      if (!box) return note(true, "the item box is not readable right now");
      if (r.index < 0 || r.index >= b.size || r.index >= b.slots) return note(true, "slot %d is not one the player has", r.index + 1);
      const game::Stock s = b.slot[r.index];
      if (!seen_ok(s, r.seen)) return note(true, "slot %d changed before the store - nothing was moved", r.index + 1);
      // A dead slot holding an item (Bag) is stored like any other: that is how it gets out from under.
      if (game::is_blank(s.item)) return note(true, "slot %d holds nothing to store", r.index + 1);
      const game::Item moved = s.item;
      const bool weapon = game::is_weapon(moved);
      const bool calls = call::ready() && b.numbered;
      if (weapon && !calls)
        return note(true, "weapons need the game's own inventory functions, and they are not available: %s",
                    b.numbered ? call::status() : "the inventory's slot numbers do not add up (see the log)");
      // Every call below names the slot by number: it must be this stock's.
      if (calls && s.index != r.index)
        return note(true, "slot %d's item is numbered %d by the game - nothing was moved", r.index + 1, s.index + 1);
      static StorePlan p;  // the game tick's alone
      plan_store(*box, moved, weapon ? 0 : game::stack_max(moved.item_id), &p);
      if (!p.stacked && p.to < 0) return note(true, "the item box is full");
      // What a failed later step puts back: the stacks' counts as they were.
      auto unstack = [&] {
        for (int k = 0; k < p.stacks; ++k) game::write_item(box->entry[p.onto[k]], box->entry[p.onto[k]].item);
      };
      // 1. The stacks the box holds are topped up - their counts, as the game's own screen adds to them.
      for (int k = 0; k < p.stacks; ++k) {
        game::Item v = box->entry[p.onto[k]].item;
        v.count += p.add[k];
        if (!game::write_item(box->entry[p.onto[k]], v)) {
          unstack();
          return note(true, "the item box's %s could not be topped up - nothing was moved", name_of(moved));
        }
      }
      // What found no room stays in the slot, as at the game's own box.
      if (p.to < 0 && p.rest > 0) {
        game::Item left = moved;
        left.count = p.rest;
        if (!game::write_item(s, left)) {
          unstack();
          return note(true, "slot %d could not be written - nothing was moved", r.index + 1);
        }
        if (calls) call::update_inventory_flags(b.manager);
        return note(false, "stored %d of %s x%d from slot %d on the item box's stacks - they have no room for the other %d",
                    p.stacked, name_of(moved), moved.count, r.index + 1, p.rest);
      }
      // 2. The rest - all of it, when nothing stacked - into an empty entry: the
      // game's own copy (ItemData.setData), given the rest's count.
      if (p.to >= 0) {
        bool copied_ok;
        if (calls) {
          game::Stock copied;
          copied_ok = call::copy_item(box->entry[p.to].obj, s.obj) && game::read_stock(box->entry[p.to].obj, &copied) &&
                      game::same_item(copied.item, moved);
          if (copied_ok && p.rest != moved.count) {
            game::Item v = copied.item;
            v.count = p.rest;
            copied_ok = game::write_item(copied, v);
          }
        } else {
          game::Item v = moved;
          v.count = p.rest;
          copied_ok = game::write_item(box->entry[p.to], v);
        }
        if (!copied_ok) {
          empty_box_entry(b, *box, p.to);
          unstack();
          return note(true, "%s could not be copied into the item box - nothing was moved", name_of(moved));
        }
      }
      // 3. The slot, emptied the way the game's item box screen empties one: a
      // weapon is unequipped and loses its shortcut, then InventoryManager.removeStock.
      if (calls) {
        if (weapon) {
          call::unequip_slot(b.inventory, r.index);
          call::put_shortcut_weapon(b.manager, r.index);
        }
        call::remove_stock(b.manager, r.index);
        // Whether the item left the inventory decides what happens to the box's
        // side: taken back only when the item is plainly still in its stock - a
        // doubt keeps both rather than risk losing it. (The game may blank the
        // stock in place or give the slot a new one.)
        game::Bag after;
        if (!game::read_bag(&after))
          return note(true, "%s is in the item box, but the inventory could not be read back - check slot %d",
                      name_of(moved), r.index + 1);
        for (int i = 0; i < after.slots; ++i)
          if (after.slot[i].obj == s.obj && game::same_item(after.slot[i].item, moved)) {
            if (p.to >= 0) empty_box_entry(after, *box, p.to);
            unstack();
            return note(true, "the game did not empty slot %d (%s) - nothing was moved", r.index + 1, call::status());
          }
        call::update_inventory_flags(b.manager);
      } else if (!game::write_item(s, blank_like(b))) {
        if (p.to >= 0) empty_box_entry(b, *box, p.to);
        unstack();
        return note(true, "slot %d could not be emptied - nothing was moved", r.index + 1);
      }
      if (!p.stacked) return note(false, "stored %s x%d from slot %d in the item box", name_of(moved), moved.count, r.index + 1);
      if (p.to < 0)
        return note(false, "stored %s x%d from slot %d in the item box, on the stacks it held", name_of(moved), moved.count,
                    r.index + 1);
      return note(false, "stored %s x%d from slot %d in the item box: %d on the stacks it held, %d in a new stack",
                  name_of(moved), moved.count, r.index + 1, p.stacked, p.rest);
    }
    case kDrop: {
      if (!box || r.index < 0 || r.index >= box->count) return note(true, "that item box entry is gone");
      const game::Stock& e = box->entry[r.index];
      if (!seen_ok(e, r.seen)) return note(true, "the item box changed before the throw-away - nothing was changed");
      std::snprintf(before, sizeof(before), "%s x%d", name_of(e.item), e.item.count);
      const bool keeps_blanks = blank_box_entry(*box) >= 0 || box->count >= game::kMaxBox;
      if (!(keeps_blanks ? game::write_item(e, blank_like(b)) : game::box_remove(r.index, e.obj)))
        return note(true, "the item box entry could not be emptied");
      return note(false, "threw %s away from the item box", before);
    }
  }
}

}  // namespace

const char* item_name(const game::Item& it, char* buf, size_t n) {
  if (game::is_weapon(it)) {
    for (const auto& w : items::kWeapons)
      if (w.id == it.weapon_id) return w.name;
    std::snprintf(buf, n, "weapon %d", it.weapon_id);
    return buf;
  }
  if (it.item_id <= 0) return "(empty)";
  for (const auto& d : items::kItems)
    if (d.id == it.item_id) return d.label;
  std::snprintf(buf, n, "item %d", it.item_id);
  return buf;
}

bool editable(const Entry& e) { return e.prim && !e.force_blank && !e.dead; }

View view() {
  View v;
  EnterCriticalSection(&g_cs);
  if (g_view) v = *g_view;
  LeaveCriticalSection(&g_cs);
  return v;
}

void request_set(int slot, const game::Item& want, const Entry& seen) {
  Request r;
  r.kind = kSet;
  r.index = slot;
  r.want = want;
  r.seen = seen;
  push(r);
}
void request_take(int box_index, const Entry& seen) {
  Request r;
  r.kind = kTake;
  r.index = box_index;
  r.seen = seen;
  push(r);
}
void request_store(int slot, const Entry& seen) {
  Request r;
  r.kind = kStore;
  r.index = slot;
  r.seen = seen;
  push(r);
}
void request_drop(int box_index, const Entry& seen) {
  Request r;
  r.kind = kDrop;
  r.index = box_index;
  r.seen = seen;
  push(r);
}

void init() {
  InitializeCriticalSection(&g_cs);
  g_view = new View;
}

void tick(bool in_game, bool paused) {
  const bool open = in_game && paused;
  const bool wanted = open || (in_game && config::get().always_show);
  Request queue[kQueue];
  int nq = 0;
  EnterCriticalSection(&g_cs);
  nq = g_nq;
  for (int i = 0; i < nq; ++i) queue[i] = g_queue[i];
  g_nq = 0;
  LeaveCriticalSection(&g_cs);
  static bool s_shown = false;
  static DWORD s_read = 0;
  const bool opened = wanted && !s_shown;
  if (!wanted && !nq) {
    if (s_shown) {
      s_shown = false;
      EnterCriticalSection(&g_cs);
      g_view->known = false;
      g_view->open = false;
      LeaveCriticalSection(&g_cs);
    }
    return;
  }
  s_shown = wanted;
  // Read when the panel opens, after each request, and twice a second while it
  // is up - never every frame (the box is hundreds of entries).
  const DWORD now = GetTickCount();
  if (!opened && !nq && now - s_read < 500) return;
  s_read = now;
  static game::Bag bag;
  static game::Box box;
  bool have_bag = in_game && game::read_bag(&bag);
  bool have_box = have_bag && game::read_box(&box);
  for (int i = 0; i < nq; ++i) {
    if (!open) {
      note(true, "changes are only made while the pause menu is up");
      break;
    }
    if (!have_bag) {
      note(true, "the inventory cannot be read right now");
      break;
    }
    apply(queue[i], bag, have_box ? &box : nullptr);
    // The next request sees what this one did.
    have_bag = game::read_bag(&bag);
    have_box = have_bag && game::read_box(&box);
  }
  static View v;
  v = View{};
  v.known = have_bag;
  v.open = open;
  v.columns = game::inventory_columns();
  if (have_bag) {
    v.size = bag.size;
    v.slots = bag.slots;
    for (int i = 0; i < bag.slots && i < kBagMax; ++i) {
      const game::Item& it = bag.slot[i].item;
      v.bag[i] = Entry{it, bag.slot[i].obj, bag.slot[i].prim, bag.force_blank[i], bag.dead[i],
                       !game::is_blank(it) && bag.fat[i]};
      v.free_slots += free_at(bag, i);
    }
    for (int i = 0; i + 1 < bag.size && i + 1 < bag.slots; ++i)
      v.free_pairs += i % v.columns != v.columns - 1 && free_at(bag, i) && free_at(bag, i + 1);
  }
  v.box_known = have_box;
  if (have_box) {
    v.box_count = box.count;
    static game::FatRule fat;  // the game tick's alone
    game::read_fat_rule(&fat);
    for (int i = 0; i < box.count && i < kBoxMax; ++i) {
      const game::Item& it = box.entry[i].item;
      v.box[i] = Entry{it, box.entry[i].obj, box.entry[i].prim, false, false, !game::is_blank(it) && game::fat_by(fat, it)};
      v.box_blank += box.entry[i].prim && game::is_blank(it);
    }
    for (int i = 0; i < bag.slots && i < kBagMax; ++i)
      if (bag.slot[i].prim && !game::is_blank(bag.slot[i].item))
        v.stack_room[i] = stack_room(box, bag.slot[i].item);
  }
  // What each weapon the editor offers holds when it is full, for the count it
  // starts a newly picked weapon at. The panel may not read the game itself,
  // and game::weapon_full_count reads each weapon once and keeps it.
  if (have_bag)
    for (const auto& w : items::kWeapons)
      if (w.listed && w.id > 0 && w.id < kWeaponIds) v.weapon_full[w.id] = game::weapon_full_count(w.id, 0);
  v.calls = call::ready() && have_bag && bag.numbered;
  std::snprintf(v.calls_status, sizeof(v.calls_status), "%s",
                have_bag && !bag.numbered ? "the inventory's slot numbers do not add up (see the log)" : call::status());
  std::snprintf(v.note, sizeof(v.note), "%s", g_note);
  v.note_error = g_note_error;
  EnterCriticalSection(&g_cs);
  *g_view = v;
  LeaveCriticalSection(&g_cs);
}

}  // namespace re2cc::inventory
