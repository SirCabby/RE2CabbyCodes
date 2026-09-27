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
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

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

// The DLLs in the game that are not Windows' own - REFramework, a DXGI
// wrapper, the Steam overlay, their plugins -, logged once the panel's way onto
// the screen is settled: what a report of a clash with another mod needs first
// (2026-09-26: REFramework and OptiScaler, known from their logs and a size).
void log_other_modules() {
  using namespace re2cc;
  wchar_t windir[MAX_PATH] = L"", gamedir[MAX_PATH] = L"";
  const UINT wn = GetWindowsDirectoryW(windir, MAX_PATH);
  GetModuleFileNameW(nullptr, gamedir, MAX_PATH);
  if (wchar_t* slash = std::wcsrchr(gamedir, L'\\')) slash[1] = L'\0';
  const size_t gn = std::wcslen(gamedir);
  HANDLE snap = INVALID_HANDLE_VALUE;
  for (int tries = 0; tries < 5 && snap == INVALID_HANDLE_VALUE; ++tries) {
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;  // (a DLL loading meanwhile: again)
  }
  if (snap == INVALID_HANDLE_VALUE) {
    logf("modules: the list could not be read (error %lu)", GetLastError());
    return;
  }
  std::string list;
  int count = 0;
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    if (me.hModule == GetModuleHandleW(nullptr) || me.hModule == g_self) continue;
    if (wn && _wcsnicmp(me.szExePath, windir, wn) == 0) continue;  // Windows' own (System32, WinSxS, drivers)
    const wchar_t* shown = me.szExePath;
    if (gn && _wcsnicmp(shown, gamedir, gn) == 0) shown += gn;  // the game's folder, relative
    char name[MAX_PATH * 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, shown, -1, name, sizeof(name), nullptr, nullptr)) continue;
    char item[sizeof(name) + 32];
    std::snprintf(item, sizeof(item), "%s%s (%.1f MB)", count ? ", " : "", name, me.modBaseSize / 1048576.0);
    list += item;
    ++count;
  }
  CloseHandle(snap);
  logf("modules: %d in the game besides its own and Windows': %s", count, count ? list.c_str() : "none");
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

// The engine's own window, once it is up: class 'via', visible, this process.
BOOL CALLBACK find_via_window(HWND hwnd, LPARAM lp) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  char cls[16] = {};
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || !GetClassNameA(hwnd, cls, sizeof(cls)) ||
      std::strcmp(cls, "via") != 0)
    return TRUE;
  *reinterpret_cast<HWND*>(lp) = hwnd;
  return FALSE;
}

DWORD WINAPI mod_thread(LPVOID) {
  using namespace re2cc;
  logf("mod thread started (thread %lu)", GetCurrentThreadId());
  // The game's swap chains are adopted as its factory makes them (overlay_d3d.cpp,
  // hooks in since DllMain); the mod thread only watches that one of them
  // presents the game's window. Should none within 10 s of the window, or the
  // import hooks not be in, the first design goes in instead - the class's
  // vtable through a throwaway swap chain, once the window has been up 2 s
  // (made while the game was making its own devices, the throwaway crashed
  // inside NVIDIA's D3D11 driver on Windows, 2026-09-25).
  bool overlay_pending = !config::get().disable_overlay;
  bool window_up = false;
  DWORD window_since = 0;
  const bool game_on = !config::get().disable_game;
  const bool tick_on = game_on && !config::get().disable_dispatch;
  if (config::get().disable_game) logf("game hooks disabled by config");
  else if (config::get().disable_dispatch) logf("the game tick is disabled by config");
  // One loop for everything that comes up while the game starts: the panel's
  // own work every 100 ms, the type database until the VM has loaded it (before
  // the title screen), the game's classes once it has, and the frame hook once
  // via.Application has registered and sorted its entries.
  enum class Re { Waiting, Ready, GaveUp } re_state = Re::Waiting;
  bool app_searched = false, app_hooked = false, modules_logged = false;
  char last[320] = "";
  const DWORD start = GetTickCount();
  DWORD last_second = start;
  for (int n = 0;; ++n) {
    if (!modules_logged && (!overlay_pending || n >= 600)) {
      modules_logged = true;
      log_other_modules();
    }
    if (overlay_pending && n % 5 == 0) {
      HWND via = nullptr;
      EnumWindows(find_via_window, reinterpret_cast<LPARAM>(&via));
      // (A tick marked with `| 1` was the first build's flag - a tick ahead of the
      // clock half the time, so the wait "expired" at once: 2026-09-25.)
      if (!via) {
        window_up = false;
      } else if (!window_up) {
        window_up = true;
        window_since = GetTickCount();
      }
      const DWORD up = window_up ? GetTickCount() - window_since : 0;
      if (overlay::d3d::adopting() && overlay::d3d::presenting()) {
        overlay_pending = false;
      } else if (window_up && up >= (overlay::d3d::adopting() ? 10000u : 2000u)) {
        overlay_pending = false;
        if (overlay::d3d::adopting())
          logf("overlay: no swap chain of the game's presented its window in 10 s - hooking the swap chain class's vtable instead");
        if (overlay::install()) logf("overlay: waiting for the game's next frame");
        else logf("ERROR: overlay: not installed - no panel (the cheats still run from their ini switches)");
      }
    }
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
      } else if (re::unsupported()) {
        logf("ERROR: re: %s - the mod leaves the game alone", re::status());
        re_state = Re::GaveUp;
      } else if (std::strcmp(re::status(), last) != 0) {
        std::snprintf(last, sizeof(last), "%s", re::status());
        logf("re: %s", last);
      } else if (GetTickCount() - start > 10u * 60u * 1000u) {
        logf("ERROR: re: gave up after 10 minutes: %s", re::status());
        re_state = Re::GaveUp;
      }
    }
    // A version of the game the mod cannot read (the first re::init says so,
    // before this runs) gets no frame hook either: the panel only says why.
    const bool tick_here = tick_on && !re::unsupported();
    if (tick_here && !app_searched) {
      app_searched = true;
      app::find();  // logs what it found, or why there is no game tick
    }
    if (tick_here && !app_hooked && n % 5 == 0) app_hooked = app::hook();
    const DWORD now = GetTickCount();
    if (now - last_second >= 1000) {
      last_second = now;
      if (app_hooked) {
        app::check();
        app::log_rates();
      }
      if (re_state == Re::Ready) events::service();  // vtable hooks as their classes come up
    } else if (re_state == Re::Ready && events::pending()) {
      events::service();  // some still waiting: every 100 ms (events.h)
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

      // Before the first guarded read (the proxy reads re2.exe's import tables
      // through it, and the import slots below are read through it).
      if (!mem::guard_install())
        logf("ERROR: memory: the fault handler could not be installed - guarded reads will all fail");
      if (!proxy::load_original(g_dir)) {
        MessageBoxA(nullptr, proxy::failure(), "RE2CabbyCodes", MB_OK | MB_ICONERROR);
        return FALSE;
      }

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
