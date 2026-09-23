#include "dispatch.h"

#include "cheats.h"
#include "crash.h"
#include "game.h"
#include "inventory.h"
#include "log.h"
#include "re.h"
#include "records.h"
#include "savefiles.h"

namespace re2cc::dispatch {
namespace {

volatile HWND g_window = nullptr;
Snapshot g_snap;           // the tick's (under g_tick_cs)
DWORD g_snap_time = 0;
Snapshot g_published;      // what the panel reads (under g_snap_cs)
DWORD g_published_time = 0;
CRITICAL_SECTION g_tick_cs;
CRITICAL_SECTION g_snap_cs;
volatile LONG g_updated = 0;  // UpdateBehavior ticked since the last EndRendering
thread_local bool t_in_tick = false;
DWORD g_last_second = 0;
bool g_in_game_prev = false;
bool g_paused_prev = false;
unsigned long g_last_thread = 0;
int g_thread_logs = 0;

void tick() {
  const DWORD now = GetTickCount();
  const unsigned long me = GetCurrentThreadId();
  if (!g_snap.ticks) logf("dispatch: the game's frame ticks the mod (thread %lu)", me);
  if (me != g_last_thread) {
    if (g_last_thread && g_thread_logs < 5) {
      ++g_thread_logs;
      logf("dispatch: the tick moved from thread %lu to %lu%s", g_last_thread, me,
           g_thread_logs == 5 ? " (no more of these)" : "");
    }
    g_last_thread = me;
  }
  if (now - g_last_second >= 1000) {
    g_last_second = now;
    reassert_crash_filter();
  }

  Snapshot s;
  s.ticking = true;
  s.thread = me;
  s.ticks = g_snap.ticks + 1;
  s.game_ready = re::ready() && game::ready();
  if (s.game_ready) {
    // The one read every frame: where the game is. Everything else happens on
    // the game's events (events.h) or while the pause menu is up.
    s.state = game::main_state();
    s.in_game = game::in_game_state(s.state);
    s.paused = game::pause_state_is(s.state);
    s.title = game::title_state_is(s.state);
    game::trace_tick(s.state);
    if (s.in_game != g_in_game_prev) {
      logf("game session %s", s.in_game ? "started" : "ended");
      // The scenes of the session that ended are gone, and with them the objects
      // Infinite wooden boards remembered marking.
      if (s.in_game) game::forget_board_marks();
      g_in_game_prev = s.in_game;
    }
    if (s.paused != g_paused_prev) {
      logf("pause menu %s", s.paused ? "opened" : "closed");
      g_paused_prev = s.paused;
    }
    // The panel's requests, switches just flipped, and the panel's figures while it is up.
    cheats::tick(s.state, s.in_game, s.paused);
    inventory::tick(s.in_game, s.paused);
    // The records panel: its requests while a records screen is up, its holds, its save.
    records::tick(s.state);
    s.records_screen = records::screen_open();
    // The save files: their requests and the change under way while the Load Game screen is up.
    savefiles::tick();
    s.load_screen = savefiles::screen_open();
  }
  s.show_panel = s.paused || s.records_screen >= 0 || s.load_screen;
  g_snap = s;
  g_snap_time = now;
  EnterCriticalSection(&g_snap_cs);
  g_published = s;
  g_published_time = now;
  LeaveCriticalSection(&g_snap_cs);
}

}  // namespace

void init() {
  InitializeCriticalSection(&g_tick_cs);
  InitializeCriticalSection(&g_snap_cs);
}

void frame(int source) {
  // One tick a frame: UpdateBehavior's, or EndRendering's when no behaviour
  // update came since the last one.
  if (source == kEndRendering && InterlockedExchange(&g_updated, 0)) return;
  // A tick still running elsewhere keeps this one out rather than queueing it:
  // the entries are jobs, and a job must not wait on the mod.
  if (!TryEnterCriticalSection(&g_tick_cs)) return;
  if (t_in_tick) {  // re-entered from inside the tick (it never calls the game, but be sure)
    LeaveCriticalSection(&g_tick_cs);
    return;
  }
  if (source == kUpdateBehavior) InterlockedExchange(&g_updated, 1);
  t_in_tick = true;
  tick();
  t_in_tick = false;
  LeaveCriticalSection(&g_tick_cs);
}

void set_game_window(HWND hwnd) {
  if (!hwnd || hwnd == g_window) return;
  g_window = hwnd;
  char cls[64] = "?";
  GetClassNameA(hwnd, cls, sizeof(cls));
  logf("dispatch: the game window is %p (class '%s', thread %lu)", static_cast<void*>(hwnd), cls,
       GetWindowThreadProcessId(hwnd, nullptr));
}

HWND game_window() { return g_window; }

Snapshot snapshot() {
  EnterCriticalSection(&g_snap_cs);
  Snapshot s = g_published;
  const DWORD at = g_published_time;
  LeaveCriticalSection(&g_snap_cs);
  s.age_ms = s.ticks ? GetTickCount() - at : 0;
  return s;
}

bool in_tick() { return t_in_tick; }

}  // namespace re2cc::dispatch
