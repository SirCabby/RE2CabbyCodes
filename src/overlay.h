#pragma once

#include <windows.h>

// The Dear ImGui panel and the machinery that gets it on screen. RE2 renders
// through Direct3D 11 or 12 (its own option), and both present through a DXGI
// swap chain - DXVK's under Proton, dxgi.dll's on Windows, one class for both
// APIs - so the panel hooks that class's Present, Present1 and ResizeBuffers
// through the vtable of a throwaway swap chain of our own, and draws with the
// DX11 or the DX12 backend depending on which device the game's swap chain
// belongs to.
namespace re2cc::overlay {

void install_early();  // from DllMain: the locks, and re2.exe's ClipCursor import
bool install();        // mod thread: the swap chain hooks
void uninstall();
void service();        // mod thread, every 100 ms: work Present asked for (the DX12 queue's discovery)

bool ensure_context(HWND hwnd);  // ImGui context + Win32 backend + window procedure, once
bool wants_draw();               // the visibility rule for this frame (lock held)
void draw_panel();               // between ImGui::NewFrame() and ImGui::Render() (lock held)
void shutdown_imgui();

// One ImGui context; the window procedure (the window's thread) feeds it
// messages and the present hook (the render thread) draws from it, so every
// touch of the context is taken under this lock (RE0 learned it the hard way).
void lock_imgui();
void unlock_imgui();
struct ImGuiLock {
  ImGuiLock() { lock_imgui(); }
  ~ImGuiLock() { unlock_imgui(); }
  ImGuiLock(const ImGuiLock&) = delete;
  ImGuiLock& operator=(const ImGuiLock&) = delete;
};

// What the panel is using this frame, for the input guard (input.cpp).
bool capturing_mouse();     // the pointer is over the panel
bool capturing_keyboard();  // a panel field has keyboard focus
bool visible();             // the panel was drawn in the last frame

namespace d3d {  // overlay_d3d.cpp
bool install();
void uninstall();
void service();
void log_rates();  // mod thread, once a second
}  // namespace d3d

}  // namespace re2cc::overlay
