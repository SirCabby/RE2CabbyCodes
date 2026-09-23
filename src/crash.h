#pragma once

namespace re2cc {

// Install a last-resort exception filter that records where a fatal fault
// happened, and in which module. Injected mods are the usual suspect for
// crashes, so it needs to be possible to tell from the log whether the fault
// is in this DLL or somewhere else entirely.
void install_crash_logger();

// The game's own SetUnhandledExceptionFilter import is pointed at this (from
// DllMain): the game then sets the filter we chain to, and ours stays on top.
void* crash_filter_hook();
void* crash_real_set_filter();  // what that import held before

// Main thread, about once a second: put our filter back on top if something
// replaced it without going through the game's import.
void reassert_crash_filter();

// "module+0xRVA" for an address, for log lines.
void describe_address(const void* addr, char* out, unsigned n);

}  // namespace re2cc
