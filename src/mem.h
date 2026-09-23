#pragma once

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>
#include <vector>

// Small memory helpers, in the spirit of the sibling mods' mem.h, for 64-bit:
// everything the mod pokes at lives in another module's pages, so every write
// of code or import tables has to unprotect, write, restore and flush - and
// every read of game memory has to be guarded, because a wrong guess must
// produce a log line, not a crash. mingw has no __try, so "guarded" means a
// copy whose faults a vectored exception handler turns into a false return
// (mem.cpp). It costs a copy: the VirtualQuery the mod used to ask first scans
// Wine's page table and took the game from 120 fps to 30.
namespace re2cc::mem {

struct Range {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  bool empty() const { return end <= begin; }
  size_t size() const { return empty() ? 0 : end - begin; }
  bool contains(uintptr_t a) const { return a >= begin && a < end; }
};

inline bool write_bytes(uintptr_t target, const void* data, size_t n) {
  DWORD old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(target), n, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(reinterpret_cast<void*>(target), data, n);
  VirtualProtect(reinterpret_cast<void*>(target), n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), n);
  return true;
}

template <typename T>
inline bool write(uintptr_t target, const T& value) {
  return write_bytes(target, &value, sizeof(T));
}

template <typename T>
inline T read(uintptr_t target) {
  T v{};
  std::memcpy(&v, reinterpret_cast<const void*>(target), sizeof(T));
  return v;
}

// The user-mode address space of a 64-bit process: anything outside it is not
// a pointer, whatever it looks like.
inline bool plausible(uintptr_t a) { return a >= 0x10000 && a < 0x00007FFFFFFF0000ull; }

// --- guarded copies (mem.cpp) -------------------------------------------------
// The first use installs the fault handler (DllMain installs it up front).
bool guard_install();
long guard_faults();  // faults caught so far (the log)
bool copy_from(void* dst, uintptr_t src, size_t n);      // game memory -> ours; false = it faulted
bool copy_to(uintptr_t dst, const void* src, size_t n);  // ours -> game memory (no unprotecting)

// True when [p, p+n) can be read now: one guarded byte per page. Safe on any
// value that merely looks like a pointer.
bool readable(uintptr_t a, size_t n);
inline bool readable(const void* p, size_t n) { return readable(reinterpret_cast<uintptr_t>(p), n); }

// Page protections a write goes through as they are. Copy-on-write counts: an
// image's writable sections are PAGE_WRITECOPY, and under Wine they stay so
// however much they have been written (re2.exe's .data, the type database).
constexpr DWORD kWritableProtect = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;

// True when [p, p+n) is committed and writable as it is (data, not code).
// VirtualQuery - slow under Wine, so only for rare checks; store() needs none.
inline bool writable(uintptr_t a, size_t n) {
  if (!plausible(a)) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & kWritableProtect))
    return false;
  return a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// Exchange a pointer in place, only while it holds `expected` (a compare-and-swap:
// the game may read it at any moment). A page that cannot be written as it is -
// say one the runtime protected after filling it - is made writable for the
// exchange and given its protection back. `protect` receives the protection found.
inline bool exchange_ptr(uintptr_t at, uintptr_t expected, uintptr_t replacement, DWORD* protect = nullptr) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (protect) *protect = 0;
  if (!plausible(at) || !VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
    return false;
  if (protect) *protect = mbi.Protect;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const bool as_is = (mbi.Protect & kWritableProtect) != 0;
  const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  DWORD old = 0;
  if (!as_is && !VirtualProtect(reinterpret_cast<void*>(at), sizeof(uintptr_t),
                                (mbi.Protect & executable) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old))
    return false;
  const bool done = InterlockedCompareExchangePointer(reinterpret_cast<PVOID*>(at), reinterpret_cast<PVOID>(replacement),
                                                      reinterpret_cast<PVOID>(expected)) == reinterpret_cast<PVOID>(expected);
  if (!as_is) VirtualProtect(reinterpret_cast<void*>(at), sizeof(uintptr_t), old, &old);
  return done;
}

template <typename T>
inline bool read_safe(uintptr_t target, T* out) { return copy_from(out, target, sizeof(T)); }

// A plain store into game data (a heap object's field). A page that is not
// writable faults, which fails the store before its first byte.
template <typename T>
inline bool store(uintptr_t target, const T& value) { return copy_to(target, &value, sizeof(T)); }

inline uintptr_t read_ptr(uintptr_t target) {
  uintptr_t v = 0;
  return read_safe(target, &v) && plausible(v) ? v : 0;
}

// Replace one entry of a vtable and hand back what was there.
inline void* hook_vtable(uintptr_t vtable, int slot, void* replacement) {
  const uintptr_t entry = vtable + static_cast<uintptr_t>(slot) * sizeof(void*);
  void* original = read<void*>(entry);
  if (!write<void*>(entry, replacement)) return nullptr;
  return original;
}

// The absolute target of a RIP-relative operand: the disp32 at insn+disp_at,
// counted from the end of the instruction (insn+len).
inline uintptr_t rip_target(uintptr_t insn, int disp_at, int len) {
  return insn + static_cast<uintptr_t>(len) + static_cast<intptr_t>(read<int32_t>(insn + disp_at));
}

// --- PE sections -------------------------------------------------------------
inline IMAGE_NT_HEADERS* nt_headers(HMODULE m) {
  auto base = reinterpret_cast<uintptr_t>(m);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

inline Range section_range(HMODULE m, const IMAGE_SECTION_HEADER& s) {
  auto base = reinterpret_cast<uintptr_t>(m);
  DWORD size = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
  return Range{base + s.VirtualAddress, base + s.VirtualAddress + size};
}

// The section with this (up to 8 character) name, or an empty range.
inline Range section(HMODULE m, const char* name) {
  auto nt = nt_headers(m);
  if (!nt) return {};
  auto* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    if (std::strncmp(reinterpret_cast<const char*>(s[i].Name), name, 8) == 0) return section_range(m, s[i]);
  return {};
}

inline Range module_range(HMODULE m) {
  auto nt = nt_headers(m);
  if (!nt) return {};
  auto base = reinterpret_cast<uintptr_t>(m);
  return Range{base, base + nt->OptionalHeader.SizeOfImage};
}

// The address of a module's import slot for an import (0 when absent), through
// the OriginalFirstThunk names. Everything read out of the table is
// range-checked first: this runs inside DllMain, where a fault is not a log line
// but a process that never starts.
inline uintptr_t iat_slot(HMODULE module, const char* dll_name, const char* fn_name) {
  IMAGE_NT_HEADERS* nt = nt_headers(module);
  if (!nt) return 0;
  const auto base = reinterpret_cast<uintptr_t>(module);
  const Range image = module_range(module);
  auto in_image = [&](uintptr_t a, size_t n) { return a >= image.begin && a + n <= image.end && readable(a, n); };
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return 0;
  for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       in_image(reinterpret_cast<uintptr_t>(imp), sizeof(*imp)) && imp->Name; ++imp) {
    const uintptr_t name_at = base + imp->Name;
    if (!in_image(name_at, 1) || _stricmp(reinterpret_cast<const char*>(name_at), dll_name) != 0) continue;
    if (!imp->OriginalFirstThunk) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    const auto* orig = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    for (; in_image(reinterpret_cast<uintptr_t>(orig), sizeof(*orig)) && orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      const uintptr_t by_name = base + orig->u1.AddressOfData;
      if (!in_image(by_name, sizeof(IMAGE_IMPORT_BY_NAME))) continue;
      if (std::strcmp(reinterpret_cast<const char*>(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(by_name)->Name),
                      fn_name) == 0)
        return reinterpret_cast<uintptr_t>(&thunk->u1.Function);
    }
  }
  return 0;
}

// Replace one entry in a module's import address table; returns what was there.
inline void* iat_hook(HMODULE module, const char* dll_name, const char* fn_name, void* replacement,
                      uintptr_t* slot_out = nullptr) {
  const uintptr_t slot = iat_slot(module, dll_name, fn_name);
  if (!slot) return nullptr;
  void* previous = read<void*>(slot);
  if (!write<void*>(slot, replacement)) return nullptr;
  if (slot_out) *slot_out = slot;
  return previous;
}

// Put an import slot back, but only if it still holds our hook.
inline void iat_restore(uintptr_t slot, void* hook, void* original) {
  if (slot && original && read<void*>(slot) == hook) write<void*>(slot, original);
}

// --- pattern scanning --------------------------------------------------------
// Patterns are IDA/CE style: "48 8B 0D ?? ?? ?? ?? BA FF FF FF FF". "??" is a wildcard.
struct Pattern {
  std::vector<int16_t> bytes;  // -1 = wildcard
};

inline Pattern parse_pattern(const char* s) {
  Pattern p;
  for (const char* c = s; *c;) {
    while (*c == ' ') ++c;
    if (!*c) break;
    if (c[0] == '?') {
      p.bytes.push_back(-1);
      while (*c == '?') ++c;
      continue;
    }
    auto hex = [](char h) -> int {
      if (h >= '0' && h <= '9') return h - '0';
      if (h >= 'a' && h <= 'f') return h - 'a' + 10;
      if (h >= 'A' && h <= 'F') return h - 'A' + 10;
      return -1;
    };
    const int hi = hex(c[0]), lo = c[1] ? hex(c[1]) : -1;
    if (hi < 0 || lo < 0) break;  // malformed; stop where it stops making sense
    p.bytes.push_back(static_cast<int16_t>(hi * 16 + lo));
    c += 2;
  }
  return p;
}

// True when the bytes at `a` match the pattern.
inline bool matches(uintptr_t a, const Pattern& p) {
  if (!readable(a, p.bytes.size())) return false;
  auto* m = reinterpret_cast<const uint8_t*>(a);
  for (size_t i = 0; i < p.bytes.size(); ++i)
    if (p.bytes[i] >= 0 && m[i] != static_cast<uint8_t>(p.bytes[i])) return false;
  return true;
}
inline bool matches(uintptr_t a, const char* s) { return matches(a, parse_pattern(s)); }

// First match at or after `from` (0 = the range start), or 0 when absent. The
// range must be readable as a whole (a section of the game's image).
inline uintptr_t find_pattern(const Range& r, const Pattern& p, uintptr_t from = 0) {
  const size_t n = p.bytes.size();
  if (!n || r.size() < n) return 0;
  uintptr_t a = from > r.begin ? from : r.begin;
  const bool first_wild = p.bytes[0] < 0;
  const uint8_t first = static_cast<uint8_t>(p.bytes[0]);
  for (; a + n <= r.end; ++a) {
    auto* m = reinterpret_cast<const uint8_t*>(a);
    if (!first_wild) {
      const void* hit = std::memchr(m, first, r.end - n + 1 - a);
      if (!hit) return 0;
      a = reinterpret_cast<uintptr_t>(hit);
      m = reinterpret_cast<const uint8_t*>(a);
    }
    size_t i = 1;
    for (; i < n; ++i)
      if (p.bytes[i] >= 0 && m[i] != static_cast<uint8_t>(p.bytes[i])) break;
    if (i == n) return a;
  }
  return 0;
}

inline uintptr_t find_pattern(const Range& r, const char* s, uintptr_t from = 0) {
  return find_pattern(r, parse_pattern(s), from);
}

inline std::vector<uintptr_t> find_all(const Range& r, const Pattern& p, size_t limit = 64) {
  std::vector<uintptr_t> out;
  for (uintptr_t a = find_pattern(r, p); a && out.size() < limit; a = find_pattern(r, p, a + 1)) out.push_back(a);
  return out;
}

inline std::vector<uintptr_t> find_all(const Range& r, const char* s, size_t limit = 64) {
  return find_all(r, parse_pattern(s), limit);
}

// --- the calling thread's stack ------------------------------------------------
// TEB StackLimit..StackBase: a scan for a value must skip it, or it finds its
// own argument.
inline void current_stack(uintptr_t* lo, uintptr_t* hi) {
  *hi = __readgsqword(0x08);
  *lo = __readgsqword(0x10);
}

// --- code patches -----------------------------------------------------------------
// A byte patch that remembers what it replaced. prepare() insists the live
// bytes match a pattern first (so a site found by signature is re-verified at
// the moment of use), apply() refuses if they changed since, revert() only
// writes when the site still holds the replacement.
struct Patch {
  uintptr_t at = 0;
  std::vector<uint8_t> original, replacement;
  const char* label = "";
  bool applied = false;

  bool prepared() const { return at != 0 && !original.empty(); }

  bool prepare(uintptr_t site, const char* expect_pattern, const std::vector<uint8_t>& bytes, const char* name) {
    const Pattern p = parse_pattern(expect_pattern);
    if (!site || p.bytes.size() != bytes.size() || !matches(site, p)) return false;
    at = site;
    label = name;
    original.assign(reinterpret_cast<const uint8_t*>(site), reinterpret_cast<const uint8_t*>(site) + bytes.size());
    replacement = bytes;
    applied = false;
    return true;
  }
  bool apply() {
    if (!prepared() || applied) return applied;
    if (!readable(at, original.size()) || std::memcmp(reinterpret_cast<const void*>(at), original.data(), original.size()) != 0)
      return false;  // someone else changed the site: leave it alone
    if (!write_bytes(at, replacement.data(), replacement.size())) return false;
    applied = true;
    return true;
  }
  bool revert() {
    if (!prepared() || !applied) return true;
    if (!readable(at, replacement.size()) ||
        std::memcmp(reinterpret_cast<const void*>(at), replacement.data(), replacement.size()) != 0) {
      applied = false;  // not ours any more; do not stomp on whatever is there now
      return false;
    }
    const bool ok = write_bytes(at, original.data(), original.size());
    if (ok) applied = false;
    return ok;
  }
};

inline std::vector<uint8_t> nops(size_t n) { return std::vector<uint8_t>(n, 0x90); }

}  // namespace re2cc::mem
