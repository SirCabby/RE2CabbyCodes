#pragma once

#include <windows.h>

// The panel's keyboard. RE2 reads the keyboard through DirectInput, at the
// cooperative level the engine's settings give it
// (via.hid.HIDEntry.KeyboardCooperativeLevel: Background, Foreground,
// ForegroundNoWinKey, ForegroundExclusive). Taken exclusively, the keyboard
// sends the game's window no key messages at all - Wine's DirectInput drops
// every key at its low-level hook - so nothing typed reaches a panel field
// through WM_KEYDOWN and WM_CHAR. The panel reads the keyboard through a
// DirectInput keyboard of its own instead: non-exclusive, which is always
// allowed and gets every key whatever the game's device does.
//
// While a panel field has the keyboard, the game's DirectInput keyboard is
// told no key went down (Escape excepted: it always reaches the pause menu).
// IDirectInputDevice8W's GetDeviceState and GetDeviceData are hooked in
// dinput8's device vtable, which every device of the process shares, so the
// hooks pass the panel's own device and every device that is not a keyboard
// through untouched. A key still held when the field lets go of the keyboard
// (the Enter that committed a value) stays hidden from the game until it is
// released.
namespace re2cc::keyboard {

// Render thread, under the ImGui lock, once the game's window is known: the
// device, and the hooks unless the input guard is disabled. False when there
// is no DirectInput keyboard to be had: the panel then takes the window's key
// messages, as before.
bool start(HWND window);
bool active();  // the panel's keys come from here, not from the window's messages (any thread)

// Render thread, under the ImGui lock, every frame: the keys since the last
// frame. They go to ImGui when `to_panel` (the panel is up and the game's
// window has the focus); the result is how often `toggle_vk` went down.
int pump(bool to_panel, int toggle_vk);

void stop();  // unload: the hooks put back, the device released

}  // namespace re2cc::keyboard
