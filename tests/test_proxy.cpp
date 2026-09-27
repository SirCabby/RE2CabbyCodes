// The proxy's run-time checks (src/proxy.cpp, mem::imported_names) on the real files:
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_proxy.cpp
//       src/proxy.cpp src/log.cpp src/mem.cpp -o tests/build/test_proxy.exe
//   wine tests/build/test_proxy.exe 'Z:\...\re2.exe' 'Z:\...\steam_api64_orig.dll' ['Z:\...\steam_api64.dll' ...]
//
// First what re2.exe imports from steam_api64.dll: its delay-load table, read
// from the image the loader maps, must give build 11636119's twelve names. Then
// the original loaded from a folder the way DllMain loads it, once per Steam DLL
// given - the game's own first, then any other (another game's, another SDK's):
// with the game's twelve imports the load must succeed exactly when the DLL has
// all twelve, and then every thunk must point at the original's function of its
// name, or at the stub (answering 0) for one the original lacks; the log must
// name the game's own as such, whatever it lacks of the proxy's exports (the
// dx11_non-rt beta's lacks 367), and never call it the wrong file. It must fail,
// with the message for the player, when the program imports a name the proxy
// does not export, and when there is no original; a steam_api64_orig.dll.dll
// (the .dll typed twice) must do.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/log.h"
#include "../src/mem.h"
#include "../src/proxy.h"
#include "../src/proxy_exports.inc"

extern "C" {
extern void* g_proxy_orig[PROXY_EXPORT_COUNT];
void steam_missing();
}

namespace {

using namespace re2cc;

int g_failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

constexpr const char* const kNames[] = {
#define PROXY_NAME(i, name) name,
    PROXY_EXPORTS(PROXY_NAME)
#undef PROXY_NAME
};

// The delay-load tables for steam_api64.dll, in their order (tools/gen_proxy.py
// reads the same from the files): build 11636119's, and the dx11_non-rt build's.
const std::vector<std::string> kRtImports = {
    "SteamAPI_UnregisterCallback",   "SteamAPI_Init",
    "SteamAPI_RunCallbacks",         "SteamAPI_RestartAppIfNecessary",
    "SteamAPI_IsSteamRunning",       "SteamAPI_RegisterCallResult",
    "SteamAPI_UnregisterCallResult", "SteamInternal_FindOrCreateUserInterface",
    "SteamAPI_RegisterCallback",     "SteamInternal_ContextInit",
    "SteamAPI_Shutdown",             "SteamAPI_GetHSteamUser"};
const std::vector<std::string> kDx11Imports = {
    "SteamInternal_CreateInterface", "SteamAPI_Init",
    "SteamAPI_Shutdown",             "SteamAPI_GetHSteamUser",
    "SteamAPI_RunCallbacks",         "SteamAPI_RegisterCallback",
    "SteamAPI_UnregisterCallback",   "SteamAPI_IsSteamRunning",
    "SteamAPI_UnregisterCallResult", "SteamAPI_RestartAppIfNecessary",
    "SteamAPI_GetHSteamPipe",        "SteamAPI_RegisterCallResult"};
std::vector<std::string> kGameImports;  // whichever the exe given has

std::string g_root;  // the scratch folder, with a trailing separator
int g_folders = 0;

// A new folder under the scratch one, holding `dll` copied in as `as` (nothing when dll is null).
std::string folder(const char* dll, const char* as) {
  const std::string dir = g_root + std::to_string(++g_folders) + "\\";
  CreateDirectoryA(dir.c_str(), nullptr);
  if (dll && !CopyFileA(dll, (dir + as).c_str(), FALSE)) std::printf("  (could not copy %s: %lu)\n", dll, GetLastError());
  return dir;
}

bool says(const char* text) { return std::strstr(proxy::failure(), text) != nullptr; }

// What the log gained since `from` (a size it had).
std::string log_since(long from) {
  std::string text;
  if (FILE* f = std::fopen((g_root + "RE2CabbyCodes.log").c_str(), "rb")) {
    std::fseek(f, from, SEEK_SET);
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
  }
  return text;
}
long log_size() {
  long n = 0;
  if (FILE* f = std::fopen((g_root + "RE2CabbyCodes.log").c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    n = std::ftell(f);
    std::fclose(f);
  }
  return n;
}

void imports_of_the_game(const char* exe) {
  std::printf("what %s imports from steam_api64.dll\n", exe);
  HMODULE m = LoadLibraryExA(exe, nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!m) {
    std::printf("  FAIL  could not map it (%lu)\n", GetLastError());
    ++g_failures;
    return;
  }
  const std::vector<std::string> got = mem::imported_names(m, "steam_api64.dll");
  kGameImports = got == kDx11Imports ? kDx11Imports : kRtImports;
  check(got == kRtImports || got == kDx11Imports,
        got == kDx11Imports ? "its delay-load table: the dx11_non-rt build's twelve" : "its delay-load table: build 11636119's twelve");
  check(mem::imported_names(m, "STEAM_API64.DLL") == got, "the DLL's name matched without case");
  bool filter = false;
  for (const std::string& n : mem::imported_names(m, "kernel32.dll")) filter |= n == "SetUnhandledExceptionFilter";
  check(filter, "its import table too (kernel32.dll's SetUnhandledExceptionFilter)");
  check(mem::imported_names(m, "no_such.dll").empty(), "nothing from a DLL it does not import");
  check(mem::imported_names(GetModuleHandleA(nullptr), "steam_api64.dll").empty(), "nothing in this test's own exe");
}

void original(const char* dll, bool the_games) {
  std::printf("the original: %s\n", dll);
  // What it has, asked of a copy the proxy never sees.
  const std::string ref_dir = folder(dll, "reference.dll");
  HMODULE ref = LoadLibraryA((ref_dir + "reference.dll").c_str());
  if (!ref) {
    std::printf("  FAIL  could not load it (%lu)\n", GetLastError());
    ++g_failures;
    return;
  }
  int lacks = 0, lacks_imported = 0;
  for (const char* n : kNames) lacks += !GetProcAddress(ref, n);
  for (const std::string& n : kGameImports) lacks_imported += !GetProcAddress(ref, n.c_str());
  std::printf("  (it lacks %d of the proxy's %d functions, %d of the game's %d)\n", lacks, PROXY_EXPORT_COUNT,
              lacks_imported, static_cast<int>(kGameImports.size()));
  if (the_games) check(lacks_imported == 0, "the game's own has every function the game imports");

  const long before = log_size();
  const bool loaded = proxy::load_original_as(folder(dll, "steam_api64_orig.dll").c_str(), kGameImports);
  check(loaded == (lacks_imported == 0), "loads with the game's imports exactly when it has all of them");
  if (the_games) {
    // Either version's own DLL is named as such, whatever it lacks of the
    // proxy's exports (the dx11_non-rt beta's lacks 367): a player reads this
    // log, and 1.0.1/1.0.2 called the beta's own "not the Steam DLL the mod was
    // made for" (2026-09-24).
    const std::string said = log_since(before);
    check(said.find("current version's own") != std::string::npos ||
              said.find("dx11_non-rt beta's own") != std::string::npos,
          "the log names it as the game's own Steam DLL");
    check(said.find("not a Steam DLL the game ships") == std::string::npos &&
              said.find("made for") == std::string::npos,
          "and never calls it the wrong one");
  }
  if (loaded) {
    check(proxy::missing() == lacks, "missing() counts what it lacks");
    bool table = true;
    for (int i = 0; i < PROXY_EXPORT_COUNT; ++i) {
      void* want = GetProcAddress(ref, kNames[i]) ? reinterpret_cast<void*>(GetProcAddress(proxy::original(), kNames[i]))
                                                  : reinterpret_cast<void*>(&steam_missing);
      table &= g_proxy_orig[i] == want;
    }
    check(table, "every thunk: the original's function of its name, or the stub");
  } else {
    check(says("is not the game's own Steam DLL"), "the message: not the game's own Steam DLL");
  }
  const std::vector<std::string> newer = {"SteamAPI_Init", "SteamAPI_NoSuchFunction"};
  check(!proxy::load_original_as(folder(dll, "steam_api64_orig.dll").c_str(), newer) &&
            says("does not work with this version of Resident Evil 2"),
        "refused when the program imports what the proxy does not export");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: test_proxy.exe <re2.exe> <the game's steam_api64.dll> [other steam_api64.dll ...]\n");
    return 2;
  }
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  g_root = std::string(temp) + "re2cc_test_proxy_" + std::to_string(GetCurrentProcessId()) + "\\";
  CreateDirectoryA(g_root.c_str(), nullptr);
  log_init(g_root.c_str());

  imports_of_the_game(argv[1]);
  for (int i = 2; i < argc; ++i) original(argv[i], i == 2);

  std::printf("no original at all\n");
  check(!proxy::load_original_as(folder(nullptr, nullptr).c_str(), kGameImports) &&
            says("the game's own Steam DLL is missing"),
        "refused, and says the original is missing");
  std::printf("the original renamed with its .dll typed twice\n");
  check(proxy::load_original_as(folder(argv[2], "steam_api64_orig.dll.dll").c_str(), kGameImports),
        "steam_api64_orig.dll.dll is used");

  std::printf("the stub\n");
  check(reinterpret_cast<long long (*)()>(&steam_missing)() == 0, "answers 0");
  check(reinterpret_cast<double (*)()>(&steam_missing)() == 0.0, "answers 0.0 for a float");

  std::printf("\nthe log (%sRE2CabbyCodes.log):\n", g_root.c_str());
  if (FILE* f = std::fopen((g_root + "RE2CabbyCodes.log").c_str(), "r")) {
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) std::printf("  %s", line);
    std::fclose(f);
  }
  std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "passed", g_failures);
  return g_failures ? 1 : 0;
}
