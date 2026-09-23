#include "cheats.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "call.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "savefiles.h"

namespace re2cc::cheats {
namespace {

volatile LONG g_on[kCount] = {};
CRITICAL_SECTION g_cs;  // g_published, the requests and g_last
Status g_published;
int g_req_saves = -1;
int g_req_playtime = -1;
int g_req_difficulty = -1;
int g_req_steps = -1;

// Save without counting: the count saves keep (events.cpp reads it at each save).
volatile LONG g_held_saves = -1;
// Freeze step count: the step count held (events.cpp puts it back after each footstep).
volatile LONG g_held_steps = -1;

// --- Freeze play time and the play-time editor ------------------------------------------------------------
// The time the game *records* is held here, never in the game's clock. The clock
// always runs: the enemies are on it (game.h, enemy_time), and a clock stopped or
// set backwards leaves every EnemyManager.FrameTimer running for good - think-off,
// no-attack, attack-through - and canRestrictMove holding enemies still, which
// stops them moving and stalls scripted set pieces until the game is restarted.
// (That is what the first build of this cheat did, with the editor set to 0:00:00;
// seen in game 2026-09-17 and 2026-09-20.)
//   The held time is put into the clock only where the game reads it for the
// record - the save that captures GameClock's save data (events.cpp) and the
// clear (the RESULT states) - and taken out again straight after.
volatile LONG g_hold_play = 0;           // a time is held (the freeze, or the editor set one)
std::atomic<uint64_t> g_held_play{0};    // what the game should record, microseconds
std::atomic<int64_t> g_play_taken{0};    // what a hold took out of the clock, to give back (may be negative)
std::atomic<int64_t> g_play_inv_taken{0};  // and out of the inventory's time, so the enemies' clock stood still
volatile LONG g_play_taken_at = 0;       // when it was taken (GetTickCount): it is never left in
bool g_play_for_clear = false;           // the tick is holding it for the clear (the tick's alone)
// A hook owns the hold for the length of one of the game's calls (the extra modes'
// record, events.cpp - any thread). While it is set nothing else gives the hold
// back, so the tick's cap cannot put the clock right in the middle of the call the
// hold was taken for; the hook clears it as it gives the hold back itself.
std::atomic<bool> g_play_for_hook{false};
uint64_t g_play_last = 0;                // the record time the tick last saw (to advance the held value)
bool g_play_last_ok = false;
// A save is captured in one call, but its bytes are written later: the time goes
// back when the manager is idle again, and at the latest after this.
constexpr DWORD kPlayHoldCapMs = 5000;

// What the game's clock really reads for the record, with whatever a hold took
// out of it added back (so the figure is continuous while a hold is in).
bool real_record_time(uint64_t* us) {
  game::Clock c;
  if (!game::read_clock(&c)) return false;
  const int64_t real = static_cast<int64_t>(game::play_time(c)) + g_play_taken.load(std::memory_order_relaxed);
  *us = real > 0 ? static_cast<uint64_t>(real) : 0;
  return true;
}

// Putting the held time into the clock and taking it back out again. The save's
// hook (a game thread) and the tick can both be at it, so the clock is only ever
// moved under g_cs - and never logged to under it.
bool take_play_hold() {  // g_cs held
  if (!g_hold_play || g_play_taken.load(std::memory_order_relaxed) != 0) return false;
  const game::Held took = game::hold_record_time(g_held_play.load(std::memory_order_relaxed));
  if (!took.any()) return false;
  InterlockedExchange(&g_play_taken_at, static_cast<LONG>(GetTickCount()));
  g_play_inv_taken.store(took.inventory, std::memory_order_relaxed);
  g_play_taken.store(took.elapsed, std::memory_order_relaxed);
  return true;
}

int64_t give_play_hold_back() {  // g_cs held; what went back
  if (g_play_for_hook.load(std::memory_order_relaxed)) return 0;  // a hook's, for the length of its call
  game::Held back;
  back.elapsed = g_play_taken.exchange(0, std::memory_order_relaxed);
  back.inventory = g_play_inv_taken.exchange(0, std::memory_order_relaxed);
  return back.any() && game::release_record_time(back) ? back.elapsed : 0;
}

bool locked_take_play_hold() {
  EnterCriticalSection(&g_cs);
  const bool took = take_play_hold();
  LeaveCriticalSection(&g_cs);
  return took;
}

int64_t locked_give_play_hold_back() {
  EnterCriticalSection(&g_cs);
  const int64_t back = give_play_hold_back();
  LeaveCriticalSection(&g_cs);
  return back;
}

// What the panel was last given, and when (the tick's alone).
DWORD g_refreshed = 0;
char g_last[160] = {};

void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char* fmt, ...) {
  char text[160];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(text, sizeof(text), fmt, a);
  va_end(a);
  EnterCriticalSection(&g_cs);
  std::snprintf(g_last, sizeof(g_last), "%s", text);
  LeaveCriticalSection(&g_cs);
  logf("%s", text);
}

void clear_note() {
  EnterCriticalSection(&g_cs);
  g_last[0] = 0;
  LeaveCriticalSection(&g_cs);
}

// A switch just turned on (or a game started with it on). The events do the
// cheats' work as it happens (events.cpp); this puts right, once, what happened
// before - a hurt player, a worn knife, the item box's and recovery items' counts
// (0 while their switches are on, whatever the save loaded had) - and takes the
// save count and the step count to hold.
void switched_on(Kind k) {
  switch (k) {
    case kGodMode: {
      game::Player pl[4];
      const int n = game::players(pl, 4);
      for (int i = 0; i < n; ++i) {
        game::heal_player(pl[i].cond);
        game::cure_poison(pl[i].cond);
      }
      break;
    }
    case kNoDurabilityLoss: {
      static game::Bag bag;
      if (game::parts().durability && game::read_bag(&bag))
        for (int i = 0; i < bag.slots && i < game::kMaxSlots; ++i)
          if (game::keep_knife_full(bag.slot[i])) logf("no durability loss: the knife in slot %d is full again", i + 1);
      break;
    }
    case kNoSaveCount: {
      const int held = game::header_save_times() >= 0 ? game::header_save_times() : game::save_count();
      InterlockedExchange(&g_held_saves, held);
      logf("save without counting: saves keep the count at %d", held);
      break;
    }
    case kNoItemBoxCount:
    case kNoHealCount: {
      const game::Counter c = k == kNoItemBoxCount ? game::Counter::kItemBox : game::Counter::kHeals;
      const int was = game::counter(c);
      if (was > 0 && game::set_counter(c, 0)) logf("%s: the count goes from %d to 0", name(k), was);
      break;
    }
    case kFreezeSteps: {
      const int held = game::counter(game::Counter::kSteps);
      InterlockedExchange(&g_held_steps, held);
      if (held >= 0) logf("freeze step count: the step count stays at %d", held);
      break;
    }
    case kFreezePlaytime: {
      // The time to record from here on. One the editor set is kept; the game's
      // clock is not touched either way.
      uint64_t real = 0;
      if (!g_hold_play && real_record_time(&real)) {
        g_held_play.store(real, std::memory_order_relaxed);
        InterlockedExchange(&g_hold_play, 1);
      }
      if (g_hold_play) {
        const uint64_t held = g_held_play.load(std::memory_order_relaxed) / 1000000ull;
        logf("freeze play time: saves and the clear record %llu:%02llu:%02llu from now on (the game's clock keeps "
             "running - the enemies' timers are on it)",
             static_cast<unsigned long long>(held / 3600), static_cast<unsigned long long>((held / 60) % 60),
             static_cast<unsigned long long>(held % 60));
      }
      break;
    }
    default:
      break;
  }
}

// The difficulty switch: the game's own MainFlowManager.setDifficulty, the
// call a new game and every load make - the header's difficulty and the global
// flags set from it, nothing reloaded. The pause menu is up (the tick's caller
// sees to that).
void switch_difficulty(game::Difficulty want) {
  const game::Difficulty was = game::difficulty();
  const uintptr_t owner = game::difficulty_owner();
  if (was == game::Difficulty::kUnknown || !owner) return note("the difficulty cannot be switched right now");
  if (was == want) return;
  if (!call::difficulty_ready()) return note("the difficulty cannot be switched: %s", call::difficulty_status());
  // After the game over screen's continue on Assisted, setDifficulty writes
  // Assisted whatever it is asked, at every load until the title: the switch
  // ends that hold (and puts it back if the game does not take the switch).
  const int held = want != game::Difficulty::kAssisted ? game::force_easy_continue() : 0;
  if (held > 0 && !game::set_force_easy_continue(0))
    return note("the game's continue on Assisted could not be ended - the difficulty stays %s",
                game::difficulty_name(was));
  const bool called = call::set_difficulty(owner, game::difficulty_value(want));
  const game::Difficulty now = game::difficulty();
  if (!called || now != want) {
    if (held > 0) game::set_force_easy_continue(held);
    if (!called) return note("the game did not switch the difficulty (%s) - see the log", call::difficulty_status());
    return note("the game kept the difficulty at %s", game::difficulty_name(now));
  }
  if (held > 0) logf("difficulty: the continue on Assisted the game held (ForceEasyContinue %d) is over", held);
  clear_note();  // the panel's buttons show it; the log only
  logf("difficulty: %s -> %s", game::difficulty_name(was), game::difficulty_name(now));
}

// What the panel shows, read when it opens and while it is up - never while
// the game is being played.
void snapshot(Status& s) {
  const game::Parts& parts = game::parts();
  game::Player pl[4];
  const int np = game::players(pl, 4);
  s.players = np;
  if (np > 0) {
    s.hp = pl[0].hp;
    s.max_hp = pl[0].max_hp;
  }
  game::Enemy em[96];
  const int ne = game::enemies(em, 96);
  s.enemies = 0;
  for (int i = 0; i < ne; ++i) s.enemies += em[i].hp > 0;
  static game::Bag bag;
  static game::Box box;
  s.ink = s.boards = -1;
  if (parts.bag && game::read_bag(&bag)) {
    const bool box_read = parts.box && game::read_box(&box);
    auto carried_and_boxed = [&](int id) {
      int total = 0;
      for (int i = 0; i < bag.slots && i < game::kMaxSlots; ++i)
        if (bag.slot[i].item.item_id == id && !game::is_weapon(bag.slot[i].item)) total += bag.slot[i].item.count;
      for (int i = 0; box_read && i < box.count; ++i)
        if (box.entry[i].item.item_id == id && !game::is_weapon(box.entry[i].item)) total += box.entry[i].item.count;
      return total;
    };
    s.ink = carried_and_boxed(game::ink_ribbon_id());
    s.boards = carried_and_boxed(game::wooden_boards_id());
  }
  s.difficulty = static_cast<int>(game::difficulty());
  s.header_saves = game::header_save_times();
  s.saves = s.header_saves >= 0 ? s.header_saves : game::save_count();
  uint64_t real = 0;
  s.play_held = g_hold_play != 0;
  // What the game would record: the held time while one is held, the clock's own
  // otherwise (the clock itself always runs - the panel never shows it stopped).
  s.play_seconds = real_record_time(&real)
                       ? static_cast<double>(s.play_held ? g_held_play.load(std::memory_order_relaxed) : real) / 1e6
                       : -1.0;
  s.item_box_opens = game::counter(game::Counter::kItemBox);
  s.heals_used = game::counter(game::Counter::kHeals);
  s.steps = game::counter(game::Counter::kSteps);
  s.steps_limit = game::steps_limit();
}

}  // namespace

void init() {
  InitializeCriticalSection(&g_cs);
  const config::Settings& c = config::get();
  g_on[kGodMode] = c.god_mode;
  g_on[kOneHitKills] = c.one_hit_kills;
  g_on[kInfiniteAmmo] = c.infinite_ammo;
  g_on[kSaveWithoutInk] = c.save_without_ink;
  g_on[kNoSaveCount] = c.no_save_count;
  g_on[kFreezePlaytime] = c.freeze_playtime;
  g_on[kFreezeCountdown] = c.freeze_countdown;
  g_on[kNoDurabilityLoss] = c.no_durability_loss;
  g_on[kInfiniteBoards] = c.infinite_boards;
  g_on[kNoItemBoxCount] = c.no_item_box_count;
  g_on[kNoHealCount] = c.no_recovery_item_count;
  g_on[kFreezeSteps] = c.freeze_steps;
}

const char* name(Kind k) {
  static const char* const kNames[kCount] = {"God mode", "One hit kills", "Infinite ammo", "Save without ink ribbons",
                                             "Save without counting", "Freeze play time", "Freeze countdown timer",
                                             "No durability loss", "Infinite wooden boards", "Item box without counting",
                                             "Recovery items without counting", "Freeze step count"};
  return k >= 0 && k < kCount ? kNames[k] : "?";
}

bool enabled(Kind k) { return k >= 0 && k < kCount && g_on[k] != 0; }

void set_enabled(Kind k, bool on) {
  if (k >= 0 && k < kCount) InterlockedExchange(&g_on[k], on ? 1 : 0);
}

int held_save_count() { return g_held_saves; }
int held_steps() { return g_held_steps; }

void hold_playtime_for_save() {
  if (!enabled(kFreezePlaytime) && !g_hold_play) return;
  if (locked_take_play_hold()) {
    const uint64_t held = g_held_play.load(std::memory_order_relaxed) / 1000000ull;
    logf("freeze play time: this save records %llu:%02llu:%02llu, not the clock's time",
         static_cast<unsigned long long>(held / 3600), static_cast<unsigned long long>((held / 60) % 60),
         static_cast<unsigned long long>(held % 60));
  }
}

// A hook takes the hold for the length of one of the game's calls - where the game
// records a time (the extra modes, game.h) and where it draws one from the clock
// itself (The Ghost Survivors' HUD timer). It is the hook's while it is in -
// give_play_hold_back refuses anyone else - so the tick's cap cannot put the clock
// back in the middle of the call the hold was taken for. False when a hold is
// already in (a save's, the clear's): the clock reads the held time anyway, and
// whoever took it gives it back.
bool take_hook_hold() {
  if (!enabled(kFreezePlaytime) && !g_hold_play) return false;
  EnterCriticalSection(&g_cs);
  const bool took = take_play_hold();
  if (took) g_play_for_hook.store(true, std::memory_order_relaxed);
  LeaveCriticalSection(&g_cs);
  return took;
}

// The extra modes' clear time, written while the run is still IN_GAME (game.h).
// Once a run, so it says so in the log.
bool hold_playtime_for_extra_record() {
  if (!take_hook_hold()) return false;
  const uint64_t held = g_held_play.load(std::memory_order_relaxed) / 1000000ull;
  logf("freeze play time: this run records %llu:%02llu:%02llu, not the clock's time",
       static_cast<unsigned long long>(held / 3600), static_cast<unsigned long long>((held / 60) % 60),
       static_cast<unsigned long long>(held % 60));
  return true;
}

// The Ghost Survivors' run timer on the HUD, every frame it is drawn: silent.
bool hold_playtime_for_display() { return take_hook_hold(); }

void give_playtime_hold_back(const char* what) {
  EnterCriticalSection(&g_cs);
  g_play_for_hook.store(false, std::memory_order_relaxed);
  const int64_t back = give_play_hold_back();
  LeaveCriticalSection(&g_cs);
  if (what && back && config::get().trace)
    logf("freeze play time: the clock has its %.1f s back after the %s", static_cast<double>(back) / 1e6, what);
}

Status status() {
  EnterCriticalSection(&g_cs);
  Status s = g_published;
  LeaveCriticalSection(&g_cs);
  return s;
}

void request_saves(int saves) {
  EnterCriticalSection(&g_cs);
  g_req_saves = saves < 0 ? 0 : saves;
  LeaveCriticalSection(&g_cs);
}

void request_playtime_seconds(int seconds) {
  EnterCriticalSection(&g_cs);
  g_req_playtime = seconds < 0 ? 0 : seconds;
  LeaveCriticalSection(&g_cs);
}

void request_difficulty(int difficulty) {
  EnterCriticalSection(&g_cs);
  g_req_difficulty = difficulty;
  LeaveCriticalSection(&g_cs);
}

void request_steps(int steps) {
  EnterCriticalSection(&g_cs);
  g_req_steps = steps < 0 ? 0 : steps;
  LeaveCriticalSection(&g_cs);
}

void tick(int state, bool in_game, bool paused) {
  static bool s_in_game = false, s_paused = false;
  static bool s_on[kCount] = {};
  const bool started = in_game && !s_in_game;
  const bool opened = paused && !s_paused;
  s_in_game = in_game;
  s_paused = paused;
  bool refresh = opened || (started && config::get().always_show);

  // The panel's requests.
  EnterCriticalSection(&g_cs);
  const int req_saves = g_req_saves, req_play = g_req_playtime, req_difficulty = g_req_difficulty;
  const int req_steps = g_req_steps;
  g_req_saves = g_req_playtime = g_req_difficulty = g_req_steps = -1;
  LeaveCriticalSection(&g_cs);
  if (req_saves >= 0) {
    refresh = true;
    if (in_game && game::set_save_count(req_saves)) {
      if (enabled(kNoSaveCount)) InterlockedExchange(&g_held_saves, req_saves);
      note("save count set to %d", req_saves);
    } else {
      note("the save count could not be set (no game running?)");
    }
  }
  if (req_play >= 0) {
    refresh = true;
    // The game's clock is not moved: it is what the enemies' timers run on, and
    // one set backwards leaves them running for good. The time asked for is what
    // a save and the clear record, and it keeps counting from there.
    uint64_t real = 0;
    if (in_game && real_record_time(&real)) {
      g_held_play.store(static_cast<uint64_t>(req_play) * 1000000ull, std::memory_order_relaxed);
      InterlockedExchange(&g_hold_play, 1);
      clear_note();  // the panel's play time shows it; the log only
      logf("play time set to %d:%02d:%02d - what saves and the clear record (the game's clock is left running)",
           req_play / 3600, (req_play / 60) % 60, req_play % 60);
    } else {
      note("the play time could not be set (no game running?)");
    }
  }
  if (req_difficulty >= 0) {
    refresh = true;
    if (in_game && paused) switch_difficulty(static_cast<game::Difficulty>(req_difficulty));
    else note("the difficulty is only switched while the pause menu is up");
  }
  if (req_steps >= 0) {
    refresh = true;
    // Frozen, the count held changes first: a footstep in between then puts
    // back the new count, not the old one.
    const LONG held = g_held_steps;
    const bool frozen = enabled(kFreezeSteps);
    if (frozen) InterlockedExchange(&g_held_steps, req_steps);
    if (in_game && game::set_counter(game::Counter::kSteps, req_steps)) {
      clear_note();  // the panel's step count shows it; the log only
      logf("step count set to %d", req_steps);
    } else {
      if (frozen) InterlockedExchange(&g_held_steps, held);
      note("the step count could not be set (no game running?)");
    }
  }

  // --- Freeze play time: the held time, and where it goes into the clock ---------------------
  // A game starting (a load, a new game) brings the save's own clock, so the held
  // time is taken afresh - the switch loop below takes it again when the freeze is
  // on. A hold still in from the game that ended is dropped, not given back: the
  // clock it was taken from is gone. (Not reset when the game is merely not being
  // played: the ending, the staff roll and the results screens are where the clear
  // is recorded, and the hold has to still be there.)
  if (started) {
    g_play_taken.store(0, std::memory_order_relaxed);
    g_play_inv_taken.store(0, std::memory_order_relaxed);
    g_play_for_clear = false;
    g_play_for_hook.store(false, std::memory_order_relaxed);
    InterlockedExchange(&g_hold_play, 0);
    g_held_play.store(0, std::memory_order_relaxed);
    g_play_last_ok = false;
  }
  // A clock the enemies cannot run on - the inventory's time above the elapsed
  // time, which the game itself can never write - is put right here, once a second
  // and never under a hold. It is what a save made by a build before 2026-09-21
  // carries: the loaded game would have every enemy standing still (Mr. X in the
  // sewers, 2026-09-21) until it had been played for as long as the inventory had
  // ever been open. The recorded play time is left as it is.
  static DWORD s_checked = 0;
  const DWORD now_ms = GetTickCount();
  if (in_game && !g_play_taken.load(std::memory_order_relaxed) && now_ms - s_checked >= 1000) {
    s_checked = now_ms;
    if (const uint64_t behind = game::repair_enemy_clock())
      logf("the enemies' clock was %.1f s behind and could not move (a save an older build wrote): "
           "put right, the play time left as it is",
           static_cast<double>(behind) / 1e6);
  }
  {
    uint64_t real = 0;
    const bool read = real_record_time(&real);
    // While nothing is frozen the held time follows the clock, so a play time the
    // editor set keeps counting from what it was set to; frozen, it stands still.
    if (read && g_hold_play && g_play_last_ok && !enabled(kFreezePlaytime) && real > g_play_last)
      g_held_play.fetch_add(real - g_play_last, std::memory_order_relaxed);
    g_play_last = real;
    g_play_last_ok = read;
  }
  // The clear: ResultFlow.update reads the clock for the time the results screen
  // shows and ranks, and for the clear-time record. The held time goes in for the
  // whole of those states.
  static bool s_clear_logged = false;
  const bool clearing = g_hold_play && game::result_state_is(state);
  if (clearing) {
    if (!g_play_for_clear) {
      g_play_for_clear = true;
      if (locked_take_play_hold() && !s_clear_logged) {
        s_clear_logged = true;
        logf("freeze play time: the clear records the time held, not the clock's");
      }
    }
    // Should the clear have beaten this by a frame, the time it shows is put right.
    const uint64_t held = g_held_play.load(std::memory_order_relaxed);
    const uint64_t shown = game::clear_time();
    if (shown > held && game::set_clear_time(held))
      logf("freeze play time: the clear time shown goes back to the time held");
  } else if (g_play_for_clear) {
    g_play_for_clear = false;
    s_clear_logged = false;
    locked_give_play_hold_back();
  }
  // What a save's hook took out of the clock goes back once the game's saves are
  // idle again - the save captures the clock's save data by reference, so it must
  // stay in until the file is written - and at the latest after kPlayHoldCapMs, so
  // the clock is never left behind.
  if (!g_play_for_clear && g_play_taken.load(std::memory_order_relaxed)) {
    const DWORD since = GetTickCount() - static_cast<DWORD>(g_play_taken_at);
    if (savefiles::game_saves_busy() != 1 || since > kPlayHoldCapMs) {
      const int64_t back = locked_give_play_hold_back();
      if (back && config::get().trace)
        logf("freeze play time: the clock has its %.1f s back after the save", static_cast<double>(back) / 1e6);
    }
  }

  // Switches flipped since the last tick, and a game starting with them on.
  for (int k = 0; k < kCount; ++k) {
    const bool on = enabled(static_cast<Kind>(k));
    if (on != s_on[k]) refresh = true;
    if (in_game && on && (!s_on[k] || started)) switched_on(static_cast<Kind>(k));
    if (!on && s_on[k] && k == kNoSaveCount) InterlockedExchange(&g_held_saves, -1);
    if (!on && s_on[k] && k == kFreezeSteps) InterlockedExchange(&g_held_steps, -1);
    s_on[k] = on;
  }
  // Taken afresh when a game starts: a new game's or the loaded save's.
  if (!in_game && enabled(kNoSaveCount)) InterlockedExchange(&g_held_saves, -1);
  if (!in_game && enabled(kFreezeSteps)) InterlockedExchange(&g_held_steps, -1);
  // A step count that could not be read when the switch went on: taken when the pause menu opens.
  if (in_game && opened && enabled(kFreezeSteps) && g_held_steps < 0) switched_on(kFreezeSteps);
  // A play time that could not be read when the freeze went on (no clock yet): tried again.
  if (in_game && enabled(kFreezePlaytime) && !g_hold_play) switched_on(kFreezePlaytime);

  // The panel's figures: while it is up, twice a second.
  const DWORD now = GetTickCount();
  if (in_game && (paused || config::get().always_show) && now - g_refreshed >= 500) refresh = true;
  if (!refresh && in_game == g_published.in_game) return;
  Status s;
  EnterCriticalSection(&g_cs);
  s = g_published;
  LeaveCriticalSection(&g_cs);
  s.in_game = in_game;
  const game::Parts& parts = game::parts();
  s.players_ok = parts.players;
  s.enemies_ok = parts.enemies;
  s.ammo_ok = parts.equipped;
  s.durability_ok = parts.equipped && parts.durability;
  s.ink_ok = parts.bag;
  s.typewriter_ok = parts.typewriter;
  s.records_ok = parts.header || parts.records;
  s.clock_ok = parts.clock;
  s.countdown_ok = parts.countdown;
  s.item_box_count_ok = parts.item_box_count;
  s.heal_count_ok = parts.heal_count;
  s.steps_ok = parts.steps;
  s.difficulty_ok = parts.difficulty && call::difficulty_ready();
  std::snprintf(s.difficulty_why, sizeof(s.difficulty_why), "%s",
                !parts.difficulty ? "the game's difficulty was not found - see the log"
                : s.difficulty_ok ? ""
                                  : call::difficulty_status());
  if (in_game && refresh) {
    snapshot(s);
    g_refreshed = now;
  }
  EnterCriticalSection(&g_cs);
  std::snprintf(s.last_action, sizeof(s.last_action), "%s", g_last);
  g_published = s;
  LeaveCriticalSection(&g_cs);
}

void remove_hooks() {
  for (auto& v : g_on) InterlockedExchange(&v, 0);
}

}  // namespace re2cc::cheats
