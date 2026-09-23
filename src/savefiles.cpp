#include "savefiles.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include "call.h"
#include "log.h"
#include "mem.h"
#include "proxy.h"
#include "re.h"
#include "savefiles_rules.h"
#include "switch_eval.h"

namespace re2cc::savefiles {
namespace {

// --- names, found once (discover) -------------------------------------------------------------------
int g_missing = 0;
char g_status[200] = "Not searched yet.";  // why nothing can change, for the panel (under g_cs once init() ran)

uint32_t need_type(const char* name) {
  const uint32_t t = re::find_type(name);
  if (!t) {
    logf("save files: class %s not found", name);
    ++g_missing;
  }
  return t;
}

re::Field need_field(uint32_t t, const char* name) {
  if (!t) {
    ++g_missing;
    return {};
  }
  const re::Field f = re::find_field(t, name);
  if (!f.valid()) {
    logf("save files: %s.%s not found", re::full_name(t).c_str(), name);
    ++g_missing;
  }
  return f;
}

template <typename T>
bool get(uintptr_t obj, const re::Field& f, T* out) {
  const uintptr_t a = obj && f.valid() ? re::field_addr(obj, f) : 0;
  return a && mem::read_safe(a, out);
}

template <typename T>
bool put(uintptr_t obj, const re::Field& f, const T& v) {
  const uintptr_t a = obj && f.valid() ? re::field_addr(obj, f) : 0;
  return a && mem::store(a, v);
}

uintptr_t ref(uintptr_t obj, const re::Field& f) {
  const uintptr_t a = obj && f.valid() ? re::field_addr(obj, f) : 0;
  return a ? mem::read_ptr(a) : 0;
}

// The Load Game screen: gui.LoadBehavior, whose fields and update its base
// class SaveLoadBaseBehavior declares.
struct Screen {
  uint32_t load = 0, base = 0;
  uint32_t update = 0;  // SaveLoadBaseBehavior.update() (method index; LoadBehavior inherits it)
  re::Field f_details, f_texts, f_mode, f_loaded, f_saved, f_keep;
  int scenario = 1;     // SaveDataManager.SaveMode.SCENARIO: the main game's list
  bool ok = false;
} Scr;

// The classes the list is made of. via.storage.saveService.SaveFileDetail is an
// engine object with no fields in the database (its getters' code says where
// they read, see Code); the game's list holds one per slot, or null for a slot
// with no save (SaveDataManager.getSaveFileDetailList adds what the engine's
// table has for each slot of the mode, null included).
struct Types {
  uint32_t detail = 0, list = 0, text_lists = 0, texts = 0, string = 0;
} T;

// SaveDataManager: the request fields requestRemoveGameData sets, the others
// get_IsBusy reads, its step, and the user its list of slots was read for.
struct Saves {
  uint32_t mgr = 0;
  re::Field inst, f_save, f_load, f_remove, f_remove_all, f_slot, f_step, f_user;
  int idle[3] = {0, 1, 15};  // SaveLoadStep INITIALIZE, REQUEST_WAIT, PS5_CROSSSAVE_DIALOG: get_IsBusy's "not busy"
  bool ok = false;
} S;

uint32_t g_service = 0;  // via.storage.saveService.SaveService
int g_user_count = 16;   // via.UserIndex: User0..User15 are users (Reserved, Max, System, Invalid follow)

// What comes out of the game's code (resolve_code): SaveFileDetail's getters
// (Slot, Title, SubTitle, Detail, LastUpdateTimeStamp, UseSize, and the slot an
// empty detail reads), System.String's (its length and UTF-16 characters), the
// main game's list - its first slot and length, SaveDataManager.getSaveDataIndex
// (SCENARIO, 0) and getSaveListCount(SCENARIO) - and the two refresh calls.
// Written once, by the tick, and published by g_code_ok: only read after that.
struct Code {
  int32_t slot = -1, title = -1, subtitle = -1, detail = -1, stamp = -1, use_size = -1;
  int32_t invalid_slot = 0;
  int32_t length = -1, chars = -1;
  int32_t first = -1, count = 0;
  uintptr_t set_details = 0;  // SaveLoadBaseBehavior.set_SaveFileDetailList: a reference store the VM counts
  uintptr_t refresh = 0;      // SaveService.updateSaveFileDetailTbl(via.UserIndex), static
};
Code C;
std::atomic<bool> g_code_ok{false};

// Steam's remote storage, the API the engine's save service keeps the slots in
// (the original steam_api64.dll's flat functions).
struct Steam {
  void* (*storage)() = nullptr;  // SteamAPI_SteamRemoteStorage_v014
  bool (*write)(void*, const char*, const void*, int32_t) = nullptr;
  int32_t (*read)(void*, const char*, void*, int32_t) = nullptr;
  bool (*remove)(void*, const char*) = nullptr;
  int32_t (*size)(void*, const char*) = nullptr;
  int32_t (*count)(void*) = nullptr;
  const char* (*name_and_size)(void*, int, int32_t*) = nullptr;
  bool ok = false;
} St;

// --- state shared with the panel, the hook and the copy thread -----------------------------------------------
std::atomic<bool> g_discovered{false};
CRITICAL_SECTION g_cs;  // g_rows, g_queue, the status lines
bool g_cs_ready = false;

struct Rows {
  int count = 0;
  int first_slot = -1;
  bool consistent = false;  // laid out as the game's code says, every entry a detail of its own slot
  Row row[kMaxRows];
  DWORD taken = 0;  // when it was read (GetTickCount)
};
Rows g_rows;  // the list as last read (the hook writes it, under g_cs)

char g_note[240] = {};
bool g_note_error = false;

struct Req {
  bool copy = false;
  int from = -1, to = -1;  // rows (a delete: `to`)
  Row from_seen, to_seen;
};
constexpr int kQueue = 4;
Req g_queue[kQueue];
int g_nq = 0;

// The screen's frames (the hook): when it was last seen at all, last seen open on
// the main game's list with no load under way (the panel shows), and last seen
// with its list read (a change can be asked for - the list is null while the
// game reads it again).
std::atomic<DWORD> g_seen{0}, g_open{0}, g_input{0};

bool recent(const std::atomic<DWORD>& at, DWORD now) {
  const DWORD t = at.load(std::memory_order_relaxed);
  return t && now - t < 400;
}
bool screen_seen() { return recent(g_seen, GetTickCount()); }  // it has frames: it is not closed
bool list_ready() { return screen_seen() && recent(g_input, GetTickCount()); }

// A refresh the tick asked for, carried out by the hook; the user it is for.
std::atomic<bool> g_refresh_want{false};
std::atomic<DWORD> g_refreshed{0};  // when the hook made it
std::atomic<int> g_refresh_user{-1};
std::atomic<bool> g_refresh_broken{false};  // a refresh call left an exception, or was not there: no more this session
// The hook's own: the list it last read, and a read it owes (after a refresh).
uintptr_t g_snap_list = 0, g_snap_texts = 0;
DWORD g_snap_at = 0;
std::atomic<bool> g_snap_logged{false};  // the rows are logged once each time the screen comes up
std::atomic<bool> g_odd_logged{false};   // the list not laid out as the code says (logged once)
std::atomic<bool> g_snap_want{false};
std::atomic<bool> g_busy{false};  // for the panel: a change under way, or the game's saves busy

void set_status(const char* text) {
  if (g_cs_ready) EnterCriticalSection(&g_cs);
  std::snprintf(g_status, sizeof(g_status), "%s", text);
  if (g_cs_ready) LeaveCriticalSection(&g_cs);
}

void note(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void note(bool error, const char* fmt, ...) {
  char text[240];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(text, sizeof(text), fmt, a);
  va_end(a);
  logf("save files: %s", text);
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  std::snprintf(g_note, sizeof(g_note), "%s", text);
  g_note_error = error;
  LeaveCriticalSection(&g_cs);
}

const char* slot_name(int slot, char* buf, size_t n) {
  if (slot == 0) std::snprintf(buf, n, "the auto-save");
  else if (slot > 0) std::snprintf(buf, n, "slot %d", slot);
  else std::snprintf(buf, n, "that slot");
  return buf;
}

// --- the game's code -----------------------------------------------------------------------------------------
// The getters' code, decoded by savefiles_rules.h.
int32_t getter_offset(uintptr_t code, bool wide) {
  uint8_t b[10] = {};
  return code && mem::copy_from(b, code, sizeof(b)) ? rules::getter_offset(b, sizeof(b), wide) : -1;
}

bool getter_constant(uintptr_t code, int32_t* out) {
  uint8_t b[6] = {};
  return code && mem::copy_from(b, code, sizeof(b)) && rules::getter_constant(b, sizeof(b), out);
}

int32_t chars_offset(uintptr_t code) {
  uint8_t b[48] = {};
  return code && mem::copy_from(b, code, sizeof(b)) ? rules::chars_offset(b, sizeof(b)) : -1;
}

// A static method of SaveDataManager over the SaveMode (a jump table), run for
// the main game's list without calling it (switch_eval.h): the mode in rdx, the
// offset (when it takes one) in r8.
bool eval_list(uintptr_t fn, bool with_offset, int32_t* out) {
  const HMODULE exe = GetModuleHandleA(nullptr);
  const mem::Range text = mem::section(exe, ".text"), image = mem::module_range(exe);
  return fn && code::eval_switch(
                   fn, 2, static_cast<uint64_t>(Scr.scenario),
                   [](uintptr_t at, uint8_t* o, size_t n) { return mem::copy_from(o, at, n); },
                   [&](uintptr_t at) { return text.contains(at); }, [&](uintptr_t at) { return image.contains(at); }, out,
                   with_offset ? 8 : -1, 0);
}

// Everything the mod reads out of the game's code, into a copy, published at
// once. The classes are found by name at start-up; their code is the runtime's
// to link, and the engine's own via.* classes are the first the mod reads code
// of - so this runs from the tick, and only there, until it reads.
bool resolve_code(bool log_failure) {
  Code c;
  auto getter = [](const char* name) { return T.detail ? re::method_code(T.detail, name, 0) : 0; };
  c.slot = getter_offset(getter("get_Slot"), false);
  c.title = getter_offset(getter("get_Title"), true);
  c.subtitle = getter_offset(getter("get_SubTitle"), true);
  c.detail = getter_offset(getter("get_Detail"), true);
  c.stamp = getter_offset(getter("get_LastUpdateTimeStamp"), true);
  c.use_size = getter_offset(getter("get_UseSize"), true);
  const bool invalid = getter_constant(getter("get_InvalidSlot"), &c.invalid_slot);
  const bool details = c.slot > 0 && c.title > 0 && c.subtitle > 0 && c.detail > 0 && c.stamp > 0 && invalid;
  c.length = getter_offset(T.string ? re::method_code(T.string, "get_Length", 0) : 0, false);
  c.chars = chars_offset(T.string ? re::method_code_typed(T.string, "get_Chars", {"System.Int32"}) : 0);
  const bool strings = c.length > 0 && c.chars > 0;
  const char* const kMode = "app.ropeway.gamemastering.SaveDataManager.SaveMode";
  const bool layout =
      eval_list(re::method_code_typed(S.mgr, "getSaveDataIndex", {kMode, "System.Int32"}, true), true, &c.first) &&
      eval_list(re::method_code_typed(S.mgr, "getSaveListCount", {kMode}, true), false, &c.count) && c.first >= 0 &&
      c.count > 0 && c.count <= kMaxRows && c.first + c.count - 1 <= rules::kMaxSlot;
  c.set_details = re::method_code_typed(Scr.base, "set_SaveFileDetailList",
                                        {"System.Collections.Generic.List`1<via.storage.saveService.SaveFileDetail>"});
  c.refresh = g_service ? re::method_code_typed(g_service, "updateSaveFileDetailTbl", {"via.UserIndex"}, true) : 0;
  const bool ok = details && strings && layout && c.set_details && c.refresh;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto rva = [base](uintptr_t a) { return a > base ? static_cast<unsigned long long>(a - base) : 0ull; };
  if (ok || log_failure)
    logf("save files: %s: details %d (slot +0x%X, strings +0x%X/+0x%X/+0x%X, stamp +0x%X, size +0x%X, empty = 0x%X), "
         "strings %d (+0x%X/+0x%X), the main game's list %d (slots %d-%d), set_SaveFileDetailList exe+0x%llX, "
         "SaveService.updateSaveFileDetailTbl exe+0x%llX",
         ok ? "the game's code read" : "ERROR: the game's code not understood after a minute (still trying)", details,
         c.slot, c.title, c.subtitle, c.detail, c.stamp, c.use_size, static_cast<uint32_t>(c.invalid_slot), strings,
         c.length, c.chars, layout, c.first, c.first + c.count - 1, rva(c.set_details), rva(c.refresh));
  if (!ok) return false;
  C = c;
  set_status("");
  g_code_ok.store(true, std::memory_order_release);
  return true;
}

bool code_ok() { return g_code_ok.load(std::memory_order_acquire); }

// The rows are the list as the game has it now: no refresh pending, and read
// since the last one (a list dropped for a refresh keeps its old rows on the
// panel until the game has read it again).
bool list_current(const Rows& rows) {
  if (g_refresh_want.load()) return false;
  const DWORD made = g_refreshed.load();
  return !made || (rows.taken && rows.taken != made && rows.taken - made < 0x80000000u);
}
bool names_ok() { return g_discovered.load(std::memory_order_acquire) && Scr.ok && S.ok; }

// --- reading the game's list ------------------------------------------------------------------------------------
// A managed string as UTF-8 (cut at `n`, on a character boundary). false: not a string.
bool read_string(uintptr_t s, char* out, size_t n) {
  if (n) out[0] = '\0';
  if (!s || !re::is_a(s, T.string)) return false;
  int32_t len = 0;
  if (!mem::read_safe(s + static_cast<uintptr_t>(C.length), &len) || len < 0 || len > 4096) return false;
  wchar_t w[512];
  const int take = len < 511 ? len : 511;
  if (take && !mem::copy_from(w, s + static_cast<uintptr_t>(C.chars), static_cast<size_t>(take) * 2)) return false;
  char u[1600];
  const int got = take ? WideCharToMultiByte(CP_UTF8, 0, w, take, u, sizeof(u) - 1, nullptr, nullptr) : 0;
  size_t m = got > 0 ? static_cast<size_t>(got) : 0;
  if (m >= n) {
    m = n - 1;
    while (m > 0 && (static_cast<uint8_t>(u[m]) & 0xC0) == 0x80) --m;  // not inside a character
  }
  std::memcpy(out, u, m);
  out[m] = '\0';
  return true;
}

// One row of the list (slot `slot`): the engine's detail of the slot (null: no
// save) and the strings the game's row shows. false: the entry is not what the
// list is made of - not a SaveFileDetail, or a detail of another slot.
bool read_row(uintptr_t detail, uintptr_t texts, int slot, Row* r) {
  *r = Row{};
  r->slot = slot;
  int32_t own = C.invalid_slot;
  if (detail) {
    if (!re::is_a(detail, T.detail)) return false;
    mem::read_safe(detail + static_cast<uintptr_t>(C.slot), &own);
  }
  r->used = detail && own != C.invalid_slot;
  char title[256] = {}, detail_text[512] = {};
  long long stamp = 0, size = -1;
  if (r->used) {
    read_string(mem::read_ptr(detail + static_cast<uintptr_t>(C.title)), title, sizeof(title));
    read_string(mem::read_ptr(detail + static_cast<uintptr_t>(C.subtitle)), r->subtitle, sizeof(r->subtitle));
    read_string(mem::read_ptr(detail + static_cast<uintptr_t>(C.detail)), detail_text, sizeof(detail_text));
    mem::read_safe(detail + static_cast<uintptr_t>(C.stamp), &stamp);
    if (C.use_size >= 0) mem::read_safe(detail + static_cast<uintptr_t>(C.use_size), &size);
    r->unix_time = rules::unix_seconds(stamp);
    r->size = size;
    re::List tl;
    if (texts && re::is_a(texts, T.texts) && re::read_list(texts, &tl))
      for (int i = 0; i < tl.count && r->n_texts < kMaxTexts; ++i)
        read_string(re::list_ref(tl, i), r->texts[r->n_texts++], sizeof(r->texts[0]));
  }
  // What the save is: its slot, whether there is one, the engine's three strings
  // (a copy of a save has the same), and the time stamp.
  uint32_t h = rules::fnv1a(&slot, sizeof(slot));
  h = rules::fnv1a(&r->used, sizeof(r->used), h);
  h = rules::fnv1a(title, std::strlen(title), h);
  h = rules::fnv1a(r->subtitle, std::strlen(r->subtitle), h);
  h = rules::fnv1a(detail_text, std::strlen(detail_text), h);
  r->hash = rules::fnv1a(&stamp, sizeof(stamp), h);
  return !r->used || own == slot;
}

// What identifies a save apart from its slot and time: the engine's strings. A
// copy must read the same.
bool same_contents(const Row& a, const Row& b) {
  if (!a.used || !b.used || std::strcmp(a.subtitle, b.subtitle) != 0 || a.n_texts != b.n_texts) return false;
  for (int i = 0; i < a.n_texts; ++i)
    if (std::strcmp(a.texts[i], b.texts[i]) != 0) return false;
  return true;
}

void log_row(const char* what, const Row& r) {
  char when[40] = "?";
  if (r.unix_time > 0) {
    const time_t t = static_cast<time_t>(r.unix_time);
    if (const tm* g = std::gmtime(&t)) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", g);
  }
  char texts[400] = {};
  size_t n = 0;
  for (int i = 0; i < r.n_texts && n + 4 < sizeof(texts); ++i)
    n += static_cast<size_t>(std::snprintf(texts + n, sizeof(texts) - n, "%s'%s'", i ? " " : "", r.texts[i]));
  if (!r.used) logf("save files: %s: slot %d, no save", what, r.slot);
  else
    logf("save files: %s: slot %d, '%s', saved %s, %lld byte(s); the game's row: %s", what, r.slot, r.subtitle, when,
         r.size, texts);
}

// The screen's list, as the game's code lays it out: row i is slot first + i,
// `count` rows. A list that is not - another length, an entry that is not a
// SaveFileDetail, a detail of another slot - is shown, but no change goes by it.
void snapshot(uintptr_t list, uintptr_t texts) {
  re::List l, tl;
  if (!re::is_a(list, T.list) || !re::read_list(list, &l)) return;
  const bool have_texts = texts && re::is_a(texts, T.text_lists) && re::read_list(texts, &tl) && tl.count == l.count;
  auto rows = std::make_unique<Rows>();
  rows->count = l.count < kMaxRows ? l.count : kMaxRows;
  rows->first_slot = C.first;
  int odd = l.count == C.count ? -1 : l.count;  // the first row that does not fit (the count, when that is wrong)
  for (int i = 0; i < rows->count; ++i)
    if (!read_row(re::list_ref(l, i), have_texts ? re::list_ref(tl, i) : 0, C.first + i, &rows->row[i]) && odd < 0)
      odd = i;
  rows->consistent = odd < 0;
  rows->taken = GetTickCount() | 1;
  if (odd >= 0 && !g_odd_logged.exchange(true))
    logf("save files: the game's list is not laid out as its code says (%d rows, the code: %d from slot %d; row %d does "
         "not fit) - no change goes by it",
         l.count, C.count, C.first, odd);
  if (!g_snap_logged.exchange(true)) {
    logf("save files: the game's list has %d row(s) (%d list(s) of strings), slots %d-%d", rows->count,
         have_texts ? tl.count : -1, rows->first_slot, rows->first_slot + rows->count - 1);
    for (int i = 0; i < rows->count; ++i)
      if (rows->row[i].used) log_row("the game's list", rows->row[i]);
  }
  EnterCriticalSection(&g_cs);
  g_rows = *rows;
  LeaveCriticalSection(&g_cs);
}

// The engine's table of the slots read again, and the screen's list dropped so
// its own update reads the table again and rebuilds its rows. From the hook: the
// screen's own thread, inside the game's frame (ctx is the hook's).
void refresh(uintptr_t ctx, uintptr_t screen) {
  if (!C.set_details || !C.refresh) {
    g_refresh_broken = true;
    logf("ERROR: save files: the refresh calls are not there - no more refreshes this session");
    return;
  }
  const int user = g_refresh_user.load();
  if (user >= 0 && user < g_user_count) {
    reinterpret_cast<void (*)(uintptr_t, int32_t)>(C.refresh)(ctx, user);
    if (call::exception_pending(ctx)) {
      g_refresh_broken = true;
      logf("ERROR: save files: SaveService.updateSaveFileDetailTbl left an exception - no more refreshes this session");
      return;
    }
  } else {
    logf("save files: the user the game's list was read for (%d) is no user - only the screen's list is read again", user);
  }
  reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t)>(C.set_details)(ctx, screen, 0);
  if (call::exception_pending(ctx)) {
    g_refresh_broken = true;
    logf("ERROR: save files: set_SaveFileDetailList left an exception - no more refreshes this session");
    return;
  }
  g_refresh_want = false;
  g_refreshed = GetTickCount() | 1;
  g_snap_want = true;
  logf("save files: the game's list is being read again (SaveService.updateSaveFileDetailTbl(User%d), the screen's "
       "list dropped)",
       user);
}

// --- the game's saves ----------------------------------------------------------------------------------------
uintptr_t save_manager() {
  const uintptr_t at = S.ok ? re::static_addr(S.inst) : 0;
  const uintptr_t obj = at ? mem::read_ptr(at) : 0;
  return obj && re::is_a(obj, S.mgr) ? obj : 0;
}

// The manager's requests and step (false: not read).
struct Requests {
  uint8_t save = 1, load = 1, remove = 1, remove_all = 1;
  int32_t slot = -1, step = -1;
  bool idle_step = false;
};
bool read_requests(uintptr_t sdm, Requests* q) {
  if (!sdm || !get(sdm, S.f_save, &q->save) || !get(sdm, S.f_load, &q->load) || !get(sdm, S.f_remove, &q->remove) ||
      !get(sdm, S.f_remove_all, &q->remove_all) || !get(sdm, S.f_slot, &q->slot) || !get(sdm, S.f_step, &q->step))
    return false;
  q->idle_step = false;
  for (int v : S.idle) q->idle_step = q->idle_step || q->step == v;
  return true;
}

// SaveDataManager.get_IsBusy: a request pending, or a step between the idle ones.
bool saves_busy(uintptr_t sdm) {
  Requests q;
  return !read_requests(sdm, &q) || q.save || q.load || q.remove || q.remove_all || !q.idle_step;
}

// The four request flags and SlotId side by side in one aligned qword (this
// build: the manager's fields +0x38..+0x3F): when the layout says so, the delete
// is armed with one compare-exchange from "no request" to "remove `slot`", so no
// request of the game's can land in between. `tried`: the layout allowed it.
bool arm_delete_atomic(uintptr_t sdm, int slot, bool* tried) {
  *tried = false;
  const uintptr_t a = re::field_addr(sdm, S.f_save);
  if (!a || (a & 7) || re::field_addr(sdm, S.f_load) != a + 1 || re::field_addr(sdm, S.f_remove) != a + 2 ||
      re::field_addr(sdm, S.f_remove_all) != a + 3 || re::field_addr(sdm, S.f_slot) != a + 4)
    return false;
  *tried = true;
  uint64_t before = 0;
  if (!mem::read_safe(a, &before) || (before & 0xFFFFFFFFull)) return false;  // a request pending
  // Little-endian: SaveRequest, LoadRequest, RemoveRequest, RemoveAllRequest, then SlotId.
  const uint64_t after = (static_cast<uint64_t>(static_cast<uint32_t>(slot)) << 32) | (1ull << 16);
  return mem::exchange_ptr(a, static_cast<uintptr_t>(before), static_cast<uintptr_t>(after));
}

// The game's delete, armed the way requestRemoveGameData arms it - but only
// while the manager is idle, and atomically where the layout allows (above).
// Otherwise field by field, the slot first, read again just before and just
// after: a request of the game's own (a load picked in the same instant) shares
// SlotId, and had one come in between, the delete is withdrawn at once and the
// log says so, rather than left to run with another request's slot.
bool arm_delete(uintptr_t sdm, int slot) {
  Requests q;
  if (!read_requests(sdm, &q) || q.save || q.load || q.remove || q.remove_all || !q.idle_step) {
    note(true, "The game is busy with its saves - nothing was deleted; try again in a moment.");
    return false;
  }
  bool tried = false;
  const bool armed = arm_delete_atomic(sdm, slot, &tried);
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    logf("save files: the delete is armed %s", tried ? "with one compare-exchange (the requests and SlotId share a qword)"
                                                     : "field by field (the requests and SlotId are not one qword)");
  }
  if (tried) {
    if (armed && read_requests(sdm, &q) && q.remove && q.slot == slot) return true;
    if (armed) {
      logf("save files: the delete was armed but reads back otherwise (remove %d, SlotId %d)", q.remove, q.slot);
      note(true, "The game's delete did not read back as asked - see the log.");
    } else {
      note(true, "The game started a save or load at the same moment - nothing was deleted.");
    }
    return false;
  }
  if (!put<int32_t>(sdm, S.f_slot, slot)) {
    note(true, "The game's delete could not be asked for - nothing was changed.");
    return false;
  }
  if (!read_requests(sdm, &q) || q.save || q.load || q.remove || q.remove_all || q.slot != slot) {
    logf("save files: the game made a request of its own while the delete was armed (save %d, load %d, SlotId %d) - "
         "the delete was not armed",
         q.save, q.load, q.slot);
    note(true, "The game started a save or load at the same moment - nothing was deleted.");
    return false;
  }
  if (!put<uint8_t>(sdm, S.f_remove, 1)) {
    note(true, "The game's delete could not be asked for - nothing was changed.");
    return false;
  }
  if (!read_requests(sdm, &q) || q.save || q.load || q.remove_all || q.slot != slot) {
    put<uint8_t>(sdm, S.f_remove, 0);
    logf("save files: the game made a request of its own as the delete was armed (save %d, load %d, SlotId %d) - "
         "withdrawn",
         q.save, q.load, q.slot);
    note(true, "The game started a save or load at the same moment - the delete was withdrawn.");
    return false;
  }
  return true;
}

// --- the copy (a thread of its own: Steam's remote storage, no game memory) -------------------------------------
struct Job {
  int from_slot = -1, to_slot = -1;
  int first = -1, rows = 0;
  bool used[kMaxRows] = {};
  std::atomic<bool> done{false};
  bool ok = false;
  bool changed = false;  // the target's file changed (the game's list is read again either way)
  char msg[240] = {};
};

void job_fail(Job* j, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void job_fail(Job* j, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(j->msg, sizeof(j->msg), fmt, a);
  va_end(a);
  j->ok = false;
}

bool steam_read(void* rs, const std::string& name, std::vector<uint8_t>* out) {
  const int32_t size = St.size(rs, name.c_str());
  if (size <= 0 || size > (64 << 20)) return false;
  out->assign(static_cast<size_t>(size), 0);
  return St.read(rs, name.c_str(), out->data(), size) == size;
}

void run_copy(Job* j) {
  char a[32], b[32];
  slot_name(j->from_slot, a, sizeof(a));
  slot_name(j->to_slot, b, sizeof(b));
  void* rs = St.storage ? St.storage() : nullptr;
  if (!rs) return job_fail(j, "Steam's remote storage is not available - nothing was changed.");
  // Steam's files, beside the game's list: the names must be the game's.
  auto files = std::make_unique<rules::SteamFiles>();
  const int32_t n = St.count(rs);
  for (int32_t i = 0; i < n; ++i) {
    int32_t size = 0;
    const char* name = St.name_and_size(rs, i, &size);
    if (name) files->add(name, size);
  }
  if (files->mixed_dirs || files->dir.empty())
    return job_fail(j, "Steam's save files are not laid out as the game's (see the log) - nothing was changed.");
  const int bad = rules::first_mismatch(*files, j->first, j->used, j->rows);
  if (bad >= 0) {
    const int s = j->first + bad;
    logf("save files: Steam has %d file(s); slot %d: the game's list says %s, Steam %s (%s)", n, s,
         j->used[bad] ? "a save" : "empty", s >= 0 && s <= rules::kMaxSlot && files->exists[s] ? "has its file" : "has none",
         rules::file_name(files->dir, s).c_str());
    return job_fail(j, "Steam's save files do not match the game's list (slot %d) - nothing was changed.", s);
  }
  const std::string from = files->name[j->from_slot];
  const bool had = files->exists[j->to_slot];
  const std::string to = had ? files->name[j->to_slot] : rules::file_name(files->dir, j->to_slot);
  std::vector<uint8_t> data, before, back;
  if (!steam_read(rs, from, &data))
    return job_fail(j, "%s could not be read from Steam (%s) - nothing was changed.", a, from.c_str());
  if (had && !steam_read(rs, to, &before))
    return job_fail(j, "%s could not be read from Steam (%s) - nothing was changed.", b, to.c_str());
  logf("save files: copying %s -> %s (%llu bytes, through Steam's remote storage)%s", from.c_str(), to.c_str(),
       static_cast<unsigned long long>(data.size()), had ? ", replacing the save there" : "");
  if (!St.write(rs, to.c_str(), data.data(), static_cast<int32_t>(data.size())))
    return job_fail(j, "Steam refused to write %s (%s) - nothing was changed.", b, to.c_str());
  j->changed = true;
  // Read back: what the game will load is what Steam holds.
  if (!steam_read(rs, to, &back) || back != data) {
    const bool put_back = had ? St.write(rs, to.c_str(), before.data(), static_cast<int32_t>(before.size()))
                              : St.remove(rs, to.c_str());
    logf("save files: %s did not read back as written - %s", to.c_str(), put_back ? "put back as it was" : "NOT put back");
    return job_fail(j, "%s did not read back as the copy - %s.", b,
                    put_back ? "it was put back as it was" : "it could not be put back; check the saves in the game");
  }
  j->ok = true;
  std::snprintf(j->msg, sizeof(j->msg), "%s was copied to %s.", a, b);
}

DWORD WINAPI copy_thread(LPVOID p) {
  Job* j = static_cast<Job*>(p);
  run_copy(j);
  j->done.store(true, std::memory_order_release);
  return 0;
}

// --- the change under way (the tick's) -----------------------------------------------------------------------
// A delete: kRemoving (the game's request) -> kRefreshing. A copy: kCopying (the
// thread) -> kSettling (the game's saves idle again) -> kRefreshing. The list,
// read again, says how it went - unless the change already failed, whose note
// stays.
enum Phase { kIdle = 0, kRemoving, kCopying, kSettling, kRefreshing };
struct Op {
  Phase phase = kIdle;
  bool copy = false;
  int from_row = -1, to_row = -1;
  int from_slot = -1, to_slot = -1;
  Row from_seen, to_seen;
  DWORD since = 0;       // this phase began
  bool failed = false;   // its note is an error that stands
  bool waited = false;   // the copy's "still waiting" was said
  Job* job = nullptr;
} g_op;

void finish_op() {
  if (g_op.job) {
    if (g_op.job->done.load(std::memory_order_acquire)) delete g_op.job;  // else the thread still has it (never freed)
    g_op.job = nullptr;
  }
  g_op.phase = kIdle;
}

void enter(Phase p, DWORD now) {
  g_op.phase = p;
  g_op.since = now;
  if (p == kRefreshing) g_refresh_want = true;
}

// Row i of the game's list is slot first_slot + i, the list as the code lays it out.
bool slot_fits(const Rows& rows, int i) {
  return rows.consistent && i >= 0 && i < rows.count && rows.row[i].slot == rows.first_slot + i &&
         rows.row[i].slot >= 0 && rows.row[i].slot <= rules::kMaxSlot;
}

// A request is taken only while the screen is up, the game's saves are idle, the
// list is laid out as the code says, and the rows it names still hold what the
// panel showed.
bool check_request(const Req& r, const Rows& rows, bool up, bool busy) {
  if (!up) {
    note(true, "The Load Game screen is not up any more - nothing was changed.");
    return false;
  }
  if (g_op.phase != kIdle) {
    note(true, "A change is still under way - nothing else was changed.");
    return false;
  }
  if (busy) {
    note(true, "The game is busy with its saves - try again in a moment.");
    return false;
  }
  if (r.to < 0 || r.to >= rows.count || (r.copy && (r.from < 0 || r.from >= rows.count || r.from == r.to))) {
    note(true, "That is not a slot of the game's list - nothing was changed.");
    return false;
  }
  if (!same_save(rows.row[r.to], r.to_seen) || (r.copy && !same_save(rows.row[r.from], r.from_seen))) {
    note(true, "The saves changed since that was clicked - nothing was changed.");
    return false;
  }
  if (!slot_fits(rows, r.to) || (r.copy && !slot_fits(rows, r.from))) {
    note(true, "The game's list is not laid out as expected (see the log) - nothing was changed.");
    return false;
  }
  return true;
}

void start_delete(const Req& r, const Rows& rows, uintptr_t sdm, DWORD now) {
  const Row& row = rows.row[r.to];
  char s[32];
  slot_name(row.slot, s, sizeof(s));
  if (!row.used) {
    note(true, "%s is already empty.", s);
    return;
  }
  if (!arm_delete(sdm, row.slot)) return;
  log_row("deleting (SaveDataManager.RemoveRequest)", row);
  g_op = Op{};
  g_op.to_row = r.to;
  g_op.to_slot = row.slot;
  g_op.to_seen = row;
  enter(kRemoving, now);
  note(false, "Deleting %s...", s);
}

void start_copy(const Req& r, const Rows& rows, DWORD now) {
  const Row& from = rows.row[r.from];
  const Row& to = rows.row[r.to];
  if (!from.used) {
    note(true, "That slot has no save to copy.");
    return;
  }
  if (!St.ok) {
    note(true, "Steam's remote storage was not found - saves cannot be copied (see the log).");
    return;
  }
  auto* j = new Job;
  j->from_slot = from.slot;
  j->to_slot = to.slot;
  j->first = rows.first_slot;
  j->rows = rows.count;
  for (int i = 0; i < rows.count; ++i) j->used[i] = rows.row[i].used;
  HANDLE t = CreateThread(nullptr, 0, copy_thread, j, 0, nullptr);
  if (!t) {
    delete j;
    note(true, "The copy could not be started (error %lu) - nothing was changed.", GetLastError());
    return;
  }
  CloseHandle(t);
  log_row("copying from", from);
  if (to.used) log_row("copying over", to);
  g_op = Op{};
  g_op.copy = true;
  g_op.from_row = r.from;
  g_op.to_row = r.to;
  g_op.from_slot = from.slot;
  g_op.to_slot = to.slot;
  g_op.from_seen = from;
  g_op.to_seen = to;
  g_op.job = j;
  enter(kCopying, now);
  char a[32], b[32];
  note(false, "Copying %s to %s...", slot_name(from.slot, a, sizeof(a)), slot_name(to.slot, b, sizeof(b)));
}

// After the change, the list the game read again says how it went (the row the
// change was made to - the list's layout does not move).
void check_result(const Rows& rows) {
  char a[32], b[32];
  slot_name(g_op.from_slot, a, sizeof(a));
  slot_name(g_op.to_slot, b, sizeof(b));
  const Row* got = slot_fits(rows, g_op.to_row) ? &rows.row[g_op.to_row] : nullptr;
  if (got) log_row("the target, as the game lists it now", *got);
  // A delete: the list read again is the truth, even after a withdrawal (the game
  // may have taken the request in the same instant).
  if (!g_op.copy) {
    if (got && !got->used) note(false, "%s was deleted.", b);
    else if (!g_op.failed) note(true, "The game did not delete %s (see the log).", b);
    return;
  }
  if (g_op.failed) return;  // a copy's error already said stands
  if (got && same_contents(*got, g_op.from_seen)) {
    note(false, "%s was copied to %s.", a, b);
  } else {
    note(true, "%s was written, but the game's list does not show it as the copy (see the log).", b);
  }
}

void service_op(DWORD now, uintptr_t sdm, const Rows& rows) {
  char s[32];
  slot_name(g_op.to_slot, s, sizeof(s));
  switch (g_op.phase) {
    case kIdle:
      return;
    case kRemoving: {
      Requests q;
      const bool read = read_requests(sdm, &q);
      if (read && !q.remove && !saves_busy(sdm) && now - g_op.since > 250) {
        logf("save files: the game's delete of slot %d is done", g_op.to_slot);
        enter(kRefreshing, now);
      } else if (read && q.remove && q.idle_step && now - g_op.since > 10000) {
        // Never left armed: a request the game took later would delete the slot
        // long after the panel said it had not. The list, read again, says what
        // happened.
        put<uint8_t>(sdm, S.f_remove, 0);
        g_op.failed = true;
        note(true, "The game did not take the delete of %s in 10 s - it was withdrawn (see the log).", s);
        enter(kRefreshing, now);
      } else if (now - g_op.since > 60000) {
        // Taken but not finished (or not readable): disarmed all the same, so it
        // cannot run later against whatever SlotId holds then.
        if (read && q.remove) put<uint8_t>(sdm, S.f_remove, 0);
        note(true, "The game has not finished deleting %s after a minute (step %d) - see the log.", s, q.step);
        g_refresh_want = true;  // the list is read again the next time the screen has a frame
        finish_op();
      }
      return;
    }
    case kCopying: {
      Job* j = g_op.job;
      if (!j || !j->done.load(std::memory_order_acquire)) {
        if (!g_op.waited && now - g_op.since > 60000) {
          g_op.waited = true;
          note(false, "Still waiting for Steam to write %s...", s);
        }
        return;
      }
      if (j->ok) {
        logf("save files: %s", j->msg);
        enter(kSettling, now);
      } else {
        note(true, "%s", j->msg);
        g_op.failed = true;
        if (j->changed) enter(kSettling, now);  // what Steam holds now, whatever it is
        else finish_op();
      }
      return;
    }
    case kSettling:
      // The list is read again only while the game's saves are idle: a load the
      // player started meanwhile goes first (the screen then closes, and the
      // next one reads the list itself).
      if (sdm && !saves_busy(sdm) && now - g_op.since > 250) {
        enter(kRefreshing, now);
      } else if (now - g_op.since > 60000) {
        if (!g_op.failed) note(false, "Done - the game's list shows it the next time the Load Game screen opens.");
        g_refresh_want = true;
        finish_op();
      }
      return;
    case kRefreshing: {
      if (g_refresh_broken.load()) {
        if (!g_op.failed) note(true, "Done, but the game's list could not be read again - reopen the Load Game screen.");
        finish_op();
        return;
      }
      const DWORD made = g_refreshed.load();
      const bool read_again = !g_refresh_want.load() && made && rows.taken && rows.taken - made < 0x80000000u &&
                              rows.taken != made && !g_snap_want.load();
      if (read_again && list_ready()) {
        check_result(rows);
        finish_op();
      } else if (!screen_seen() && now - g_op.since > 1000) {
        // The screen closed first (no frames of it): the next time it opens, the
        // hook reads the game's list again before anything else.
        if (!g_op.failed) note(false, "Done - the game's list shows it the next time the Load Game screen opens.");
        finish_op();
      } else if (now - g_op.since > 15000) {
        if (!g_op.failed)
          note(true, "The game's list was not read again after 15 s - reopen the Load Game screen to see the change.");
        finish_op();
      }
      return;
    }
  }
}

}  // namespace

// SaveDataManager.get_IsBusy as a tri-state, for the tick (savefiles.h).
int game_saves_busy() {
  const uintptr_t sdm = save_manager();
  Requests q;
  if (!sdm || !read_requests(sdm, &q)) return -1;
  return q.save || q.load || q.remove || q.remove_all || !q.idle_step ? 1 : 0;
}

// --- set-up ---------------------------------------------------------------------------------------------------
void init() {
  InitializeCriticalSection(&g_cs);
  g_cs_ready = true;
}

bool discover() {
  g_missing = 0;
  // The screen.
  Scr.load = need_type("app.ropeway.gui.LoadBehavior");
  Scr.base = need_type("app.ropeway.gui.SaveLoadBaseBehavior");
  Scr.update = Scr.base ? re::find_method(Scr.base, "update", {}) : 0;
  Scr.f_details = need_field(Scr.base, "<SaveFileDetailList>k__BackingField");
  Scr.f_texts = need_field(Scr.base, "<SaveFileDetailTextList>k__BackingField");
  Scr.f_mode = need_field(Scr.base, "SaveModeValue");
  Scr.f_loaded = need_field(Scr.base, "IsLoaded");
  Scr.f_saved = need_field(Scr.base, "IsSaved");
  Scr.f_keep = need_field(Scr.base, "<KeepRequest>k__BackingField");
  const uint32_t mode = re::find_type("app.ropeway.gamemastering.SaveDataManager.SaveMode");
  if (mode) Scr.scenario = re::enum_value(mode, "SCENARIO", Scr.scenario);
  const bool setter =
      Scr.base && re::find_method(Scr.base, "set_SaveFileDetailList",
                                  {"System.Collections.Generic.List`1<via.storage.saveService.SaveFileDetail>"});
  if (!setter) {
    logf("save files: SaveLoadBaseBehavior.set_SaveFileDetailList(List<SaveFileDetail>) not found");
    ++g_missing;
  }
  Scr.ok = Scr.load && Scr.base && re::derives(Scr.load, Scr.base) && Scr.update &&
           re::method_vt_index(Scr.update) >= 0 && Scr.f_details.valid() && Scr.f_texts.valid() && Scr.f_mode.valid() &&
           Scr.f_loaded.valid() && Scr.f_saved.valid() && Scr.f_keep.valid() && setter;

  // The engine's slot details and the lists the screen keeps them in.
  T.detail = need_type("via.storage.saveService.SaveFileDetail");
  T.list = need_type("System.Collections.Generic.List`1<via.storage.saveService.SaveFileDetail>");
  T.text_lists = need_type("System.Collections.Generic.List`1<System.Collections.Generic.List`1<System.String>>");
  T.texts = need_type("System.Collections.Generic.List`1<System.String>");
  T.string = need_type("System.String");

  // The game's saves, and the engine's table of the slots.
  S.mgr = need_type("app.ropeway.gamemastering.SaveDataManager");
  S.inst = need_field(S.mgr, "_Instance");
  S.f_save = need_field(S.mgr, "SaveRequest");
  S.f_load = need_field(S.mgr, "LoadRequest");
  S.f_remove = need_field(S.mgr, "RemoveRequest");
  S.f_remove_all = need_field(S.mgr, "RemoveAllRequest");
  S.f_slot = need_field(S.mgr, "SlotId");
  S.f_step = need_field(S.mgr, "<saveLoadStep>k__BackingField");
  S.f_user = need_field(S.mgr, "<LastDetailUserIndex>k__BackingField");
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.SaveDataManager.SaveLoadStep")) {
    S.idle[0] = re::enum_value(e, "INITIALIZE", S.idle[0]);
    S.idle[1] = re::enum_value(e, "REQUEST_WAIT", S.idle[1]);
    S.idle[2] = re::enum_value(e, "PS5_CROSSSAVE_DIALOG", S.idle[2]);
  }
  const char* const kMode = "app.ropeway.gamemastering.SaveDataManager.SaveMode";
  const bool layout = S.mgr && re::find_method(S.mgr, "getSaveDataIndex", {kMode, "System.Int32"}, true) &&
                      re::find_method(S.mgr, "getSaveListCount", {kMode}, true);
  if (!layout) {
    logf("save files: SaveDataManager.getSaveDataIndex / getSaveListCount not found");
    ++g_missing;
  }
  S.ok = S.mgr && S.inst.valid() && S.f_save.valid() && S.f_load.valid() && S.f_remove.valid() &&
         S.f_remove_all.valid() && S.f_slot.valid() && S.f_step.valid() && S.f_user.valid() && mode && layout;
  g_service = need_type("via.storage.saveService.SaveService");
  if (g_service && !re::find_method(g_service, "updateSaveFileDetailTbl", {"via.UserIndex"}, true)) {
    logf("save files: SaveService.updateSaveFileDetailTbl(UserIndex) not found");
    ++g_missing;
  }
  if (const uint32_t u = re::find_type("via.UserIndex")) g_user_count = re::enum_value(u, "Reserved", g_user_count);

  // Steam's remote storage: the original DLL's flat API.
  if (HMODULE orig = proxy::original()) {
    auto fn = [orig](const char* name) { return reinterpret_cast<void*>(GetProcAddress(orig, name)); };
    St.storage = reinterpret_cast<decltype(St.storage)>(fn("SteamAPI_SteamRemoteStorage_v014"));
    St.write = reinterpret_cast<decltype(St.write)>(fn("SteamAPI_ISteamRemoteStorage_FileWrite"));
    St.read = reinterpret_cast<decltype(St.read)>(fn("SteamAPI_ISteamRemoteStorage_FileRead"));
    St.remove = reinterpret_cast<decltype(St.remove)>(fn("SteamAPI_ISteamRemoteStorage_FileDelete"));
    St.size = reinterpret_cast<decltype(St.size)>(fn("SteamAPI_ISteamRemoteStorage_GetFileSize"));
    St.count = reinterpret_cast<decltype(St.count)>(fn("SteamAPI_ISteamRemoteStorage_GetFileCount"));
    St.name_and_size = reinterpret_cast<decltype(St.name_and_size)>(fn("SteamAPI_ISteamRemoteStorage_GetFileNameAndSize"));
  }
  St.ok = St.storage && St.write && St.read && St.remove && St.size && St.count && St.name_and_size;
  if (!St.ok) logf("save files: Steam's remote storage functions not found in the original steam_api64.dll - no Copy");

  const bool names = Scr.ok && S.ok && g_missing == 0;
  set_status(names ? "Reading the game's code..." : "The game's save list was not understood in this build (see the log).");
  logf("save files: discovery (%d name(s) missing): screen %d (update [%u] virtual[%d]), saves %d, users %d, Steam "
       "remote storage %d; the game's code is read from the tick",
       g_missing, Scr.ok, Scr.update, Scr.update ? re::method_vt_index(Scr.update) : -1, S.ok, g_user_count, St.ok);
  g_discovered.store(true, std::memory_order_release);
  return names;
}

bool ready() { return names_ok() && code_ok(); }
const char* status() { return g_status; }

uint32_t screen_class() { return Scr.ok ? Scr.load : 0; }
uint32_t screen_update() { return Scr.ok ? Scr.update : 0; }

void screen_frame(uintptr_t ctx, uintptr_t screen) {
  // The screen's own fields are found by name: it is known up (and the panel
  // says why nothing can change) even before - or without - the code read.
  if (!names_ok() || !screen || !re::is_a(screen, Scr.load)) return;
  const bool code = code_ok();
  const DWORD now = GetTickCount() | 1;
  const DWORD last = g_seen.exchange(now);
  int32_t mode = -1;
  uint8_t loaded = 0, saved = 0, keep = 0;
  get(screen, Scr.f_mode, &mode);
  get(screen, Scr.f_loaded, &loaded);
  get(screen, Scr.f_saved, &saved);
  get(screen, Scr.f_keep, &keep);
  // Open: the main game's list, no load under way.
  const bool open = mode == Scr.scenario && !loaded && !saved && !keep;
  if (open) g_open.store(now);
  // A refresh a change asked for - never while a load is starting.
  if (code && open && g_refresh_want.load() && !g_refresh_broken.load()) refresh(ctx, screen);
  const uintptr_t list = ref(screen, Scr.f_details);
  const uintptr_t texts = ref(screen, Scr.f_texts);
  if (open && list) g_input.store(now);  // taking input: its list read
  // The rows: when the list is another one, after a refresh, when the screen
  // comes back, and twice a second while it is up.
  if (code && mode == Scr.scenario && list &&
      (list != g_snap_list || texts != g_snap_texts || g_snap_want.load() || now - last > 400 || now - g_snap_at >= 500)) {
    g_snap_want = false;
    g_snap_list = list;
    g_snap_texts = texts;
    g_snap_at = now;
    snapshot(list, texts);
  }
}

bool screen_open() { return screen_seen() && recent(g_open, GetTickCount()); }

View view() {
  View v;
  v.ready = ready();
  if (!g_cs_ready) return v;
  const bool open = screen_open() && list_ready();
  v.copy_ok = St.ok;
  v.busy = g_busy.load();
  EnterCriticalSection(&g_cs);
  v.up = open && list_current(g_rows);
  v.first_slot = g_rows.first_slot;
  v.count = g_rows.count;
  v.consistent = g_rows.consistent;
  for (int i = 0; i < g_rows.count; ++i) v.row[i] = g_rows.row[i];
  if (!v.ready) {
    std::snprintf(v.status, sizeof(v.status), "%s", g_status);
    v.status_error = true;
  } else if (g_note[0]) {
    std::snprintf(v.status, sizeof(v.status), "%s", g_note);
    v.status_error = g_note_error;
  }
  LeaveCriticalSection(&g_cs);
  return v;
}

void request_delete(int row, const Row& seen) {
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  if (g_nq < kQueue) g_queue[g_nq++] = Req{false, -1, row, Row{}, seen};
  LeaveCriticalSection(&g_cs);
}

void request_copy(int from_row, int to_row, const Row& from_seen, const Row& to_seen) {
  if (!g_cs_ready) return;
  EnterCriticalSection(&g_cs);
  if (g_nq < kQueue) g_queue[g_nq++] = Req{true, from_row, to_row, from_seen, to_seen};
  LeaveCriticalSection(&g_cs);
}

void tick() {
  if (!g_discovered.load(std::memory_order_acquire) || !g_cs_ready) return;
  static bool s_open = false;
  static DWORD s_polled = 0, s_code_tried = 0;
  static int s_code_tries = 0;
  const DWORD now = GetTickCount();
  // The game's code: here only (one thread writes it), once a second until it reads.
  if (names_ok() && !code_ok() && s_code_tries < 600 && (!s_code_tries || now - s_code_tried >= 1000)) {
    s_code_tried = now;
    ++s_code_tries;
    // After a minute of tries the reason is logged (once); after ten, no more.
    if (!resolve_code(s_code_tries == 60) && s_code_tries == 1)
      logf("save files: the game's code is not linked yet - read again once a second");
    if (!code_ok() && s_code_tries == 600) {
      logf("save files: the game's code was not understood in ten minutes - no save files this session");
      set_status("The game's save list was not understood in this build (see the log).");
    }
  }
  const bool open = screen_open();
  if (open != s_open) {
    logf("save files: the Load Game screen is %s", open ? "up" : "closed");
    s_open = open;
    if (open) g_snap_logged = false;  // the list is logged once each time the screen comes up
  }
  Req q[kQueue];
  EnterCriticalSection(&g_cs);
  const int nq = g_nq;
  std::memcpy(static_cast<void*>(q), g_queue, sizeof(Req) * static_cast<size_t>(nq));
  g_nq = 0;
  LeaveCriticalSection(&g_cs);
  // The game's saves: read at most four times a second, and only while the
  // screen is up or a change is under way.
  if (!nq && !open && g_op.phase == kIdle) return;
  if (nq == 0 && now - s_polled < 250) return;
  s_polled = now;
  if (!ready()) {
    char why[sizeof(g_status)];
    EnterCriticalSection(&g_cs);
    std::snprintf(why, sizeof(why), "%s", g_status);
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < nq; ++i) note(true, "%s", why);
    return;
  }
  const uintptr_t sdm = save_manager();
  const bool busy = !sdm || saves_busy(sdm);
  int32_t user = -1;
  if (sdm && get(sdm, S.f_user, &user)) g_refresh_user = user;
  static Rows rows;  // the tick's
  EnterCriticalSection(&g_cs);
  rows = g_rows;
  LeaveCriticalSection(&g_cs);
  const bool up = open && list_ready() && list_current(rows);  // a change can be asked for
  for (int i = 0; i < nq; ++i) {
    const Req& r = q[i];
    if (!check_request(r, rows, up, busy)) continue;
    if (r.copy) start_copy(r, rows, now);
    else start_delete(r, rows, sdm, now);
  }
  service_op(now, sdm, rows);
  g_busy = busy || g_op.phase != kIdle;
}

}  // namespace re2cc::savefiles
