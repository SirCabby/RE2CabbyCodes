#include "events.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "call.h"
#include "config.h"
#include "game.h"
#include "items.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "records.h"
#include "savefiles.h"

namespace re2cc::events {
namespace {

enum Hook : int {
  // Database entries: the delegates the game makes.
  kEnemyDamage = 0,
  kReactionDamage,
  kPlayerCheck,
  kPlayerDamage,
  kPlayerHp,
  kTypewriterCheck,
  kTypewriterRibbon,
  kWeaponHit,
  kDefendCategory,
  kDefendType,
  kRogueCountdown,
  kUseItem,
  kUseUseless,
  // Vtable slots: virtual methods.
  kFire,
  kMelee,
  kSaveData,
  kClockSave,
  kExtraRecord,  // the extra modes' clear time: EndMeasureAndRecordGameElapsedTimeForExtra.start
  kRogueTimer,   // The Ghost Survivors' HUD run timer: RogueCountDownBehavior.lateUpdate
  kCountdownUpdate,
  kItemBoxOpen,
  kInventoryUpdate,
  kFootstep,
  kRecordsScreen,       // the records panel: RecordBehavior.update
  kRogueRecordsScreen,  // RogueRecordBehavior.update
  kLoadScreen,          // the save files: LoadBehavior.update (SaveLoadBaseBehavior's, in LoadBehavior's vtable)
  kHookCount
};
constexpr int kFirstSlotHook = kFire;

struct State {
  const char* name = "";
  uint32_t method = 0;
  uint32_t cls = 0;             // vtable hooks: the class whose vtable carries it
  uintptr_t replacement = 0;
  std::atomic<uintptr_t> original{0};  // the game's code, forwarded to
  std::atomic<long> calls{0};
  uintptr_t record = 0;         // the database entry exchanged
  uintptr_t slot[8] = {};       // the vtable / interface slots exchanged
  int slots = 0;
  bool failed = false;          // not hookable this session (logged)
  bool first_logged = false;
  DWORD waiting_since = 0;      // a vtable hook still waiting for its class (the log)
};
State g_hook[kHookCount];
bool g_disabled = false;

inline void count(Hook h) { g_hook[h].calls.fetch_add(1, std::memory_order_relaxed); }
template <typename Fn>
inline Fn original(Hook h) {
  return reinterpret_cast<Fn>(g_hook[h].original.load(std::memory_order_relaxed));
}
inline bool on(cheats::Kind k) { return cheats::enabled(k); }
bool hooked(Hook h) { return g_hook[h].original.load() && (g_hook[h].record || g_hook[h].slots > 0); }

bool is_sub_weapon(int weapon_id) {
  for (const auto& w : items::kWeapons)
    if (w.id == weapon_id) return w.sub;
  return false;
}

using InfoFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t info);
using CheckFn = bool (*)(uintptr_t ctx, uintptr_t self, uintptr_t arg);
using IntFn = void (*)(uintptr_t ctx, uintptr_t self, int32_t value);
using DefendFn = uintptr_t (*)(uintptr_t ret, uintptr_t ctx, uintptr_t self, int32_t which, int32_t num);
using FireFn = bool (*)(uintptr_t ctx, uintptr_t self, int32_t weapon_type, int32_t num);
using MeleeFn = uintptr_t (*)(uintptr_t ret, uintptr_t ctx, uintptr_t self, uintptr_t info, int32_t attack,
                              int32_t reduce_point);
using RefFn = uintptr_t (*)(uintptr_t ctx, uintptr_t self);
using UpdateFn = void (*)(uintptr_t ctx, uintptr_t self);
using ActionFn = void (*)(uintptr_t ctx, uintptr_t self, uintptr_t arg);  // a behaviour tree action's (ActionArg)
using LandFn = void (*)(uintptr_t ctx, uintptr_t self, int32_t part, int32_t side, uintptr_t pos);  // pos: a via.vec3*

// --- One hit kills --------------------------------------------------------------------------------------
// An enemy is hit (HitController.DamageHitHandler, after the game has worked
// the damage out): its HP is lowered to the damage and the hit marked a kill, so
// the game's own handler takes the rest and kills it - drops and death as usual.
// One that cannot die for now loses its HP all the same: Mr. X goes down on his
// knees, as he does when enough damage runs his HP out.
void trace_lethal(const char* handler, uintptr_t enemy, int was) {
  if (was > 0 && config::get().trace)
    logf("one hit kills: enemy %p loses all %d HP to this hit (%s)", reinterpret_cast<void*>(enemy), was, handler);
}

// G2 in the sewers is staggered, not killed: the crane does that. Its stagger
// counter is topped up here, before the game's handler adds this hit's share,
// so the first hit tires it out. Any other enemy is left to make_lethal.
void trace_stun(uintptr_t enemy, int done) {
  if (done > 0 && config::get().trace)
    logf("one hit kills: enemy %p (G2) is staggered by this hit", reinterpret_cast<void*>(enemy));
}

// The plants are neither: only fire kills them, and the game keeps a second
// health for it. That one is emptied here, so this hit burns the plant up
// through the game's own onHitDamage - no glands to break first, no fire, and
// none of the lying there in between. Any other enemy is left to make_lethal.
void trace_plant(uintptr_t enemy, int done) {
  if (done == game::kPlantBurned && config::get().trace)
    logf("one hit kills: plant %p burns up on this hit", reinterpret_cast<void*>(enemy));
}

// The three are exclusive: an enemy is a plant, G2, or neither, and each looks
// at the enemy's own class first. A plant is the game's to kill even on a hit
// nothing was written for - a lethal blow there would be a plain death where
// the game wants the burn-up.
void on_hit(const char* handler, uintptr_t enemy, uintptr_t hpc, uintptr_t info) {
  const int plant = game::wither_plant(enemy, hpc, info);
  trace_plant(enemy, plant);
  if (plant != game::kNotAPlant) return;
  trace_lethal(handler, enemy, game::make_lethal(hpc, enemy, info));
  trace_stun(enemy, game::stun_g2(enemy));
}

void on_enemy_damage(uintptr_t ctx, uintptr_t enemy, uintptr_t info) {
  count(kEnemyDamage);
  if (on(cheats::kOneHitKills)) on_hit("enemy controller", enemy, game::enemy_hit_points(enemy), info);
  original<InfoFn>(kEnemyDamage)(ctx, enemy, info);
}

// The same hit, when the enemy's reaction handles it first: the reaction then
// sees a killing blow too.
void on_reaction_damage(uintptr_t ctx, uintptr_t reaction, uintptr_t info) {
  count(kReactionDamage);
  if (on(cheats::kOneHitKills)) {
    uintptr_t hpc = 0, enemy = 0;
    if (game::reaction_targets(reaction, &hpc, &enemy)) on_hit("its reaction", enemy, hpc, info);
  }
  original<InfoFn>(kReactionDamage)(ctx, reaction, info);
}

// --- God mode ---------------------------------------------------------------------------------------------
// A hit on a survivor, before the game decides whether it kills: the player is
// brought back to full first, so only a hit worth more than all of it can.
bool on_player_check(uintptr_t ctx, uintptr_t survivor, uintptr_t info) {
  count(kPlayerCheck);
  if (on(cheats::kGodMode) && game::is_player(survivor)) game::heal_player(survivor);
  return original<CheckFn>(kPlayerCheck)(ctx, survivor, info);
}

// The hit applied: the health it took is given back here, and poison with it.
// `checkHitDamage` above only decides that the hit does not kill - the game still
// takes the health - and the OnChangeHitPoint handler that was meant to put it
// back has never once been called by the game (both logs, every session), so the
// health is restored on this hook, which does run on every hit. A blow that took
// everything is still the game's to play out (heal_player leaves the dead alone).
void on_player_damage(uintptr_t ctx, uintptr_t player, uintptr_t info) {
  count(kPlayerDamage);
  original<InfoFn>(kPlayerDamage)(ctx, player, info);
  if (on(cheats::kGodMode) && !call::exception_pending(ctx)) {
    const int hp = game::heal_player(player);
    if (hp > 0 && config::get().trace) logf("god mode: player back to %d HP after this hit", hp);
    game::cure_poison(player);
  }
}

// The player's health changed (OnChangeHitPoint, raised by every change): back
// to full while alive, and the game's handler told the health it has now.
void on_player_hp(uintptr_t ctx, uintptr_t player, int32_t hp) {
  count(kPlayerHp);
  if (on(cheats::kGodMode)) {
    const int now = game::heal_player(player);
    if (now > 0) hp = now;
    game::cure_poison(player);
  }
  original<IntFn>(kPlayerHp)(ctx, player, hp);
}

// --- Save without ink ribbons ---------------------------------------------------------------------------
// On Hardcore a typewriter's 'Check' trigger answers no and 'UseInkRibbon' asks
// for a ribbon. With the cheat on each runs the other's check in its place: the
// HARD one lets 'Check' through - straight to the save menu, as on Standard -
// and the not-HARD one turns 'UseInkRibbon' away, so no ribbon is asked for.
// One of the game's two checks answers either way, as the game calls it. (The
// save menu itself still spends a ribbon the player carries on Hardcore: its
// decide callback is GimmickTypeWriter.reduceInkRibbonNum, not hooked.)
bool swap_typewriter_checks() {
  return on(cheats::kSaveWithoutInk) && hooked(kTypewriterCheck) && hooked(kTypewriterRibbon) && game::hardcore();
}

bool on_typewriter_check(uintptr_t ctx, uintptr_t typewriter, uintptr_t trigger) {
  count(kTypewriterCheck);
  return original<CheckFn>(swap_typewriter_checks() ? kTypewriterRibbon : kTypewriterCheck)(ctx, typewriter, trigger);
}

bool on_typewriter_ribbon(uintptr_t ctx, uintptr_t typewriter, uintptr_t trigger) {
  count(kTypewriterRibbon);
  return original<CheckFn>(swap_typewriter_checks() ? kTypewriterCheck : kTypewriterRibbon)(ctx, typewriter, trigger);
}

// --- No durability loss ----------------------------------------------------------------------------------
// A knife's hit, or a weapon used to break a grab, takes from the equipped
// knife's count - and the hit that empties it removes the knife in the same
// call. So the knife is full before the game's code runs, and full again after.
// A knife the game removed anyway stays removed: nothing is ever put into a
// slot here.
void keep_knife(uintptr_t survivor) {
  game::Stock s;
  if (survivor && game::equipped_stock(survivor, true, &s) && game::keep_knife_full(s) && config::get().trace)
    logf("no durability loss: knife (weapon %d) back to full from %d", s.item.weapon_id, s.item.count);
}

void on_weapon_hit(uintptr_t ctx, uintptr_t weapon, uintptr_t info) {
  count(kWeaponHit);
  const uintptr_t survivor = on(cheats::kNoDurabilityLoss) ? game::survivor_of(weapon) : 0;
  keep_knife(survivor);
  original<InfoFn>(kWeaponHit)(ctx, weapon, info);
  if (survivor && !call::exception_pending(ctx)) keep_knife(survivor);
}

uintptr_t defend(Hook h, uintptr_t ret, uintptr_t ctx, uintptr_t equipment, int32_t which, int32_t num) {
  count(h);
  const uintptr_t survivor = on(cheats::kNoDurabilityLoss) ? game::survivor_of(equipment) : 0;
  keep_knife(survivor);
  const uintptr_t r = original<DefendFn>(h)(ret, ctx, equipment, which, num);
  if (survivor && !call::exception_pending(ctx)) keep_knife(survivor);
  return r;
}
uintptr_t on_defend_category(uintptr_t ret, uintptr_t ctx, uintptr_t equipment, int32_t category, int32_t num) {
  return defend(kDefendCategory, ret, ctx, equipment, category, num);
}
uintptr_t on_defend_type(uintptr_t ret, uintptr_t ctx, uintptr_t equipment, int32_t weapon_type, int32_t num) {
  return defend(kDefendType, ret, ctx, equipment, weapon_type, num);
}

uintptr_t on_melee(uintptr_t ret, uintptr_t ctx, uintptr_t equipment, uintptr_t info, int32_t attack, int32_t reduce_point) {
  count(kMelee);
  const uintptr_t survivor = on(cheats::kNoDurabilityLoss) ? game::survivor_of(equipment) : 0;
  keep_knife(survivor);
  const uintptr_t r = original<MeleeFn>(kMelee)(ret, ctx, equipment, info, attack, reduce_point);
  if (survivor && !call::exception_pending(ctx)) keep_knife(survivor);
  return r;
}

// --- Infinite wooden boards --------------------------------------------------------------------------------
// A window takes its boards through a use-item trigger. The inventory, opened
// for it, calls the trigger's callback for the item picked - <registerUseMode>b__0
// for an item the trigger wants, b__1 for one it lists as useless - which queues
// the work for the interact task, whose next update runs TriggerUseItem.onUsedBase:
// that takes the work item data's Count of the item from the inventory unless the
// item data is marked CanReuse. With the cheat on, wooden boards' item data is
// marked before the callback queues the work, so the window is boarded and no
// board is taken; with it off, a mark the mod made is taken off again the next
// time that trigger is used. Only the item data the callback hands over is touched.
void keep_boards(Hook h, uintptr_t ctx, uintptr_t closure) {
  count(h);
  const int done = game::mark_boards_reusable(closure, on(cheats::kInfiniteBoards));
  if (done != 0 && config::get().trace)
    logf("infinite wooden boards: %s", done > 0 ? "this use takes no board" : "a window takes its boards again");
  original<UpdateFn>(h)(ctx, closure);
}

void on_use_item(uintptr_t ctx, uintptr_t closure) { keep_boards(kUseItem, ctx, closure); }
void on_use_useless(uintptr_t ctx, uintptr_t closure) { keep_boards(kUseUseless, ctx, closure); }

// --- Infinite ammo ----------------------------------------------------------------------------------------
// A shot (Equipment.executeFire -> use -> useMainWeapon -> Inventory.reduceSlot):
// the equipped gun's loaded rounds are read before and put back after, so the
// shot takes nothing. A reload between two shots raises the count, and that is
// kept; knives and grenades (sub weapons) are left alone.
bool on_fire(uintptr_t ctx, uintptr_t equipment, int32_t weapon_type, int32_t num) {
  count(kFire);
  game::Stock before;
  const uintptr_t survivor = on(cheats::kInfiniteAmmo) ? game::survivor_of(equipment) : 0;
  const bool keep = survivor && game::equipped_stock(survivor, false, &before) && game::is_weapon(before.item) &&
                    !is_sub_weapon(before.item.weapon_id);
  const bool fired = original<FireFn>(kFire)(ctx, equipment, weapon_type, num);
  if (keep && !call::exception_pending(ctx) && game::restore_count(before) && config::get().trace)
    logf("infinite ammo: weapon %d keeps its %d round(s)", before.item.weapon_id, before.item.count);
  return fired;
}

// --- Save without counting ----------------------------------------------------------------------------
// A save adds one to the game header's SaveTimes and captures the header in the
// same call (SaveDataManager.saveGameSaveData -> MainFlowManager.addSaveTimes,
// then every IGameSaveData's saveGameSaveData). This runs in between: the count
// goes back to the one held before the file is written.
uintptr_t on_save_data(uintptr_t ctx, uintptr_t main_flow) {
  count(kSaveData);
  if (on(cheats::kNoSaveCount)) {
    const int held = cheats::held_save_count();
    const int was = game::cap_save_times(main_flow, held);
    if (was >= 0) logf("save without counting: this save records %d save(s), not %d", held, was);
  }
  return original<RefFn>(kSaveData)(ctx, main_flow);
}

// --- Freeze play time -------------------------------------------------------------------------------------
// The clock is never stopped: the enemies run on it (game.h, enemy_time), and one
// that does not move leaves EnemyManager's think-off, no-attack and attack-through
// timers running for good and canRestrictMove holding enemies still - enemies that
// stand about and scripted set pieces that never fire, until the game is restarted.
// (The first build of this cheat cleared GameClock._MeasureGameElapsedTime around
// the clock's own update, which did exactly that; seen in game 2026-09-17 and
// 2026-09-20.)
//   Instead the time the game should record is held mod-side (cheats.cpp) and put
// into the clock here, where a save captures the clock's own save data - the same
// pass as the header's, so it lands in the file. The save captures it by
// reference, so the tick is what takes the time back out again.
uintptr_t on_clock_save(uintptr_t ctx, uintptr_t clock) {
  count(kClockSave);
  cheats::hold_playtime_for_save();
  return original<RefFn>(kClockSave)(ctx, clock);
}

// The extra modes do not wait for their result state to record the run: this FSM
// action's start, the last thing the run does while the main state is still
// IN_GAME, stops the clock measuring and writes the clear time into the save there
// and then - The Ghost Survivors' four missions into the rogue save's per-mission
// clear times (RogueRecordManager.backupRoguePlayData, which also decides the
// screen's NEW RECORD), The 4th Survivor's and The Tofu Survivor's into the system
// save (RecordManager.backupGameElapsedTimeForExtra). Both read the clock, about a
// second before ROGUE_RESULT / RESULT_EXTRA, where the tick puts the held time in -
// so without this the results screen showed the held time and the record kept the
// clock's. The hold goes in for this one call and comes straight back out; it is
// given back whatever the game's code left behind, since it is only a field write
// of the mod's.
void on_extra_record(uintptr_t ctx, uintptr_t action, uintptr_t arg) {
  count(kExtraRecord);
  const bool held = cheats::hold_playtime_for_extra_record();
  original<ActionFn>(kExtraRecord)(ctx, action, arg);
  if (held) cheats::give_playtime_hold_back("record");
}

// The Ghost Survivors' run timer on the HUD is the clock itself, not a time the
// game recorded: RogueCountDownBehavior.lateUpdate -> updateCountUp reads
// GameClock.get_ActualRecordTime every frame into <CurrentTimerFrame>, and
// dispCountUp draws it in the same call. Putting the field back afterwards would
// be undone by the next frame's read, so the held time goes into the clock for the
// length of this call and comes out again at once - the timer the player watches
// stands at the held time while the clock underneath keeps running for the enemies.
// (The count-*down* side of this behaviour is Freeze countdown timer's, on its own
// hook: it works off the frame's time, not the clock, so the hold does not touch
// it.)
void on_rogue_timer(uintptr_t ctx, uintptr_t behaviour) {
  count(kRogueTimer);
  const bool held = cheats::hold_playtime_for_display();
  original<UpdateFn>(kRogueTimer)(ctx, behaviour);
  if (held) cheats::give_playtime_hold_back();
}

// --- Freeze countdown timer ------------------------------------------------------------------------------
// A countdown's frame (CountDownBehavior.lateUpdate -> updateCountDown: the timer
// less the frame's time, the seconds-left flags it passes, the game over at 0 -
// and the Ghost Survivors' RogueCountDownBehavior.updateCountDown alike) runs as
// the game has it, and the timer is put back where it was. A timer that ran out
// in this very frame is left to the game (its game over has already begun).
void freeze_countdown(Hook h, uintptr_t ctx, uintptr_t behaviour) {
  count(h);
  uintptr_t timer = 0;
  float before = 0.0f;
  if (on(cheats::kFreezeCountdown) && (timer = game::countdown_timer(behaviour)) != 0 && !mem::read_safe(timer, &before))
    timer = 0;
  original<UpdateFn>(h)(ctx, behaviour);
  float after = 0.0f;
  if (timer && before > 0.0f && mem::read_safe(timer, &after) && after > 0.0f && after < before)
    mem::store<float>(timer, before);
}

void on_countdown_update(uintptr_t ctx, uintptr_t behaviour) { freeze_countdown(kCountdownUpdate, ctx, behaviour); }
void on_rogue_countdown(uintptr_t ctx, uintptr_t behaviour) { freeze_countdown(kRogueCountdown, ctx, behaviour); }

// --- The records' counters ------------------------------------------------------------------------------
// Three records are counted as the game is played (game.h): the item box opened,
// recovery items used, the player's steps. Each count moves inside one of the
// game's calls - the item box's FSM action opening it (SwitchItemToInventory.update
// -> GUIMaster.openInventoryItemBoxMode), the inventory screen's frame (a recovery
// item is used from inside NewInventorySlotBehavior.update), a footstep
// (PlayerFootEffectController.onLand) - and while its switch is on, the count is
// put back as soon as that call returns: 0 for the item box and recovery items,
// the count held for the steps. So what the game saves, and checks at the clear,
// never moves.
void hold_count(cheats::Kind k, game::Counter c, int held, uintptr_t ctx) {
  if (held < 0 || call::exception_pending(ctx)) return;
  const int was = game::hold_counter(c, held);
  if (was >= 0 && config::get().trace) logf("%s: the count goes back from %d to %d", cheats::name(k), was, held);
}

void on_item_box_open(uintptr_t ctx, uintptr_t action, uintptr_t arg) {
  count(kItemBoxOpen);
  original<ActionFn>(kItemBoxOpen)(ctx, action, arg);
  if (on(cheats::kNoItemBoxCount)) hold_count(cheats::kNoItemBoxCount, game::Counter::kItemBox, 0, ctx);
}

// Every frame the inventory screen updates: while the switch is on, the count
// and a few pointers leading to it are read (where it is, is kept - game.h);
// nothing otherwise.
void on_inventory_update(uintptr_t ctx, uintptr_t screen) {
  count(kInventoryUpdate);
  original<UpdateFn>(kInventoryUpdate)(ctx, screen);
  if (on(cheats::kNoHealCount)) hold_count(cheats::kNoHealCount, game::Counter::kHeals, 0, ctx);
}

void on_footstep(uintptr_t ctx, uintptr_t feet, int32_t part, int32_t side, uintptr_t pos) {
  count(kFootstep);
  original<LandFn>(kFootstep)(ctx, feet, part, side, pos);
  if (on(cheats::kFreezeSteps)) hold_count(cheats::kFreezeSteps, game::Counter::kSteps, cheats::held_steps(), ctx);
}

// --- The records panel ---------------------------------------------------------------------------------------
// A records screen's own frame: RecordBehavior.update and RogueRecordBehavior.update
// run only while their screen's object is active - from the start of its opening
// to the end of its closing (RopewayGuiBehaviorRoot.enableObject) - so the panel's
// records window is up exactly then (records.h). Nothing of the game is read here.
void on_records_screen(uintptr_t ctx, uintptr_t screen) {
  count(kRecordsScreen);
  original<UpdateFn>(kRecordsScreen)(ctx, screen);
  if (!call::exception_pending(ctx)) records::screen_frame(records::kMain, screen);
}

void on_rogue_records_screen(uintptr_t ctx, uintptr_t screen) {
  count(kRogueRecordsScreen);
  original<UpdateFn>(kRogueRecordsScreen)(ctx, screen);
  if (!call::exception_pending(ctx)) records::screen_frame(records::kRogue, screen);
}

// --- The save files ------------------------------------------------------------------------------------------
// The Load Game screen's own frame: LoadBehavior.update is its base class's
// (SaveLoadBaseBehavior.update), hooked in LoadBehavior's own vtable - so a
// typewriter's save screen (SaveBehavior, the same code through its own table)
// is not heard. It runs only while the screen is up; after the game's update the
// panel's save-file window reads the list the screen shows, and a change the
// panel made has the game's list read again (savefiles.h).
void on_load_screen(uintptr_t ctx, uintptr_t screen) {
  count(kLoadScreen);
  original<UpdateFn>(kLoadScreen)(ctx, screen);
  if (!call::exception_pending(ctx)) savefiles::screen_frame(ctx, screen);
}

// --- installing ------------------------------------------------------------------------------------------------
uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }
unsigned long long rva(uintptr_t a) { return a > exe_base() ? static_cast<unsigned long long>(a - exe_base()) : 0; }

void define(Hook h, const char* name, uint32_t method, uintptr_t replacement, uint32_t cls = 0) {
  State& st = g_hook[h];
  st.name = name;
  st.method = method;
  st.replacement = replacement;
  st.cls = cls;
}

void fail(State& st, const char* why) {
  st.failed = true;
  logf("events: %s - not hooked: %s", st.name, why);
}

void hook_entry(Hook h) {
  State& st = g_hook[h];
  if (!st.method) return fail(st, "its method is not in the database");
  const uintptr_t record = re::method_record_at(st.method);
  const uintptr_t code = re::method_code_at(st.method);
  if (!record || !code) return fail(st, "its database entry holds no code");
  // Delegates already made call the replacement, which forwards to `original`:
  // only ever the game's own code.
  if (!mem::section(GetModuleHandleA(nullptr), ".text").contains(code))
    return fail(st, "its database entry holds something other than the game's code (another mod?)");
  st.original = code;  // before the exchange: the game may call it at once
  unsigned long protect = 0;
  const uintptr_t was = re::hook_method(record, st.replacement, &protect);
  if (!was) {
    char why[160];
    std::snprintf(why, sizeof(why), "its database entry %p could not be exchanged (page protection 0x%lX)",
                  reinterpret_cast<void*>(record), protect);
    return fail(st, why);
  }
  if (was != code) st.original = was;
  st.record = record;
  logf("events: %s hooked in its database entry %p (code exe+0x%llX; %d global(s) of the game point at the entry)",
       st.name, reinterpret_cast<void*>(record), rva(was), re::globals_pointing_at(record));
}

// A virtual method: its slot in the class's vtable - and any interface table
// slot holding the same code (a save goes through IGameSaveData) - once the
// class has its REObjectInfo. The code must be the method's own, and only its:
// a folded body several methods share is not hooked.
void hook_slots(Hook h, bool interfaces) {
  State& st = g_hook[h];
  if (st.failed || st.slots > 0) return;
  if (!st.method || !st.cls) return fail(st, "its class or method is not in the database");
  const uintptr_t code = re::method_code_at(st.method);
  if (!code) return fail(st, "its database entry holds no code");
  if (!re::vtable_of(st.cls)) {
    if (!st.waiting_since) {
      st.waiting_since = GetTickCount();
      logf("events: %s - waiting for %s to come up", st.name, re::full_name(st.cls).c_str());
    }
    return;
  }
  uintptr_t found[9];
  int n = 0;
  if (const uintptr_t slot = re::vtable_slot(st.cls, st.method, code)) found[n++] = slot;
  if (interfaces) {
    uintptr_t more[8];
    const int m = re::interface_slots(st.cls, code, more, 8);
    for (int i = 0; i < m; ++i) {
      bool dup = false;
      for (int k = 0; k < n; ++k) dup = dup || found[k] == more[i];
      if (!dup && n < 9) found[n++] = more[i];
    }
  }
  if (n == 0) {
    // A vtable not filled in yet is waited for; one holding other code is not the game's as expected.
    const int vt = re::method_vt_index(st.method);
    const uintptr_t table = re::vtable_of(st.cls);
    if (vt >= 0 && table && mem::read_ptr(table + static_cast<uintptr_t>(vt) * 8) == 0) return;
    return fail(st, "no slot of the class's vtable holds the method's code");
  }
  st.original = code;
  for (int i = 0; i < n && st.slots < 8; ++i)
    if (re::swap_slot(found[i], code, st.replacement)) st.slot[st.slots++] = found[i];
  if (st.slots == 0) return fail(st, "its vtable slot could not be exchanged");
  logf("events: %s hooked in %d slot(s) of %s's tables (first %p, code exe+0x%llX; %d other class(es) share the vtable)",
       st.name, st.slots, re::full_name(st.cls).c_str(), reinterpret_cast<void*>(st.slot[0]), rva(code),
       re::classes_sharing_vtable(st.cls));
}

}  // namespace

void install() {
  const game::EventTargets& ev = game::event_targets();
  define(kEnemyDamage, "EnemyController.HitController_OnHitDamage (One hit kills)", ev.enemy_damage,
         reinterpret_cast<uintptr_t>(&on_enemy_damage));
  define(kReactionDamage, "EnemyReactionController.onHitDamage (One hit kills)", ev.reaction_damage,
         reinterpret_cast<uintptr_t>(&on_reaction_damage));
  define(kPlayerCheck, "SurvivorCondition.checkHitDamage (God mode)", ev.player_check,
         reinterpret_cast<uintptr_t>(&on_player_check));
  define(kPlayerDamage, "PlayerCondition.onHitDamage (God mode)", ev.player_damage,
         reinterpret_cast<uintptr_t>(&on_player_damage));
  define(kPlayerHp, "PlayerCondition OnChangeHitPoint handler (God mode)", ev.player_hp,
         reinterpret_cast<uintptr_t>(&on_player_hp));
  define(kTypewriterCheck, "GimmickTypeWriter 'Check' lambda (Save without ink ribbons)",
         game::parts().typewriter ? ev.tw_check[0] : 0, reinterpret_cast<uintptr_t>(&on_typewriter_check));
  define(kTypewriterRibbon, "GimmickTypeWriter 'UseInkRibbon' lambda (Save without ink ribbons)",
         game::parts().typewriter ? ev.tw_check[1] : 0, reinterpret_cast<uintptr_t>(&on_typewriter_ribbon));
  define(kWeaponHit, "Weapon.onHitAttack (No durability loss)", ev.weapon_hit, reinterpret_cast<uintptr_t>(&on_weapon_hit));
  define(kDefendCategory, "Equipment.defend(EquipCategory) (No durability loss)", ev.defend_category,
         reinterpret_cast<uintptr_t>(&on_defend_category));
  define(kDefendType, "Equipment.defend(WeaponType) (No durability loss)", ev.defend_type,
         reinterpret_cast<uintptr_t>(&on_defend_type));
  define(kFire, "Equipment.executeFire (Infinite ammo)", ev.fire, reinterpret_cast<uintptr_t>(&on_fire), ev.equipment);
  define(kMelee, "Equipment.onHitMeleeAttack (No durability loss)", ev.melee, reinterpret_cast<uintptr_t>(&on_melee),
         ev.equipment);
  define(kSaveData, "MainFlowManager.saveGameSaveData (Save without counting)", ev.save_data,
         reinterpret_cast<uintptr_t>(&on_save_data), ev.main_flow);
  define(kClockSave, "GameClock.saveGameSaveData (Freeze play time)", ev.clock_save,
         reinterpret_cast<uintptr_t>(&on_clock_save), ev.clock);
  define(kExtraRecord, "EndMeasureAndRecordGameElapsedTimeForExtra.start (Freeze play time, the extra modes)",
         ev.extra_record, reinterpret_cast<uintptr_t>(&on_extra_record), ev.extra_record_action);
  define(kRogueTimer, "RogueCountDownBehavior.lateUpdate (Freeze play time, the Ghost Survivors' run timer)",
         ev.rogue_timer_late, reinterpret_cast<uintptr_t>(&on_rogue_timer), ev.rogue_countdown);
  define(kRogueCountdown, "RogueCountDownBehavior.updateCountDown (Freeze countdown timer)", ev.rogue_countdown_update,
         reinterpret_cast<uintptr_t>(&on_rogue_countdown));
  define(kUseItem, "TriggerUseItem use-item callback <registerUseMode>b__0 (Infinite wooden boards)",
         game::parts().boards ? ev.use_item : 0, reinterpret_cast<uintptr_t>(&on_use_item));
  define(kUseUseless, "TriggerUseItem useless-item callback <registerUseMode>b__1 (Infinite wooden boards)",
         game::parts().boards ? ev.use_useless : 0, reinterpret_cast<uintptr_t>(&on_use_useless));
  define(kCountdownUpdate, "CountDownBehavior.lateUpdate (Freeze countdown timer)", ev.countdown_update,
         reinterpret_cast<uintptr_t>(&on_countdown_update), ev.countdown);
  define(kItemBoxOpen, "SwitchItemToInventory.update (Item box without counting)",
         game::parts().item_box_count ? ev.item_box_open : 0, reinterpret_cast<uintptr_t>(&on_item_box_open),
         ev.item_box_action);
  define(kInventoryUpdate, "NewInventorySlotBehavior.update (Recovery items without counting)",
         game::parts().heal_count ? ev.inventory_update : 0, reinterpret_cast<uintptr_t>(&on_inventory_update),
         ev.inventory_screen);
  define(kFootstep, "PlayerFootEffectController.onLand (Freeze step count)", game::parts().steps ? ev.footstep : 0,
         reinterpret_cast<uintptr_t>(&on_footstep), ev.foot_effects);
  define(kRecordsScreen, "RecordBehavior.update (the records panel)",
         records::ready(records::kMain) ? records::screen_update(records::kMain) : 0,
         reinterpret_cast<uintptr_t>(&on_records_screen), records::screen_class(records::kMain));
  define(kRogueRecordsScreen, "RogueRecordBehavior.update (the records panel)",
         records::ready(records::kRogue) ? records::screen_update(records::kRogue) : 0,
         reinterpret_cast<uintptr_t>(&on_rogue_records_screen), records::screen_class(records::kRogue));
  define(kLoadScreen, "LoadBehavior.update (the save files)", savefiles::screen_update(),
         reinterpret_cast<uintptr_t>(&on_load_screen), savefiles::screen_class());
  if (config::get().disable_events) {
    g_disabled = true;
    logf("events: disabled by config - the cheats do nothing this session");
    return;
  }
  for (int h = 0; h < kFirstSlotHook; ++h) hook_entry(static_cast<Hook>(h));
  service();
}

void service() {
  if (g_disabled) return;
  hook_slots(kFire, false);
  hook_slots(kMelee, false);
  hook_slots(kSaveData, true);
  hook_slots(kClockSave, true);
  hook_slots(kExtraRecord, false);
  hook_slots(kRogueTimer, false);
  hook_slots(kCountdownUpdate, false);
  hook_slots(kItemBoxOpen, false);
  hook_slots(kInventoryUpdate, false);
  hook_slots(kFootstep, false);
  hook_slots(kRecordsScreen, false);
  hook_slots(kRogueRecordsScreen, false);
  hook_slots(kLoadScreen, false);
  for (State& st : g_hook) {
    // An entry the runtime filled again (it links a class's code into its
    // entries as it starts): the game's code taken as the original, hooked again.
    if (st.record && re::method_code_at(st.method) != st.replacement) {
      const uintptr_t code = re::method_code_at(st.method);
      logf("events: %s - its database entry holds exe+0x%llX again; hooking it again", st.name, rva(code));
      st.record = 0;
      hook_entry(static_cast<Hook>(&st - g_hook));
    }
    // A slot someone else put back (or the class's tables rebuilt): logged once, hooked again.
    for (int i = 0; i < st.slots; ++i)
      if (mem::read_ptr(st.slot[i]) != st.replacement) {
        logf("events: %s - slot %p no longer holds the hook; looking again", st.name, reinterpret_cast<void*>(st.slot[i]));
        st.slots = 0;
        break;
      }
    if (!st.first_logged && st.calls.load() > 0) {
      st.first_logged = true;
      logf("events: %s called by the game (first time)", st.name);
    }
  }
}

bool ready(cheats::Kind k) {
  if (g_disabled) return false;
  const game::Parts& p = game::parts();
  switch (k) {
    case cheats::kOneHitKills: return p.enemies && p.damage && hooked(kEnemyDamage);
    case cheats::kGodMode: return p.players && hooked(kPlayerHp);
    case cheats::kInfiniteAmmo: return p.equipped && hooked(kFire);
    case cheats::kNoDurabilityLoss: return p.equipped && p.durability && (hooked(kMelee) || hooked(kWeaponHit));
    case cheats::kSaveWithoutInk: return p.typewriter && hooked(kTypewriterCheck) && hooked(kTypewriterRibbon);
    case cheats::kNoSaveCount: return p.header && hooked(kSaveData);
    case cheats::kFreezePlaytime: return p.clock && hooked(kClockSave);
    // Switchable while its class is still to come up: the hook takes it from there.
    case cheats::kFreezeCountdown: return p.countdown && (hooked(kCountdownUpdate) || !g_hook[kCountdownUpdate].failed);
    case cheats::kInfiniteBoards: return p.boards && hooked(kUseItem);
    // The records' counters: the switch clears or takes the count at once (cheats.cpp), and a
    // class still to come up (an area with an item box, the player) is hooked as it does.
    case cheats::kNoItemBoxCount: return p.item_box_count && (hooked(kItemBoxOpen) || !g_hook[kItemBoxOpen].failed);
    case cheats::kNoHealCount: return p.heal_count && (hooked(kInventoryUpdate) || !g_hook[kInventoryUpdate].failed);
    case cheats::kFreezeSteps: return p.steps && (hooked(kFootstep) || !g_hook[kFootstep].failed);
    default: return false;
  }
}

bool records_screen_hooked(int set) {
  if (g_disabled) return false;
  return hooked(set == records::kRogue ? kRogueRecordsScreen : kRecordsScreen);
}

bool load_screen_hooked() { return !g_disabled && hooked(kLoadScreen); }

const char* why_not(cheats::Kind k) {
  if (g_disabled) return "events disabled in the ini";
  auto state = [](Hook h) { return g_hook[h].failed ? "not hookable (see the log)" : "waiting for the game"; };
  switch (k) {
    case cheats::kOneHitKills: return state(kEnemyDamage);
    case cheats::kGodMode: return state(kPlayerHp);
    case cheats::kInfiniteAmmo: return state(kFire);
    case cheats::kNoDurabilityLoss: return !game::parts().durability ? "knives not found" : state(kMelee);
    case cheats::kSaveWithoutInk: return !game::parts().typewriter ? "typewriters not understood" : state(kTypewriterCheck);
    case cheats::kNoSaveCount: return state(kSaveData);
    case cheats::kFreezePlaytime: return state(kClockSave);
    case cheats::kFreezeCountdown: return !game::parts().countdown ? "countdown not found" : state(kCountdownUpdate);
    case cheats::kInfiniteBoards: return !game::parts().boards ? "use-item triggers not found" : state(kUseItem);
    case cheats::kNoItemBoxCount: return !game::parts().item_box_count ? "item box count not found" : state(kItemBoxOpen);
    case cheats::kNoHealCount: return !game::parts().heal_count ? "recovery item count not found" : state(kInventoryUpdate);
    case cheats::kFreezeSteps: return !game::parts().steps ? "step count not found" : state(kFootstep);
    default: return "not supported yet";
  }
}

void uninstall() {
  // Delegates the game made from an exchanged entry keep the mod's code: only a
  // DLL that stays loaded (it is pinned) can be called through them.
  for (State& st : g_hook) {
    const uintptr_t orig = st.original.load();
    if (st.record && re::unhook_method(st.record, st.replacement, orig)) st.record = 0;
    for (int i = 0; i < st.slots; ++i) re::swap_slot(st.slot[i], st.replacement, orig);
    st.slots = 0;
  }
}

}  // namespace re2cc::events
