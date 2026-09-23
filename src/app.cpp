#include "app.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "dispatch.h"
#include "log.h"
#include "mem.h"

namespace re2cc::app {
namespace {

// add_function(Application*, object, group, priority, function pair*, name), as
// build 11636119 has it at exe+0x1F64580, from its 22nd byte:
//   mov r10d,[rcx+count]; lea r14,[rcx+table]; mov edi,r8d; mov rbx,rdx; xor edx,edx; mov r8d,size;
//   movzx esi,r9w; lea eax,[r10+1]; mov [rcx+count],eax; imul rax,r10,size; add r14,rax; mov rcx,r14;
//   call memset; mov rax,[rsp+50]; mov [r14],rbx; mov rbx,[rsp+30]; mov [r14+group],di; mov rdi,[rsp+40];
//   mov [r14+priority],si; movups xmm0,[rax]; mov rax,[rsp+58]; mov rsi,[rsp+38];
//   movups [r14+function],xmm0; mov [r14+name],rax
// (the function pair is {function, this-adjustment}: the engine calls
// `function(object + (int32)adjustment)`).
constexpr const char* kAddFunction =
    "44 8B 91 ?? ?? ?? ?? 4C 8D B1 ?? ?? ?? ?? 41 8B F8 48 8B DA 33 D2 41 B8 ?? ?? ?? ?? 41 0F B7 F1 41 8D 42 01 "
    "89 81 ?? ?? ?? ?? 49 69 C2 ?? ?? ?? ?? 4C 03 F0 49 8B CE E8 ?? ?? ?? ?? 48 8B 44 24 50 49 89 1E 48 8B 5C 24 30 "
    "66 41 89 7E ?? 48 8B 7C 24 40 66 41 89 76 ?? 0F 10 00 48 8B 44 24 58 48 8B 74 24 38 41 0F 11 46 ?? 49 89 46 ??";
// mov [rsp+8],rbx; mov [rsp+10],rsi; mov [rsp+18],rdi; push r14; sub rsp,20 - right in front of it.
constexpr const char* kPrologue = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20";
constexpr uintptr_t kPrologueLen = 21;

struct Layout {
  uintptr_t app_slot = 0;  // the global holding via.Application
  int32_t count_off = 0, table_off = 0, stride = 0;
  uint8_t func_off = 0, name_off = 0, prio_off = 0, group_off = 0;
} L;
bool g_found = false;
char g_status[240] = "not searched yet";
mem::Range g_text;
uint32_t g_last_count = 0;  // hook(): the count must hold still between two polls

using EntryFn = void (*)(void*);
constexpr int kThreads = 8;
struct Hook {
  const char* name;
  int source;
  EntryFn replacement;
  uintptr_t entry = 0;  // where in the table (mod thread)
  volatile EntryFn original = nullptr;
  volatile LONG calls = 0;
  volatile LONG threads[kThreads] = {};
  int rate_logs = 0;
  bool moved_logged = false;
};

void hk_update_behavior(void* arg);
void hk_end_rendering(void* arg);
Hook g_hooks[2] = {{"UpdateBehavior", dispatch::kUpdateBehavior, &hk_update_behavior},
                   {"EndRendering", dispatch::kEndRendering, &hk_end_rendering}};

// The first threads each entry runs on, for the log.
void note_thread(Hook& h) {
  const LONG me = static_cast<LONG>(GetCurrentThreadId());
  for (volatile LONG& t : h.threads) {
    if (t == me) return;
    if (!t && InterlockedCompareExchange(&t, me, 0) == 0) {
      logf("app: %s runs on thread %lu", h.name, GetCurrentThreadId());
      return;
    }
  }
}

void entered(Hook& h) {
  InterlockedIncrement(&h.calls);
  note_thread(h);
  dispatch::frame(h.source);
}

void hk_update_behavior(void* arg) {
  entered(g_hooks[0]);
  g_hooks[0].original(arg);
}

void hk_end_rendering(void* arg) {
  entered(g_hooks[1]);
  g_hooks[1].original(arg);
}

void set_status(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void set_status(const char* fmt, ...) {
  char buf[sizeof(g_status)];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  if (std::strcmp(buf, g_status) != 0) {
    std::snprintf(g_status, sizeof(g_status), "%s", buf);
    logf("app: %s", g_status);
  }
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }

uintptr_t entry_at(uintptr_t app, uint32_t i) {
  return app + static_cast<uintptr_t>(L.table_off) + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(L.stride);
}

bool name_is(uintptr_t entry, const char* want) {
  const uintptr_t name = mem::read_ptr(entry + L.name_off);
  const size_t len = std::strlen(want) + 1;
  return name && mem::readable(name, len) && std::memcmp(reinterpret_cast<const char*>(name), want, len) == 0;
}

}  // namespace

bool find() {
  if (g_found) return true;
  HMODULE exe = GetModuleHandleA(nullptr);
  const mem::Range image = mem::module_range(exe);
  g_text = mem::section(exe, ".text");
  const mem::Range data = mem::section(exe, ".data");
  const auto hits = mem::find_all(g_text, kAddFunction, 4);
  if (hits.size() != 1) {
    set_status("the code that fills via.Application's entry table matched %u times - no game tick", static_cast<unsigned>(hits.size()));
    return false;
  }
  const uintptr_t at = hits[0];
  const int32_t count_a = mem::read<int32_t>(at + 3), table = mem::read<int32_t>(at + 10);
  const int32_t size_a = mem::read<int32_t>(at + 24), count_b = mem::read<int32_t>(at + 38);
  const int32_t size_b = mem::read<int32_t>(at + 45);
  // The count is read and written back, the size cleared and multiplied: each
  // pair must name the same thing, or this is not the function it looks like.
  if (count_a != count_b || size_a != size_b || size_a < 0x28 || size_a > 0x400 || table <= count_a ||
      table > 0x10000) {
    set_status("via.Application's entry-table code does not add up (count +0x%X/+0x%X, entry size 0x%X/0x%X) - no "
               "game tick",
               count_a, count_b, size_a, size_b);
    return false;
  }
  const uintptr_t fn = at - kPrologueLen;
  if (!mem::matches(fn, kPrologue)) {
    set_status("via.Application's entry-table function does not start where expected - no game tick");
    return false;
  }
  // Its one caller: call rel32 -> fn.
  uintptr_t call = 0;
  int calls = 0;
  const auto* const end = reinterpret_cast<const uint8_t*>(g_text.end) - 5;
  for (auto* c = reinterpret_cast<const uint8_t*>(g_text.begin); c < end; ++c) {
    c = static_cast<const uint8_t*>(std::memchr(c, 0xE8, static_cast<size_t>(end - c)));
    if (!c) break;
    const uintptr_t site = reinterpret_cast<uintptr_t>(c);
    if (site + 5 + static_cast<intptr_t>(mem::read<int32_t>(site + 1)) == fn) {
      call = site;
      ++calls;
    }
  }
  if (calls != 1) {
    set_status("via.Application's entry-table function has %d callers, not one - no game tick", calls);
    return false;
  }
  // The Application object the caller hands it: mov rcx,[rip+global], shortly before.
  uintptr_t slot = 0;
  for (uintptr_t p = call - 7; p + 0x60 >= call && !slot; --p)
    if (mem::matches(p, "48 8B 0D")) {
      const uintptr_t t = mem::rip_target(p, 3, 7);
      if (data.contains(t)) slot = t;
    }
  if (!slot) {
    set_status("the via.Application global was not found at its entry table's caller - no game tick");
    return false;
  }
  L.app_slot = slot;
  L.count_off = count_a;
  L.table_off = table;
  L.stride = size_a;
  L.group_off = mem::read<uint8_t>(at + 77);
  L.prio_off = mem::read<uint8_t>(at + 87);
  L.func_off = mem::read<uint8_t>(at + 105);
  L.name_off = mem::read<uint8_t>(at + 109);
  if (L.prio_off + 2u > static_cast<unsigned>(L.stride) || L.group_off + 2u > static_cast<unsigned>(L.stride) ||
      L.func_off + 8u > static_cast<unsigned>(L.stride) || L.name_off + 8u > static_cast<unsigned>(L.stride)) {
    set_status("via.Application's entry fields fall outside its 0x%X-byte entries - no game tick", L.stride);
    return false;
  }
  logf("app: via.Application's entry table: filled at exe+0x%llX (called from exe+0x%llX), count +0x%X, table "
       "+0x%X, 0x%X-byte entries (function +0x%X, name +0x%X, priority +0x%X, group +0x%X); the Application global "
       "is exe+0x%llX",
       static_cast<unsigned long long>(fn - image.begin), static_cast<unsigned long long>(call - image.begin),
       L.count_off, L.table_off, L.stride, L.func_off, L.name_off, L.prio_off, L.group_off,
       static_cast<unsigned long long>(slot - image.begin));
  g_found = true;
  set_status("found; waiting for via.Application");
  return true;
}

bool hook() {
  if (!g_found) return false;
  const uintptr_t app = mem::read_ptr(L.app_slot);
  if (!app) {
    set_status("waiting for via.Application");
    return false;
  }
  uint32_t count = 0;
  if (!mem::read_safe(app + static_cast<uintptr_t>(L.count_off), &count) || !count || count > 1024 ||
      !mem::readable(entry_at(app, 0), static_cast<size_t>(count) * static_cast<size_t>(L.stride))) {
    set_status("waiting for via.Application's entries");
    return false;
  }
  // The engine registers the entries, then sorts them by (group, priority) in
  // place: an entry may only be hooked once the table holds still and is in
  // order, or the sort could move another entry into the slot being written.
  bool sorted = true;
  for (uint32_t i = 1; i < count && sorted; ++i)
    sorted = mem::read<uint16_t>(entry_at(app, i - 1) + L.group_off) < mem::read<uint16_t>(entry_at(app, i) + L.group_off) ||
             (mem::read<uint16_t>(entry_at(app, i - 1) + L.group_off) == mem::read<uint16_t>(entry_at(app, i) + L.group_off) &&
              mem::read<uint16_t>(entry_at(app, i - 1) + L.prio_off) <= mem::read<uint16_t>(entry_at(app, i) + L.prio_off));
  const bool still = count == g_last_count;
  g_last_count = count;
  if (!sorted || !still) {
    set_status("waiting for via.Application's entries to be registered and sorted (%u so far)", count);
    return false;
  }
  int done = 0;
  for (Hook& h : g_hooks) {
    if (h.entry) {
      ++done;
      continue;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uintptr_t entry = entry_at(app, i);
      if (!name_is(entry, h.name)) continue;
      const uintptr_t code = mem::read_ptr(entry + L.func_off);
      if (!g_text.contains(code)) {
        logf("app: %s's function %p is not the game's code - not hooked", h.name, reinterpret_cast<void*>(code));
        break;
      }
      h.original = reinterpret_cast<EntryFn>(code);  // before the store: the game may call it at once
      if (!mem::store<uintptr_t>(entry + L.func_off, reinterpret_cast<uintptr_t>(h.replacement))) {
        logf("app: %s's entry could not be written - not hooked", h.name);
        break;
      }
      h.entry = entry;
      logf("app: %s (entry %u of %u, group %u, priority %u, function exe+0x%llX) now ticks the mod first", h.name, i,
           count, mem::read<uint16_t>(entry + L.group_off), mem::read<uint16_t>(entry + L.prio_off),
           static_cast<unsigned long long>(code - exe_base()));
      ++done;
      break;
    }
  }
  if (done == 2) {
    set_status("UpdateBehavior and EndRendering hooked");
    return true;
  }
  set_status("via.Application has %u entries, but not both UpdateBehavior and EndRendering", count);
  return false;
}

void check() {
  const uintptr_t app = g_found ? mem::read_ptr(L.app_slot) : 0;
  uint32_t count = 0;
  if (!app || !mem::read_safe(app + static_cast<uintptr_t>(L.count_off), &count) || count > 1024) return;
  for (Hook& h : g_hooks) {
    if (!h.entry || (name_is(h.entry, h.name) && mem::read_ptr(h.entry + L.func_off) == reinterpret_cast<uintptr_t>(h.replacement)))
      continue;
    // Moved (the table was sorted again): follow it, and make sure no other
    // entry carries the mod's function.
    uintptr_t found = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uintptr_t entry = entry_at(app, i);
      if (mem::read_ptr(entry + L.func_off) != reinterpret_cast<uintptr_t>(h.replacement)) continue;
      if (name_is(entry, h.name)) {
        found = entry;
      } else {
        mem::store<uintptr_t>(entry + L.func_off, reinterpret_cast<uintptr_t>(h.original));
        logf("ERROR: app: another entry held %s's hook - put its function back", h.name);
      }
    }
    if (!h.moved_logged) {
      h.moved_logged = true;
      logf("app: %s's entry moved in the table (%s)", h.name, found ? "followed" : "no longer hooked");
    }
    h.entry = found;
  }
}

void unhook() {
  const uintptr_t app = g_found ? mem::read_ptr(L.app_slot) : 0;
  for (Hook& h : g_hooks) {
    if (!h.entry || !app) continue;
    if (mem::read_ptr(h.entry + L.func_off) == reinterpret_cast<uintptr_t>(h.replacement))
      mem::store<uintptr_t>(h.entry + L.func_off, reinterpret_cast<uintptr_t>(h.original));
    h.entry = 0;
  }
}

const char* status() { return g_status; }

void log_rates() {
  for (Hook& h : g_hooks) {
    const LONG n = InterlockedExchange(&h.calls, 0);
    if (h.entry && h.rate_logs < 5) {
      ++h.rate_logs;
      logf("app: %s ran %ld times in the last second", h.name, n);
    }
  }
}

}  // namespace re2cc::app
