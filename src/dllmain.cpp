// RE2CabbyCodes - proxy entry point.
//
// We ship as steam_api64.dll (see proxy.cpp for why). re2.exe delay-loads it,
// so DllMain runs on the game's first Steam call, early in WinMain and long
// after SteamStub has unpacked the image. DllMain only loads the original,
// re-points a few of the exe's import slots (the key-state and ClipCursor
// imports for the panel, the crash filter) and starts a thread; that thread
// hooks the swap chain, waits for the game's managed runtime to come up, finds
// everything by name through its type database, and hooks the game's frame
// (via.Application's entries) for the tick.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "app.h"
#include "call.h"
#include "cheats.h"
#include "config.h"
#include "crash.h"
#include "dispatch.h"
#include "events.h"
#include "game.h"
#include "input.h"
#include "inventory.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"
#include "proxy.h"
#include "re.h"
#include "records.h"
#include "savefiles.h"
#include "version.h"

namespace {

HMODULE g_self = nullptr;
char g_dir[MAX_PATH] = {};
uintptr_t g_filter_slot = 0;

// Directory this DLL was loaded from, with a trailing separator.
void resolve_own_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(g_self, path, MAX_PATH);
  char* slash = std::strrchr(path, '\\');
  if (!slash) slash = std::strrchr(path, '/');
  if (slash) {
    const size_t len = static_cast<size_t>(slash - path) + 1;
    if (len < sizeof(g_dir)) {
      std::memcpy(g_dir, path, len);
      g_dir[len] = '\0';
    }
  }
}

// The game's window when the overlay is off (the swap chain names it otherwise):
// the largest visible top-level window of this process.
struct Biggest {
  HWND hwnd = nullptr;
  long area = 0;
};

BOOL CALLBACK pick_window(HWND hwnd, LPARAM lp) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
  RECT rc{};
  GetClientRect(hwnd, &rc);
  auto* b = reinterpret_cast<Biggest*>(lp);
  const long area = (rc.right - rc.left) * (rc.bottom - rc.top);
  if (area > b->area) {
    b->area = area;
    b->hwnd = hwnd;
  }
  return TRUE;
}

DWORD WINAPI mod_thread(LPVOID) {
  using namespace re2cc;
  logf("mod thread started (thread %lu)", GetCurrentThreadId());
  if (!config::get().disable_overlay) {
    if (overlay::install()) logf("overlay: waiting for the game's first frame");
    else logf("ERROR: overlay: not installed - no panel (the cheats still run from their ini switches)");
  }
  const bool game_on = !config::get().disable_game;
  const bool tick_on = game_on && !config::get().disable_dispatch;
  if (config::get().disable_game) logf("game hooks disabled by config");
  else if (config::get().disable_dispatch) logf("the game tick is disabled by config");
  // One loop for everything that comes up while the game starts: the panel's
  // own work every 100 ms, the type database until the VM has loaded it (before
  // the title screen), the game's classes once it has, and the frame hook once
  // via.Application has registered and sorted its entries.
  enum class Re { Waiting, Ready, GaveUp } re_state = Re::Waiting;
  bool app_searched = false, app_hooked = false;
  char last[200] = "";
  const DWORD start = GetTickCount();
  DWORD last_second = start;
  for (int n = 0;; ++n) {
    overlay::service();
    if (!dispatch::game_window() && (config::get().disable_overlay || n > 200) && n % 10 == 0) {
      Biggest b;
      EnumWindows(pick_window, reinterpret_cast<LPARAM>(&b));
      if (b.hwnd) dispatch::set_game_window(b.hwnd);
    }
    if (game_on && re_state == Re::Waiting && n % 3 == 0) {
      if (re::init()) {
        re_state = Re::Ready;
        game::discover();
        records::discover();
        savefiles::discover();
        // Before the game makes a single delegate of the methods it hooks: the
        // title screen is still seconds away.
        events::install();
        call::init();  // logs what it found, or why the item box cannot move weapons
        if (config::get().dump_methods) {
          char path[MAX_PATH];
          std::snprintf(path, sizeof(path), "%sRE2CabbyCodes.methods.bin", g_dir);
          re::dump_methods(path);
        }
      } else if (std::strcmp(re::status(), last) != 0) {
        std::snprintf(last, sizeof(last), "%s", re::status());
        logf("re: %s", last);
      } else if (GetTickCount() - start > 10u * 60u * 1000u) {
        logf("ERROR: re: gave up after 10 minutes: %s", re::status());
        re_state = Re::GaveUp;
      }
    }
    if (tick_on && !app_searched) {
      app_searched = true;
      app::find();  // logs what it found, or why there is no game tick
    }
    if (tick_on && !app_hooked && n % 5 == 0) app_hooked = app::hook();
    const DWORD now = GetTickCount();
    if (now - last_second >= 1000) {
      last_second = now;
      if (app_hooked) {
        app::check();
        app::log_rates();
      }
      if (re_state == Re::Ready) events::service();  // vtable hooks as their classes come up
    }
    Sleep(100);
  }
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  using namespace re2cc;
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      g_self = module;
      DisableThreadLibraryCalls(module);
      // The mod thread runs for the life of the process: the DLL must never be
      // unmapped under it, whatever calls FreeLibrary.
      {
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCSTR>(&mod_thread), &pinned);
      }
      resolve_own_dir();
      log_init(g_dir);
      logf("RE2CabbyCodes " RE2CC_VERSION " loaded (steam_api64.dll proxy), dir=%s pid=%lu thread=%lu", g_dir,
           GetCurrentProcessId(), GetCurrentThreadId());

      if (!proxy::load_original(g_dir)) {
        MessageBoxA(nullptr,
                    "RE2CabbyCodes: steam_api64_orig.dll is missing or broken.\n\n"
                    "The mod ships as steam_api64.dll and needs the game's original steam_api64.dll beside it, "
                    "renamed to steam_api64_orig.dll. See INSTALL.txt / README.md, or verify the game files "
                    "in Steam and reinstall the mod.",
                    "RE2CabbyCodes", MB_OK | MB_ICONERROR);
        return FALSE;
      }

      // Before the first guarded read (the import slots below are read through it).
      if (!mem::guard_install())
        logf("ERROR: memory: the fault handler could not be installed - guarded reads will all fail");
      install_crash_logger();
      if (!mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "SetUnhandledExceptionFilter", crash_filter_hook(),
                         &g_filter_slot))
        logf("crash: re2.exe's SetUnhandledExceptionFilter import not found - the filter is re-asserted instead");
      config::load(g_dir);
      cheats::init();
      inventory::init();
      records::init(g_dir);
      savefiles::init();
      overlay::install_early();
      dispatch::init();
      input::install();

      if (HANDLE t = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr)) CloseHandle(t);
      break;
    }
    case DLL_PROCESS_DETACH:
      // The DLL is pinned, so this only ever runs as the process exits: every
      // other thread is gone and the graphics runtime may already be torn down,
      // so nothing is released - touching a dead device would be the crash.
      if (reserved) {
        logf("the process is exiting");
        log_shutdown();
        break;
      }
      // (An unload that is not the process exiting: undo every patch before this
      // image goes away.)
      logf("unloading - removing hooks");
      re2cc::app::unhook();
      events::uninstall();
      input::uninstall();
      overlay::uninstall();
      cheats::remove_hooks();
      mem::iat_restore(g_filter_slot, crash_filter_hook(), crash_real_set_filter());
      logf("hooks removed cleanly");
      log_shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
