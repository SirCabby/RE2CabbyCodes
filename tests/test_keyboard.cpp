// src/keyboard.cpp against a real DirectInput (Wine's, or Windows'), with a
// DirectInput keyboard of the "game's" beside it: exclusive (`exclusive` on the
// command line) - the cooperative level RE2's engine can give its keyboard
// (via.hid.HIDEntry.KeyboardCooperativeLevel ForegroundExclusive) - or not.
// Keys are sent with SendInput to a window of our own; the test reports what
// the window's procedure got (WM_KEYDOWN, WM_CHAR) and checks what the
// panel's own device read, what it handed ImGui, and what the game's device
// is told while a panel field has the keyboard.
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static
//       tests/test_keyboard.cpp src/keyboard.cpp src/log.cpp src/mem.cpp src/config.cpp
//       contrib/imgui/imgui.cpp contrib/imgui/imgui_draw.cpp contrib/imgui/imgui_tables.cpp
//       contrib/imgui/imgui_widgets.cpp -ldinput8 -ldxguid -luser32 -o tests/build/test_keyboard.exe
//   WINEPREFIX=<scratch> wine tests/build/test_keyboard.exe exclusive
//   WINEPREFIX=<scratch> wine tests/build/test_keyboard.exe shared
// Headless: set the scratch prefix's graphics driver to "null"
// (HKCU\Software\Wine\Drivers, Graphics = null) - the wineserver still routes
// SendInput through the low-level hooks and DirectInput to the foreground window.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <cstring>

#include "../src/keyboard.h"
#include "../src/log.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace re2cc::overlay {
bool g_test_typing = false;
bool capturing_keyboard() { return g_test_typing; }
}  // namespace re2cc::overlay

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
  if (!ok) ++g_fail;
}

int g_keydowns = 0, g_chars = 0;
LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) ++g_keydowns;
  if (m == WM_CHAR) ++g_chars;
  return DefWindowProcW(h, m, w, l);
}

void pump_messages(DWORD ms) {
  const DWORD until = GetTickCount() + ms;
  do {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  } while (GetTickCount() < until);
}

void send_key(WORD vk, WORD scan, bool up) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.wScan = scan;
  in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
  SendInput(1, &in, sizeof(in));
}

// What the panel handed ImGui since the queue was last emptied.
struct Handed {
  int text = 0;
  wchar_t first_text = 0;
  int key_down = 0, key_up = 0;
  ImGuiKey first_key = ImGuiKey_None;
};
Handed handed() {
  Handed h;
  for (const ImGuiInputEvent& e : GImGui->InputEventsQueue) {
    if (e.Type == ImGuiInputEventType_Text) {
      if (!h.text++) h.first_text = static_cast<wchar_t>(e.Text.Char);
    } else if (e.Type == ImGuiInputEventType_Key) {
      if (e.Key.Down) ++h.key_down;
      else ++h.key_up;
      if (h.first_key == ImGuiKey_None) h.first_key = e.Key.Key;
    }
  }
  GImGui->InputEventsQueue.resize(0);
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  const bool exclusive = argc > 1 && std::strcmp(argv[1], "exclusive") == 0;
  char dir[MAX_PATH];
  GetModuleFileNameA(nullptr, dir, MAX_PATH);
  if (char* s = std::strrchr(dir, '\\')) s[1] = 0;
  re2cc::log_init(dir);
  ImGui::CreateContext();

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"re2cc_keyboard_test";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"keyboard test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 320, 200,
                              nullptr, nullptr, wc.hInstance, nullptr);
  check(hwnd != nullptr, "a window of our own");
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  pump_messages(200);
  check(GetForegroundWindow() == hwnd, "it is the foreground window");

  // The game's keyboard.
  IDirectInput8W* di = nullptr;
  IDirectInputDevice8W* game = nullptr;
  HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&di), nullptr);
  if (SUCCEEDED(hr)) hr = di->CreateDevice(GUID_SysKeyboard, &game, nullptr);
  if (SUCCEEDED(hr)) hr = game->SetDataFormat(&c_dfDIKeyboard);
  if (SUCCEEDED(hr))
    hr = game->SetCooperativeLevel(hwnd, exclusive ? DISCL_EXCLUSIVE | DISCL_FOREGROUND : DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
  if (SUCCEEDED(hr)) {
    DIPROPDWORD b{};
    b.diph.dwSize = sizeof(b);
    b.diph.dwHeaderSize = sizeof(b.diph);
    b.diph.dwHow = DIPH_DEVICE;
    b.dwData = 64;
    hr = game->SetProperty(DIPROP_BUFFERSIZE, &b.diph);
  }
  if (SUCCEEDED(hr)) hr = game->Acquire();
  std::printf("the game's keyboard: %s, hr 0x%08lX\n", exclusive ? "exclusive" : "non-exclusive", static_cast<unsigned long>(hr));
  check(SUCCEEDED(hr), "the game's DirectInput keyboard is acquired");

  check(re2cc::keyboard::start(hwnd), "the panel's own DirectInput keyboard starts");
  check(re2cc::keyboard::active(), "and is active");
  re2cc::keyboard::pump(true, VK_F7);  // anything from before
  handed();

  // Typing '5' into a panel field.
  re2cc::overlay::g_test_typing = true;
  g_keydowns = g_chars = 0;
  send_key('5', 0x06, false);
  send_key('5', 0x06, true);
  pump_messages(150);
  std::printf("the window's procedure got %d WM_KEYDOWN and %d WM_CHAR for it\n", g_keydowns, g_chars);
  if (exclusive) check(g_keydowns == 0 && g_chars == 0, "an exclusive DirectInput keyboard leaves the window no key messages (the bug)");
  else check(g_keydowns == 1 && g_chars == 1, "a shared DirectInput keyboard leaves the window its key messages");
  int toggles = re2cc::keyboard::pump(true, VK_F7);
  Handed h = handed();
  std::printf("the panel handed ImGui %d character(s) (first U+%04X), %d key down(s), %d key up(s)\n", h.text,
              static_cast<unsigned>(h.first_text), h.key_down, h.key_up);
  check(h.text == 1 && h.first_text == L'5', "the panel's keyboard typed '5'");
  check(h.key_down == 1 && h.key_up == 1 && h.first_key == ImGuiKey_5, "and pressed and released ImGuiKey_5");
  check(toggles == 0, "no toggle");

  // What the game's keyboard is told meanwhile: nothing went down.
  DIDEVICEOBJECTDATA ev[16];
  DWORD n = 16;
  hr = game->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), ev, &n, 0);
  int game_downs = 0, game_ups = 0;
  for (DWORD i = 0; SUCCEEDED(hr) && i < n; ++i) (ev[i].dwData & 0x80 ? game_downs : game_ups)++;
  std::printf("the game's buffer: hr 0x%08lX, %d press(es), %d release(s)\n", static_cast<unsigned long>(hr), game_downs, game_ups);
  check(SUCCEEDED(hr) && game_downs == 0, "while typing, the game's buffer holds no press");
  check(game_ups == 1, "but the release still comes through");

  // A key held while typing, and still held after the field lets go.
  send_key('7', 0x08, false);
  pump_messages(100);
  re2cc::keyboard::pump(true, VK_F7);
  BYTE state[256];
  hr = game->GetDeviceState(sizeof(state), state);
  check(SUCCEEDED(hr) && state[DIK_7] == 0, "while typing, the game's state has '7' up though it is held");
  re2cc::overlay::g_test_typing = false;
  re2cc::keyboard::pump(false, VK_F7);
  hr = game->GetDeviceState(sizeof(state), state);
  check(SUCCEEDED(hr) && state[DIK_7] == 0, "after the field lets go, '7' still held stays hidden from the game");
  send_key('7', 0x08, true);
  pump_messages(100);
  re2cc::keyboard::pump(false, VK_F7);
  n = 16;
  game->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), ev, &n, 0);  // drained
  h = handed();
  check(h.text == 1 && h.first_text == L'7', "the held '7' was typed into the panel once");

  // Not typing: the game gets its keys as ever, and the toggle key counts.
  send_key('8', 0x09, false);
  pump_messages(100);
  hr = game->GetDeviceState(sizeof(state), state);
  check(SUCCEEDED(hr) && state[DIK_8] == 0x80, "not typing, the game's state has '8' down");
  n = 16;
  hr = game->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), ev, &n, 0);
  check(SUCCEEDED(hr) && n == 1 && ev[0].dwOfs == DIK_8 && (ev[0].dwData & 0x80), "and its buffer has the press");
  send_key('8', 0x09, true);
  send_key(VK_F7, 0x41, false);
  pump_messages(100);
  // The mod's old way to the toggle key when the window gets no key messages.
  std::printf("GetAsyncKeyState sees F7 held: %s\n", (GetAsyncKeyState(VK_F7) & 0x8000) ? "yes" : "no");
  send_key(VK_F7, 0x41, true);
  pump_messages(100);
  toggles = re2cc::keyboard::pump(false, VK_F7);
  check(toggles == 1, "the toggle key (F7) is counted once");
  h = handed();
  check(h.text == 0 && h.key_down == 0, "and nothing reaches ImGui while the panel is not taking keys");

  re2cc::keyboard::stop();
  check(!re2cc::keyboard::active(), "stopped");
  hr = game->GetDeviceState(sizeof(state), state);
  check(SUCCEEDED(hr), "the game's keyboard still reads after the hooks are gone");
  game->Unacquire();
  game->Release();
  di->Release();
  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
