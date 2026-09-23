#pragma once

#include <cstdint>

// The game side: RE2's managers and the fields the mod reads and writes. Every
// class and field is resolved by name through the type database once, at
// start-up (discover()); every object is re-read from its singleton on each
// call, so nothing the game frees or replaces - a new game, a loaded save, a
// scene change - is ever used stale. Nothing here is an absolute address or a
// hard-coded offset.
namespace re2cc::game {

// --- discovery (mod thread, once re::ready()) ----------------------------------------
bool discover();          // true when the flow manager was found: the panel can tell where the game is
bool ready();
const char* status_text();

// Which parts were found (each feature greys itself out without its part).
struct Parts {
  bool flow = false, players = false, enemies = false, records = false, header = false, clock = false;
  bool bag = false, box = false, fat = false, countdown = false, typewriter = false, durability = false;
  bool damage = false;    // a hit's DamageInfo and the HP controller's NoDamage (One hit kills)
  bool equipped = false;  // a survivor's equipped main and sub slots (Infinite ammo, No durability loss)
  bool measure = false;   // GameClock._MeasureGameElapsedTime (Freeze play time)
  bool stacks = false;    // which items stack and how far (Store stacks like the game's item box)
  bool boards = false;    // a use-item trigger's callbacks and the item data they carry (Infinite wooden boards)
  bool g2_stun = false;   // Em7100Think.TiredHp, G2's stagger counter (One hit kills, see stun_g2)
  bool plants = false;    // Em5000Think.<FlameOnlyHitPoint>, the plants' real health (One hit kills, see wither_plant)
  bool difficulty = false;  // the header's difficulty, the continue-on-Assisted hold and GlobalUserDataManager (the switch)
  // The records' counters (Counter below).
  bool item_box_count = false;  // RecordManager.gameSaveData.OpenItemBox
  bool heal_count = false;      // RecordManager.gameSaveData.UseHealItem
  bool steps = false;           // PlayerManager.<Pedometer>
};
const Parts& parts();

// --- where the game is (the game tick) -----------------------------------------------------
int main_state();           // MainFlowManager._CurrentMainState, -1 unknown
// What a state read once means (the tick reads it once a frame).
bool in_game_state(int state);  // IN_GAME or PAUSE
bool pause_state_is(int state);
bool title_state_is(int state);
// The run is over and the game is about to record the clear: ENDING, the staff
// roll, RESULT / RESULT_EXTRA and the Ghost Survivors' ROGUE_RESULT. ResultFlow
// runs in RESULT and its update calls RecordManager.setClearedGame, which reads
// GameClock.get_ActualRecordTime for the time it shows, ranks and checks the
// clear-time record against - so a held play time is put into the clock for
// these states (Freeze play time, cheats.cpp).
bool result_state_is(int state);
const char* state_name(int state);

// --- the players (PlayerManager.PlayerList) and enemies (EnemyManager.ActiveEnemyList) ------------
struct Player {
  uintptr_t cond = 0;  // PlayerCondition
  uintptr_t hpc = 0;   // its HitPointController
  int hp = 0, max_hp = 0;
  bool poisoned = false;
  int survivor = -1;   // SurvivorDefine.SurvivorType
};
int players(Player* out, int max);

struct Enemy {
  uintptr_t ctl = 0;  // EnemyController
  uintptr_t hpc = 0;  // its EnemyHitPointController
  int hp = 0, max_hp = 0;
  uint64_t skip_restrict = 0;  // EnemyController.SkipRestrictMoveFrame (see enemy_time)
  uint32_t flags2 = 0;         // EnemyDefine.ConditionStateBitFlag2
  bool no_damage = false;
};
int enemies(Enemy* out, int max);

// --- records --------------------------------------------------------------------------------
int save_count();              // RecordManager.<CurrentSaveCount>, -1 unknown
int header_save_times();       // MainFlowManager.gameHeaderSaveData.SaveTimes (what a save records), -1 unknown
bool set_save_count(int n);    // both of them

// Three of the records the results screen awards are counted as the game is
// played, and checked when it is cleared (RecordManager.setClearedGame ->
// checkRecordProgress, by the record's RecordManager.ProgressType):
//  - ITEMBOX: RecordManager.gameSaveData.OpenItemBox, one for each time the item
//    box opens (GUIMaster.openInventoryItemBoxMode). The record needs 0.
//  - CURE: gameSaveData.UseHealItem, one for each recovery item used that healed
//    (InventoryManager.useHealItem). The record needs 0.
//  - WALK: PlayerManager.<Pedometer>, the player's steps, one a footstep
//    (PlayerFootEffectController.onLand -> addPedometer). The record needs at
//    most its RecordNode.ClearCount (14000 in this build).
// Saves keep all three, and a load puts the save's back.
enum class Counter { kItemBox = 0, kHeals, kSteps };
constexpr int kCounters = 3;
int counter(Counter c);              // -1 unknown
bool set_counter(Counter c, int v);
int steps_limit();                   // the WALK record's ClearCount, -1 unknown
// For the event hooks: the counter back to `held` if the game moved it. What it
// was, or -1 when nothing was written (it holds `held` already, or it could not
// be read). Any thread: where the counter is, is worked out again only when the
// object holding it changed (a load replaces RecordManager's save data).
int hold_counter(Counter c, int held);

// --- the clock (GameClock._GameSaveData, microseconds) ----------------------------------------------
struct Clock {
  uint64_t elapsed = 0, demo = 0, inventory = 0, pause = 0;
};
bool read_clock(Clock* out);
// The clear time the game shows and ranks: elapsed less cutscenes and pauses
// (the inventory's time counts).
inline uint64_t play_time(const Clock& c) {
  const uint64_t off = c.demo + c.pause;
  return c.elapsed > off ? c.elapsed - off : 0;
}
// What the enemies run on: GameClock.get_ActualPlayingTime (exe+0x9D59F0), the
// clear time less the inventory's as well. EnemyController.canRestrictMove holds
// an enemy still while its SkipRestrictMoveFrame equals this, and
// EnemyManager.FrameTimer runs while this is below the timer's end - so a clock
// that does not move (Freeze play time, or the editor set backwards) holds them.
inline uint64_t enemy_time(const Clock& c) {
  const uint64_t off = c.demo + c.inventory + c.pause;
  return c.elapsed > off ? c.elapsed - off : 0;
}
// Their clock clamped at 0 *and* unable to move: it stays there until the game has
// been played for as long as it is behind. Every enemy that has never been stamped
// then matches it (canRestrictMove: SkipRestrictMoveFrame 0 == 0) and no
// EnemyManager.FrameTimer ever expires - the enemies stand still. The game cannot
// write this (it only ever adds to the three inside the elapsed time); the mod's
// holds could, and did. Equality is sound: their clock is 0 but moves next frame,
// which is where a game just begun starts.
inline bool enemies_stuck(const Clock& c) { return c.elapsed < c.demo + c.inventory + c.pause; }
bool set_elapsed(uint64_t elapsed);
bool set_inventory_time(uint64_t us);

// Freeze play time and the play-time editor hold the time the game *records*,
// mod-side: the clock itself is never stopped and never set backwards, because
// the enemies run on it (enemy_time above). The held time is put into the clock
// only where the game reads it for the record - the save that captures
// GameClock's save data (events.h) and the clear (result_state_is) - and taken
// out again afterwards.
//   hold_record_time lowers _GameElapsedTime so get_ActualRecordTime reads
// `held_us`, and answers what it took out; release_record_time adds that back,
// keeping whatever the clock counted in between. A save captures the clock's
// save data *by reference* (SaveDataManager.saveGameSaveData collects the
// objects and the bytes are made later), so what a save's hook takes is given
// back only once the save is written.
//   The inventory's time goes down with the elapsed time and comes back with it,
// because the enemies' clock is the record time *less the inventory's* - holding
// the elapsed time alone would put their clock at `held - inventory`, which is
// stuck at 0 (and every enemy with it) until the game has been played for as long
// as the inventory has ever been open. The save made under the hold carries that
// pair, so before 2026-09-21 every load of such a save began with the enemies
// frozen: seen in game 2026-09-17, 09-20 and 09-21 (Mr. X standing still in the
// sewers), and repair_enemy_clock below heals those saves.
struct Held {
  int64_t elapsed = 0;    // out of _GameElapsedTime (negative: put in)
  int64_t inventory = 0;  // out of _InventorySpendingTime, so enemy_time does not move
  bool any() const { return elapsed != 0 || inventory != 0; }
};
// A field less what a hold takes out of it, never below 0 (and a hold given back
// is the same with the sign turned round).
inline uint64_t less_by(uint64_t v, int64_t take) {
  const int64_t r = static_cast<int64_t>(v) - take;
  return r > 0 ? static_cast<uint64_t>(r) : 0;
}
// What a hold has to take out of the clock for the game to record `held_us`, and
// out of the inventory's time so that the enemies' clock does not move with it.
// Taking more out of the inventory's time than it holds would make it negative -
// clamped, which leaves the enemies the clock a game just begun has. Pure: the
// writes are hold_record_time's (tests/test_clock.cpp).
inline Held hold_by(const Clock& c, uint64_t held_us) {
  Held h;
  const uint64_t want = held_us + c.demo + c.pause;  // so elapsed - (demo + pause) == held_us
  if (c.elapsed == want) return h;
  h.elapsed = static_cast<int64_t>(c.elapsed) - static_cast<int64_t>(want);
  h.inventory = h.elapsed > static_cast<int64_t>(c.inventory) ? static_cast<int64_t>(c.inventory) : h.elapsed;
  return h;  // both negative when the time held is above the clock's: the inventory's grows too
}
inline Clock taken_from(const Clock& c, const Held& h) {
  Clock r = c;
  r.elapsed = less_by(c.elapsed, h.elapsed);
  r.inventory = less_by(c.inventory, h.inventory);
  return r;
}
Held hold_record_time(uint64_t held_us);
bool release_record_time(const Held& taken);

// A clock whose elapsed time is below the cutscene, inventory and pause times it
// is measured against: get_ActualPlayingTime is clamped at 0 and cannot move, so
// EnemyManager's timers never expire and canRestrictMove answers yes for every
// enemy that has never been stamped (SkipRestrictMoveFrame 0 == the clock's 0) -
// the enemies stand still. The game can never make that state; the mod's older
// holds wrote it into saves. Puts the inventory's time back inside the elapsed
// time, which leaves the recorded play time alone, and answers how far behind the
// enemies' clock was (0: nothing to do, or it could not be written).
uint64_t repair_enemy_clock();
// What that clock should hold instead. The recorded play time - what the player
// sees and what the freeze holds - is left alone, so it is the inventory's time
// that comes down, to the most it can be: all of the elapsed time that is not a
// cutscene or a pause.
inline Clock repaired(const Clock& c) {
  Clock r = c;
  if (!enemies_stuck(c)) return r;
  r.inventory = play_time(c);
  const uint64_t floor = c.demo + c.pause;  // no hold can write this (elapsed = held + demo + pause)
  if (r.elapsed < floor) r.elapsed = floor;
  return r;
}

// RecordManager.<CurrentClearTime>: the clear time the results screen shows,
// written by setClearedGame from the clock. Read and written so a held time is
// what the clear shows even if the clear beat the tick to the state by a frame.
uint64_t clear_time();          // 0: not read
bool set_clear_time(uint64_t us);

// --- items -----------------------------------------------------------------------------------------
// An InventoryManager.PrimitiveItem: what a stock (an inventory slot's
// StockItem, an item box entry's ItemData) holds.
struct Item {
  int item_id = 0;     // Item.ID (0 = none)
  int weapon_id = -1;  // WeaponType (-1 = none)
  int parts = 0;       // WeaponParts
  int bullet_id = 0;   // Item.ID of what a weapon is loaded with
  int count = 0;       // how many; a weapon's loaded rounds
};
inline bool is_weapon(const Item& i) { return i.weapon_id > 0; }
inline bool is_blank(const Item& i) { return i.item_id <= 0 && i.weapon_id <= 0; }
inline bool same_item(const Item& a, const Item& b) {
  return a.item_id == b.item_id && a.weapon_id == b.weapon_id && a.parts == b.parts && a.bullet_id == b.bullet_id &&
         a.count == b.count;
}

struct Stock {
  uintptr_t obj = 0;   // the ItemData / StockItem
  uintptr_t prim = 0;  // its DefaultItem
  Item item;
  int index = -1;      // StockItem.Index: the game's number for the slot holding it (-1: an ItemData, or unread)
};
bool read_stock(uintptr_t data_obj, Stock* out);
bool write_item(const Stock& s, const Item& v);  // into its DefaultItem, the object re-checked first

constexpr int kMaxSlots = 24;  // the inventory grows to 20
// slot[i] is the game's slot i: the one whose stock's Index is i. The game
// finds a slot by that number (Inventory.getSlot: `_Slots.Find(s => s._Stock.Index
// == i)`), and its inventory screen moves items by rewriting the numbers
// (Inventory.exchangeSlot), never the list - so the _Slots list's order says
// nothing about where an item is once the player has rearranged anything.
struct Bag {
  uintptr_t manager = 0;    // the InventoryManager singleton
  uintptr_t inventory = 0;  // survivor.Inventory
  int size = 0;             // _CurrentSlotSize: the slots the player has
  int slots = 0;            // the _Slots list's length
  bool numbered = false;    // the stocks' Index values are the numbers 0..slots-1 (else slot[] is in list order)
  Stock slot[kMaxSlots];
  int list_pos[kMaxSlots] = {};  // where slot i sits in the _Slots list (the log)
  // Slot.<IsForceBlank>: the game counts the slot as blank whatever its stock
  // holds (Slot.get_Type), so the mod leaves such a slot alone.
  bool force_blank[kMaxSlots] = {};
  bool fat[kMaxSlots] = {};  // its item takes two slots (FatRule)
  // The second half of a two-slot item (dead_slots). The game never fills one
  // itself - StockItem.isBlank is false for it - but InventoryManager.setStock
  // does not check, so a dead slot can still hold an item: version 0.1.0's Take
  // put one behind the Matilda once its stock was fitted.
  bool dead[kMaxSlots] = {};
};
bool read_bag(Bag* out);  // InventoryManager.<CurrentInventory>

// Which stocks take two slots: InventoryManager.isFatStock, which asks every
// stock all three whatever it holds - its ItemId on FatItemUserData's
// FatItemList, its WeaponParts sharing a bit with `parts`, its WeaponId on
// FatWeaponList. The parts come first in InventoryManager.isFatWeapon: a weapon
// grows to two slots when one of them is fitted, whatever the list says (the
// Matilda with its stock, WeaponParts.A).
struct FatRule {
  int items[256] = {}, weapons[256] = {};
  int n_items = 0, n_weapons = 0;
  int parts = 0;
};
inline bool fat_by(const FatRule& r, const Item& it) {
  for (int i = 0; i < r.n_items; ++i)
    if (r.items[i] == it.item_id) return true;
  if (it.parts & r.parts) return true;
  for (int i = 0; i < r.n_weapons; ++i)
    if (r.weapons[i] == it.weapon_id) return true;
  return false;
}
// dead[i] for slots 0..n-1 of an inventory holding a two-slot item in slot i
// when fat[i]: InventoryManager.isDeadSlot(i) is `(i & 3) != 0 && i <
// _CurrentSlotSize && isFatStock(getSlot(i - 1)._Stock)` - not in the first
// column, a slot the player has, and a two-slot item to its left.
inline void dead_slots(const bool* fat, int n, int size, int columns, bool* dead) {
  for (int i = 0; i < n; ++i) dead[i] = i > 0 && i < size && columns > 0 && i % columns != 0 && fat[i - 1];
}
void read_fat_rule(FatRule* out);  // the lists as the game holds them now (none when unreadable), and the parts

// Where each slot of the _Slots list is filed (Bag): pos[i] for list element i.
// number[i] is its stock's Index (-1 when there is no stock). The slots in play
// must be numbered 0..size-1, once each; the rest follow in list order. When
// they are not, every slot keeps its list position and the result is false.
inline bool file_slots(const int* number, int n, int size, int* pos) {
  constexpr int kMax = 64;
  bool taken[kMax] = {};
  bool numbered = n <= kMax && size >= 0 && size <= n;
  for (int i = 0; i < n && numbered; ++i)
    if (number[i] >= 0 && number[i] < size) {
      if (taken[number[i]]) numbered = false;
      taken[number[i]] = true;
    }
  for (int k = 0; k < size && numbered; ++k) numbered = taken[k];
  int next = size;
  for (int i = 0; i < n; ++i) pos[i] = !numbered ? i : number[i] >= 0 && number[i] < size ? number[i] : next++;
  return numbered;
}
int inventory_columns();  // slots per row (Inventory.isRightEdgeSlot: index & 3 == 3 -> 4)

constexpr int kMaxBox = 400;  // ItemLockerManager.BOXSLOT_MAX
struct Box {
  uintptr_t list = 0;  // ItemLockerSaveData._Items
  int count = 0;
  int capacity = 0;    // the list's array
  Stock entry[kMaxBox];
};
bool read_box(Box* out);  // ItemLockerManager's item box

// A list that holds item stocks, as the game's own List<T>.RemoveAt leaves it:
// the entries after `index` move down, the last one is cleared.
bool box_remove(int index, uintptr_t expect_obj);

bool is_fat(const Item& it);        // it takes two slots (FatRule; reads the lists)
int ink_ribbon_id();                // Item.ID sm70_201
int wooden_boards_id();             // Item.ID sm70_202

// How many of an item one stock holds when the game stacks it - the rule the
// game's item box screen stores by (NewInventorySlotBehavior.isAbleToCombineSlotAndBox,
// getItemMax): an item stacks when its ItemManager.<ItemElementTable> entry is
// Disposable.MultipleUse, up to ItemManager.getItemMultipleUseMax(id), the
// maximum an inventory slot has too (Slot.get_MaxNumber). That maximum is a
// compiled switch, run for every Item.ID at start-up (switch_eval.h). 0 when the
// item does not stack or the rule could not be read. The game tick only (the
// table is read on each call). Weapons are not items here: the game also stacks
// its support weapons other than knives (by WeaponBulletUserData), the mod moves
// weapons whole.
int stack_max(int item_id);

// --- durability ---------------------------------------------------------------------------------------------
// A knife wears down: its slot's count is its durability, each hit takes some
// (Equipment.onHitMeleeAttack -> useSubWeapon -> Inventory.reduceSlot), and the
// hit that empties it removes the knife in the same call (removeSubSlot). The
// game's knives are the weapons EquipmentDefine.getCategory files as
// WeaponCategory.Knife - what isMeleeWeapon asks - read out of that method's
// jump table.
bool is_melee_weapon(int weapon_id);
// A weapon's full count as the game gives it: WeaponBulletUserData's entry for
// the weapon, the LoadingPartsCombination whose parts it has and that overwrites
// the number, lowest _Priority first (WeaponLoadingpartsCombination.getNumber).
// 0 when unknown, -1 for an unlimited one.
int weapon_full_count(int weapon_id, int parts);
// The ammo a weapon is loaded with, as a stock records it in BulletId: the same
// entry's `_Kind` (an EquipmentDefine.Bullet), turned into an Item.ID by the
// game's own EquipmentDefine.getItemID - a compiled switch run here
// (switch_eval.h). 0 for a weapon that takes no ammo (a knife, a grenade) and
// when the rule could not be read; a weapon made with 0 records no ammo, which
// is what an empty one holds. Both figures are read once per weapon and parts.
int weapon_bullet_id(int weapon_id, int parts);

// --- difficulty -----------------------------------------------------------------------------------------
// The game's difficulty is the save header's CurrentDifficulty
// (MainFlowManager.Difficulty: EASY is Assisted, NORMAL Standard, HARD
// Hardcore). Saves record it, and the game reads it as it goes: the typewriters,
// the rank's range, the results screen's records; EnemyManager copies it every
// frame. MainFlowManager.setDifficulty - what a new game and every load run, a
// load with the loaded save's - writes it and sets four of GlobalUserDataManager's
// flags from it (DifficultyEasy, DifficultyNormal, DifficultyHard,
// DifficultyNoHard: the loading tips, one Hardcore-only checkpoint, some
// Assisted-only enemy behaviour). The mod switches it through that call (call.h).
enum class Difficulty { kUnknown = -1, kAssisted, kStandard, kHardcore };
Difficulty difficulty();                    // the header's; kUnknown when unreadable, or a value this build does not name
const char* difficulty_name(Difficulty d);  // "Assisted", "Standard", "Hardcore", "?"
int difficulty_value(Difficulty d);         // its MainFlowManager.Difficulty value in this build (-1: kUnknown)
// What setDifficulty is called on: the MainFlowManager, 0 when the game cannot
// take a switch now - no header, or no GlobalUserDataManager for the flags
// (setDifficulty would throw with the header already written).
uintptr_t difficulty_owner();
// MainFlowManager.<ForceEasyContinue>: the game over screen's "continue on
// Assisted" sets it to 1, and while it is above 0 setDifficulty writes EASY
// whatever it is asked (at 1 it also resets the rank and makes it 2). Only the
// way back to the title clears it (MainFlowManager.resetGame). -1 unknown.
int force_easy_continue();
bool set_force_easy_continue(int v);

// --- typewriters ----------------------------------------------------------------------------------------
// A typewriter offers its save through one of its triggers (gimmick.action.Trigger),
// chosen by difficulty: GimmickTypeWriter.getTriggerFuncCheckValid gives the trigger
// 'Check' the lambda <getTriggerFuncCheckValid>b__8_0 - `difficulty != HARD &&
// checkEnableButton()`, the save menu straight away as on Standard - and
// 'UseInkRibbon' b__8_1 - `difficulty == HARD && ...`, a ribbon chosen from the
// inventory first. It makes each a delegate as an area loads (events.cpp hooks both).
bool hardcore();  // MainFlowManager.gameHeaderSaveData.CurrentDifficulty is HARD

// --- the event hooks' work (events.cpp) --------------------------------------------------------------------
// The methods the game calls the mod through (database method indices, 0 when
// not found) and the classes they belong to.
struct EventTargets {
  uint32_t enemy_controller = 0, reaction = 0, survivor = 0, player = 0, typewriter = 0, weapon = 0, equipment = 0,
           main_flow = 0, clock = 0;
  // Delegates the game makes from the method's entry: hooked there, before the game makes them.
  uint32_t enemy_damage = 0;     // EnemyController.HitController_OnHitDamage(DamageInfo): an enemy is hit
  uint32_t reaction_damage = 0;  // EnemyReactionController.onHitDamage(DamageInfo): its reaction to the hit
  uint32_t player_check = 0;     // SurvivorCondition.checkHitDamage(DamageInfo): a hit on a survivor, before it counts
  uint32_t player_damage = 0;    // PlayerCondition.onHitDamage(DamageInfo): the hit applied
  uint32_t player_hp = 0;        // PlayerCondition.<doSurvivorStart>b__36_0(int): OnChangeHitPoint
  uint32_t tw_check[2] = {};     // GimmickTypeWriter.<getTriggerFuncCheckValid>b__8_0 ('Check'), b__8_1 ('UseInkRibbon')
  uint32_t weapon_hit = 0;       // Weapon.onHitAttack(DamageInfo): a melee weapon's hit
  uint32_t defend_category = 0;  // Equipment.defend(EquipCategory, int): a weapon used against a grab
  uint32_t defend_type = 0;      // Equipment.defend(WeaponType, int)
  // Virtual methods: hooked in the class's vtable, once the class is up.
  uint32_t fire = 0;             // Equipment.executeFire(WeaponType, int): a shot
  uint32_t melee = 0;            // Equipment.onHitMeleeAttack(DamageInfo, Melee.Attack, int): a knife's hit
  uint32_t save_data = 0;        // MainFlowManager.saveGameSaveData(): a save capturing the header (IGameSaveData)
  uint32_t clock_save = 0;       // GameClock.saveGameSaveData(): the same save capturing the clock
  uint32_t countdown_update = 0;  // CountDownBehavior.lateUpdate(): a countdown's frame (the self-destruct)
  uint32_t rogue_countdown_update = 0;  // RogueCountDownBehavior.updateCountDown() (entry: reached by reflection)
  // RogueCountDownBehavior.lateUpdate(): the same behaviour's count-*up* side - The
  // Ghost Survivors' run timer on the HUD. updateCountUp reads the clock itself
  // every frame, so the held time goes in around this call (Freeze play time; the
  // timer the player watches, not a time the game records).
  uint32_t rogue_timer_late = 0;
  uint32_t countdown = 0, rogue_countdown = 0;  // their classes
  // TriggerUseItem.<>c__DisplayClass33_0.<registerUseMode>b__0() and _1's b__1(): the callbacks the
  // inventory, opened for a use-item trigger, calls for the item picked (one the trigger wants, one
  // it lists as useless). Delegates made in TriggerUseItem.registerUseMode.
  uint32_t use_item = 0, use_useless = 0;
  // Where the records' counters move (Counter), all virtual:
  uint32_t item_box_action = 0, item_box_open = 0;  // fsmv2.SwitchItemToInventory.update(ActionArg): the item box opens
  uint32_t inventory_screen = 0, inventory_update = 0;  // gui.NewInventorySlotBehavior.update(): a recovery item is used
  uint32_t foot_effects = 0, footstep = 0;  // PlayerFootEffectController.onLand(JointPartsType, JointSideType, vec3)
  // The extra modes record their clear time while the run is still IN_GAME, not at
  // the result states the tick holds the time for: fsmv2.EndMeasureAndRecordGameElapsedTimeForExtra
  // .start(ActionArg) stops the clock measuring and writes the run's time into the
  // save then and there - The Ghost Survivors' four missions through
  // RogueRecordManager.backupRoguePlayData, The 4th Survivor's and The Tofu
  // Survivor's through RecordManager.backupGameElapsedTimeForExtra, both reading
  // GameClock.get_ActualRecordTime (Freeze play time, see the extra modes below).
  uint32_t extra_record_action = 0, extra_record = 0;
};
const EventTargets& event_targets();
int pause_state();  // MainFlowManager.MainState.PAUSE

// One hit kills. The enemy's HitPointController `hpc` (and its EnemyController,
// 0 when unknown) is about to take the DamageInfo `info`: its HP is lowered to
// the damage and the hit marked a kill, so the game's own code kills it. Not for
// a hit that does no damage, an enemy that takes none, or one whose HP the game
// holds for now (STOP_DAMAGE, GUTS_MODE). One that cannot die for now (NO_DEATH:
// Mr. X, set pieces) still loses it. The HP it had, or -1.
uintptr_t enemy_hit_points(uintptr_t enemy_controller);             // EnemyController.<HitPoint>
bool reaction_targets(uintptr_t reaction, uintptr_t* hpc, uintptr_t* enemy);  // EnemyReactionController's
int make_lethal(uintptr_t hpc, uintptr_t enemy, uintptr_t info);

// God mode.
bool is_player(uintptr_t survivor_condition);  // a PlayerCondition
int heal_player(uintptr_t player_condition);   // HP to its default while alive; the HP now, -1 unknown
bool cure_poison(uintptr_t player_condition);  // _IsPoison cleared (true when it was set)

// Infinite ammo and No durability loss.
uintptr_t survivor_of(uintptr_t equipment_or_weapon);  // its <Condition>, 0 when neither
bool equipped_stock(uintptr_t survivor, bool sub, Stock* out);  // Inventory._MainSlot / _SubSlot's stock
bool keep_knife_full(const Stock& s);  // a knife below its full count set back to full (true when written)
bool restore_count(const Stock& before);  // the same stock's count back up to `before` (true when written)

// Save without counting: a save's header SaveTimes back to `held`. What it was, or -1.
int cap_save_times(uintptr_t main_flow, int held);

// Freeze play time: where GameClock._MeasureGameElapsedTime lives in `clock`, 0 when unknown.
uintptr_t clock_measure_flag(uintptr_t clock);

// Freeze countdown timer: where a countdown behaviour (CountDownBehavior or
// RogueCountDownBehavior) keeps its <CurrentTimerFrame>, 0 when `behaviour` is neither.
uintptr_t countdown_timer(uintptr_t behaviour);

// Infinite wooden boards. `closure` is a use-item callback's target (the
// DisplayClass holding the trigger's ItemWork `wk`). TriggerUseItem.onUsedBase
// takes the work's TriggerUseItem.ItemData - Count of ItemId - from the
// inventory when its Stock is ITEM, unless CanReuse is set. When that item data
// asks for wooden boards: `keep` marks it reusable (1); without `keep`, a mark
// made here earlier is taken off again (-1). 0: nothing changed - not boards,
// the game's own reusable data, or unreadable. Touches only the item data the
// closure holds; the marks made are remembered so that no other is ever cleared.
int mark_boards_reusable(uintptr_t closure, bool keep);

// G2's stagger (the crane fight in the sewers) is not its health: Em7100Think
// keeps its own counter, TiredHp, and its onHitDamage does
// `TiredHp += tiredDamage(hit); if (TiredHp >= getTiredThreshoud()) tired`.
// GUTS_MODE is on there, so One hit kills leaves the HP alone (make_lethal) and
// nothing it writes could stagger him. This tops the counter up instead, before
// the game's handler adds this hit's share: 1 (staggered by this hit), 0 (not
// this enemy, or unreadable). The value is INT32_MAX / 2 - past any real
// threshold, and still short of the INT32_MAX the game uses for a phase that
// cannot be staggered at all, which is left to stand.
int stun_g2(uintptr_t enemy);

// The plants (app.ropeway.enemy.em5000: the Ivy and the Poison Ivy are one
// class). Their health is not what kills them - fire is. Em5000Think keeps a
// second, flame-only health, and its onHitDamage is
//   `if (IsLive && FlameOnlyHitPoint.CurrentHitPoint <= 0) forceDead();      // the burn-up
//    else if (HitPoint.CurrentHitPoint == 1) setupFakeDead(true);`           // the knockdown
// - the flame-only health first, so a plant with an empty one burns up on the
// spot however it was hit, the knockdown never reached. Called for the enemy
// `enemy` (its HitPointController `hpc`) about to take `info`: the flame-only
// health is emptied, and the health is put where the game's own addDamage lands
// it on 1, which is all it takes to keep the plant alive long enough to read
// that first test. Nothing is written for a hit the game is holding off
// (STOP_DAMAGE), one that does no damage, or a plant already dying. kNotAPlant
// tells another enemy apart from a plant left alone - a plant is never handed to
// make_lethal, whose kill would be a plain death where the game wants a burn-up.
enum PlantHit { kNotAPlant = -1, kPlantLeftAlone = 0, kPlantBurned };
int wither_plant(uintptr_t enemy, uintptr_t hpc, uintptr_t info);

// The marks above are addresses of the game's objects, so they mean nothing once
// the scenes holding them are gone: forgotten as a game session starts, before
// any of the new ones can land on a remembered address and have its mark taken
// off as if the mod had made it.
void forget_board_marks();

// --- diagnostics ---------------------------------------------------------------------------------------
void log_bag(const char* why);
void log_box(const char* why);
void trace_tick(int main_state);  // state changes; Trace = 1: the inventory, box, players and clock at the pause menu

}  // namespace re2cc::game
