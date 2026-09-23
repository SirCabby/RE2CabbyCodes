#pragma once

#include <cstdint>

// The cheats. Their switches are shared between the panel (drawn on the render
// thread), the game tick (dispatch.h) and the game's own threads, which call
// the mod at the moments the cheats act on - a hit, a shot, a save (events.h) -
// so the switches are plain atomics and edits travel through a small request
// queue. Nothing is read or written while the game is played unless one of
// those events happens: the tick only applies the panel's requests, puts right
// once what happened before a switch was turned on, and reads the figures the
// panel shows while it is up.
namespace re2cc::cheats {

enum Kind : int {
  kGodMode = 0,
  kOneHitKills,
  kInfiniteAmmo,
  kSaveWithoutInk,
  kNoSaveCount,
  kFreezePlaytime,
  kFreezeCountdown,
  kNoDurabilityLoss,
  kInfiniteBoards,
  kNoItemBoxCount,  // the records' counters (game::Counter): the item box opened
  kNoHealCount,     // recovery items used
  kFreezeSteps,     // the player's steps
  kCount
};

void init();  // from DllMain: the locks and the ini's start-up switches
const char* name(Kind k);
bool enabled(Kind k);
void set_enabled(Kind k, bool on);  // any thread; the events see it at once
int held_save_count();              // Save without counting: the count saves keep (-1: none yet)
int held_steps();                   // Freeze step count: the step count held (-1: none yet)

// Freeze play time (and the play-time editor). The time the game should record
// is held here, not in the game's clock: the clock is never stopped and never
// set backwards, because the enemies run on it - EnemyManager's think-off,
// no-attack and attack-through timers never end on a clock that does not move,
// and canRestrictMove holds enemies still, which stalls scripted set pieces
// (seen in game 2026-09-17 and 2026-09-20). The held time is put into the clock
// only where the game reads it for the record: this, from the save that
// captures GameClock's save data (events.cpp), and the tick for the clear. The
// tick takes it straight back out.
void hold_playtime_for_save();

// The same, for the extra modes' clear time. The main game's is read in the
// RESULT state, which the tick covers; The Ghost Survivors' and The 4th Survivor's
// / The Tofu Survivor's are written a state earlier, while the run is still
// IN_GAME, by the FSM action EndMeasureAndRecordGameElapsedTimeForExtra.start
// (game.h). Its hook takes the hold before the game's code and gives it back
// straight after: true when this call took it, and only then is it given back.
// While a hook holds it nothing else may - the tick's own give-back stands off.
bool hold_playtime_for_extra_record();
// And for a display that reads the clock itself: The Ghost Survivors' run timer on
// the HUD (RogueCountDownBehavior.lateUpdate -> updateCountUp). Silent - it runs
// every frame the timer is drawn - and otherwise the same: the timer the player
// watches then sits at the held time, while the clock underneath keeps running for
// the enemies. `what` on the give-back names the call in the Trace log, or is null.
bool hold_playtime_for_display();
void give_playtime_hold_back(const char* what = nullptr);

// What the panel shows (built by tick, handed over under a lock).
struct Status {
  bool in_game = false;
  bool players_ok = false;
  int players = 0;
  int hp = 0, max_hp = 0;       // the first player's
  bool enemies_ok = false;
  int enemies = 0;              // live enemies this tick
  bool ammo_ok = false;         // the inventory can be read
  bool durability_ok = false;   // the game's knives are known
  bool ink_ok = false;
  int ink = -1;                 // ink ribbons carried and in the box
  int boards = -1;              // wooden boards carried and in the box
  bool typewriter_ok = false;   // typewriters can be told to save without a ribbon (Hardcore)
  bool records_ok = false;
  int saves = -1;               // the save count: the game header's SaveTimes (RecordManager's when unknown)
  int header_saves = -1;        // the game header's SaveTimes, what a save records
  bool clock_ok = false;
  double play_seconds = -1.0;   // the clear time the game would record now
  bool play_held = false;       // it is held there (Freeze play time, or the editor set it)
  // The records' counters (game::Counter; -1: unknown).
  bool item_box_count_ok = false, heal_count_ok = false, steps_ok = false;
  int item_box_opens = -1;      // the times the item box was opened (its record needs 0)
  int heals_used = -1;          // recovery items used (their record needs 0)
  int steps = -1;               // the player's steps
  int steps_limit = -1;         // the most the step record allows
  bool countdown_ok = false;
  bool difficulty_ok = false;   // the difficulty can be switched (the game's setDifficulty, see game.h)
  int difficulty = -1;          // game::Difficulty, the game's now (-1: unknown)
  char difficulty_why[160] = {};  // why it cannot be switched, when it cannot
  char last_action[160] = {};
};
Status status();

// Requests from the panel (applied by the next game tick).
void request_saves(int saves);
void request_playtime_seconds(int seconds);
void request_steps(int steps);
void request_difficulty(int difficulty);  // a game::Difficulty; made only while the pause menu is up

void tick(int state, bool in_game, bool paused);  // the game tick (state: game::main_state)
void remove_hooks();                   // teardown: every switch off

}  // namespace re2cc::cheats
