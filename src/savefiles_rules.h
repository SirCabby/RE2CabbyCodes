#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// The save files' rules on plain values - no game memory, no Steam here
// (savefiles.cpp applies them; tests/test_savefiles.cpp checks them).
//
// RE2's saves are one file per slot in the game's Steam Cloud folder, read and
// written through Steam's remote storage (ISteamRemoteStorage: the engine's
// save service asks Steam for FileExists, GetFileSize, FileReadAsync,
// FileWriteAsync and FileDelete - never the disk). A slot's file is named by
// its number: the auto-save, slot 0, is `data000.bin`; slots 1 and up are
// `data001Slot.bin`, `data002Slot.bin`, ... (the system save is `data00-1.bin`,
// The Ghost Survivors' `data021Slot.bin` - neither is on the main game's load
// list, slots 0-20). Steam lists them under the folder the game uses
// (`win64_save/`).
namespace re2cc::savefiles::rules {

constexpr int kMaxSlot = 999;  // three digits

// A slot's file name in `dir` (Steam's folder prefix, "win64_save/"). Empty for
// a slot no file can have.
inline std::string file_name(const std::string& dir, int slot) {
  if (slot < 0 || slot > kMaxSlot) return {};
  char name[32];
  if (slot == 0) std::snprintf(name, sizeof(name), "data000.bin");
  else std::snprintf(name, sizeof(name), "data%03dSlot.bin", slot);
  return dir + name;
}

// The slot a Steam file holds (-1: none of the game's slots - the system save,
// anything else) and its folder prefix (up to and including the last '/').
inline int slot_of(const char* steam_name, std::string* dir = nullptr) {
  if (!steam_name) return -1;
  const char* slash = std::strrchr(steam_name, '/');
  const char* base = slash ? slash + 1 : steam_name;
  if (std::strncmp(base, "data", 4) != 0) return -1;
  const char* d = base + 4;
  for (int i = 0; i < 3; ++i)
    if (d[i] < '0' || d[i] > '9') return -1;
  const int n = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
  const char* rest = d + 3;
  // Slot 0 has no "Slot" in its name, and every other slot has.
  if (std::strcmp(rest, n == 0 ? ".bin" : "Slot.bin") != 0) return -1;
  if (dir) dir->assign(steam_name, static_cast<size_t>(base - steam_name));
  return n;
}

// Steam's files set beside the game's list - row i is slot `first + i`, as the
// game's code lays the list out (SaveDataManager.getSaveDataIndex): every row that holds a save must
// have its file, and no empty row may have one - the names are then the ones
// the game uses, and a copy can go by them. Returns the first row that does not
// fit (-1: all fit).
struct SteamFiles {
  static constexpr int kSlots = kMaxSlot + 1;
  bool exists[kSlots] = {};
  int size[kSlots] = {};
  std::string name[kSlots];
  std::string dir;       // the folder prefix of the slots' files ("" until one is seen)
  bool mixed_dirs = false;  // slot files in more than one folder: not the game's layout

  void add(const char* steam_name, int bytes) {
    std::string d;
    const int s = slot_of(steam_name, &d);
    if (s < 0) return;
    if (dir.empty()) dir = d;
    else if (d != dir) mixed_dirs = true;
    exists[s] = true;
    size[s] = bytes;
    name[s] = steam_name;
  }
};

inline int first_mismatch(const SteamFiles& f, int first, const bool* row_used, int rows) {
  for (int i = 0; i < rows; ++i) {
    const int s = first + i;
    if (s < 0 || s >= SteamFiles::kSlots) return i;
    if (f.exists[s] != row_used[i]) return i;
  }
  return -1;
}

// A save's time stamp (SaveFileDetail.LastUpdateTimeStamp: .NET ticks, UTC -
// SaveDataManager.getDayStringFromTicks makes a DateTime of kind Utc from it)
// as Unix seconds; -1 when it is no plausible date (before 2019, past 2100).
inline long long unix_seconds(long long ticks) {
  constexpr long long kTicksPerSecond = 10000000LL;
  constexpr long long kUnixEpochSeconds = 62135596800LL;  // 0001-01-01 to 1970-01-01
  if (ticks <= 0) return -1;
  const long long s = ticks / kTicksPerSecond - kUnixEpochSeconds;
  return s >= 1546300800LL && s < 4102444800LL ? s : -1;
}

// --- reading the engine's objects out of their getters' code ---------------------------------------------
// via.storage.saveService.SaveFileDetail has no fields in the type database, and
// System.String's are not listed either; their getters are a load or two, and
// say where. A managed method is `ret f(context, this, ...)`: the object is rdx.

// `[REX.W] 8B 42 d8 C3` / `[REX.W] 8B 82 d32 C3` - `mov eax/rax,[rdx+d]; ret`:
// the offset d (-1: not that shape). `wide`: the 64-bit load (a reference, an int64).
inline int32_t getter_offset(const uint8_t* b, size_t n, bool wide) {
  size_t i = 0;
  if (wide) {
    if (n < 1 || b[0] != 0x48) return -1;
    i = 1;
  } else if (n >= 1 && (b[0] & 0xF0) == 0x40) {
    return -1;
  }
  if (n < i + 4 || b[i] != 0x8B) return -1;
  if (b[i + 1] == 0x42 && b[i + 3] == 0xC3) return static_cast<int8_t>(b[i + 2]);
  if (n >= i + 7 && b[i + 1] == 0x82 && b[i + 6] == 0xC3) {
    int32_t d = 0;
    std::memcpy(&d, &b[i + 2], 4);
    return d;
  }
  return -1;
}

// The dx11_non-rt build's text getters (SaveFileDetail.get_Title and the rest) keep
// the text as the engine's own UTF-16 string and make a managed one of it each call:
//   add rdx,OFF; lea rcx,[rsp+T]; call copy;   (the detail's string copied to rsp+T)
//   cmp dword [rsp+C],INLINE; lea r8,[rsp+T]; mov edx,[rsp+L]; ...; cmovae r8,[rsp+T]
// - its characters inline at +0 while the capacity (+C-T) is below INLINE, else a
// pointer there; the length (+L-T) in characters. Build 11055033: OFF 0x18/0x38/0x58,
// capacity +0x1C, length +0x18, inline below 12.
struct NativeText {
  int32_t offset = -1, length = -1, capacity = -1, inline_below = -1;
};
inline bool native_text_getter(const uint8_t* b, size_t n, NativeText* out) {
  for (size_t i = 0; i + 14 <= n; ++i) {
    // add rdx,imm8; lea rcx,[rsp+d8]; call rel32
    if (!(b[i] == 0x48 && b[i + 1] == 0x83 && b[i + 2] == 0xC2 && b[i + 4] == 0x48 && b[i + 5] == 0x8D &&
          b[i + 6] == 0x4C && b[i + 7] == 0x24 && b[i + 9] == 0xE8))
      continue;
    NativeText t;
    t.offset = static_cast<int8_t>(b[i + 3]);
    const int temp = b[i + 8];
    for (size_t j = i + 14; j + 4 <= n; ++j) {
      if (b[j] == 0x83 && b[j + 1] == 0x7C && b[j + 2] == 0x24 && t.capacity < 0) {  // cmp dword [rsp+d8],imm8
        t.capacity = b[j + 3] - temp;
        t.inline_below = b[j + 4];
      }
      if (b[j] == 0x8B && b[j + 1] == 0x54 && b[j + 2] == 0x24 && t.length < 0) t.length = b[j + 3] - temp;  // mov edx,[rsp+d8]
    }
    if (t.offset > 0 && t.capacity >= 0 && t.length >= 0 && t.inline_below > 0) {
      *out = t;
      return true;
    }
    return false;
  }
  return false;
}

// `B8 imm32 C3` - `mov eax,imm32; ret`: a getter returning a constant.
inline bool getter_constant(const uint8_t* b, size_t n, int32_t* out) {
  if (n < 6 || b[0] != 0xB8 || b[5] != 0xC3) return false;
  std::memcpy(out, &b[1], 4);
  return true;
}

// String.get_Chars(index): `movzx eax, word [rdx+reg*2+d8]` (0F B7 44 SIB d8,
// base rdx, scale 2) among its first bytes: where the UTF-16 characters start.
inline int32_t chars_offset(const uint8_t* b, size_t n) {
  for (size_t i = 0; i + 5 <= n; ++i)
    if (b[i] == 0x0F && b[i + 1] == 0xB7 && (b[i + 2] & 0xC7) == 0x44 && (b[i + 3] & 0xC7) == 0x42)
      return static_cast<int8_t>(b[i + 4]);
  return -1;
}

// FNV-1a over a few values: what a request checks it acts on the save the
// panel showed.
inline uint32_t fnv1a(const void* data, size_t n, uint32_t h = 2166136261u) {
  const auto* b = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
  return h;
}

}  // namespace re2cc::savefiles::rules
