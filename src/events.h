#pragma once

#include "cheats.h"

// The game calls the mod at its own events - an enemy or the player hit, a
// shot, a knife's hit, a typewriter's check, an item picked for a gimmick, a
// save, the clock's frame, the item box opening, the inventory screen's frame,
// a footstep -
// instead of the mod reading the game every frame. Two kinds of pointer are
// exchanged, both data; no code is patched:
//  - a method's entry in the type database, for the event handlers and
//    callbacks the game makes delegates of. The runtime copies a delegate's
//    code out of that entry when it makes the delegate (the new-delegate helper,
//    re.h), so the entries are exchanged at start-up, before the game has made
//    any - long before the title screen.
//  - a slot of a class's vtable, for virtual methods. A virtual call reads its
//    slot every time, so the slot is exchanged once the class has its
//    REObjectInfo (looked for every second).
// Each replacement does its cheat's work around a call to the code it replaced -
// the game's own, forwarded unchanged - on whatever thread the game called it
// from, touching only the objects the game handed it (and what they hold), and
// returning at once when that code left a managed exception pending. The
// switches it reads are atomics (cheats.h). The user approved these hooks on
// 2026-09-16, the records' counters' three (the item box opening, the
// inventory screen's frame, a footstep) on 2026-09-19, and the same day the two
// records screens' frames (RecordBehavior / RogueRecordBehavior.update), which
// tell the records panel (records.h) that a records screen is up, and the Load
// Game screen's (LoadBehavior.update), where the save-file window (savefiles.h)
// reads the game's list of saves and has it read again after a change.
namespace re2cc::events {

void install();    // mod thread, right after game::discover(): the database entries; logs each
void service();    // mod thread, every second: vtable slots as classes come up; first calls to the log
bool ready(cheats::Kind k);           // the cheat's events are hooked (it works)
bool records_screen_hooked(int set);  // records::Set: the mod hears that records screen's frames
bool load_screen_hooked();            // the mod hears the Load Game screen's frames (the save files)
const char* why_not(cheats::Kind k);  // for the panel when it does not
void uninstall();  // a DLL unload that is not the process exiting: puts back what it exchanged

}  // namespace re2cc::events
