#pragma once

#include <cstdint>

// Calls into the game's own code. Everything else the mod does is a field
// write; the item box's Take and Store are not, because a weapon only gets its
// model when the game's inventory code adds it and only loses it when that code
// removes it (Inventory.OnAddSlot -> Equipment.onAddWeapon). The user approved
// calling the game for those two on 2026-09-16, on 2026-09-18 for the
// difficulty switch, whose global flags live in the engine's native variables
// where no field write reaches them, and on 2026-09-19 for the records panel's
// Steam achievements: a record switched on has its achievement unlocked through
// the game's AchievementManager, the call RecordManager.clearRecord makes when
// a record is earned. Nothing else calls it from here; the save files' two
// refresh calls (the same day's approval) are made from the Load Game screen's
// hook with the context the game hands it, so they need no bridge (savefiles.h).
//
// The item box's calls are the ones the game's own item box screen makes
// (NewInventorySlotBehavior.exchangeItemSlotAndBox): unequip and clear the
// shortcut of a weapon going out, InventoryManager.removeStock / setStock, and
// ItemData's copy and clear for the box's side. The difficulty's is the one a
// new game and every load make, MainFlowManager.setDifficulty. A managed method is called as
// the engine calls one from native code: `method(thread context, this, args)`,
// inside the VM's global frame (begin on the first reference, and on the last
// one the pending exception handled, the local frame collected, the frame
// ended). Every helper is found by the code that uses it hundreds of times over
// - no address is assumed - and the methods by name and parameter types. A call
// that leaves an exception behind is handed to the VM's own handler and turns
// every further call off for the session.
namespace re2cc::call {

bool init();  // mod thread, once re::ready(): the helpers and the methods; logs what it found
bool ready(); // the item box's calls: found, and no call has failed
const char* status();
bool difficulty_ready();  // the difficulty's call: found, and no call has failed
const char* difficulty_status();
bool achievements_ready();  // the records' achievement calls: found, and no call has failed
const char* achievements_status();

// An event hook forwarding the game's call (events.cpp): true when the
// original left a managed exception pending in `ctx` - the hook must then
// return at once, as the game's own code does. false when unknown.
bool exception_pending(uintptr_t ctx);

// Game tick only (anywhere else they refuse). false: the call was not made, or it threw.
bool set_stock(uintptr_t inventory_manager, int slot, uintptr_t item_data);  // InventoryManager.setStock
bool remove_stock(uintptr_t inventory_manager, int slot);                    // InventoryManager.removeStock
bool put_shortcut_weapon(uintptr_t inventory_manager, int slot);             // InventoryManager.putShortcutWeapon
bool update_inventory_flags(uintptr_t inventory_manager);                    // InventoryManager.updateInventoryFlags
bool unequip_slot(uintptr_t inventory, int slot);                            // Inventory.unequipSlot (only the equipped one)
bool copy_item(uintptr_t dest_item_data, uintptr_t src_item_data);           // ItemData.setData(ItemData)
bool blank_item(uintptr_t item_data);                                        // ItemData.setBlank
bool set_difficulty(uintptr_t main_flow, int difficulty);                    // MainFlowManager.setDifficulty
// The Steam achievement of a record, if it has one (AchievementDefine maps 41 of
// the main game's records and 2 of The Ghost Survivors'; for any other the game's
// call does nothing): AchievementManager.unlockRecord(id, immediate = true), as
// clearRecord calls it, and unlockRogueClearRecord(id).
bool unlock_record_achievement(uintptr_t achievement_manager, int record_id);
bool unlock_rogue_achievement(uintptr_t achievement_manager, int rogue_record_id);

}  // namespace re2cc::call
