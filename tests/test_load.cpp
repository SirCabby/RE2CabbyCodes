// Smoke test of the built proxy under Wine, outside the game:
//
//   cp build/steam_api64.dll <dir>/; cp "$GAME_DIR/steam_api64.dll" <dir>/steam_api64_orig.dll
//   wine tests/build/test_load.exe <dir>
//
// Loads the proxy the way re2.exe's delay-load helper does, calls through two
// of its thunks and reads its one forwarded data export, lets the mod thread
// run for a few seconds (it finds no game, which it must say rather than
// crash), then unloads it - DllMain's teardown has to leave the process whole.
#include <windows.h>

#include <cstdio>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_load.exe <dir with steam_api64.dll and steam_api64_orig.dll>\n");
    return 2;
  }
  SetCurrentDirectoryA(argv[1]);
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\steam_api64.dll", argv[1]);
  HMODULE m = LoadLibraryA(path);
  std::printf("LoadLibrary(%s) = %p (error %lu)\n", path, static_cast<void*>(m), m ? 0 : GetLastError());
  if (!m) return 1;
  using BoolFn = bool(__cdecl*)();
  using UserFn = int(__cdecl*)();
  auto running = reinterpret_cast<BoolFn>(GetProcAddress(m, "SteamAPI_IsSteamRunning"));
  auto user = reinterpret_cast<UserFn>(GetProcAddress(m, "SteamAPI_GetHSteamUser"));
  void* data = reinterpret_cast<void*>(GetProcAddress(m, "g_pSteamClientGameServer"));
  HMODULE orig = GetModuleHandleA("steam_api64_orig.dll");
  void* data_orig = orig ? reinterpret_cast<void*>(GetProcAddress(orig, "g_pSteamClientGameServer")) : nullptr;
  std::printf("thunks: SteamAPI_IsSteamRunning %p, SteamAPI_GetHSteamUser %p\n", reinterpret_cast<void*>(running),
              reinterpret_cast<void*>(user));
  std::printf("data export forwarded: %s (%p vs the original's %p)\n", data && data == data_orig ? "yes" : "NO", data, data_orig);
  if (running) std::printf("SteamAPI_IsSteamRunning() through the thunk = %d\n", running() ? 1 : 0);
  if (user) std::printf("SteamAPI_GetHSteamUser() through the thunk = %d\n", user());
  Sleep(4000);
  std::printf("FreeLibrary = %d\n", FreeLibrary(m));
  std::printf("process still alive after unload\n");
  return 0;
}
