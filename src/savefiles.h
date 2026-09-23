#pragma once

#include <cstdint>

// The save files, managed beside the game's Load Game screen (the title's Load
// Game, the pause menu's, the game over screen's): while it is up the panel
// lists the game's own slots - the auto-save and slots 1-20 - and can delete a
// save or copy one into another slot, empty or not. RE1CabbyCodes' save-file
// manager, done RE2's way:
//
// - The screen is gui.LoadBehavior. Its update (SaveLoadBaseBehavior.update,
//   virtual[15], in LoadBehavior's own vtable) runs only while it is up; the mod
//   hooks it (events.cpp, user-approved 2026-09-19) to know the screen is up and
//   to read the list the game shows: SaveFileDetailList, one engine
//   SaveFileDetail per slot, and SaveFileDetailTextList, the strings its rows show.
// - Delete is the game's own: SaveDataManager.requestRemoveGameData - which the
//   game has but never calls - is `if (!IsBusy) { RemoveRequest = true; SlotId =
//   slot; }`, and the manager's update removes the slot through the engine's save
//   service. The mod writes those two fields, as the records panel asks for its
//   system save.
// - Copy has no counterpart in the game. The engine's save service keeps every
//   slot in Steam Cloud through ISteamRemoteStorage, so the copy is made there
//   too: the source slot's file read and written under the target slot's name
//   with the Steam API the game saves through (FileRead, FileWrite), read back and
//   compared, from a thread of the mod's own - never the game's.
// - The game's list is then read again: the engine's
//   via.storage.saveService.SaveService.updateSaveFileDetailTbl(user) marks its
//   table of the slots stale and has its worker read every slot again, and the
//   screen's SaveFileDetailList set to null (its property setter) makes the
//   screen's own update read the table again and rebuild its rows
//   (reloadSaveDetail, initList, setList). Both calls are made from the hook, on
//   the screen's own thread, after the game's update (user-approved 2026-09-19).
//
// Neither change can be undone: the panel asks first, and every request carries
// what the panel showed of the slots it names, so a slot that changed in between
// is left alone.
namespace re2cc::savefiles {

// SaveDataManager.get_IsBusy, for anyone who must not step on a save under way:
// 1 busy, 0 idle, -1 not known. (Freeze play time gives the clock the time a
// save's hook took out of it once the save is written - cheats.cpp.)
int game_saves_busy();

constexpr int kMaxRows = 32;  // the game's list: 21 rows in this build (the auto-save and slots 1-20)
constexpr int kMaxTexts = 8;  // strings the game's row shows (scenario, difficulty, saves, location, area, ...)

struct Row {
  bool used = false;     // the slot holds a save (the game's row is not "No Data")
  int slot = -1;         // the game's slot number (0 = the auto-save): row i is the list's first slot + i
  long long unix_time = -1;  // when it was saved (-1 not known)
  long long size = -1;   // SaveFileDetail.UseSize (-1 not known)
  char subtitle[96] = {};    // the scenario ("Leon A"...), as the engine keeps it
  char texts[kMaxTexts][96] = {};  // the game's own strings for the row (localised)
  int n_texts = 0;
  uint32_t hash = 0;     // of the slot and what identifies its save (the engine's strings, time stamp): a request checks it
};
// Two readings of a row are the same save.
inline bool same_save(const Row& a, const Row& b) { return a.used == b.used && a.slot == b.slot && a.hash == b.hash; }

struct View {
  bool ready = false;    // everything the manager needs was found (else `status` says why)
  bool up = false;       // the Load Game screen is up and taking input, its list read
  bool copy_ok = false;  // Steam's remote storage is there (Copy)
  bool busy = false;     // a change is under way, or the game's own saves are busy
  bool consistent = false;  // the list is laid out as the game's code says (SaveDataManager.getSaveDataIndex /
                            // getSaveListCount), every entry a detail of its own slot: changes may go by it
  int first_slot = -1;   // row i is slot first_slot + i (getSaveDataIndex(SCENARIO, 0): 0)
  int count = 0;
  Row row[kMaxRows];
  char status[240] = {};  // what the last change came to, or why nothing can change
  bool status_error = false;
};

void init();       // from DllMain: the lock
bool discover();   // mod thread, once re::ready(): logs what it found
bool ready();
const char* status();

// For events.cpp: the screen's class (LoadBehavior) and its update method
// (SaveLoadBaseBehavior.update, inherited), 0 when not found.
uint32_t screen_class();
uint32_t screen_update();
// The screen's frame, after the game's update (the hook, on the game's thread):
// the list's snapshot, and the refresh a change asked for.
void screen_frame(uintptr_t ctx, uintptr_t screen);
// The Load Game screen is up on the main game's list with no load under way, as
// of its last frame (its list may be being read again: View.up says it is read).
bool screen_open();

View view();  // any thread (a copy)

// Requests from the panel, carried out from the game tick. What the panel saw of
// the rows goes with them: the list can move on between the click and the tick,
// and changing a save that is not the one clicked is worse than doing nothing.
void request_delete(int row, const Row& seen);
void request_copy(int from_row, int to_row, const Row& from_seen, const Row& to_seen);

void tick();  // the game tick: the requests, the change under way, the game's save state

}  // namespace re2cc::savefiles
