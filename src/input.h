#pragma once

#include <windows.h>

// The input guard. RE2 reads the mouse as Raw Input (WM_INPUT through its
// window procedure, GetRawInputData), the keyboard through DirectInput and
// some keys with GetAsyncKeyState/GetKeyState; the pad through
// XInput/DirectInput. While the panel is using the mouse (the pointer is over
// it) or the keyboard (a text field has focus), the window procedure hook
// (overlay.cpp) keeps those messages from the game, the exe's two key-state
// imports, hooked here, read every key as up, and the game's DirectInput
// keyboard is told no key went down (keyboard.h) - except Escape, which always
// reaches the pause menu.
namespace re2cc::input {

void install();    // from DllMain: re2.exe's GetAsyncKeyState/GetKeyState imports
void uninstall();

// For the window procedure: a WM_INPUT message's device (RIM_TYPEMOUSE,
// RIM_TYPEKEYBOARD, ...), or -1.
int raw_input_type(LPARAM lp);

}  // namespace re2cc::input
