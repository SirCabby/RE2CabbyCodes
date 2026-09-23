#include "keyboard.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "imgui.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"

namespace re2cc::keyboard {
namespace {

using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
constexpr int kGetDeviceState = 9, kGetDeviceData = 10;  // IDirectInputDevice8W's vtable

IDirectInput8W* g_dinput = nullptr;
std::atomic<IDirectInputDevice8W*> g_device{nullptr};  // the panel's own keyboard
std::atomic<bool> g_active{false};
bool g_tried = false;
HWND g_window = nullptr;

// The game's keyboard: the hooks, and the keys it is not told about.
GetStateFn g_real_state = nullptr;
GetDataFn g_real_data = nullptr;
uintptr_t g_state_slot = 0, g_data_slot = 0;
std::atomic<uint64_t> g_hidden[4];  // DIK codes that went down while a panel field had the keyboard, until they come up

// What the panel's device has seen (the render thread's).
bool g_down[256];
bool g_caps = false;
bool g_was_typing = false;
bool g_first_logged = false;

bool hidden(int dik) { return (g_hidden[dik >> 6].load(std::memory_order_relaxed) >> (dik & 63)) & 1; }
void hide(int dik, bool on) {
  const uint64_t bit = 1ull << (dik & 63);
  if (on) g_hidden[dik >> 6].fetch_or(bit, std::memory_order_relaxed);
  else g_hidden[dik >> 6].fetch_and(~bit, std::memory_order_relaxed);
}
bool any_hidden() {
  for (const auto& w : g_hidden)
    if (w.load(std::memory_order_relaxed)) return true;
  return false;
}

// Kept from the game: every key but Escape while a panel field has the
// keyboard, and after that the keys still held from then.
bool keep_from_game(int dik, bool typing) { return dik != DIK_ESCAPE && (typing || hidden(dik)); }

// A keyboard of the game's: any keyboard device but the panel's own.
bool games_keyboard(IDirectInputDevice8W* dev) {
  if (!dev || dev == g_device.load(std::memory_order_relaxed)) return false;
  DIDEVICEINSTANCEW info{};
  info.dwSize = sizeof(info);
  return SUCCEEDED(dev->GetDeviceInfo(&info)) && GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_KEYBOARD;
}

// The game's keyboard read at once (a byte per key, DIK order, 0x80 down).
HRESULT STDMETHODCALLTYPE hk_get_state(IDirectInputDevice8W* dev, DWORD size, LPVOID data) {
  const HRESULT hr = g_real_state(dev, size, data);
  const bool typing = overlay::capturing_keyboard();
  if (FAILED(hr) || !data || (!typing && !any_hidden()) || !games_keyboard(dev)) return hr;
  BYTE* keys = static_cast<BYTE*>(data);
  if (size == 256) {
    for (int k = 0; k < 256; ++k)
      if (keep_from_game(k, typing)) keys[k] = 0;
  } else if (typing) {
    std::memset(data, 0, size);  // a data format of the game's own: nothing at all
  }
  return hr;
}

// The game's keyboard read from its buffer: the presses are taken out, the
// releases kept, so a key that was down when a field took the keyboard still
// comes up for the game.
HRESULT STDMETHODCALLTYPE hk_get_data(IDirectInputDevice8W* dev, DWORD object_size, LPDIDEVICEOBJECTDATA data,
                                      LPDWORD count, DWORD flags) {
  const HRESULT hr = g_real_data(dev, object_size, data, count, flags);
  const bool typing = overlay::capturing_keyboard();
  if (FAILED(hr) || !data || !count || !*count || object_size < 2 * sizeof(DWORD) || (!typing && !any_hidden()) ||
      !games_keyboard(dev))
    return hr;
  BYTE* at = reinterpret_cast<BYTE*>(data);
  DWORD kept = 0;
  for (DWORD i = 0; i < *count; ++i) {
    const BYTE* e = at + static_cast<size_t>(i) * object_size;
    DWORD ofs = 0, value = 0;  // DIDEVICEOBJECTDATA's dwOfs (the DIK code) and dwData
    std::memcpy(&ofs, e, sizeof(ofs));
    std::memcpy(&value, e + sizeof(DWORD), sizeof(value));
    if ((value & 0x80) && keep_from_game(static_cast<int>(ofs & 0xFF), typing)) continue;
    if (kept != i) std::memmove(at + static_cast<size_t>(kept) * object_size, e, object_size);
    ++kept;
  }
  *count = kept;
  return hr;
}

// The virtual key a DIK code stands for. MapVirtualKeyEx reads a scan code in
// the keyboard layout (an AZERTY 'A' sits where a QWERTY 'Q' does); the
// numpad's scan codes alone read as their cursor keys, and DirectInput numbers
// the extended keys from 0x80, so those are named here.
UINT vk_of(int dik, HKL layout) {
  switch (dik) {
    case DIK_NUMPAD0: return VK_NUMPAD0;
    case DIK_NUMPAD1: return VK_NUMPAD1;
    case DIK_NUMPAD2: return VK_NUMPAD2;
    case DIK_NUMPAD3: return VK_NUMPAD3;
    case DIK_NUMPAD4: return VK_NUMPAD4;
    case DIK_NUMPAD5: return VK_NUMPAD5;
    case DIK_NUMPAD6: return VK_NUMPAD6;
    case DIK_NUMPAD7: return VK_NUMPAD7;
    case DIK_NUMPAD8: return VK_NUMPAD8;
    case DIK_NUMPAD9: return VK_NUMPAD9;
    case DIK_DECIMAL: return VK_DECIMAL;
    case DIK_ADD: return VK_ADD;
    case DIK_SUBTRACT: return VK_SUBTRACT;
    case DIK_MULTIPLY: return VK_MULTIPLY;
    case DIK_DIVIDE: return VK_DIVIDE;
    case DIK_NUMPADENTER: return VK_RETURN;
    case DIK_NUMLOCK: return VK_NUMLOCK;
    case DIK_LSHIFT: return VK_LSHIFT;
    case DIK_RSHIFT: return VK_RSHIFT;
    case DIK_LCONTROL: return VK_LCONTROL;
    case DIK_RCONTROL: return VK_RCONTROL;
    case DIK_LMENU: return VK_LMENU;
    case DIK_RMENU: return VK_RMENU;
    case DIK_HOME: return VK_HOME;
    case DIK_UP: return VK_UP;
    case DIK_PRIOR: return VK_PRIOR;
    case DIK_LEFT: return VK_LEFT;
    case DIK_RIGHT: return VK_RIGHT;
    case DIK_END: return VK_END;
    case DIK_DOWN: return VK_DOWN;
    case DIK_NEXT: return VK_NEXT;
    case DIK_INSERT: return VK_INSERT;
    case DIK_DELETE: return VK_DELETE;
    case DIK_LWIN: return VK_LWIN;
    case DIK_RWIN: return VK_RWIN;
    case DIK_APPS: return VK_APPS;
    case DIK_PAUSE: return VK_PAUSE;
    case DIK_SYSRQ: return VK_SNAPSHOT;
    default: break;
  }
  return dik > 0 && dik < 0x80 ? MapVirtualKeyExW(static_cast<UINT>(dik), MAPVK_VSC_TO_VK, layout) : 0;
}

// The keys a text field acts on.
ImGuiKey imgui_key(UINT vk, bool keypad_enter) {
  if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + static_cast<int>(vk - '0'));
  if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + static_cast<int>(vk - 'A'));
  switch (vk) {
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_RETURN: return keypad_enter ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_END: return ImGuiKey_End;
    case VK_HOME: return ImGuiKey_Home;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    default: return ImGuiKey_None;
  }
}

// The keys as the device holds them now, when its buffer could not be trusted
// (lost, or overflowed): a key found up is up, and the game may have it again.
void resync(IDirectInputDevice8W* dev) {
  BYTE now[256];
  if (FAILED(dev->GetDeviceState(sizeof(now), now))) std::memset(now, 0, sizeof(now));
  for (int k = 0; k < 256; ++k) {
    g_down[k] = (now[k] & 0x80) != 0;
    if (!g_down[k]) hide(k, false);
  }
}

// The text a key types in the layout, with the modifiers held (Ctrl or Alt
// alone make a shortcut, not text; both together are AltGr).
void type(UINT vk, int dik, HKL layout, bool shift, bool ctrl, bool alt) {
  if (ctrl != alt) return;
  BYTE state[256] = {};
  if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
  if (ctrl) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
  if (alt) state[VK_MENU] = state[VK_RMENU] = 0x80;
  state[VK_CAPITAL] = g_caps ? 1 : 0;
  state[VK_NUMLOCK] = 1;
  wchar_t text[8];
  // 0x4: the thread's own keyboard state is left alone (a dead key is not kept for the next one).
  const int n = ToUnicodeEx(vk, static_cast<UINT>(dik & 0x7F), state, text, 8, 0x4, layout);
  ImGuiIO& io = ImGui::GetIO();
  for (int i = 0; i < n; ++i)
    if (text[i] >= 0x20 && text[i] != 0x7F) io.AddInputCharacterUTF16(text[i]);
}

}  // namespace

bool start(HWND window) {
  if (g_active || g_tried) return g_active;
  g_tried = true;
  g_window = window;
  if (config::get().disable_keyboard) {
    logf("keyboard: disabled by config - the panel takes keys from the window's messages");
    return false;
  }
  // The game has dinput8 loaded (it imports it); DirectInput8Create is taken
  // from it at run time, so the mod imports no dinput8 of its own.
  HMODULE dll = GetModuleHandleW(L"dinput8.dll");
  if (!dll) dll = LoadLibraryW(L"dinput8.dll");
  const auto create = dll ? reinterpret_cast<CreateFn>(GetProcAddress(dll, "DirectInput8Create")) : nullptr;
  HMODULE self = nullptr;  // DirectInput wants the module asking: this DLL
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&g_tried), &self);
  IDirectInputDevice8W* dev = nullptr;
  const char* step = "DirectInput8Create";
  HRESULT hr = create ? create(self, DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&g_dinput), nullptr)
                      : E_NOINTERFACE;
  if (SUCCEEDED(hr)) {
    step = "CreateDevice";
    hr = g_dinput->CreateDevice(GUID_SysKeyboard, &dev, nullptr);
  }
  if (SUCCEEDED(hr)) {
    step = "SetDataFormat";
    hr = dev->SetDataFormat(&c_dfDIKeyboard);
  }
  if (SUCCEEDED(hr)) {
    // Non-exclusive: allowed whatever the game's keyboard is. In the background
    // too, so the focus is never the device's to lose: pump() asks the window.
    step = "SetCooperativeLevel";
    hr = dev->SetCooperativeLevel(window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
  }
  if (SUCCEEDED(hr)) {
    step = "SetProperty(DIPROP_BUFFERSIZE)";
    DIPROPDWORD buffer{};
    buffer.diph.dwSize = sizeof(buffer);
    buffer.diph.dwHeaderSize = sizeof(buffer.diph);
    buffer.diph.dwHow = DIPH_DEVICE;
    buffer.dwData = 256;
    hr = dev->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph);
  }
  if (SUCCEEDED(hr)) {
    step = "Acquire";
    hr = dev->Acquire();
  }
  if (FAILED(hr)) {
    logf("keyboard: no DirectInput keyboard of the panel's own (%s failed, 0x%08lX) - the panel takes keys from the "
         "window's messages", step, static_cast<unsigned long>(hr));
    if (dev) dev->Release();
    if (g_dinput) g_dinput->Release();
    g_dinput = nullptr;
    return false;
  }
  g_device = dev;
  g_active = true;
  // The game's keyboard is kept from what is typed into the panel: the two
  // reads, in the vtable the panel's device shares with the game's.
  bool guarded = false;
  if (!config::get().disable_input) {
    auto** vt = *reinterpret_cast<void***>(dev);
    g_real_state = reinterpret_cast<GetStateFn>(vt[kGetDeviceState]);
    g_real_data = reinterpret_cast<GetDataFn>(vt[kGetDeviceData]);
    const uintptr_t state_slot = reinterpret_cast<uintptr_t>(&vt[kGetDeviceState]);
    const uintptr_t data_slot = reinterpret_cast<uintptr_t>(&vt[kGetDeviceData]);
    if (mem::exchange_ptr(state_slot, reinterpret_cast<uintptr_t>(g_real_state), reinterpret_cast<uintptr_t>(&hk_get_state)))
      g_state_slot = state_slot;
    if (mem::exchange_ptr(data_slot, reinterpret_cast<uintptr_t>(g_real_data), reinterpret_cast<uintptr_t>(&hk_get_data)))
      g_data_slot = data_slot;
    guarded = g_state_slot && g_data_slot;
  }
  logf("keyboard: the panel reads the keyboard through its own DirectInput device (non-exclusive); the game's "
       "DirectInput keyboard is %s",
       guarded                        ? "kept from what is typed into the panel (GetDeviceState/GetDeviceData hooked in "
                                        "dinput8's device vtable)"
       : config::get().disable_input ? "not guarded (the input guard is disabled by config)"
                                      : "NOT guarded - its vtable could not be hooked");
  return true;
}

bool active() { return g_active.load(std::memory_order_relaxed); }

int pump(bool to_panel, int toggle_vk) {
  IDirectInputDevice8W* dev = g_device.load(std::memory_order_relaxed);
  if (!dev || !active()) return 0;
  // A field just took the keyboard: the keys already held stay the game's
  // secret too, or letting go of the field would press them again for it.
  const bool typing = overlay::capturing_keyboard();
  if (typing && !g_was_typing)
    for (int k = 0; k < 256; ++k)
      if (g_down[k] && k != DIK_ESCAPE) hide(k, true);
  g_was_typing = typing;
  const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(g_window, nullptr));
  ImGuiIO& io = ImGui::GetIO();
  int toggles = 0;
  DIDEVICEOBJECTDATA ev[64];
  for (int round = 0; round < 8; ++round) {
    DWORD n = 64;
    const HRESULT hr = dev->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), ev, &n, 0);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
      if (FAILED(dev->Acquire())) break;  // the next frame tries again
      resync(dev);
      continue;
    }
    if (hr == DI_BUFFEROVERFLOW) resync(dev);  // what the buffer still holds follows
    if (FAILED(hr)) {
      static bool s_logged = false;
      if (!s_logged) logf("keyboard: the panel's DirectInput keyboard cannot be read (0x%08lX)", static_cast<unsigned long>(hr));
      s_logged = true;
      break;
    }
    for (DWORD i = 0; i < n; ++i) {
      const int dik = static_cast<int>(ev[i].dwOfs & 0xFF);
      const bool down = (ev[i].dwData & 0x80) != 0;
      if (g_down[dik] == down) continue;
      g_down[dik] = down;
      if (!g_first_logged) {
        g_first_logged = true;
        logf("keyboard: the panel's DirectInput keyboard gets keys (first: DIK 0x%02X)", dik);
      }
      if (down && typing && dik != DIK_ESCAPE) hide(dik, true);
      else if (!down) hide(dik, false);
      if (dik == DIK_CAPITAL && down) g_caps = !g_caps;
      const UINT vk = vk_of(dik, layout);
      if (down && vk && static_cast<int>(vk) == toggle_vk) {
        ++toggles;
        continue;
      }
      if (!to_panel) continue;
      const bool shift = g_down[DIK_LSHIFT] || g_down[DIK_RSHIFT];
      const bool ctrl = g_down[DIK_LCONTROL] || g_down[DIK_RCONTROL];
      const bool alt = g_down[DIK_LMENU] || g_down[DIK_RMENU];
      if (vk == VK_LSHIFT || vk == VK_RSHIFT) io.AddKeyEvent(ImGuiMod_Shift, shift);
      else if (vk == VK_LCONTROL || vk == VK_RCONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
      else if (vk == VK_LMENU || vk == VK_RMENU) io.AddKeyEvent(ImGuiMod_Alt, alt);
      else {
        const ImGuiKey key = imgui_key(vk, dik == DIK_NUMPADENTER);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
        if (down && vk) type(vk, dik, layout, shift, ctrl, alt);
      }
    }
    if (n < 64) break;
  }
  return toggles;
}

void stop() {
  if (g_state_slot)
    mem::exchange_ptr(g_state_slot, reinterpret_cast<uintptr_t>(&hk_get_state), reinterpret_cast<uintptr_t>(g_real_state));
  if (g_data_slot)
    mem::exchange_ptr(g_data_slot, reinterpret_cast<uintptr_t>(&hk_get_data), reinterpret_cast<uintptr_t>(g_real_data));
  g_state_slot = g_data_slot = 0;
  g_active = false;
  if (IDirectInputDevice8W* dev = g_device.exchange(nullptr)) {
    dev->Unacquire();
    dev->Release();
  }
  if (g_dinput) g_dinput->Release();
  g_dinput = nullptr;
}

}  // namespace re2cc::keyboard
