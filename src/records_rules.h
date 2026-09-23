#pragma once

#include <cstdint>

// What switching a record on or off writes, as rules on plain values - no game
// memory here (records.cpp applies them; tests/test_records.cpp checks them).
//
// The main game's records (RecordManager) are earned by a progress number:
// RecordManager.checkClearedRecord(id) - what the records screen runs each time
// it opens, and what AchievementManager.unlockRecord asks before it unlocks a
// Steam achievement - compares SystemSaveData.RecordProgressNumber[id] with the
// record's ClearCount by its ProgressType: NONE and COUNT_UP need
// `goal <= progress`, COUNT_DOWN (the fewest moves, the most time left)
// `0 < progress <= goal`, and the end-of-game kinds (CLEARTIME, ITEMBOX, CURE,
// WALK, ADA_GUN) `progress == goal` - RecordManager.clearRecord writes the goal
// itself. A record switched on gets what clearRecord leaves (a better number
// kept); one switched off drops below its goal, or the next records screen hands
// it straight back. The Ghost Survivors' records (RogueRecordManager) have no
// progress number: their check works them out from the rogue save's own flags
// and counts (records.cpp).
namespace re2cc::records::rules {

enum Kind : int { kAtLeast = 0, kWithin, kExactly };

// RecordManager.ProgressType's values as this build has them (read from the enum).
struct ProgressTypes {
  int none = 0, count_up = 1, count_down = 2;
};

inline Kind kind_of(const ProgressTypes& t, int progress_type) {
  if (progress_type == t.none || progress_type == t.count_up) return kAtLeast;
  if (progress_type == t.count_down) return kWithin;
  return kExactly;
}

// The game's own check: the progress earns the record.
inline bool meets(Kind k, int progress, int goal) {
  switch (k) {
    case kAtLeast: return goal <= progress;
    case kWithin: return progress > 0 && progress <= goal;
    default: return progress == goal;
  }
}

// Switched on: what clearRecord leaves - the goal - unless the progress already
// earns it (a count above the goal, fewer moves than it asks).
inline int on_progress(Kind k, int progress, int goal) { return meets(k, progress, goal) ? progress : goal; }

// Switched off: short of the goal, so the game's check cannot hand it back. A
// count stays just short (one more kill earns it again); the rest go to 0, the
// game's "never done".
inline int off_progress(Kind k, int progress, int goal) {
  if (!meets(k, progress, goal)) return progress;
  if (k == kAtLeast && goal > 0) return goal - 1 < progress ? goal - 1 : progress;
  return 0;
}

// Several records can grant one reward (the tofu survivors come from two): a
// reward is taken back only when no other record that stays cleared grants it.
// rewards[r * per + k] is record r's k-th reward (-1: none).
inline bool granted_elsewhere(int reward, int except, const bool* cleared, const int* rewards, int records, int per) {
  for (int r = 0; r < records; ++r) {
    if (r == except || !cleared[r]) continue;
    for (int k = 0; k < per; ++k)
      if (rewards[r * per + k] == reward) return true;
  }
  return false;
}

// Mr. Raccoons: GimmickRaccoonFigureManager keeps which ones were ever broken
// (a bool per raccoon, in the system save), and RecordManager.fixRecordProgress
// sets the raccoon records' progress to that count at every clear - so a raccoon
// record switched off stays off only once enough raccoons are unbroken again:
// the highest-numbered ones, down to `keep` (they can be broken again). The same
// for the safes and locks opened. Returns how many were unmarked; broken[] is
// changed in place.
inline int unbreak_to(bool* broken, int n, int keep) {
  int count = 0;
  for (int i = 0; i < n; ++i) count += broken[i];
  int undone = 0;
  for (int i = n - 1; i >= 0 && count > keep; --i)
    if (broken[i]) {
      broken[i] = false;
      --count;
      ++undone;
    }
  return undone;
}

// --- The Ghost Survivors ------------------------------------------------------------------------------------
// RogueRecordManager.checkClearedRogueRecord_Simple, what its records screen
// runs each time it opens (and the end of every run): by the record's
// RogueRecordType, CLEAR_NORMAL is IsClearedRogueGame_Normal[scenario],
// CLEAR_TRAINING `Training[scenario] || Normal[scenario]`, SPECIAL_CONDITION
// IsSpecialClearedRogue[scenario], COUNT_UP the play count (two records) or the
// raccoons broken (one) against its goal. No Way Out, scenario D, is open while
// A, B and C are each cleared either way (RogueRecordManager.get_IsOpenedScenarioD) -
// there is no flag of its own.
struct RogueTypes {
  int clear_normal = 0, clear_training = 1, special = 2, count_up = 3;
};
struct RogueSave {
  bool normal[4] = {}, training[4] = {}, special[4] = {};
  bool route_c[2] = {};  // IsClearedTypeScenarioC: No Time To Mourn's two routes ("Sewer Explorer")
  int plays = 0;         // RoguePlayCount
  int raccoons = 0;      // RogueRaccoonFigureCount
};
constexpr int kScenarioC = 2, kScenarioD = 3;

inline bool no_way_out_open(const RogueSave& s) {
  for (int t = 0; t < kScenarioD; ++t)
    if (!s.normal[t] && !s.training[t]) return false;
  return true;
}

// What a record counts, when it is COUNT_UP (read out of the check's code).
enum Counted : int { kNotCounted = 0, kPlays, kRaccoons };

// Switched on: the save is given what earning the record leaves in it - so the
// game's own check agrees (and so does its achievement's). A scenario cleared
// "either way" is given the training clear: a normal one would earn the
// no-training record as well, at the next check.
inline void rogue_on(const RogueTypes& t, int type, int scenario, int goal, Counted counted, RogueSave* s) {
  if (scenario < 0 || scenario > kScenarioD) return;
  if (type == t.clear_training) {
    if (!s->normal[scenario] && !s->training[scenario]) s->training[scenario] = true;
  } else if (type == t.clear_normal) {
    s->normal[scenario] = true;
  } else if (type == t.special) {
    s->special[scenario] = true;
    if (scenario == kScenarioC) s->route_c[0] = s->route_c[1] = true;
  } else if (type == t.count_up) {
    if (counted == kPlays && s->plays < goal) s->plays = goal;
    if (counted == kRaccoons && s->raccoons < goal) s->raccoons = goal;
  }
}

// Switched off: the save loses what earns the record, so the next check cannot
// hand it back and doing it again earns it - except where that would lock
// something away. Returns true when the record must be held off instead: it is
// one of "Mission Complete?", "Reunited", "Getting Over It" (a scenario A-C
// cleared either way) while No Way Out is open, and un-clearing the scenario
// would close No Way Out again. The no-training records keep their scenario
// cleared as a training clear for the same reason.
inline bool rogue_off(const RogueTypes& t, int type, int scenario, int goal, Counted counted, RogueSave* s) {
  if (scenario < 0 || scenario > kScenarioD) return false;
  if (type == t.clear_training) {
    if (scenario < kScenarioD && no_way_out_open(*s)) return true;
    s->normal[scenario] = s->training[scenario] = false;
  } else if (type == t.clear_normal) {
    if (s->normal[scenario] && !s->training[scenario]) s->training[scenario] = true;  // still cleared: training
    s->normal[scenario] = false;
  } else if (type == t.special) {
    s->special[scenario] = false;
    if (scenario == kScenarioC) s->route_c[0] = s->route_c[1] = false;
  } else if (type == t.count_up) {
    if (counted == kPlays && goal > 0 && s->plays >= goal) s->plays = goal - 1;
    // The raccoons are unbroken by the caller (unbreak_to); the count follows them.
  }
  return false;
}

// A held record: its RogueRecordNode's type set to COUNT_UP. The check counts
// only a few COUNT_UP records (the play count's two, the raccoons' one - read
// out of its code) and answers "not earned" for any other, and the game's list
// draws it with an empty gauge (getRogueRecordProgressRate: 0). The original
// type is the table's, remembered by the caller. A hold is kept only while No
// Way Out is open - the one thing it is for: with it shut (another save, a save
// wiped) the record is let go, to be earned by play again.
inline bool can_hold(const RogueTypes& t, int type, int scenario, Counted counted) {
  return type == t.clear_training && scenario >= 0 && scenario < kScenarioD && counted == kNotCounted;
}

}  // namespace re2cc::records::rules
