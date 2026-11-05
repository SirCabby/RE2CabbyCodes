#pragma once

#include <windows.h>

// Moments one thread notes for another (a screen's frame, seen by its hook on a
// game thread and read by the tick and the panel): GetTickCount() with bit 0 set,
// so that none reads as 0 - "never". Their age is taken on a clock with bit 0 set
// too. GetTickCount() holds a value for up to 16 ms (on Windows, and under Wine
// when its server is idle), and a moment noted while it held an even one was a
// millisecond ahead of it: an age of -1, which unsigned is 49 days. Aged on the
// bare clock, the records screens and the Load Game screen read as closed in
// those frames - at 120 frames a second and up, a fifth of them - and the panel
// beside them flashed: the cheats instead of the records on the pause menu,
// nothing at the title, the cursor shown and hidden with it (reported
// 2026-11-02; RE4CabbyCodes found the same on 2026-10-23).
//
// A moment another thread writes is loaded before the clock is read (these read
// the clock after their arguments), so it is never from a later millisecond.
namespace re2cc::stamp {

inline DWORD now() { return GetTickCount() | 1; }

// Milliseconds from the moment `at` to `clock`, a GetTickCount() read after it.
inline DWORD age(DWORD at, DWORD clock) { return (clock | 1) - at; }
inline DWORD age(DWORD at) { return age(at, GetTickCount()); }

// `at` is a moment less than `ms` ago (0, none: never).
inline bool within(DWORD at, DWORD ms, DWORD clock) { return at && age(at, clock) < ms; }
inline bool within(DWORD at, DWORD ms) { return within(at, ms, GetTickCount()); }

}  // namespace re2cc::stamp
