#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace re2cc::proxy {

// Load steam_api64_orig.dll from `dir` (absolute path, trailing separator), pin
// it, and point every thunk at the original's function of the same name - or,
// for a function the original lacks, at a stub that answers 0. False when the
// game cannot run with it: the file missing or not loadable, the original
// lacking a function the game imports, or the game importing one this proxy
// does not export (another version of the game). The caller shows failure()
// and refuses to load.
bool load_original(const char* dir);
// The same, as if the program imported `imports` from steam_api64.dll (tests).
bool load_original_as(const char* dir, const std::vector<std::string>& imports);

const char* failure();  // why the last load failed, written for the player (a message box)
int missing();          // functions the original lacks, answering 0 (after a load)
HMODULE original();     // steam_api64_orig.dll once it is loaded (null before)

}  // namespace re2cc::proxy
