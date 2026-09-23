#pragma once

#include <cstdint>

// The records panel: RE2's two sets of records - the main game's
// (RecordManager, 91) and The Ghost Survivors' (RogueRecordManager, 15) -
// switched on and off beside the game's own records screens (the title's Bonus
// menu, the pause menu), their rewards following.
//
// A record is the game's data, not a switch the mod holds: its cleared and NEW
// flags, what earns it (a progress number; for The Ghost Survivors the rogue
// save's clear flags and counts) and each reward's unlocked flag, all in the
// system saves. Switched on, a record gets what the game writes when it is
// earned (RecordManager.openRecord / RogueRecordManager.openRogueRecord, and
// what earns it - records_rules.h), and its Steam achievement is unlocked
// through the game's own call (AchievementManager, user-approved 2026-09-19).
// Switched off, the flags go, the rewards no other cleared record grants are
// locked again, and what earns it is taken back so the game's next check does
// not hand it straight back - except where that would lock content away: The
// Ghost Survivors' No Way Out is open only while scenarios A, B and C are
// cleared, so switching off "Mission Complete?", "Reunited" or "Getting Over It"
// then holds the record off in the game's in-memory record table instead
// (records_rules.h), remembered in RE2CabbyCodes.records.txt and set again as
// the game starts, before its checks.
//
// What reads the rewards reads them live - the title's menus as they open, the
// costume, figure and concept art screens - and the game itself puts the
// infinite weapons into the item box, or takes them out of it and the
// inventory, at every load and new game. Each change is saved at once: the mod
// asks the game's SaveDataManager for a system save the way the game does after
// a typewriter save (SaveRequest, SlotId -1), and the game follows it with The
// Ghost Survivors' save.
//
// The screens are known from their own frame: RecordBehavior.update and
// RogueRecordBehavior.update run only while their screen's object is active
// (events.cpp hooks both, user-approved 2026-09-19).
namespace re2cc::records {

enum Set : int { kMain = 0, kRogue = 1, kSets = 2 };

void init(const char* dir);  // from DllMain: the lock and the held records file's path
bool discover();             // mod thread, once re::ready(): logs what it found
bool ready(Set s);
const char* status(Set s);  // why a set cannot be switched (when it cannot)

// For events.cpp: the screens' classes and update methods (0 when not found).
uint32_t screen_class(Set s);
uint32_t screen_update(Set s);
// A records screen's frame (the hook, on the game's thread).
void screen_frame(Set s, uintptr_t behaviour);
// The set whose records screen is up (-1: none), as of its last frame.
int screen_open();

// --- the panel ---------------------------------------------------------------------------------------------
constexpr int kMaxRecords = 96;
constexpr int kMaxRewards = 4;

struct Reward {
  int id = -1;              // RewardId / RogueRewardId
  int type = -1;            // RecordManager.RewardType (main), -1 for an accessory
  bool open = false;        // unlocked now
  bool shared = false;      // another cleared record grants it too: switching this one off keeps it
  const char* name = nullptr;  // the game's text (nullptr: unknown)
};

struct Row {
  int id = -1;              // RecordId / RogueRecordId
  bool cleared = false;
  bool fresh = false;       // the game's NEW mark
  bool hidden = false;      // a HIDDEN record: not on the game's screen
  bool achievement = false; // switching it on unlocks a Steam achievement
  bool held = false;        // kept off by the mod (The Ghost Survivors, to keep No Way Out open)
  int progress = -1, goal = -1;  // what earns it, where there is a number (-1: none)
  int kind = -1;            // rules::Kind (main), the RogueRecordType (The Ghost Survivors)
  int n_rewards = 0;
  Reward reward[kMaxRewards];
  const char* name = nullptr;       // the game's text (nullptr: a hidden record - named by its rewards)
  const char* condition = nullptr;  // "{0}": the goal
  const char* note = nullptr;       // what switching it does beyond the obvious
};

struct View {
  bool known = false;       // the manager and its tables were read
  int count = 0;
  Row row[kMaxRecords];     // the game's order (its screen's), hidden records last
  int cleared = 0, listed = 0;  // cleared records / records the game's screen lists
  bool all_rewards = false; // RecordManager.<Naomi>: every reward shows unlocked whatever the records say
  bool achievements = false;  // the game's achievement calls were found (and none failed)
  bool achievements_known = false;  // which records carry an achievement was read (AchievementDefine)
  int special_limit = -1;   // "Gunslinger"'s shot limit (RogueRecordManager.D_COURCE_SPECIAL_CONDITION_NUM)
  bool saving = false;      // a save the mod asked for is on its way
  char status[200] = {};    // the last change, or why nothing can change
  bool status_error = false;
};
View view(Set s);  // any thread (a copy)
const char* reward_kind(int type);  // a RecordManager.RewardType as the panel names it ("Figure", "Costume", ...)

void request(Set s, int id, bool on);  // one record
void request_all(Set s, bool on);      // every record of the set (hidden ones too)

void tick(int main_state);  // the game tick: requests, the screens' snapshots, holds, the save

}  // namespace re2cc::records
