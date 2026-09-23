#include "input.h"

#include "config.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"

namespace re2cc::input {
namespace {

using KeyStateFn = SHORT(WINAPI*)(int);
KeyStateFn g_real_async = nullptr, g_real_state = nullptr;
uintptr_t g_async_slot = 0, g_state_slot = 0;

SHORT WINAPI hk_get_async_key_state(int vk) {
  // Escape stays the game's: it is how the pause menu is left.
  if (vk != VK_ESCAPE && overlay::capturing_keyboard()) return 0;
  return g_real_async ? g_real_async(vk) : 0;
}

SHORT WINAPI hk_get_key_state(int vk) {
  if (vk != VK_ESCAPE && overlay::capturing_keyboard()) return 0;
  return g_real_state ? g_real_state(vk) : 0;
}

}  // namespace

void install() {
  if (config::get().disable_input) {
    logf("input: disabled by config - the game sees what is typed into the panel");
    return;
  }
  HMODULE exe = GetModuleHandleA(nullptr);
  if (void* p = mem::iat_hook(exe, "USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&hk_get_async_key_state),
                              &g_async_slot))
    g_real_async = reinterpret_cast<KeyStateFn>(p);
  if (void* p = mem::iat_hook(exe, "USER32.dll", "GetKeyState", reinterpret_cast<void*>(&hk_get_key_state), &g_state_slot))
    g_real_state = reinterpret_cast<KeyStateFn>(p);
  logf("input: re2.exe's GetAsyncKeyState %s, GetKeyState %s", g_real_async ? "hooked" : "NOT FOUND",
       g_real_state ? "hooked" : "NOT FOUND");
}

void uninstall() {
  mem::iat_restore(g_async_slot, reinterpret_cast<void*>(&hk_get_async_key_state), reinterpret_cast<void*>(g_real_async));
  mem::iat_restore(g_state_slot, reinterpret_cast<void*>(&hk_get_key_state), reinterpret_cast<void*>(g_real_state));
}

int raw_input_type(LPARAM lp) {
  RAWINPUTHEADER h{};
  UINT size = sizeof(h);
  // RID_HEADER only reads the header; the game's own read of the full packet
  // is unaffected (GetRawInputData does not consume anything).
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_HEADER, &h, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
    return -1;
  return static_cast<int>(h.dwType);
}

}  // namespace re2cc::input
