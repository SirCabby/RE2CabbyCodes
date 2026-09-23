// The mod ships as steam_api64.dll, standing in front of the Steam API DLL the
// game ships with (renamed steam_api64_orig.dll by `make install`). Every
// function the original exports (1018 of them; the exe delay-loads 12) is a
// one-instruction thunk that jumps through a table of the real addresses - the
// stack and every register are untouched, so the callee sees exactly the call
// the game made. The one data export, g_pSteamClientGameServer, is a PE
// forwarder in the .def. The list of names lives in proxy_exports.inc,
// generated from the original DLL's export table by tools/gen_proxy.py.
//
// Why steam_api64: of re2.exe's imports it is the one that ships with the game
// and is not a Wine builtin, so Proton loads ours with no WINEDLLOVERRIDES
// (d3d11, dxgi, dinput8, xinput1_3 and version are all builtins there). The exe
// delay-loads it, so this DLL is loaded by the game's own first Steam call -
// early in WinMain, long after SteamStub has unpacked the image - and
// dinput8.dll stays free for REFramework.

#include "proxy.h"

#include <cstdio>

#include "log.h"
#include "proxy_exports.inc"

// Plain C symbol so the assembly below can name it.
extern "C" {
void* g_proxy_orig[PROXY_EXPORT_COUNT] = {};
}

// One thunk per export: `jmp [rip + g_proxy_orig + i*8]`, defined at file scope
// in assembly so no prologue or epilogue is ever emitted around the jump.
#define PROXY_THUNK(i, name)                                   \
  asm(".text\n"                                                \
      ".globl steam_" #i "\n"                                  \
      "steam_" #i ":\n"                                        \
      "\tjmp *g_proxy_orig+" #i "*8(%rip)\n");
PROXY_EXPORTS(PROXY_THUNK)
#undef PROXY_THUNK

namespace re2cc::proxy {
namespace {

HMODULE g_orig = nullptr;

}  // namespace

HMODULE original() { return g_orig; }

bool load_original(const char* dir) {
  char path[MAX_PATH] = {};
  std::snprintf(path, sizeof(path), "%ssteam_api64_orig.dll", dir);

  HMODULE orig = LoadLibraryA(path);
  if (!orig) {
    logf("FATAL: could not load %s (GetLastError=%lu). Did `make install` run?", path, GetLastError());
    return false;
  }
  // Pin it: nothing may drop the last reference while the game still uses it.
  HMODULE pinned = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);

  static const char* const kNames[] = {
#define PROXY_NAME(i, name) name,
      PROXY_EXPORTS(PROXY_NAME)
#undef PROXY_NAME
  };
  int missing = 0;
  for (int i = 0; i < PROXY_EXPORT_COUNT; ++i) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(orig, kNames[i]));
    g_proxy_orig[i] = fn;
    if (!fn) {
      ++missing;
      logf("FATAL: %s lacks export %s", path, kNames[i]);
    }
  }
  if (missing) return false;
  g_orig = orig;
  logf("forwarding %d Steam API exports -> %s", PROXY_EXPORT_COUNT, path);
  return true;
}

}  // namespace re2cc::proxy
