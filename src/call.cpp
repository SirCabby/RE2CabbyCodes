#include "call.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "dispatch.h"
#include "log.h"
#include "mem.h"
#include "re.h"

namespace re2cc::call {
namespace {

// The VM's helpers (build 11636119: get_thread_context exe+0x1F7D4C0, begin
// frame exe+0x1F777F0, exception handler exe+0x1F7A210, local frame gc
// exe+0x1F790B0, end frame exe+0x1F781F0) and the thread context's fields they
// use (reference count +0x78, frame +0x50, the frame's pending exception +0x18).
struct Vm {
  uintptr_t get_thread_context = 0, begin_frame = 0, unhandled = 0, local_gc = 0, end_frame = 0;
  uint32_t refcount_off = 0, frame_off = 0, exception_off = 0;
} V;

// The item box's methods (all of them, or none), and the difficulty switch's.
struct Methods {
  uintptr_t set_stock = 0, remove_stock = 0, put_shortcut = 0, update_flags = 0, unequip = 0, set_data = 0,
            set_blank = 0;
} M;
uintptr_t g_set_difficulty = 0;
uintptr_t g_unlock_record = 0, g_unlock_rogue = 0;  // the records' achievements (AchievementManager)

std::atomic<bool> g_vm{false}, g_failed{false};
char g_status[240] = "not searched yet";             // the VM's helpers and the item box's calls, or a call that failed
char g_difficulty_status[160] = "not searched yet";  // the difficulty's call
char g_achievements_status[160] = "not searched yet";  // the records' achievement calls
mem::Range g_text;

void set_status(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void set_status(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(g_status, sizeof(g_status), fmt, a);
  va_end(a);
  logf("call: %s", g_status);
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)); }
unsigned long long rva(uintptr_t a) { return static_cast<unsigned long long>(a - exe_base()); }

uintptr_t rel_target(uintptr_t rel32_at) { return rel32_at + 4 + static_cast<intptr_t>(mem::read<int32_t>(rel32_at)); }

// The most common key and its share, or 0.
template <typename K>
K winner(const std::unordered_map<K, int>& votes, int* count, int* total) {
  K best{};
  *count = 0;
  *total = 0;
  for (const auto& [k, n] : votes) {
    *total += n;
    if (n > *count) {
      *count = n;
      best = k;
    }
  }
  return best;
}
template <typename K>
bool clear_winner(const std::unordered_map<K, int>& votes, int min_count, K* out, int* count, int* total) {
  *out = winner(votes, count, total);
  return *count >= min_count && *count * 10 >= *total * 9;
}

// Each E8 call in .text whose target is `target`, handed to fn(call site).
template <typename F>
void each_call_to(uintptr_t target, F fn) {
  const auto* const end = reinterpret_cast<const uint8_t*>(g_text.end) - 5;
  for (auto* c = reinterpret_cast<const uint8_t*>(g_text.begin); c < end; ++c) {
    c = static_cast<const uint8_t*>(std::memchr(c, 0xE8, static_cast<size_t>(end - c)));
    if (!c) break;
    const uintptr_t site = reinterpret_cast<uintptr_t>(c);
    if (rel_target(site + 1) == target) fn(site);
  }
}

bool find_vm_helpers() {
  // get_thread_context: `mov rcx,[vm]; mov edx,-1; call X`, every one the same X.
  std::unordered_map<uintptr_t, int> gtc_votes;
  const auto vm_sites = mem::find_all(g_text, "48 8B 0D ?? ?? ?? ?? BA FF FF FF FF E8", 1024);
  for (const uintptr_t s : vm_sites) ++gtc_votes[rel_target(s + 13)];
  int n = 0, total = 0;
  if (!clear_winner(gtc_votes, 20, &V.get_thread_context, &n, &total) || !g_text.contains(V.get_thread_context)) {
    set_status("the VM's get_thread_context was not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  // begin frame: right after it, `jne +8; mov rcx,rax; call X` (the first reference).
  std::unordered_map<uintptr_t, int> begin_votes;
  for (const uintptr_t s : vm_sites) {
    if (rel_target(s + 13) != V.get_thread_context) continue;
    for (uintptr_t p = s + 17; p < s + 17 + 24; ++p)
      if (mem::matches(p, "75 08 48 8B C8 E8")) {
        ++begin_votes[rel_target(p + 6)];
        break;
      }
  }
  if (!clear_winner(begin_votes, 20, &V.begin_frame, &n, &total) || !g_text.contains(V.begin_frame)) {
    set_status("the VM's frame begin was not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  // The reference count: the field every caller of begin compares with 0 just
  // before it - `cmp dword [reg+d8],0` or `cmp [reg+d8],reg32`, then `jne +8`.
  std::unordered_map<uint32_t, int> rc_votes;
  each_call_to(V.begin_frame, [&](uintptr_t site) {
    if (!mem::matches(site - 5, "75 08 48 8B C8")) return;
    const auto* b = reinterpret_cast<const uint8_t*>(site - 9);  // the 4 bytes before `jne`
    if (b[0] == 0x83 && (b[1] & 0xF8) == 0x78 && b[3] == 0x00) ++rc_votes[b[2]];          // 83 /7 d8 00
    else if (b[1] == 0x39 && (b[2] & 0xC0) == 0x40) ++rc_votes[b[3]];                      // [REX] 39 modrm d8
  });
  if (!clear_winner(rc_votes, 20, &V.refcount_off, &n, &total)) {
    set_status("the thread context's reference count was not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  // The exception handler: `mov rax,[rcx+F]; mov rdx,rcx; mov r8,[rax+E]; mov qword [rax+E],0; mov rcx,[rcx+V]; jmp`.
  const auto unh = mem::find_all(g_text, "48 8B 41 ?? 48 8B D1 4C 8B 40 ?? 48 C7 40 ?? 00 00 00 00 48 8B 49 ?? E9", 4);
  if (unh.size() != 1 || mem::read<uint8_t>(unh[0] + 10) != mem::read<uint8_t>(unh[0] + 14)) {
    set_status("the VM's exception handler was not found (%u matches) - no game calls", static_cast<unsigned>(unh.size()));
    return false;
  }
  V.unhandled = unh[0];
  V.frame_off = mem::read<uint8_t>(unh[0] + 3);
  V.exception_off = mem::read<uint8_t>(unh[0] + 10);
  // Local frame gc and end frame: the last reference - `call handler; mov rcx,reg; call gc; mov rcx,reg; call end`.
  std::unordered_map<uint64_t, int> tail_votes;
  auto mov_rcx = [](uintptr_t p) {  // [REX.W] 8B modrm(11 001 rrr)
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    return (b[0] == 0x48 || b[0] == 0x49) && b[1] == 0x8B && (b[2] & 0xF8) == 0xC8;
  };
  each_call_to(V.unhandled, [&](uintptr_t site) {
    if (!mov_rcx(site + 5) || mem::read<uint8_t>(site + 8) != 0xE8 || !mov_rcx(site + 13) ||
        mem::read<uint8_t>(site + 16) != 0xE8)
      return;
    const uint64_t gc = rel_target(site + 9) - exe_base(), end = rel_target(site + 17) - exe_base();
    ++tail_votes[(gc << 32) | end];
  });
  uint64_t tail = 0;
  if (!clear_winner(tail_votes, 20, &tail, &n, &total)) {
    set_status("the VM's local frame gc and frame end were not found (%d of %d sites agree) - no game calls", n, total);
    return false;
  }
  V.local_gc = exe_base() + static_cast<uintptr_t>(tail >> 32);
  V.end_frame = exe_base() + static_cast<uintptr_t>(tail & 0xFFFFFFFF);
  if (!g_text.contains(V.local_gc) || !g_text.contains(V.end_frame)) {
    set_status("the VM's frame helpers are not in the game's code - no game calls");
    return false;
  }
  logf("call: VM helpers: get_thread_context exe+0x%llX, begin frame exe+0x%llX, exception handler exe+0x%llX, "
       "local frame gc exe+0x%llX, end frame exe+0x%llX; thread context: reference count +0x%X, frame +0x%X "
       "(its exception +0x%X)",
       rva(V.get_thread_context), rva(V.begin_frame), rva(V.unhandled), rva(V.local_gc), rva(V.end_frame),
       V.refcount_off, V.frame_off, V.exception_off);
  return true;
}

// A method's code, by name and parameter types - 0 when the database has no such
// method or it has no code in the game's image.
uintptr_t find_code(uint32_t type, const char* name, std::initializer_list<const char*> params) {
  const uintptr_t code = type ? re::method_code_typed(type, name, params) : 0;
  return code && g_text.contains(code) ? code : 0;
}

bool find_item_box_methods() {
  const uint32_t im = re::find_type("app.ropeway.gamemastering.InventoryManager");
  const uint32_t inv = re::find_type("app.ropeway.survivor.Inventory");
  const uint32_t item = re::find_type("app.ropeway.gamemastering.InventoryManager.ItemData");
  char found[512] = "";
  size_t used = 0;
  auto need = [&](uintptr_t* code, uint32_t type, const char* label, const char* name,
                  std::initializer_list<const char*> params) {
    *code = find_code(type, name, params);
    if (!*code) {
      set_status("%s was not found in the type database, or has no code - the item box makes no game calls", label);
      return false;
    }
    if (used < sizeof(found))
      used += static_cast<size_t>(std::snprintf(found + used, sizeof(found) - used, "%s%s exe+0x%llX", used ? ", " : "",
                                                label, rva(*code)));
    return true;
  };
  if (!need(&M.set_stock, im, "InventoryManager.setStock", "setStock",
            {"System.Int32", "app.ropeway.gamemastering.InventoryManager.StockItem"}) ||
      !need(&M.remove_stock, im, "InventoryManager.removeStock", "removeStock", {"System.Int32"}) ||
      !need(&M.put_shortcut, im, "InventoryManager.putShortcutWeapon", "putShortcutWeapon", {"System.Int32"}) ||
      !need(&M.update_flags, im, "InventoryManager.updateInventoryFlags", "updateInventoryFlags", {}) ||
      !need(&M.unequip, inv, "Inventory.unequipSlot", "unequipSlot", {"System.Int32"}) ||
      !need(&M.set_data, item, "ItemData.setData", "setData", {"app.ropeway.gamemastering.InventoryManager.ItemData"}) ||
      !need(&M.set_blank, item, "ItemData.setBlank", "setBlank", {})) {
    M = Methods{};  // all of them or none: a Take or Store is never half the game's
    return false;
  }
  logf("call: methods: %s", found);
  return true;
}

bool find_difficulty_method() {
  const uint32_t mf = re::find_type("app.ropeway.gamemastering.MainFlowManager");
  g_set_difficulty = find_code(mf, "setDifficulty", {"app.ropeway.gamemastering.MainFlowManager.Difficulty"});
  if (!g_set_difficulty) {
    std::snprintf(g_difficulty_status, sizeof(g_difficulty_status),
                  "MainFlowManager.setDifficulty was not found in the type database, or has no code");
    logf("call: %s - the difficulty cannot be switched", g_difficulty_status);
    return false;
  }
  logf("call: MainFlowManager.setDifficulty exe+0x%llX (the difficulty switch)", rva(g_set_difficulty));
  return true;
}

// The records panel's achievements: the calls RecordManager.clearRecord and the
// Ghost Survivors' end of a run make when a record is earned.
bool find_achievement_methods() {
  const uint32_t am = re::find_type("app.ropeway.gamemastering.AchievementManager");
  g_unlock_record = find_code(am, "unlockRecord", {"app.ropeway.gamemastering.RecordManager.RecordId", "System.Boolean"});
  g_unlock_rogue = find_code(am, "unlockRogueClearRecord", {"app.ropeway.gamemastering.RogueRecordManager.RogueRecordId"});
  if (!g_unlock_record || !g_unlock_rogue) {
    g_unlock_record = g_unlock_rogue = 0;
    std::snprintf(g_achievements_status, sizeof(g_achievements_status),
                  "AchievementManager.unlockRecord / unlockRogueClearRecord were not found in the type database, or have no code");
    logf("call: %s - records switched on unlock no achievement", g_achievements_status);
    return false;
  }
  logf("call: AchievementManager.unlockRecord exe+0x%llX, unlockRogueClearRecord exe+0x%llX (the records' achievements)",
       rva(g_unlock_record), rva(g_unlock_rogue));
  return true;
}

// A call's frame, as the engine's native-to-managed bridges keep it. Opened
// only for a method that was found (`code`).
class Scope {
 public:
  Scope(const char* what, uintptr_t code) : what_(what) {
    if (!code || !g_vm || g_failed || !dispatch::in_tick()) return;
    const uintptr_t vm = re::vm();
    if (!vm) return;
    using GetCtx = uintptr_t (*)(uintptr_t vm, int32_t unk);
    ctx_ = reinterpret_cast<GetCtx>(V.get_thread_context)(vm, -1);
    if (!ctx_ || !mem::writable(ctx_ + V.refcount_off, 4) || !mem::readable(ctx_ + V.frame_off, 8)) {
      logf("call: %s: no VM thread context on thread %lu", what_, GetCurrentThreadId());
      ctx_ = 0;
      return;
    }
    const int32_t refs = mem::read<int32_t>(ctx_ + V.refcount_off);
    if (refs == 0) helper(V.begin_frame);
    mem::store<int32_t>(ctx_ + V.refcount_off, refs + 1);
    if (pending()) helper(V.unhandled);  // one left over from before: the engine reports it here too
  }
  ~Scope() {
    if (!ctx_) return;
    const int32_t refs = mem::read<int32_t>(ctx_ + V.refcount_off) - 1;
    mem::store<int32_t>(ctx_ + V.refcount_off, refs);
    if (refs == 0) {
      if (pending()) helper(V.unhandled);
      helper(V.local_gc);
      helper(V.end_frame);
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  uintptr_t ctx() const { return ctx_; }
  // After the call: true when it returned normally. An exception it left is
  // named in the log, handed to the VM's handler, and ends the mod's calls.
  bool returned() {
    if (!pending()) return true;
    const uintptr_t ex = mem::read_ptr(mem::read_ptr(ctx_ + V.frame_off) + V.exception_off);
    const uint32_t t = re::type_of(ex);
    g_failed = true;
    std::snprintf(g_status, sizeof(g_status), "%s threw %s - game calls are off until the game restarts", what_,
                  t ? re::full_name(t).c_str() : "an exception");
    logf("ERROR: call: %s", g_status);
    helper(V.unhandled);
    return false;
  }

 private:
  bool pending() const {
    const uintptr_t frame = mem::read_ptr(ctx_ + V.frame_off);
    return frame && mem::readable(frame + V.exception_off, 8) && mem::read_ptr(frame + V.exception_off) != 0;
  }
  void helper(uintptr_t fn) const { reinterpret_cast<void (*)(uintptr_t)>(fn)(ctx_); }

  const char* what_;
  uintptr_t ctx_ = 0;
};

}  // namespace

bool init() {
  if (g_vm) return true;
  if (!re::ready() || !re::vm()) {
    set_status("waiting for the VM");
    return false;
  }
  g_text = mem::section(GetModuleHandleA(nullptr), ".text");
  if (!find_vm_helpers()) {
    std::snprintf(g_difficulty_status, sizeof(g_difficulty_status), "%s", g_status);
    std::snprintf(g_achievements_status, sizeof(g_achievements_status), "%s", g_status);
    return false;
  }
  const bool item_box = find_item_box_methods();
  if (find_difficulty_method()) std::snprintf(g_difficulty_status, sizeof(g_difficulty_status), "ready");
  if (find_achievement_methods()) std::snprintf(g_achievements_status, sizeof(g_achievements_status), "ready");
  g_vm = true;
  if (item_box) set_status("ready");
  return true;
}

bool ready() { return g_vm && M.set_stock && !g_failed; }
const char* status() { return g_status; }
bool difficulty_ready() { return g_vm && g_set_difficulty && !g_failed; }
const char* difficulty_status() { return !g_vm || g_failed ? g_status : g_difficulty_status; }
bool achievements_ready() { return g_vm && g_unlock_record && g_unlock_rogue && !g_failed; }
const char* achievements_status() { return !g_vm || g_failed ? g_status : g_achievements_status; }

bool exception_pending(uintptr_t ctx) {
  if (!ctx || !V.frame_off) return false;  // the offsets come with the VM's helpers (init)
  const uintptr_t frame = mem::read_ptr(ctx + V.frame_off);
  return frame && mem::read_ptr(frame + V.exception_off) != 0;
}

bool set_stock(uintptr_t inventory_manager, int slot, uintptr_t item_data) {
  Scope s("InventoryManager.setStock", M.set_stock);
  if (!s.ctx() || !inventory_manager || !item_data) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t, uintptr_t)>(M.set_stock)(s.ctx(), inventory_manager, slot,
                                                                                    item_data);
  return s.returned();
}

bool remove_stock(uintptr_t inventory_manager, int slot) {
  Scope s("InventoryManager.removeStock", M.remove_stock);
  if (!s.ctx() || !inventory_manager) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(M.remove_stock)(s.ctx(), inventory_manager, slot);
  return s.returned();
}

bool put_shortcut_weapon(uintptr_t inventory_manager, int slot) {
  Scope s("InventoryManager.putShortcutWeapon", M.put_shortcut);
  if (!s.ctx() || !inventory_manager) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(M.put_shortcut)(s.ctx(), inventory_manager, slot);
  return s.returned();
}

bool update_inventory_flags(uintptr_t inventory_manager) {
  Scope s("InventoryManager.updateInventoryFlags", M.update_flags);
  if (!s.ctx() || !inventory_manager) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(M.update_flags)(s.ctx(), inventory_manager);
  return s.returned();
}

bool unequip_slot(uintptr_t inventory, int slot) {
  Scope s("Inventory.unequipSlot", M.unequip);
  if (!s.ctx() || !inventory) return false;
  reinterpret_cast<bool (*)(uintptr_t, uintptr_t, int32_t)>(M.unequip)(s.ctx(), inventory, slot);
  return s.returned();
}

bool copy_item(uintptr_t dest_item_data, uintptr_t src_item_data) {
  Scope s("ItemData.setData", M.set_data);
  if (!s.ctx() || !dest_item_data || !src_item_data) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t)>(M.set_data)(s.ctx(), dest_item_data, src_item_data);
  return s.returned();
}

bool blank_item(uintptr_t item_data) {
  Scope s("ItemData.setBlank", M.set_blank);
  if (!s.ctx() || !item_data) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(M.set_blank)(s.ctx(), item_data);
  return s.returned();
}

bool set_difficulty(uintptr_t main_flow, int difficulty) {
  Scope s("MainFlowManager.setDifficulty", g_set_difficulty);
  if (!s.ctx() || !main_flow) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(g_set_difficulty)(s.ctx(), main_flow, difficulty);
  return s.returned();
}

bool unlock_record_achievement(uintptr_t achievement_manager, int record_id) {
  Scope s("AchievementManager.unlockRecord", g_unlock_record);
  if (!s.ctx() || !achievement_manager) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t, bool)>(g_unlock_record)(s.ctx(), achievement_manager,
                                                                                  record_id, true);
  return s.returned();
}

bool unlock_rogue_achievement(uintptr_t achievement_manager, int rogue_record_id) {
  Scope s("AchievementManager.unlockRogueClearRecord", g_unlock_rogue);
  if (!s.ctx() || !achievement_manager) return false;
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, int32_t)>(g_unlock_rogue)(s.ctx(), achievement_manager,
                                                                           rogue_record_id);
  return s.returned();
}

}  // namespace re2cc::call
