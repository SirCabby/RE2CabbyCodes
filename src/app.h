#pragma once

// The game's frame, from the inside. RE Engine's via.Application runs the game
// as a table of module entries - UpdateBehavior, LateUpdateBehavior,
// BeginRendering, EndRendering, ... - each {object, function, this-adjustment,
// name, priority, group}, and calls `function(object + adjustment)` for each,
// in priority order, every frame. The mod ticks from two of them: the function
// pointers of UpdateBehavior and EndRendering are swapped for the mod's own,
// which tick and then call through. It is a pointer store into the Application
// object, like REFramework's application-entry hooks - no code is touched and
// no game function is called.
//
// RE2 has no per-frame message pump to tick from: the window's thread blocks in
// GetMessageW and the game updates elsewhere. The entries of the frame's group
// (priorities 143-279) run as jobs on the engine's thread pool, each after the
// ones it depends on, so the tick is serialised by the mod (dispatch.cpp), not
// tied to one thread.
//
// The table is found from the one function that fills it (add_function): its
// instructions give the entry count's and the table's offsets in the
// Application object, the entry size, and where an entry keeps its function,
// name and priority; its single caller loads the Application object from a
// global, read out of that call site.
namespace re2cc::app {

bool find();   // mod thread, once: the layout and the global; false when the code was not found
bool hook();   // mod thread, until true: both entries hooked (the table fills and is sorted during start-up)
void check();  // mod thread, once a second after hook(): the hooked entries are still where they were
void unhook();
const char* status();
void log_rates();  // mod thread, once a second: each hook's calls, for the first seconds

}  // namespace re2cc::app
