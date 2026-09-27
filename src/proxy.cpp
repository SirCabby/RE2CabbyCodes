// The mod ships as steam_api64.dll, standing in front of the Steam API DLL the
// game ships with (renamed steam_api64_orig.dll by `make install`). Every
// function the original exports (1018 of them; the exe delay-loads 12) is a
// one-instruction thunk that jumps through a table of the real addresses - the
// stack and every register are untouched, so the callee sees exactly the call
// the game made. The one data export, g_pSteamClientGameServer, is a PE
// forwarder in the .def. The list of names lives in proxy_exports.inc,
// generated from the original DLL's export table by tools/gen_proxy.py.
//
// Any Steam DLL will do as the original if it has what the game imports - the
// names re2.exe's import and delay-load tables ask steam_api64.dll for, read as
// the proxy loads (12 in either build: the dx11_non-rt one asks for
// SteamInternal_CreateInterface and SteamAPI_GetHSteamPipe where the current one
// asks for SteamInternal_FindOrCreateUserInterface and SteamInternal_ContextInit;
// its own Steam DLL, of 2016, lacks 367 of the 1018 this proxy exports). Every
// other version of the DLL lacks some of the 1018 (Valve drops old flat
// functions as it adds new ones: a 2017 DLL lacks 340, an SDK 1.57 one 24), and
// a DLC unlocker's or another game's DLL renamed in its place is no different.
// What the original lacks answers 0 (steam_missing), and the log says how much
// that is and whether the original is one of the two DLLs the game's Steam
// versions ship (kStock). Only what the game itself needs is fatal - a function
// the original lacks (the game could not run with that DLL, mod or not), or one
// this proxy does not export at all (another version of the game) - and then
// DllMain shows failure() and refuses to load. The first release refused any
// original short of all 1018: players whose steam_api64.dll was not the stock
// one could not start the game, the log a column of "lacks export" (2026-09-23).
//
// Why steam_api64: of re2.exe's imports it is the one that ships with the game
// and is not a Wine builtin, so Proton loads ours with no WINEDLLOVERRIDES
// (d3d11, dxgi, dinput8, xinput1_3 and version are all builtins there). The exe
// delay-loads it, so this DLL is loaded by the game's own first Steam call -
// early in WinMain, long after SteamStub has unpacked the image - and
// dinput8.dll stays free for REFramework.

#include "proxy.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "log.h"
#include "mem.h"
#include "proxy_exports.inc"

// Plain C symbols so the assembly below can name them.
extern "C" {
void* g_proxy_orig[PROXY_EXPORT_COUNT] = {};
void steam_missing();
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

// What a function the original lacks answers: 0 in rax, and 0.0 in xmm0 for a
// float. The game never calls one - whatever it imports the original must have -
// so only something resolving a name at run time can get here.
asm(".text\n"
    ".globl steam_missing\n"
    "steam_missing:\n"
    "\txorl %eax, %eax\n"
    "\txorps %xmm0, %xmm0\n"
    "\tret\n");

namespace re2cc::proxy {
namespace {

constexpr const char* kName = "steam_api64.dll";  // what the game loads this DLL as
constexpr const char* const kNames[] = {
#define PROXY_NAME(i, name) name,
    PROXY_EXPORTS(PROXY_NAME)
#undef PROXY_NAME
};

HMODULE g_orig = nullptr;
int g_missing = 0;
char g_failure[1024] = "";

// The way out of every failure but the game's version: the game's own DLL back
// from Steam, renamed, and the mod's copied in again. A steam_api64_orig.dll
// already there has to go first: verifying leaves it alone (it is not one of the
// game's files), and a rename cannot replace it - Explorer offers
// "steam_api64_orig (2).dll" instead, and the wrong file is loaded again.
constexpr char kFixSteps[] =
    "in Steam, right-click Resident Evil 2 and choose Properties > Installed Files > Verify integrity of game "
    "files. That puts the game's own steam_api64.dll back, in place of the mod's. Then rename it to "
    "steam_api64_orig.dll and copy the mod's steam_api64.dll in beside it again.";

// The Steam DLLs the game's two Steam versions ship, known by size, link stamp
// and export count - the same files on every install (SHA-256 ca1fa9a7...eea73
// and 71af8666...f55a8). The current version's has every function this proxy
// exports; the dx11_non-rt beta's is from an older SDK and lacks 367 of them,
// none of which that version of the game imports. So on the beta the right file
// is one the proxy stubs for, and the log must not call it wrong: versions 1.0.1
// and 1.0.2 said "not the Steam DLL the mod was made for", and a player with the
// right file took that for the fault (2026-09-24).
struct Stock {
  unsigned long long size;
  DWORD stamp;
  unsigned names;
  const char* version;  // "<version>'s own Steam DLL"
};
constexpr Stock kStock[] = {
    {265504, 0x5FDFD901, 1019, "the current version's"},   // build 11636119, linked 2020-12-20
    {235600, 0x572906D2, 778, "the dx11_non-rt beta's"},  // build 11055033, linked 2016-05-03
};

// The module this code is in: the mod's DLL (a test's exe when linked into one).
HMODULE self() {
  HMODULE m = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(&self), &m);
  return m;
}

// True when a loaded module's export table has the name (a function or a forwarder).
bool exports_name(HMODULE m, const char* name) {
  const IMAGE_NT_HEADERS* nt = mem::nt_headers(m);
  if (!nt) return false;
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!dir.VirtualAddress) return false;
  const auto base = reinterpret_cast<uintptr_t>(m);
  const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
  for (DWORD i = 0; i < exports->NumberOfNames; ++i)
    if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) == 0) return true;
  return false;
}

// Something this DLL exports: a thunk, or the data forwarder of the .def.
bool ours(const char* name) {
  for (const char* n : kNames)
    if (std::strcmp(n, name) == 0) return true;
  return exports_name(self(), name);
}

// A link stamp as the date it was linked (UTC).
void date_of(DWORD stamp, char* out, size_t n) {
  std::snprintf(out, n, "?");
  const std::time_t t = stamp;
  if (const std::tm* tm = stamp ? std::gmtime(&t) : nullptr) std::strftime(out, n, "%Y-%m-%d", tm);
}

// The original, for the log: its size, the date its linker stamped, how many
// names it exports - and which of the game's own it is, if it is one.
const Stock* describe(const char* path, HMODULE m, char* out, size_t n) {
  WIN32_FILE_ATTRIBUTE_DATA file{};
  const unsigned long long size = GetFileAttributesExA(path, GetFileExInfoStandard, &file)
                                      ? (static_cast<unsigned long long>(file.nFileSizeHigh) << 32) | file.nFileSizeLow
                                      : 0;
  const IMAGE_NT_HEADERS* nt = mem::nt_headers(m);
  const DWORD stamp = nt ? nt->FileHeader.TimeDateStamp : 0;
  char linked[16];
  date_of(stamp, linked, sizeof(linked));
  unsigned names = 0;
  if (nt) {
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress)
      names = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(reinterpret_cast<uintptr_t>(m) + dir.VirtualAddress)
                  ->NumberOfNames;
  }
  std::snprintf(out, n, "%llu bytes, linked %s, %u exports", size, linked, names);
  for (const Stock& s : kStock)
    if (size == s.size && stamp == s.stamp && names == s.names) return &s;
  return nullptr;
}

// The game's own, for the log line of an original that is neither.
void describe_stock(char* out, size_t n) {
  out[0] = '\0';
  for (const Stock& s : kStock) {
    char linked[16];
    date_of(s.stamp, linked, sizeof(linked));
    const size_t at = std::strlen(out);
    std::snprintf(out + at, n - at, "%s%s: %llu bytes, linked %s, %u exports", at ? "; " : "", s.version, s.size,
                  linked, s.names);
  }
}

// The name the program's file has, for the log (re2.exe).
const char* program() {
  static char name[MAX_PATH] = "";
  if (!name[0]) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
  }
  return name;
}

}  // namespace

HMODULE original() { return g_orig; }
const char* failure() { return g_failure; }
int missing() { return g_missing; }

bool load_original(const char* dir) { return load_original_as(dir, mem::imported_names(GetModuleHandleA(nullptr), kName)); }

bool load_original_as(const char* dir, const std::vector<std::string>& imports) {
  g_orig = nullptr;
  g_missing = 0;
  g_failure[0] = '\0';

  char path[MAX_PATH] = {};
  std::snprintf(path, sizeof(path), "%ssteam_api64_orig.dll", dir);
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
    // Windows hides the ".dll" of a file's name unless told otherwise, so the
    // rename can come out as steam_api64_orig.dll.dll - which Explorer then
    // shows as exactly the name asked for.
    char twice[MAX_PATH] = {};
    std::snprintf(twice, sizeof(twice), "%ssteam_api64_orig.dll.dll", dir);
    if (GetFileAttributesA(twice) != INVALID_FILE_ATTRIBUTES) {
      logf("proxy: no steam_api64_orig.dll, but a steam_api64_orig.dll.dll (the .dll typed twice - Windows hides it) "
           "- using that");
      std::snprintf(path, sizeof(path), "%s", twice);
    }
  }
  HMODULE orig = LoadLibraryA(path);
  if (!orig) {
    const DWORD error = GetLastError();
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
      logf("FATAL: there is no %s - the game's own steam_api64.dll was not renamed to it", path);
      std::snprintf(g_failure, sizeof(g_failure),
                    "RE2CabbyCodes: the game's own Steam DLL is missing.\n\n"
                    "The mod takes the place of the game's steam_api64.dll and passes the game's calls on to the "
                    "original, which has to sit beside it renamed to steam_api64_orig.dll. There is no "
                    "steam_api64_orig.dll in the game's folder.\n\nTo fix it: %s",
                    kFixSteps);
    } else {
      logf("FATAL: could not load %s (GetLastError=%lu)", path, error);
      std::snprintf(g_failure, sizeof(g_failure),
                    "RE2CabbyCodes: steam_api64_orig.dll could not be loaded (Windows error %lu).\n\n"
                    "It should be the game's own steam_api64.dll, renamed: the mod passes the game's calls on to "
                    "it.\n\nTo fix it: delete steam_api64_orig.dll. Then, %s",
                    error, kFixSteps);
    }
    return false;
  }
  // Pin it: nothing may drop the last reference while the game still uses it.
  HMODULE pinned = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);
  char about[96];
  const Stock* stock = describe(path, orig, about, sizeof(about));

  // What the game will ask this DLL for: it must be ours to give, and the
  // original's to answer.
  const char* unknown = nullptr;  // the first the proxy does not export
  const char* lacking = nullptr;  // the first the original lacks
  int n_unknown = 0, n_lacking = 0;
  for (const std::string& name : imports) {
    if (!ours(name.c_str())) {
      if (!n_unknown++) unknown = name.c_str();
    } else if (!GetProcAddress(orig, name.c_str())) {
      if (!n_lacking++) lacking = name.c_str();
    }
  }
  if (n_unknown) {
    logf("FATAL: %s imports %s from %s, which this proxy does not export (%d such name(s)) - not a version of the "
         "game the mod was made for (builds 11636119 and 11055033)",
         program(), unknown, kName, n_unknown);
    std::snprintf(g_failure, sizeof(g_failure),
                  "RE2CabbyCodes does not work with this version of Resident Evil 2.\n\n"
                  "The game asks its Steam DLL for %s, which the mod does not pass on: the mod was made for the "
                  "current Steam version of the game (build 11636119) and the dx11_non-rt beta (build 11055033), "
                  "and this is another one.\n\n"
                  "To play with the mod: in Steam, right-click Resident Evil 2 and choose Properties > Betas, "
                  "then None (or dx11_non-rt).\n"
                  "To play without it: delete the mod's steam_api64.dll and rename steam_api64_orig.dll back to "
                  "steam_api64.dll.",
                  unknown);
    return false;
  }
  if (n_lacking) {
    // A stock DLL here is the other version's: the dx11_non-rt beta's, kept
    // when the game was switched back to the current version (the current
    // version's has everything either version imports).
    logf("FATAL: %s lacks %s, which %s imports (%d of its %d Steam function(s) missing) - not the game's own "
         "Steam DLL (%s%s%s%s)",
         path, lacking, program(), n_lacking, static_cast<int>(imports.size()), about, stock ? ": " : "",
         stock ? stock->version : "", stock ? " own, not this version's" : "");
    char which[200];
    if (stock)
      std::snprintf(which, sizeof(which),
                    "It is %s own Steam DLL - probably kept from before the game was switched to another version "
                    "in Steam.",
                    stock->version);
    else
      std::snprintf(which, sizeof(which), "It is probably from another game, or from another version of this one.");
    std::snprintf(g_failure, sizeof(g_failure),
                  "RE2CabbyCodes: steam_api64_orig.dll is not the game's own Steam DLL.\n\n"
                  "The game needs %s from it, which it does not have, so the game cannot run with this file - with "
                  "the mod or without it. %s\n\nTo fix it: delete steam_api64_orig.dll. Then, %s",
                  lacking, which, kFixSteps);
    return false;
  }

  // Every function the proxy exports goes to the original's, or answers 0.
  int n_missing = 0;
  char listed[200] = "";  // the first few missing, for the log
  for (int i = 0; i < PROXY_EXPORT_COUNT; ++i) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(orig, kNames[i]));
    if (!fn) {
      fn = reinterpret_cast<void*>(&steam_missing);
      if (++n_missing <= 4) {
        const size_t at = std::strlen(listed);
        std::snprintf(listed + at, sizeof(listed) - at, "%s%s", at ? ", " : "", kNames[i]);
      }
    }
    g_proxy_orig[i] = fn;
  }
  g_orig = orig;
  g_missing = n_missing;
  // What is missing is only noted, never a fault: whatever the game imports was
  // checked above.
  if (!n_missing) {
    logf("forwarding %d Steam API functions -> %s (%s%s%s%s)", PROXY_EXPORT_COUNT, path, about,
         stock ? " - " : "", stock ? stock->version : "", stock ? " own" : "");
  } else {
    if (stock) {
      logf("proxy: %s is %s own Steam DLL (%s) - the right one for that version of the game", path, stock->version,
           about);
    } else {
      char known[200];
      describe_stock(known, sizeof(known));
      logf("proxy: %s is not a Steam DLL the game ships (%s; %s) - but it has every function the game imports: "
           "the game and the mod run with it",
           path, about, known);
    }
    logf("proxy: %d of the %d functions this proxy passes on are not in it (%s%s) - the game imports none of them, "
         "and they answer 0; forwarding the other %d",
         n_missing, PROXY_EXPORT_COUNT, listed, n_missing > 4 ? ", ..." : "", PROXY_EXPORT_COUNT - n_missing);
  }
  logf("proxy: %s imports %d Steam function(s) by name - the original has them all", program(),
       static_cast<int>(imports.size()));
  return true;
}

}  // namespace re2cc::proxy
