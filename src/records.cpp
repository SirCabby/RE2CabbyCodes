#include "records.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "call.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "records_rules.h"
#include "records_text.h"
#include "switch_eval.h"

namespace re2cc::records {
namespace {

// --- names, found once (discover) -------------------------------------------------------------------
int g_missing = 0;

uint32_t need_type(const char* name) {
  const uint32_t t = re::find_type(name);
  if (!t) {
    logf("records: class %s not found", name);
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
    logf("records: %s.%s not found", re::full_name(t).c_str(), name);
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

uintptr_t instance(const re::Field& inst, uint32_t t) {
  const uintptr_t at = inst.valid() ? re::static_addr(inst) : 0;
  const uintptr_t obj = at ? mem::read_ptr(at) : 0;
  return obj && re::is_a(obj, t) ? obj : 0;
}

// An array field whose elements are `size` bytes each (a bool[] 1, an int[] 4).
bool array_of(uintptr_t obj, const re::Field& f, uint32_t size, re::Array* out) {
  const uintptr_t arr = ref(obj, f);
  return arr && re::read_array(arr, out) && out->elem_size == size;
}
bool flag(const re::Array& a, int i) {
  uint8_t b = 0;
  return i >= 0 && i < a.count && mem::read_safe(a.data + static_cast<uintptr_t>(i), &b) && b != 0;
}
bool set_flag(const re::Array& a, int i, bool v) {
  return i >= 0 && i < a.count && mem::store<uint8_t>(a.data + static_cast<uintptr_t>(i), v ? 1 : 0);
}
int number(const re::Array& a, int i) {
  int32_t v = -1;
  return i >= 0 && i < a.count && mem::read_safe(a.data + static_cast<uintptr_t>(i) * 4, &v) ? v : -1;
}
bool set_number(const re::Array& a, int i, int v) {
  return i >= 0 && i < a.count && mem::store<int32_t>(a.data + static_cast<uintptr_t>(i) * 4, v);
}

// The keys of a Dictionary<enum, enum> held in a static field (AchievementDefine's maps).
void dictionary_keys(uint32_t t, const char* field, const char* dict_type, const char* entry_type, bool* out, int n) {
  const re::Field f = t ? re::find_field(t, field) : re::Field{};
  const uintptr_t at = f.valid() ? re::static_addr(f) : 0;
  const uintptr_t dict = at ? mem::read_ptr(at) : 0;
  const uint32_t dt = re::find_type(dict_type), et = re::find_type(entry_type);
  if (!dict || !dt || !et || !re::is_a(dict, dt)) {
    static int s_logged = 0;  // the tick's: once for each of the two dictionaries
    if (s_logged++ < 2) logf("records: %s not readable yet - which records carry an achievement is not shown", field);
    return;
  }
  const re::Field f_entries = re::find_field(dt, "_entries"), f_count = re::find_field(dt, "_count");
  const re::Field f_hash = re::find_field(et, "hashCode"), f_key = re::find_field(et, "key");
  re::Array entries;
  int32_t count = 0;
  if (!f_hash.valid() || !f_key.valid() || !get(dict, f_count, &count) || !re::read_array(ref(dict, f_entries), &entries))
    return;
  for (int i = 0; i < count && i < entries.count && i < 1024; ++i) {
    const uintptr_t e = entries.data + static_cast<uintptr_t>(i) * entries.elem_size;
    int32_t hash = -1, key = -1;
    if (mem::read_safe(re::value_field_addr(e, f_hash), &hash) && hash >= 0 &&
        mem::read_safe(re::value_field_addr(e, f_key), &key) && key >= 0 && key < n)
      out[key] = true;
  }
}

// --- the main game: RecordManager --------------------------------------------------------------------
struct Main {
  uint32_t mgr = 0, ssd = 0, node = 0, reward = 0, ids = 0;
  re::Field inst, f_ssd, f_nodes, f_rewards, f_order, f_naomi;
  re::Field f_progress, f_cleared, f_fresh, f_open, f_open_new, f_tofu_popup;
  re::Field f_n_rewards, f_n_progress, f_n_goal, f_n_type;
  re::Field f_w_type, f_w_name;
  re::Field f_i_id;
  int records = 91, rewards = 133, hidden = 3;
  rules::ProgressTypes pt;
  int tofu_first = 7, tofu_last = 10;  // RewardId OPEN_MODE_06..09: the tofu survivors' pop-up
  int raccoon_record[2] = {-1, -1};    // the records fixRecordProgress sets from the raccoons broken
  int reward_type[6] = {1, 2, 3, 4, 5, -1};  // RewardType FIGURE, CONCEPTART, MODE, WEAPON, COSTUME (the panel's labels)
  bool achievement[kMaxRecords] = {};
  bool achievements_read = false;
  bool ok = false;
} M;

// --- The Ghost Survivors: RogueRecordManager --------------------------------------------------------------
struct Rogue {
  uint32_t mgr = 0, ssd = 0, node = 0;
  re::Field inst, f_ssd, f_nodes, f_order, f_limit;
  re::Field f_normal, f_training, f_plays, f_special, f_route_c, f_raccoons, f_cleared, f_fresh, f_open, f_open_new;
  re::Field f_n_reward, f_n_type, f_n_lost, f_n_goal;
  rules::RogueTypes rt;
  int records = 15, rewards = 15;
  rules::Counted counted[kMaxRecords] = {};  // what each COUNT_UP record counts (the check's code)
  int accessory[kMaxRecords];                // RogueRewardId -> SurvivorDefine.Accessory (-2: unknown)
  int accessory_none = -1;                   // SurvivorDefine.Accessory.Invalid
  bool achievement[kMaxRecords] = {};
  bool achievements_read = false;
  bool ok = false;
} R;

// Things the game counts once, a bool each in a system save, that a record's
// progress is recounted from: the Mr. Raccoons ever broken (the main game's and
// The Ghost Survivors') and the safes and dial locks ever opened.
enum Tally : int { kMainRaccoons = 0, kRogueRaccoons, kLocks, kTallies };
struct TallyList {
  uint32_t mgr = 0, ssd = 0;
  re::Field inst, f_ssd, f_list;
  bool ok = false;
} g_tally[kTallies];
int g_lock_record = -1;  // the record GimmickDialLockManager.SetUnlockRecord recounts ("Master of Unlocking")

// The Ghost Survivors' equipped accessories (RogueAccessoryManager's save: a survivor and an accessory each).
struct Accessories {
  uint32_t mgr = 0, ssd = 0, data = 0;
  re::Field inst, f_ssd, f_list, f_id;
  bool ok = false;
} A;

// SaveDataManager: the request fields its requestSaveSystemDataNoSaveIcon sets, and its step.
struct Saves {
  uint32_t mgr = 0;
  re::Field inst, f_save, f_load, f_remove, f_remove_all, f_slot, f_step;
  int idle[3] = {0, 1, 15};  // SaveLoadStep INITIALIZE, REQUEST_WAIT, PS5_CROSSSAVE_DIALOG: get_IsBusy's "not busy"
  bool ok = false;
} S;

uint32_t t_achievements = 0;  // AchievementManager (the calls' `this`)
re::Field i_achievements, f_achievement_service;

struct Screens {
  uint32_t cls[kSets] = {}, update[kSets] = {};
  re::Field input[kSets];  // RecordBehavior / RogueRecordBehavior.EnableInput_: the screen takes input
} Scr;

// --- state shared with the panel and the hooks ------------------------------------------------------------
std::atomic<bool> g_discovered{false};  // set last by discover(): the tick waits for it
CRITICAL_SECTION g_cs;  // g_view, g_queue, g_status
View* g_view[kSets] = {};
struct Req {
  Set set = kMain;
  int id = -1;  // -1: all
  bool on = false;
};
constexpr int kQueue = 64;
Req g_queue[kQueue];
int g_nq = 0;
char g_status[kSets][200] = {};
bool g_status_error[kSets] = {};

// The screens' frames (their hooks, on the game's threads): when one was last
// seen, and last seen taking input; where its EnableInput_ is, worked out once
// per screen object.
std::atomic<DWORD> g_screen_seen[kSets], g_screen_input[kSets];
std::atomic<uintptr_t> g_screen_obj[kSets], g_screen_input_at[kSets];

// The Ghost Survivors' records held off (a bit per RogueRecordId), and the file keeping them.
std::atomic<uint32_t> g_held{0};
char g_held_path[MAX_PATH] = {};
bool g_holds_applied = false;  // the tick's: the holds were set once (the log says so once)
// The Ghost Survivors' record table as the game built it: a held record's node
// says COUNT_UP, so the original types are kept, per table (the list object).
int32_t g_orig_type[kMaxRecords];
uintptr_t g_orig_list = 0;

// A save the mod asked for.
bool g_save_pending = false;  // the tick's
std::atomic<bool> g_saving{false};
DWORD g_save_asked = 0;

void note(Set s, bool error, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void note(Set s, bool error, const char* fmt, ...) {
  char text[200];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(text, sizeof(text), fmt, a);
  va_end(a);
  EnterCriticalSection(&g_cs);
  std::snprintf(g_status[s], sizeof(g_status[s]), "%s", text);
  g_status_error[s] = error;
  LeaveCriticalSection(&g_cs);
  logf("records: %s", text);
}

// --- held records' file ----------------------------------------------------------------------------------
void load_held() {
  FILE* f = std::fopen(g_held_path, "r");
  if (!f) return;
  char line[512];
  uint32_t mask = 0;
  while (std::fgets(line, sizeof(line), f)) {
    const char* eq = std::strchr(line, '=');
    if (line[0] == '#' || !eq || std::strncmp(line, "HeldGhostSurvivorsRecords", 25) != 0) continue;
    for (const char* p = eq + 1; *p;) {
      char* end = nullptr;
      const long v = std::strtol(p, &end, 10);
      if (end == p) {
        ++p;
        continue;
      }
      if (v >= 0 && v < 32) mask |= 1u << v;
      p = end;
    }
  }
  std::fclose(f);
  g_held = mask;
  if (mask) logf("records: The Ghost Survivors' records held off (%s): mask 0x%X", g_held_path, mask);
}

void save_held() {
  const uint32_t mask = g_held;
  FILE* f = std::fopen(g_held_path, "w");
  if (!f) {
    logf("records: %s could not be written - the held records are forgotten when the game closes", g_held_path);
    return;
  }
  std::fprintf(f,
               "# RE2CabbyCodes - written by the mod's records panel.\n"
               "# The Ghost Survivors' records switched off while their scenario stays cleared: No Way Out is open only\n"
               "# while Forgotten Soldier, Runaway and No Time To Mourn are cleared, so these are kept off by the mod\n"
               "# (RogueRecordId numbers) instead. Switch them on in the panel to let them go.\n"
               "HeldGhostSurvivorsRecords =");
  for (int i = 0; i < 32; ++i)
    if (mask & (1u << i)) std::fprintf(f, " %d", i);
  std::fprintf(f, "\n");
  std::fclose(f);
}

// --- reward names -----------------------------------------------------------------------------------------
// A reward's name: the message GUID RecordManager gave its RewardNode, looked up
// in the game's text (records_text.h, sorted by GUID).
const char* reward_text(uintptr_t guid_at) {
  uint64_t g[2] = {};
  if (!guid_at || !mem::copy_from(g, guid_at, 16)) return nullptr;
  constexpr size_t kN = sizeof(records_text::kRewardText) / sizeof(records_text::kRewardText[0]);
  size_t lo = 0, hi = kN;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const auto& e = records_text::kRewardText[mid];
    if (e.hi < g[1] || (e.hi == g[1] && e.lo < g[0])) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= kN || records_text::kRewardText[lo].hi != g[1] || records_text::kRewardText[lo].lo != g[0]) return nullptr;
  return records_text::kRewardText[lo].text;
}

// --- the main game's records, read and written ------------------------------------------------------------
struct MainNow {
  uintptr_t mgr = 0, ssd = 0;
  re::Array progress, cleared, fresh, open, open_new;
  re::List nodes, reward_nodes;
  int goal[kMaxRecords] = {}, type[kMaxRecords] = {}, record_type[kMaxRecords] = {};
  int rewards[kMaxRecords * kMaxRewards];
  bool is_cleared[kMaxRecords] = {};
  int n = 0;
};

int list_ints(uintptr_t list, int* out, int max) {
  re::List l;
  if (!list || !re::read_list(list, &l) || (l.count > 0 && l.items.elem_size != 4)) return 0;
  int n = 0;
  for (int i = 0; i < l.count && n < max; ++i) out[n++] = mem::read<int32_t>(l.items.data + static_cast<uintptr_t>(i) * 4);
  return n;
}

bool read_main(MainNow* c) {
  if (!M.ok) return false;
  c->mgr = instance(M.inst, M.mgr);
  c->ssd = c->mgr ? ref(c->mgr, M.f_ssd) : 0;
  if (!c->ssd || !re::is_a(c->ssd, M.ssd) || !array_of(c->ssd, M.f_progress, 4, &c->progress) ||
      !array_of(c->ssd, M.f_cleared, 1, &c->cleared) || !array_of(c->ssd, M.f_fresh, 1, &c->fresh) ||
      !array_of(c->ssd, M.f_open, 1, &c->open) || !array_of(c->ssd, M.f_open_new, 1, &c->open_new) ||
      !re::read_list(ref(c->mgr, M.f_nodes), &c->nodes) || !re::read_list(ref(c->mgr, M.f_rewards), &c->reward_nodes))
    return false;
  c->n = c->nodes.count < M.records ? c->nodes.count : M.records;
  if (c->n > kMaxRecords) c->n = kMaxRecords;
  for (int id = 0; id < c->n; ++id) {
    const uintptr_t node = re::list_ref(c->nodes, id);
    int32_t goal = -1, type = -1, rtype = -1;
    if (!node || !re::is_a(node, M.node) || !get(node, M.f_n_goal, &goal) || !get(node, M.f_n_progress, &type) ||
        !get(node, M.f_n_type, &rtype))
      return false;
    c->goal[id] = goal;
    c->type[id] = type;
    c->record_type[id] = rtype;
    int* r = &c->rewards[id * kMaxRewards];
    for (int k = 0; k < kMaxRewards; ++k) r[k] = -1;
    list_ints(ref(node, M.f_n_rewards), r, kMaxRewards);
    c->is_cleared[id] = flag(c->cleared, id);
  }
  return c->n > 0;
}

bool is_raccoon_record(int id) { return id >= 0 && (id == M.raccoon_record[0] || id == M.raccoon_record[1]); }

// A tally counted down to `keep` (rules::unbreak_to: the last ones first). How many are left after, -1 unreadable.
int untally(Tally t, int keep) {
  static const char* const kWhat[kTallies] = {"Mr. Raccoon(s) of the main game can be broken",
                                              "Mr. Raccoon(s) of The Ghost Survivors can be broken",
                                              "safe(s) and lock(s) count when opened"};
  const TallyList& tl = g_tally[t];
  const uintptr_t mgr = tl.ok ? instance(tl.inst, tl.mgr) : 0;
  const uintptr_t ssd = mgr ? ref(mgr, tl.f_ssd) : 0;
  re::Array list;
  if (!ssd || !re::is_a(ssd, tl.ssd) || !array_of(ssd, tl.f_list, 1, &list) || list.count > 256) return -1;
  bool marked[256] = {}, was[256] = {};
  for (int i = 0; i < list.count; ++i) marked[i] = was[i] = flag(list, i);
  const int undone = rules::unbreak_to(marked, list.count, keep);
  int count = 0;
  for (int i = 0; i < list.count; ++i) {
    if (marked[i] != was[i]) set_flag(list, i, marked[i]);
    count += marked[i];
  }
  if (undone > 0) logf("records: %d %s again (%d of %d still counted)", undone, kWhat[t], count, list.count);
  return count;
}

uintptr_t achievement_manager() { return t_achievements ? instance(i_achievements, t_achievements) : 0; }

// A record switched on: its Steam achievement, through the game's own call
// (call.h). The game's AchievementManager returns without a word when its
// achievement service is not up, so that is checked first. true: asked.
bool unlock_achievement(Set s, int id) {
  if (!call::achievements_ready()) {
    static bool s_logged = false;  // the tick's
    if (!s_logged) {
      s_logged = true;
      logf("records: no achievement is unlocked: %s", call::achievements_status());
    }
    return false;
  }
  const uintptr_t am = achievement_manager();
  if (!am || !ref(am, f_achievement_service)) {
    static bool s_logged = false;  // the tick's
    if (!s_logged) {
      s_logged = true;
      logf("records: the game's achievement manager or its service is not up - no achievement asked for");
    }
    return false;
  }
  return s == kMain ? call::unlock_record_achievement(am, id) : call::unlock_rogue_achievement(am, id);
}

const char* record_label(Set s, int id) {
  const char* n = nullptr;
  if (s == kMain && id >= 0 && id < records_text::kRecords) n = records_text::kRecord[id].name;
  if (s == kRogue && id >= 0 && id < records_text::kRogueRecords) n = records_text::kRogueRecord[id].name;
  return n ? n : "(hidden record)";
}

// One record of the main game switched. true: something was written.
bool switch_main(MainNow& c, int id, bool on) {
  if (id < 0 || id >= c.n) return false;
  const rules::Kind kind = rules::kind_of(M.pt, c.type[id]);
  const int goal = c.goal[id], was = number(c.progress, id);
  const bool cleared = flag(c.cleared, id);
  char rewards_done[160] = "";
  size_t used = 0;
  auto noted = [&](const char* fmt, int v) {
    if (used < sizeof(rewards_done)) used += std::snprintf(rewards_done + used, sizeof(rewards_done) - used, fmt, v);
  };
  if (on) {
    const int now = rules::on_progress(kind, was, goal);
    if (now != was) set_number(c.progress, id, now);
    set_flag(c.cleared, id, true);
    if (!cleared) set_flag(c.fresh, id, true);
    c.is_cleared[id] = true;
    for (int k = 0; k < kMaxRewards; ++k) {
      const int r = c.rewards[id * kMaxRewards + k];
      if (r <= 0 || r >= M.rewards || flag(c.open, r)) continue;
      set_flag(c.open, r, true);
      set_flag(c.open_new, r, true);  // shown as new, as a reward is before it is first looked at
      if (r >= M.tofu_first && r <= M.tofu_last) put<uint8_t>(c.ssd, M.f_tofu_popup, 1);
      noted(" %d", r);
    }
    // The game's own call decides whether the record has an achievement (it does
    // nothing for one that has not), as clearRecord does.
    const bool asked = unlock_achievement(kMain, id);
    note(kMain, false, "\"%s\" on (progress %d -> %d%s%s)%s", record_label(kMain, id), was,
         rules::on_progress(kind, was, goal), used ? ", rewards unlocked:" : "", rewards_done,
         !M.achievement[id] ? "" : asked ? " - the game was asked to unlock its Steam achievement"
                                         : " - its Steam achievement could not be asked for (see the log)");
    return true;
  }
  set_flag(c.cleared, id, false);
  set_flag(c.fresh, id, false);
  c.is_cleared[id] = false;
  int now = rules::off_progress(kind, was, goal);
  if (is_raccoon_record(id)) {
    // The raccoon records are recounted from the raccoons broken at every clear
    // (fixRecordProgress): enough of them come back to be broken again.
    const int broken = untally(kMainRaccoons, goal - 1);
    if (broken < 0)
      logf("records: the Mr. Raccoons broken could not be read - the game counts them again at the next clear, and "
           "\"%s\" comes back then", record_label(kMain, id));
    for (int r : M.raccoon_record)
      if (r >= 0 && broken >= 0 && number(c.progress, r) > broken) set_number(c.progress, r, broken);
    if (broken >= 0 && now > broken) now = broken;
  } else if (id == g_lock_record) {
    // "Master of Unlocking" is recounted from the safes and locks ever opened at
    // every one opened (GimmickDialLockManager.SetUnlockRecord): the last one
    // opened counts again when it is opened again.
    const int opened = untally(kLocks, goal - 1);
    if (opened < 0)
      logf("records: the safes and locks opened could not be read - \"%s\" comes back the next time one is opened",
           record_label(kMain, id));
    if (opened >= 0 && now > opened) now = opened;
  }
  if (now != was) set_number(c.progress, id, now);
  for (int k = 0; k < kMaxRewards; ++k) {
    const int r = c.rewards[id * kMaxRewards + k];
    if (r <= 0 || r >= M.rewards || !flag(c.open, r)) continue;
    if (rules::granted_elsewhere(r, id, c.is_cleared, c.rewards, c.n, kMaxRewards)) continue;
    set_flag(c.open, r, false);
    noted(" %d", r);
  }
  note(kMain, false, "\"%s\" off (progress %d -> %d%s%s)", record_label(kMain, id), was, number(c.progress, id),
       used ? ", rewards locked:" : "", rewards_done);
  return true;
}

// --- The Ghost Survivors' records --------------------------------------------------------------------------
struct RogueNow {
  uintptr_t mgr = 0, ssd = 0;
  re::Array normal, training, special, route_c, cleared, fresh, open, open_new;
  re::List nodes;
  int type[kMaxRecords] = {}, lost[kMaxRecords] = {}, goal[kMaxRecords] = {}, reward[kMaxRecords] = {};
  uintptr_t node[kMaxRecords] = {};
  int n = 0;
  rules::RogueSave save;
};

bool read_rogue(RogueNow* c) {
  if (!R.ok) return false;
  c->mgr = instance(R.inst, R.mgr);
  c->ssd = c->mgr ? ref(c->mgr, R.f_ssd) : 0;
  if (!c->ssd || !re::is_a(c->ssd, R.ssd) || !array_of(c->ssd, R.f_normal, 1, &c->normal) ||
      !array_of(c->ssd, R.f_training, 1, &c->training) || !array_of(c->ssd, R.f_special, 1, &c->special) ||
      !array_of(c->ssd, R.f_route_c, 1, &c->route_c) || !array_of(c->ssd, R.f_cleared, 1, &c->cleared) ||
      !array_of(c->ssd, R.f_fresh, 1, &c->fresh) || !array_of(c->ssd, R.f_open, 1, &c->open) ||
      !array_of(c->ssd, R.f_open_new, 1, &c->open_new) || !re::read_list(ref(c->mgr, R.f_nodes), &c->nodes))
    return false;
  c->n = c->nodes.count < R.records ? c->nodes.count : R.records;
  if (c->n > kMaxRecords) c->n = kMaxRecords;
  for (int id = 0; id < c->n; ++id) {
    const uintptr_t node = re::list_ref(c->nodes, id);
    int32_t type = -1, lost = -1, goal = -1, reward = -1;
    if (!node || !re::is_a(node, R.node) || !get(node, R.f_n_type, &type) || !get(node, R.f_n_lost, &lost) ||
        !get(node, R.f_n_goal, &goal) || !get(node, R.f_n_reward, &reward))
      return false;
    c->node[id] = node;
    c->type[id] = type;
    c->lost[id] = lost;
    c->goal[id] = goal;
    c->reward[id] = reward;
  }
  // A table not seen before holds the game's own types (it is built once, as the
  // manager starts, and only the mod ever changes a node).
  if (c->nodes.obj != g_orig_list) {
    for (int id = 0; id < c->n; ++id) g_orig_type[id] = c->type[id];
    g_orig_list = c->nodes.obj;
  }
  rules::RogueSave& s = c->save;
  for (int t = 0; t < 4; ++t) {
    s.normal[t] = flag(c->normal, t);
    s.training[t] = flag(c->training, t);
    s.special[t] = flag(c->special, t);
  }
  s.route_c[0] = flag(c->route_c, 0);
  s.route_c[1] = flag(c->route_c, 1);
  int32_t v = 0;
  s.plays = get(c->ssd, R.f_plays, &v) ? v : 0;
  s.raccoons = get(c->ssd, R.f_raccoons, &v) ? v : 0;
  return c->n > 0;
}

void write_rogue_save(RogueNow& c, const rules::RogueSave& was) {
  const rules::RogueSave& s = c.save;
  for (int t = 0; t < 4; ++t) {
    if (s.normal[t] != was.normal[t]) set_flag(c.normal, t, s.normal[t]);
    if (s.training[t] != was.training[t]) set_flag(c.training, t, s.training[t]);
    if (s.special[t] != was.special[t]) set_flag(c.special, t, s.special[t]);
  }
  for (int k = 0; k < 2; ++k)
    if (s.route_c[k] != was.route_c[k]) set_flag(c.route_c, k, s.route_c[k]);
  if (s.plays != was.plays) put<int32_t>(c.ssd, R.f_plays, s.plays);
  if (s.raccoons != was.raccoons) put<int32_t>(c.ssd, R.f_raccoons, s.raccoons);
}

// The node's type as the game's table has it (a held one's original).
int base_type(const RogueNow& c, int id) { return c.nodes.obj == g_orig_list ? g_orig_type[id] : c.type[id]; }
// The node is held now: COUNT_UP where the table had something else.
bool node_held(const RogueNow& c, int id) { return c.type[id] == R.rt.count_up && base_type(c, id) != R.rt.count_up; }

bool set_hold(RogueNow& c, int id, bool hold) {
  if (id < 0 || id >= c.n || id >= 32 || !c.node[id]) return false;
  const int32_t want = hold ? R.rt.count_up : base_type(c, id);
  if (c.type[id] != want && !put<int32_t>(c.node[id], R.f_n_type, want)) return false;
  c.type[id] = want;
  const uint32_t mask = hold ? (g_held.load() | (1u << id)) : (g_held.load() & ~(1u << id));
  if (mask != g_held.load()) {
    g_held = mask;
    save_held();
  }
  return true;
}

// Every Ghost Survivors survivor wearing the accessory takes it off.
int unequip(int reward) {
  if (!A.ok || reward < 0 || reward >= kMaxRecords || R.accessory[reward] < 0) return 0;
  const uintptr_t mgr = instance(A.inst, A.mgr);
  const uintptr_t ssd = mgr ? ref(mgr, A.f_ssd) : 0;
  re::List l;
  if (!ssd || !re::is_a(ssd, A.ssd) || !re::read_list(ref(ssd, A.f_list), &l)) return 0;
  int n = 0;
  for (int i = 0; i < l.count && i < 64; ++i) {
    const uintptr_t d = re::list_ref(l, i);
    int32_t acc = -1;
    if (d && re::is_a(d, A.data) && get(d, A.f_id, &acc) && acc == R.accessory[reward] &&
        put<int32_t>(d, A.f_id, R.accessory_none))
      ++n;
  }
  return n;
}

bool switch_rogue(RogueNow& c, int id, bool on) {
  if (id < 0 || id >= c.n) return false;
  const int type = base_type(c, id), scenario = c.lost[id], goal = c.goal[id], reward = c.reward[id];
  const rules::Counted counted = R.counted[id];
  const bool cleared = flag(c.cleared, id);
  const rules::RogueSave before = c.save;
  if (!on && type == R.rt.count_up && counted == rules::kNotCounted) {
    // What it counts was not read out of the game's code: switched off, the game's
    // next check would hand it straight back.
    note(kRogue, true, "\"%s\" cannot be switched off in this build - see the log.", record_label(kRogue, id));
    return false;
  }
  if (on) {
    const bool was_held = node_held(c, id);
    if (was_held) set_hold(c, id, false);
    rules::rogue_on(R.rt, type, scenario, goal, counted, &c.save);
    write_rogue_save(c, before);
    set_flag(c.cleared, id, true);
    if (!cleared) set_flag(c.fresh, id, true);
    if (reward >= 0 && !flag(c.open, reward)) {
      set_flag(c.open, reward, true);
      set_flag(c.open_new, reward, true);
    }
    // The game's own call decides whether the record has an achievement (it does
    // nothing for one that has not), as its end of a run does.
    const bool asked = unlock_achievement(kRogue, id);
    note(kRogue, false, "\"%s\" on%s%s", record_label(kRogue, id), was_held ? " (no longer held off)" : "",
         !R.achievement[id] ? "" : asked ? " - the game was asked to unlock its Steam achievement"
                                         : " - its Steam achievement could not be asked for (see the log)");
    return true;
  }
  set_flag(c.cleared, id, false);
  set_flag(c.fresh, id, false);
  if (reward >= 0) {
    bool shared = false;
    for (int j = 0; j < c.n && !shared; ++j) shared = j != id && c.reward[j] == reward && flag(c.cleared, j);
    if (!shared) {
      set_flag(c.open, reward, false);
      set_flag(c.open_new, reward, false);
    }
  }
  const int taken_off = unequip(reward);
  const bool hold = rules::rogue_off(R.rt, type, scenario, goal, counted, &c.save);
  if (counted == rules::kRaccoons) {
    const int broken = untally(kRogueRaccoons, goal - 1);
    if (broken < 0)
      logf("records: The Ghost Survivors' Mr. Raccoons broken could not be read - the game counts them again at the "
           "end of the next run, and \"%s\" comes back then", record_label(kRogue, id));
    // The count the check compares goes short of the goal either way.
    const int keep = broken >= 0 ? broken : goal - 1;
    if (c.save.raccoons > keep) c.save.raccoons = keep > 0 ? keep : 0;
  }
  if (hold) set_hold(c, id, true);
  else write_rogue_save(c, before);
  note(kRogue, false, "\"%s\" off%s%s", record_label(kRogue, id),
       hold ? " - held off by the mod: its scenario stays cleared, so No Way Out stays open" : "",
       taken_off ? " (its accessory is taken off)" : "");
  return true;
}

// The records held off: their nodes set again (the game builds its table as it
// starts), a record the game's check got to first switched off again, and a
// hold let go when No Way Out is shut - with this save, nothing would be locked
// away by earning the record back (another save, a save wiped). false: the
// table could not be read yet (tried again next frame).
bool apply_holds() {
  const uint32_t mask = g_held;
  if (!mask || !R.ok) return true;
  static RogueNow c;  // the tick's alone
  if (!read_rogue(&c)) return false;
  const bool nwo = rules::no_way_out_open(c.save);
  bool corrected = false;
  for (int id = 0; id < c.n && id < 32; ++id) {
    if (!(mask & (1u << id))) continue;
    const int type = base_type(c, id);
    if (!nwo || !rules::can_hold(R.rt, type, c.lost[id], R.counted[id])) {
      set_hold(c, id, false);
      logf("records: \"%s\" is no longer held off: %s", record_label(kRogue, id),
           !nwo ? "No Way Out is not open with this save - it can be earned again by play"
                : "it is not a record the mod holds");
      continue;
    }
    if (flag(c.cleared, id)) {
      // The game's check ran before the hold was in place (a restart, a check at the
      // title): switched off again, as the panel left it.
      set_flag(c.cleared, id, false);
      set_flag(c.fresh, id, false);
      const int reward = c.reward[id];
      bool shared = false;
      for (int j = 0; j < c.n && !shared; ++j) shared = j != id && c.reward[j] == reward && flag(c.cleared, j);
      if (reward >= 0 && !shared) {
        set_flag(c.open, reward, false);
        set_flag(c.open_new, reward, false);
      }
      unequip(reward);
      corrected = true;
      logf("records: \"%s\" was given back by the game before its hold was set - switched off again",
           record_label(kRogue, id));
    }
    if (!node_held(c, id)) {
      set_hold(c, id, true);
      if (!g_holds_applied) logf("records: \"%s\" held off (No Way Out stays open)", record_label(kRogue, id));
    }
  }
  g_holds_applied = true;
  if (corrected) g_save_pending = true;
  return true;
}

// --- the panel's snapshot ------------------------------------------------------------------------------------
void snapshot_main(View* v) {
  static MainNow c;  // the tick's alone
  v->known = read_main(&c);
  if (!v->known) {
    std::snprintf(v->status, sizeof(v->status), "The records could not be read - see the log.");
    v->status_error = true;
    return;
  }
  if (!M.achievements_read) {
    dictionary_keys(re::find_type("app.ropeway.gamemastering.AchievementDefine"), "Record2AchievementID",
                    "System.Collections.Generic.Dictionary`2<app.ropeway.gamemastering.RecordManager.RecordId,"
                    "app.ropeway.gamemastering.AchievementDefine.ID>",
                    "System.Collections.Generic.Dictionary`2.Entry<app.ropeway.gamemastering.RecordManager.RecordId,"
                    "app.ropeway.gamemastering.AchievementDefine.ID>",
                    M.achievement, kMaxRecords);
    int n = 0;
    for (bool b : M.achievement) n += b;
    M.achievements_read = n > 0;
    if (n) logf("records: %d of the main game's records carry a Steam achievement", n);
  }
  uint8_t naomi = 0;
  v->all_rewards = get(c.mgr, M.f_naomi, &naomi) && naomi;
  // The game's order: RecordIdList (its screen's), then the hidden records it does not list.
  int order[kMaxRecords], n = 0;
  bool placed[kMaxRecords] = {};
  re::List ids;
  if (re::read_list(ref(c.mgr, M.f_order), &ids))
    for (int i = 0; i < ids.count && n < c.n; ++i) {
      int32_t id = -1;
      const uintptr_t e = re::list_ref(ids, i);
      if (e && get(e, M.f_i_id, &id) && id >= 0 && id < c.n && !placed[id] && c.record_type[id] != M.hidden) {
        placed[id] = true;
        order[n++] = id;
      }
    }
  for (int id = 0; id < c.n; ++id)
    if (!placed[id] && c.record_type[id] != M.hidden) order[n++] = id, placed[id] = true;
  v->listed = n;
  for (int id = 0; id < c.n; ++id)
    if (!placed[id]) order[n++] = id;
  v->count = n;
  v->cleared = 0;
  int unnamed = 0;
  for (int i = 0; i < n; ++i) {
    const int id = order[i];
    if (id < 0 || id >= c.n || id >= kMaxRecords) continue;
    Row& r = v->row[i];
    r = Row{};
    r.id = id;
    r.cleared = flag(c.cleared, id);
    r.fresh = flag(c.fresh, id);
    r.hidden = c.record_type[id] == M.hidden;
    r.achievement = M.achievement[id];
    r.progress = number(c.progress, id);
    r.goal = c.goal[id];
    r.kind = rules::kind_of(M.pt, c.type[id]);
    if (id < records_text::kRecords) {
      r.name = records_text::kRecord[id].name;
      r.condition = records_text::kRecord[id].condition;
    }
    v->cleared += r.cleared && !r.hidden;
    for (int k = 0; k < kMaxRewards; ++k) {
      const int w = c.rewards[id * kMaxRewards + k];
      if (w <= 0 || w >= M.rewards) continue;
      Reward& rw = r.reward[r.n_rewards++];
      rw.id = w;
      rw.open = flag(c.open, w);
      rw.shared = rules::granted_elsewhere(w, id, c.is_cleared, c.rewards, c.n, kMaxRewards);
      const uintptr_t node = re::list_ref(c.reward_nodes, w);
      int32_t type = -1;
      if (node && re::is_a(node, M.reward) && get(node, M.f_w_type, &type)) rw.type = type;
      if (node && re::is_a(node, M.reward)) rw.name = reward_text(re::field_addr(node, M.f_w_name));
    }
    for (int k = 0; k < r.n_rewards; ++k) unnamed += r.reward[k].name == nullptr;
    if (id == 0)
      r.note = "The game gives this one back by itself whenever every other listed record is complete (the next "
               "time the records screen opens).";
    else if (is_raccoon_record(id))
      r.note = "Switching it off makes enough Mr. Raccoons count as unbroken again that it cannot come back by itself "
               "(the game recounts them at every clear): break them again to earn it back.";
    else if (id == g_lock_record)
      r.note = "Switching it off makes one safe or lock you opened count as unopened again (the game recounts them "
               "at every one opened): open it again to earn this back.";
  }
  static bool s_logged = false;  // the tick's
  if (!s_logged) {
    s_logged = true;
    logf("records: the main game's records read: %d of %d listed complete, %d reward name(s) not in records_text.h%s",
         v->cleared, v->listed, unnamed, v->all_rewards ? "; the all-rewards unlock is active" : "");
  }
}

void snapshot_rogue(View* v) {
  static RogueNow c;  // the tick's alone
  v->known = read_rogue(&c);
  if (!v->known) {
    std::snprintf(v->status, sizeof(v->status), "The Ghost Survivors' records could not be read - see the log.");
    v->status_error = true;
    return;
  }
  if (!R.achievements_read) {
    dictionary_keys(re::find_type("app.ropeway.gamemastering.AchievementDefine"), "RogueRecord2AchievementID",
                    "System.Collections.Generic.Dictionary`2<app.ropeway.gamemastering.RogueRecordManager.RogueRecordId,"
                    "app.ropeway.gamemastering.AchievementDefine.RogueID>",
                    "System.Collections.Generic.Dictionary`2.Entry<app.ropeway.gamemastering.RogueRecordManager.RogueRecordId,"
                    "app.ropeway.gamemastering.AchievementDefine.RogueID>",
                    R.achievement, kMaxRecords);
    int n = 0;
    for (bool b : R.achievement) n += b;
    R.achievements_read = n > 0;
    if (n) logf("records: %d of The Ghost Survivors' records carry a Steam achievement", n);
  }
  if (R.f_limit.valid())
    if (const uintptr_t at = re::static_addr(R.f_limit)) {
      int32_t lim = -1;
      if (mem::read_safe(at, &lim)) v->special_limit = lim;
    }
  int order[kMaxRecords], n = 0;
  bool placed[kMaxRecords] = {};
  int listed[kMaxRecords];
  const int nl = list_ints(ref(c.mgr, R.f_order), listed, kMaxRecords);
  for (int i = 0; i < nl; ++i)
    if (listed[i] >= 0 && listed[i] < c.n && !placed[listed[i]]) placed[listed[i]] = true, order[n++] = listed[i];
  for (int id = 0; id < c.n; ++id)
    if (!placed[id]) order[n++] = id;
  v->count = v->listed = n;
  v->cleared = 0;
  const uint32_t held = g_held;
  for (int i = 0; i < n; ++i) {
    const int id = order[i];
    if (id < 0 || id >= c.n || id >= kMaxRecords) continue;
    Row& r = v->row[i];
    r = Row{};
    r.id = id;
    r.cleared = flag(c.cleared, id);
    r.fresh = flag(c.fresh, id);
    r.achievement = R.achievement[id];
    r.held = id < 32 && (held & (1u << id)) && node_held(c, id);
    const int type = base_type(c, id);
    r.kind = type;
    r.goal = c.goal[id];
    if (R.counted[id] == rules::kPlays) r.progress = c.save.plays;
    if (R.counted[id] == rules::kRaccoons) r.progress = c.save.raccoons;
    if (id < records_text::kRogueRecords) {
      r.name = records_text::kRogueRecord[id].name;
      r.condition = records_text::kRogueRecord[id].condition;
    }
    v->cleared += r.cleared;
    if (c.reward[id] >= 0 && c.reward[id] < R.rewards) {
      Reward& rw = r.reward[r.n_rewards++];
      rw.id = c.reward[id];
      rw.open = flag(c.open, c.reward[id]);
      rw.name = c.reward[id] < records_text::kRogueRecords ? records_text::kAccessory[c.reward[id]] : nullptr;
    }
    const bool abc = c.lost[id] >= 0 && c.lost[id] < rules::kScenarioD;
    if (r.held)
      r.note = "Held off by the mod: the scenario stays cleared, so No Way Out stays open. Clearing the scenario again "
               "does not bring it back - switch it on here.";
    else if (type == R.rt.clear_training && abc)
      r.note = "No Way Out opens once Forgotten Soldier, Runaway and No Time To Mourn are all cleared. While it is "
               "open, switching this off holds it off instead of un-clearing the scenario, so nothing is locked away.";
    else if (type == R.rt.clear_normal)
      r.note = "Switching it off keeps the scenario cleared - as a training clear - so No Way Out stays open. Clearing "
               "it without training mode earns it again.";
    else if (R.counted[id] == rules::kRaccoons)
      r.note = "Switching it off makes one Mr. Raccoon count as unbroken again: break it again to earn this back.";
    else if (R.counted[id] == rules::kPlays)
      r.note = "Switching it off puts the play count just under the goal: the next game you play earns it again.";
    else if (type == R.rt.special && c.lost[id] == rules::kScenarioC)
      r.note = "Switching it off forgets both routes: take both again to earn it.";
  }
}

void publish(Set s, bool read_game) {
  View* v = new View;
  if (read_game) {
    if (s == kMain) snapshot_main(v);
    else snapshot_rogue(v);
  }
  v->achievements = call::achievements_ready();
  v->achievements_known = s == kMain ? M.achievements_read : R.achievements_read;
  v->saving = g_saving || g_save_pending;
  EnterCriticalSection(&g_cs);
  if (!v->status_error) {
    std::snprintf(v->status, sizeof(v->status), "%s", g_status[s]);
    v->status_error = g_status_error[s];
  }
  View* old = g_view[s];
  g_view[s] = v;
  LeaveCriticalSection(&g_cs);
  delete old;
}

// --- the save -----------------------------------------------------------------------------------------------
bool saves_busy(uintptr_t sdm) {
  uint8_t save = 0, load = 0, remove = 0, remove_all = 0;
  int32_t step = -1;
  if (!get(sdm, S.f_save, &save) || !get(sdm, S.f_load, &load) || !get(sdm, S.f_remove, &remove) ||
      !get(sdm, S.f_remove_all, &remove_all) || !get(sdm, S.f_step, &step))
    return true;
  bool idle = false;
  for (int v : S.idle) idle = idle || step == v;
  return save || load || remove || remove_all || !idle;
}

// What SaveDataManager.requestSaveSystemDataNoSaveIcon does - the request the
// game makes after every typewriter save: `if (!IsBusy) { SaveRequest = true;
// SlotId = -1; }`. The system save follows in its update, and The Ghost
// Survivors' after it.
void service_save() {
  // Polled at most four times a second, and only while a save is pending or on its way.
  static DWORD s_polled = 0, s_pending_since = 0;
  const DWORD now = GetTickCount();
  if (!g_saving && !g_save_pending) return;
  if (now - s_polled < 250) return;
  s_polled = now;
  const uintptr_t sdm = S.ok ? instance(S.inst, S.mgr) : 0;
  if (g_saving) {
    if (!sdm || (!saves_busy(sdm) && now - g_save_asked > 500)) {
      g_saving = false;
      logf("records: saved");
    } else if (now - g_save_asked > 30000) {
      g_saving = false;
      logf("records: the save the mod asked for has not finished after 30 s");
    }
    return;
  }
  if (!s_pending_since) s_pending_since = now;
  if (!sdm) {
    g_save_pending = false;
    s_pending_since = 0;
    logf("records: SaveDataManager not found - the change is saved the next time the game saves");
    return;
  }
  if (saves_busy(sdm)) {
    // The game's own save first; asked again in a moment - but not forever (an
    // error dialog can hold its saves up): the game saves the change itself later.
    if (now - s_pending_since > 60000) {
      g_save_pending = false;
      s_pending_since = 0;
      logf("records: the game's saves have been busy for a minute - the change is saved the next time the game saves");
    }
    return;
  }
  s_pending_since = 0;
  if (!put<int32_t>(sdm, S.f_slot, -1) || !put<uint8_t>(sdm, S.f_save, 1)) {
    g_save_pending = false;
    logf("records: the save could not be requested - the change is saved the next time the game saves");
    return;
  }
  g_save_pending = false;
  g_saving = true;
  g_save_asked = now;
  logf("records: system save requested (SaveDataManager.SaveRequest, SlotId -1)");
}

}  // namespace

// --- set-up ---------------------------------------------------------------------------------------------------
void init(const char* dir) {
  InitializeCriticalSection(&g_cs);
  std::snprintf(g_held_path, sizeof(g_held_path), "%sRE2CabbyCodes.records.txt", dir);
  for (auto& a : R.accessory) a = -2;
  load_held();
}

bool discover() {
  g_missing = 0;
  // The main game.
  M.mgr = need_type("app.ropeway.gamemastering.RecordManager");
  M.ssd = need_type("app.ropeway.gamemastering.RecordManager.SystemSaveData");
  M.node = need_type("app.ropeway.gamemastering.RecordManager.RecordNode");
  M.reward = need_type("app.ropeway.gamemastering.RecordManager.RewardNode");
  M.ids = need_type("app.ropeway.gamemastering.RecordManager.RecordIds");
  M.inst = need_field(M.mgr, "_Instance");
  M.f_ssd = need_field(M.mgr, "systemSaveData");
  M.f_nodes = need_field(M.mgr, "RecordDataList");
  M.f_rewards = need_field(M.mgr, "RewardDataList");
  M.f_order = need_field(M.mgr, "RecordIdList");
  M.f_naomi = need_field(M.mgr, "<Naomi>k__BackingField");
  M.f_progress = need_field(M.ssd, "RecordProgressNumber");
  M.f_cleared = need_field(M.ssd, "IsClearedRecord");
  M.f_fresh = need_field(M.ssd, "IsNewClearedRecord");
  M.f_open = need_field(M.ssd, "IsOpenedReward");
  M.f_open_new = need_field(M.ssd, "IsNewReward");
  M.f_tofu_popup = need_field(M.ssd, "IsOpenedPopUp_TofuCharacter");
  M.f_n_rewards = need_field(M.node, "RewardList");
  M.f_n_progress = need_field(M.node, "ProgressType");
  M.f_n_goal = need_field(M.node, "ClearCount");
  M.f_n_type = need_field(M.node, "Type");
  M.f_w_type = need_field(M.reward, "Type");
  M.f_w_name = need_field(M.reward, "NameGuid");
  M.f_i_id = need_field(M.ids, "Id");
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RecordManager.RecordId"))
    M.records = re::enum_value(e, "MAX", M.records);
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RecordManager.RewardId")) {
    M.rewards = re::enum_value(e, "MAX", M.rewards);
    M.tofu_first = re::enum_value(e, "OPEN_MODE_06", M.tofu_first);
    M.tofu_last = re::enum_value(e, "OPEN_MODE_09", M.tofu_last);
  }
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RecordManager.ProgressType")) {
    M.pt.none = re::enum_value(e, "NONE", M.pt.none);
    M.pt.count_up = re::enum_value(e, "COUNT_UP", M.pt.count_up);
    M.pt.count_down = re::enum_value(e, "COUNT_DOWN", M.pt.count_down);
  }
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RecordManager.RecordType"))
    M.hidden = re::enum_value(e, "HIDDEN", M.hidden);
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RecordManager.RewardType")) {
    const char* const kTypes[5] = {"FIGURE", "CONCEPTART", "MODE", "WEAPON", "COSTUME"};
    for (int k = 0; k < 5; ++k) M.reward_type[k] = re::enum_value(e, kTypes[k], M.reward_type[k]);
  }
  // The raccoon records: fixRecordProgress opens `cmp edi,A; je; cmp edi,B; jne` - the two records it
  // sets from the raccoons broken.
  if (const uintptr_t code = M.mgr ? re::method_code(M.mgr, "fixRecordProgress", 1) : 0)
    for (uintptr_t p = code; p < code + 0x40; ++p)
      if (mem::matches(p, "83 FF ?? 74 ?? 83 FF ?? 0F 85")) {
        M.raccoon_record[0] = mem::read<uint8_t>(p + 2);
        M.raccoon_record[1] = mem::read<uint8_t>(p + 7);
        break;
      }
  if (M.raccoon_record[0] < 0)
    logf("records: RecordManager.fixRecordProgress is not the code expected - the raccoon records are not known");
  M.ok = M.mgr && M.inst.valid() && M.f_ssd.valid() && M.f_nodes.valid() && M.f_rewards.valid() && M.f_progress.valid() &&
         M.f_cleared.valid() && M.f_fresh.valid() && M.f_open.valid() && M.f_open_new.valid() && M.f_n_rewards.valid() &&
         M.f_n_progress.valid() && M.f_n_goal.valid() && M.f_n_type.valid() && M.records > 0 &&
         M.records <= kMaxRecords && M.rewards > 0;

  // The Ghost Survivors.
  R.mgr = need_type("app.ropeway.gamemastering.RogueRecordManager");
  R.ssd = need_type("app.ropeway.gamemastering.RogueRecordManager.RogueSystemSaveData");
  R.node = need_type("app.ropeway.gamemastering.RogueRecordManager.RogueRecordNode");
  R.inst = need_field(R.mgr, "_Instance");
  R.f_ssd = need_field(R.mgr, "rogueSystemSaveData");
  R.f_nodes = need_field(R.mgr, "RogueRecordDataList");
  R.f_order = need_field(R.mgr, "RogueRecordOrderList");
  R.f_limit = R.mgr ? re::find_field(R.mgr, "D_COURCE_SPECIAL_CONDITION_NUM") : re::Field{};
  R.f_normal = need_field(R.ssd, "IsClearedRogueGame_Normal");
  R.f_training = need_field(R.ssd, "IsClearedRogueGame_Training");
  R.f_plays = need_field(R.ssd, "RoguePlayCount");
  R.f_special = need_field(R.ssd, "IsSpecialClearedRogue");
  R.f_route_c = need_field(R.ssd, "IsClearedTypeScenarioC");
  R.f_raccoons = need_field(R.ssd, "RogueRaccoonFigureCount");
  R.f_cleared = need_field(R.ssd, "IsClearedRogueRecord");
  R.f_fresh = need_field(R.ssd, "IsNewClearedRogueRecord");
  R.f_open = need_field(R.ssd, "IsOpenedRogueReward");
  R.f_open_new = need_field(R.ssd, "IsNewRogueReward");
  R.f_n_reward = need_field(R.node, "RogueReward");
  R.f_n_type = need_field(R.node, "RecordType");
  R.f_n_lost = need_field(R.node, "LostType");
  R.f_n_goal = need_field(R.node, "ClearCount");
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RogueRecordManager.RogueRecordType")) {
    R.rt.clear_normal = re::enum_value(e, "CLEAR_NORMAL", R.rt.clear_normal);
    R.rt.clear_training = re::enum_value(e, "CLEAR_TRAINING", R.rt.clear_training);
    R.rt.special = re::enum_value(e, "SPECIAL_CONDITION", R.rt.special);
    R.rt.count_up = re::enum_value(e, "COUNT_UP", R.rt.count_up);
  }
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RogueRecordManager.RogueRecordId"))
    R.records = re::enum_value(e, "MAX", R.records);
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RogueRecordManager.RogueRewardId"))
    R.rewards = re::enum_value(e, "MAX", R.rewards);
  // What the COUNT_UP records count: checkClearedRogueRecord_Simple's COUNT_UP case is
  // `test r14d,r14d; je; cmp r14d,P; je; cmp r14d,Q; jne` - records 0 and P the play count,
  // Q the raccoons broken.
  bool counted_known = false;
  if (const uintptr_t code = R.mgr ? re::method_code(R.mgr, "checkClearedRogueRecord_Simple", 1) : 0)
    for (uintptr_t p = code; p < code + 0x180; ++p)
      if (mem::matches(p, "45 85 F6 74 ?? 41 83 FE ?? 74 ?? 41 83 FE ?? 75")) {
        const int plays = mem::read<uint8_t>(p + 8), raccoons = mem::read<uint8_t>(p + 14);
        if (plays < kMaxRecords && raccoons < kMaxRecords) {
          R.counted[0] = R.counted[plays] = rules::kPlays;
          R.counted[raccoons] = rules::kRaccoons;
          counted_known = true;
        }
        break;
      }
  if (!counted_known)
    logf("records: RogueRecordManager.checkClearedRogueRecord_Simple is not the code expected - The Ghost Survivors' "
         "counted records are left alone when switched off");
  R.ok = R.mgr && R.inst.valid() && R.f_ssd.valid() && R.f_nodes.valid() && R.f_normal.valid() && R.f_training.valid() &&
         R.f_plays.valid() && R.f_special.valid() && R.f_route_c.valid() && R.f_raccoons.valid() && R.f_cleared.valid() &&
         R.f_fresh.valid() && R.f_open.valid() && R.f_open_new.valid() && R.f_n_reward.valid() && R.f_n_type.valid() &&
         R.f_n_lost.valid() && R.f_n_goal.valid() && R.records > 0 && R.records <= kMaxRecords && R.records <= 32;

  // The tallies records are recounted from: the raccoons broken, both sets, and the safes and locks opened.
  const char* const kTallyNames[kTallies][4] = {
      {"app.ropeway.GimmickRaccoonFigureManager", "app.ropeway.GimmickRaccoonFigureManager.SystemSaveData",
       "_SystemSaveData", "RaccoonFigureDataList"},
      {"app.ropeway.rogue.GimmickRogueRaccoonFigureManager",
       "app.ropeway.rogue.GimmickRogueRaccoonFigureManager.RogueSystemSaveData", "_RogueSystemSaveData",
       "RaccoonFigureDataList"},
      {"app.ropeway.GimmickDialLockManager", "app.ropeway.GimmickDialLockManager.SystemSaveData", "_SystemSaveData",
       "LockRecordList"}};
  for (int t = 0; t < kTallies; ++t) {
    TallyList& tl = g_tally[t];
    tl.mgr = need_type(kTallyNames[t][0]);
    tl.ssd = need_type(kTallyNames[t][1]);
    tl.inst = need_field(tl.mgr, "_Instance");
    tl.f_ssd = need_field(tl.mgr, kTallyNames[t][2]);
    tl.f_list = need_field(tl.ssd, kTallyNames[t][3]);
    tl.ok = tl.mgr && tl.ssd && tl.inst.valid() && tl.f_ssd.valid() && tl.f_list.valid();
  }
  // The lock record: SetUnlockRecord ends `mov r9d,edx; mov r8d,ID; mov rdx,rcx` before its
  // tail jump to setRecordCount(ID, the locks opened).
  if (const uintptr_t code = g_tally[kLocks].mgr ? re::method_code(g_tally[kLocks].mgr, "SetUnlockRecord", 1) : 0)
    for (uintptr_t p = code; p < code + 0x200; ++p)
      if (mem::matches(p, "44 8B CA 41 B8 ?? ?? ?? ?? 48 8B D1")) {
        const int32_t id = mem::read<int32_t>(p + 5);
        if (id >= 0 && id < M.records) g_lock_record = id;
        break;
      }
  if (g_lock_record < 0)
    logf("records: GimmickDialLockManager.SetUnlockRecord is not the code expected - the lock record is not known");

  // The accessories worn, and which accessory a reward is: RogueAccessoryManager.convertRewardIdToAccessoryDefine,
  // a compiled switch run for each reward (an instance method: the id in r8).
  A.mgr = need_type("app.ropeway.gamemastering.RogueAccessoryManager");
  A.ssd = need_type("app.ropeway.gamemastering.RogueAccessoryManager.RogueSystemSaveData");
  A.data = need_type("app.ropeway.gamemastering.RogueAccessoryManager.AccessoryData");
  A.inst = need_field(A.mgr, "_Instance");
  A.f_ssd = need_field(A.mgr, "rogueSystemSaveData");
  A.f_list = need_field(A.ssd, "AccessoryEquipSettings");
  A.f_id = need_field(A.data, "AccessoryID");
  if (const uint32_t e = re::find_type("app.ropeway.SurvivorDefine.Accessory"))
    R.accessory_none = re::enum_value(e, "Invalid", R.accessory_none);
  int mapped = 0;
  if (const uintptr_t code = A.mgr ? re::method_code_typed(A.mgr, "convertRewardIdToAccessoryDefine",
                                                           {"app.ropeway.gamemastering.RogueRecordManager.RogueRewardId"})
                                   : 0) {
    const mem::Range text = mem::section(GetModuleHandleA(nullptr), ".text");
    const mem::Range image = mem::module_range(GetModuleHandleA(nullptr));
    for (int r = 0; r < R.rewards && r < kMaxRecords; ++r) {
      int32_t acc = -2;
      if (code::eval_switch(
              code, 8, static_cast<uint64_t>(r),
              [](uintptr_t at, uint8_t* out, size_t n) { return mem::copy_from(out, at, n); },
              [&](uintptr_t at) { return text.contains(at); }, [&](uintptr_t at) { return image.contains(at); }, &acc) &&
          acc >= 0) {
        R.accessory[r] = acc;
        ++mapped;
      }
    }
  }
  A.ok = A.mgr && A.inst.valid() && A.f_ssd.valid() && A.f_list.valid() && A.f_id.valid() && mapped > 0;
  if (!A.ok) logf("records: the accessories worn cannot be read - an accessory switched off stays on whoever wears it");

  // The save, and the achievements' manager.
  S.mgr = need_type("app.ropeway.gamemastering.SaveDataManager");
  S.inst = need_field(S.mgr, "_Instance");
  S.f_save = need_field(S.mgr, "SaveRequest");
  S.f_load = need_field(S.mgr, "LoadRequest");
  S.f_remove = need_field(S.mgr, "RemoveRequest");
  S.f_remove_all = need_field(S.mgr, "RemoveAllRequest");
  S.f_slot = need_field(S.mgr, "SlotId");
  S.f_step = need_field(S.mgr, "<saveLoadStep>k__BackingField");
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.SaveDataManager.SaveLoadStep")) {
    S.idle[0] = re::enum_value(e, "INITIALIZE", S.idle[0]);
    S.idle[1] = re::enum_value(e, "REQUEST_WAIT", S.idle[1]);
    S.idle[2] = re::enum_value(e, "PS5_CROSSSAVE_DIALOG", S.idle[2]);
  }
  S.ok = S.mgr && S.inst.valid() && S.f_save.valid() && S.f_load.valid() && S.f_remove.valid() &&
         S.f_remove_all.valid() && S.f_slot.valid() && S.f_step.valid();
  t_achievements = need_type("app.ropeway.gamemastering.AchievementManager");
  i_achievements = need_field(t_achievements, "_Instance");
  f_achievement_service = need_field(t_achievements, "AchievementService");

  // The screens (events.cpp hooks their update).
  const char* const kScreens[kSets] = {"app.ropeway.gui.RecordBehavior", "app.ropeway.gui.RogueRecordBehavior"};
  for (int s = 0; s < kSets; ++s) {
    Scr.cls[s] = need_type(kScreens[s]);
    Scr.update[s] = Scr.cls[s] ? re::find_method(Scr.cls[s], "update", {}) : 0;
    Scr.input[s] = need_field(Scr.cls[s], "EnableInput_");
  }

  g_discovered.store(true, std::memory_order_release);
  logf("records: discovery (%d name(s) missing): main %d (%d records, %d rewards, raccoon records %d/%d, lock record "
       "%d), Ghost Survivors %d (%d records, counted records known %d), raccoons %d/%d, locks %d, accessories %d (%d "
       "mapped, none = %d), save %d, screens %u/%u; %d held off",
       g_missing, M.ok, M.records, M.rewards, M.raccoon_record[0], M.raccoon_record[1], g_lock_record, R.ok, R.records,
       counted_known, g_tally[kMainRaccoons].ok, g_tally[kRogueRaccoons].ok, g_tally[kLocks].ok, A.ok, mapped,
       R.accessory_none, S.ok, Scr.update[kMain], Scr.update[kRogue], __builtin_popcount(g_held.load()));
  return M.ok || R.ok;
}

bool ready(Set s) { return s == kMain ? M.ok : R.ok; }

const char* reward_kind(int type) {
  static const char* const kNames[5] = {"Figure", "Concept art", "Game mode", "Infinite weapon", "Costume"};
  for (int k = 0; k < 5; ++k)
    if (type >= 0 && type == M.reward_type[k]) return kNames[k];
  return type < 0 ? "Accessory" : "Reward";
}

const char* status(Set s) {
  if (s == kMain ? !M.ok : !R.ok) return "the game's records were not found in this build - see the log";
  return "";
}

uint32_t screen_class(Set s) { return s >= 0 && s < kSets ? Scr.cls[s] : 0; }
uint32_t screen_update(Set s) { return s >= 0 && s < kSets ? Scr.update[s] : 0; }

void screen_frame(Set s, uintptr_t behaviour) {
  if (s < 0 || s >= kSets || !behaviour) return;
  // The screen takes input (EnableInput_: set once its opening and the game's own
  // check are done, cleared as it closes and while a dialog is up). An object the
  // field is not found in counts as taking input.
  uintptr_t at = 0;
  if (g_screen_obj[s].load(std::memory_order_relaxed) == behaviour) {
    at = g_screen_input_at[s].load(std::memory_order_relaxed);
  } else {
    at = Scr.input[s].valid() ? re::field_addr(behaviour, Scr.input[s]) : 0;
    g_screen_input_at[s].store(at, std::memory_order_relaxed);
    g_screen_obj[s].store(behaviour, std::memory_order_relaxed);
  }
  uint8_t input = 1;
  if (at && !mem::read_safe(at, &input)) input = 0;
  const DWORD now = GetTickCount() | 1;
  g_screen_seen[s].store(now, std::memory_order_relaxed);
  if (input) g_screen_input[s].store(now, std::memory_order_relaxed);
}

// The set whose records screen ran a frame lately, taking input or not (its
// opening animation included) - -1: none.
int screen_seen() {
  const DWORD now = GetTickCount();
  for (int s = 0; s < kSets; ++s) {
    const DWORD seen = g_screen_seen[s].load(std::memory_order_relaxed);
    if (seen && now - seen < 400) return s;
  }
  return -1;
}

int screen_open() {
  const DWORD now = GetTickCount();
  int best = -1;
  DWORD newest = 0;
  for (int s = 0; s < kSets; ++s) {
    const DWORD seen = g_screen_seen[s].load(std::memory_order_relaxed);
    const DWORD input = g_screen_input[s].load(std::memory_order_relaxed);
    if (seen && now - seen < 400 && input && now - input < 400 && (best < 0 || seen - newest < 0x80000000u)) {
      best = s;
      newest = seen;
    }
  }
  return best;
}

View view(Set s) {
  View v;
  EnterCriticalSection(&g_cs);
  if (s >= 0 && s < kSets && g_view[s]) v = *g_view[s];
  LeaveCriticalSection(&g_cs);
  return v;
}

void request(Set s, int id, bool on) {
  EnterCriticalSection(&g_cs);
  if (g_nq < kQueue) g_queue[g_nq++] = Req{s, id, on};
  LeaveCriticalSection(&g_cs);
}

void request_all(Set s, bool on) { request(s, -1, on); }

void tick(int main_state) {
  if (!g_discovered.load(std::memory_order_acquire)) return;
  static int s_state = -2, s_open = -1, s_seen = -1;
  static bool s_holds_pending = true;  // set again at every state change; retried each frame until the table reads
  static DWORD s_holds_since = 0;      // when the retrying began (it gives up after 10 s, until the next change)
  static DWORD s_refreshed = 0;
  const DWORD now = GetTickCount();
  const bool state_changed = main_state != s_state;
  s_state = main_state;
  const int open = screen_open();
  const bool opened = open >= 0 && open != s_open;
  if (open != s_open)
    logf("records: %s", open < 0 ? "the records screen is closed"
                        : open == kMain ? "the records screen is up (main game)"
                                        : "the records screen is up (The Ghost Survivors)");
  s_open = open;
  // The holds, before the game's own checks can run: The Ghost Survivors' records
  // screen checks as its opening animation ends (seen from its first frame, before
  // it takes input), the title checks as it comes up, a run as it ends - each a
  // state change or a screen away. At start-up they are tried every frame until the
  // manager's table exists (with no holds this reads nothing).
  const int seen = screen_seen();
  if (state_changed || (seen == kRogue && s_seen != kRogue)) {
    s_holds_pending = true;
    s_holds_since = now;
  }
  s_seen = seen;
  if (s_holds_pending) {
    if (!s_holds_since) s_holds_since = now;
    s_holds_pending = !apply_holds() && now - s_holds_since < 10000;
  }

  Req q[kQueue];
  int nq = 0;
  EnterCriticalSection(&g_cs);
  nq = g_nq;
  std::memcpy(q, g_queue, sizeof(Req) * static_cast<size_t>(nq));
  g_nq = 0;
  LeaveCriticalSection(&g_cs);
  bool changed[kSets] = {};
  for (int i = 0; i < nq; ++i) {
    const Req& r = q[i];
    if (open != r.set) {
      note(r.set, true, "The records are only switched while their records screen is open.");
      continue;
    }
    if (r.set == kMain) {
      static MainNow c;  // the tick's alone
      if (!read_main(&c)) {
        note(kMain, true, "The records could not be read - nothing was changed.");
        continue;
      }
      if (r.id >= 0) changed[kMain] |= switch_main(c, r.id, r.on);
      else {
        int n = 0;
        for (int id = 0; id < c.n; ++id)
          if (flag(c.cleared, id) != r.on) n += switch_main(c, id, r.on);
        changed[kMain] |= n > 0;
        note(kMain, false, "%d record(s) switched %s", n, r.on ? "on" : "off");
      }
    } else {
      static RogueNow c;  // the tick's alone
      if (!read_rogue(&c)) {
        note(kRogue, true, "The Ghost Survivors' records could not be read - nothing was changed.");
        continue;
      }
      if (r.id >= 0) changed[kRogue] |= switch_rogue(c, r.id, r.on);
      else {
        int n = 0;
        for (int id = 0; id < c.n; ++id)
          if (flag(c.cleared, id) != r.on) n += switch_rogue(c, id, r.on);
        changed[kRogue] |= n > 0;
        note(kRogue, false, "%d record(s) switched %s", n, r.on ? "on" : "off");
      }
    }
  }
  if (changed[kMain] || changed[kRogue]) g_save_pending = true;
  service_save();

  // The panel's figures: when a screen opens, after a change, and twice a second
  // while it is up (the game's own check grants records as the screen finishes
  // opening, and a save finishes).
  if (open >= 0 && (opened || changed[open] || now - s_refreshed >= 500)) {
    publish(static_cast<Set>(open), true);
    s_refreshed = now;
  }
}

}  // namespace re2cc::records
