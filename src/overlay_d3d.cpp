// The swap chain hooks and the two renderers.
//
// A throwaway D3D11 device and swap chain on a hidden window give us the swap
// chain class's vtable; its Present (8), ResizeBuffers (13), Present1 (22) and
// ResizeBuffers1 (39) slots are replaced, which reaches the game's swap chain
// whichever API it was made for - under Proton both are DXVK's DxgiSwapChain,
// on Windows both are dxgi.dll's. Each Present asks the swap chain which device
// it belongs to and draws the panel with the DX11 or the DX12 backend, into the
// back buffer about to be shown, on the thread that presents.
//
// DX12 needs the game's command queue to submit on, and nothing on the swap
// chain hands it out. As REFramework does, a throwaway D3D12 queue and swap
// chain show where a swap chain keeps its queue pointer - directly, or one
// pointer deeper under Proton, where DXVK's swap chain wraps vkd3d-proton's -
// and the game's queue is read from the same place, checked by its vtable.
//
// d3d11, d3d12, dxgi and d3dcompiler are all loaded at run time: the proxy
// imports none of them, so a system without one still starts the game.

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cstring>
#include <vector>

#include "overlay.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "config.h"
#include "crash.h"
#include "dispatch.h"
#include "log.h"
#include "mem.h"

// The ImGui backends compile their two shaders with D3DCompile. This stands in
// for d3dcompiler's export so the proxy does not import the DLL: it is loaded
// the first time a backend asks, and a system without it only loses the panel.
extern "C" HRESULT WINAPI D3DCompile(const void* data, SIZE_T data_size, const char* filename,
                                     const D3D_SHADER_MACRO* defines, ID3DInclude* include, const char* entrypoint,
                                     const char* target, UINT sflags, UINT eflags, ID3DBlob** shader,
                                     ID3DBlob** error_messages) {
  static pD3DCompile fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    for (const char* name : {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"}) {
      HMODULE m = LoadLibraryA(name);
      if (!m) continue;
      fn = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
      if (fn) {
        re2cc::logf("overlay: shaders compiled with %s", name);
        break;
      }
    }
    if (!fn) re2cc::logf("ERROR: overlay: no d3dcompiler_4x.dll - the panel cannot be drawn");
  }
  return fn ? fn(data, data_size, filename, defines, include, entrypoint, target, sflags, eflags, shader, error_messages)
            : E_FAIL;
}

// The DX12 backend asks DXGI whether tearing is supported; the same stand-in
// keeps dxgi.dll out of the proxy's import table (the game has it loaded anyway).
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
  using Fn = HRESULT(WINAPI*)(REFIID, void**);
  static Fn fn = []() -> Fn {
    HMODULE m = LoadLibraryA("dxgi.dll");
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, "CreateDXGIFactory1")) : nullptr;
  }();
  return fn ? fn(riid, factory) : E_FAIL;
}

// What the swap chain's vtable slots point at: an entry that opens the way
// Windows' own code does, then jumps to the hook with every register and the
// stack untouched. On Windows the Steam overlay hooks a swap chain by patching
// the start of the function each slot points to - ours, once the mod has
// hooked the class - and its decoder does not know what mingw emits (its log,
// 2026-09-25: "Unknown opcodes for AMD64 at 4 bytes: 48 83 EC 48 41 89 D1 ...
// module=steam_api64.dll,DXGISwapChain_Present"). It sent Present to its hook
// anyway, with a null original: the game crashed on its first frame with the
// panel. dxgi's own Present opens with `mov [rsp+10h],rbx; mov [rsp+18h],rsi`,
// stores into the home space the callee owns; three of those make 15 bytes it
// can move, enough for any detour, and the jump after them is never decoded.
#define RE2CC_DETOURABLE(name)                                            \
  extern "C" {                                                           \
  void* re2cc_##name##_target = nullptr;                                 \
  void re2cc_##name##_entry();                                           \
  }                                                                      \
  asm(".text\n"                                                          \
      ".p2align 4\n"                                                     \
      ".globl re2cc_" #name "_entry\n"                                   \
      "re2cc_" #name "_entry:\n"                                         \
      "\tmovq %rbx, 16(%rsp)\n"                                          \
      "\tmovq %rsi, 24(%rsp)\n"                                          \
      "\tmovq %rdi, 32(%rsp)\n"                                          \
      "\tjmp *re2cc_" #name "_target(%rip)\n");
RE2CC_DETOURABLE(present)
RE2CC_DETOURABLE(resize)
RE2CC_DETOURABLE(present1)
RE2CC_DETOURABLE(resize1)
#undef RE2CC_DETOURABLE

namespace re2cc::overlay::d3d {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);

PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
ResizeBuffersFn g_orig_resize = nullptr;
ResizeBuffers1Fn g_orig_resize1 = nullptr;
uintptr_t g_vt = 0;  // the swap chain class's vtable, patched

enum class Api { Unknown, D3D11, D3D12, Other };
IDXGISwapChain* g_sc = nullptr;  // the game's swap chain (the first one that presents a window)
Api g_api = Api::Unknown;
thread_local int t_depth = 0;
thread_local int t_calls = 0;  // calls on to a swap chain's own function under way on this thread (see original())

DWORD g_game_thread = 0;  // the thread that loaded the mod: WinMain's, which makes the game's window
mem::Range g_self{};      // the mod's own image

volatile LONG g_presents = 0;
int g_rate_logs = 0;

// --- DX12 queue discovery (mod thread) ------------------------------------------------------
volatile LONG g_need_queue = 0;      // Present saw a D3D12 swap chain
volatile LONG g_queue_known = 0;     // the offsets below are set
volatile LONG g_queue_failed = 0;
int g_q_outer = -1;                  // -1: the queue pointer is in the swap chain itself
int g_q_inner = -1;
uintptr_t g_queue_vt = 0;            // what a real ID3D12CommandQueue's vtable is

// --- the game's own objects, each given a vtable of its own ------------------------------------
// The panel needs the game's Present. Putting the mod's function into DXGI's
// class vtable (the first design, the fallback below) puts it where the Steam
// overlay looks: on Windows the overlay hooks every new swap chain by writing a
// jump into whatever function each vtable slot points to, and keeps ONE saved
// original for its Present hook (gameoverlayrenderer64.dll+0x152E50, written
// by each hook it makes). For the game's first swap chain it found Windows'
// own Present; for the second, the mod's function - so its saved original led
// into the mod's function, whose own way on was Windows' Present, by then the
// overlay's hook: a circle, and a stack overflow on the render thread within a
// second of the first frame (2026-09-25, every launch; with a function the
// overlay could not decode, a call to 0 instead). So the class vtable is left
// alone. The mod takes the factory out of the game's CreateDXGIFactory
// import, and each object of interest - the factory, what it hands out for a
// factory interface, every swap chain it makes - gets a private copy of its
// vtable with the mod's functions in it, and its own vtable pointer moved to
// the copy. No vtable another hooker reads ever holds a function of the mod's;
// the mod calls on to the class's own functions, hooked in place by the
// overlay or not; and a D3D12 swap chain comes with the command queue the
// game made it with, so no throwaway device is needed to find it.
// A swap chain: the object's own vtable. (The factory is not given one: an
// implementation that checks an object's vtable pointer against its own -
// Wine's dxgi asserts on it as the factory makes a swap chain - would refuse
// the game's factory. Its class vtable is hooked in place instead, below: the
// overlay only ever hooks the class vtable of a swap chain, and on Windows
// with the overlay the game's factory is the overlay's own wrapper object.)
// Only the GAME's swap chains, and the class's functions as they are when
// called: REFramework finds the game's Present through a throwaway swap chain
// of its own, made from a factory of the same class - so through the factory
// slots hooked here - and hooks Present in that swap chain's vtable, expecting
// DXGI's class vtable, which the game's swap chain shares; when the game's
// swap chain presents through that hook it moves its hook into the game's
// object and gives the class its slot back. Adopted, the throwaway had a copy
// of its own, REFramework's hook went into the copy, the game's frames never
// reached it, and REFramework made a new throwaway every 11 s for as long as
// the game ran - no REFramework mod worked (a player's logs, 2026-09-26, with
// 1.0.5). So a swap chain made for no window, or for a window that is not the
// game's, keeps its class's vtable (game_window), and the hooks call on through
// the class's vtable as it is at the time (original()).
//
// The slots of IDXGISwapChain3's vtable an adopted swap chain's copy has the
// mod's functions in, in the order Owned::real keeps what the class had there.
constexpr int kPresent = 8, kSetFullscreenState = 10, kResizeBuffers = 13, kPresent1 = 22, kResizeBuffers1 = 39;
constexpr int kHookedSlots[] = {kPresent, kResizeBuffers, kPresent1, kResizeBuffers1, kSetFullscreenState};
constexpr int kHooked = sizeof(kHookedSlots) / sizeof(kHookedSlots[0]);
int hooked_index(int slot) {
  for (int i = 0; i < kHooked; ++i)
    if (kHookedSlots[i] == slot) return i;
  return -1;
}
struct Owned {
  void* obj = nullptr;                  // the object; stale once the game frees it (a new one at the address is adopted afresh)
  void** orig = nullptr;                // the class's vtable: what the hooks call on to
  void** copy = nullptr;                // the object's own; never freed (the object may outlive what the mod knows)
  int slots = 0;                        // copied - more than the interface has, see adopt_chain
  int iface = 0;                        // what the object is known to be: 18, 29, 40 or 41 (IDXGISwapChain..4)
  ID3D12CommandQueue* queue = nullptr;  // a D3D12 swap chain's, as the game made it (no reference held)
  void* real[kHooked] = {};             // what the class had in kHookedSlots when the swap chain was adopted
};
// A factory class's vtable, hooked in place: what its slots held.
struct FactoryVtable {
  void** vt = nullptr;
  int slots = 0;
  void* orig[32] = {};  // by slot, for the slots hooked
};
constexpr int kChainsMax = 64, kFactoriesMax = 16;
Owned g_chains[kChainsMax];
FactoryVtable g_factories[kFactoriesMax];
int g_n_factories = 0, g_n_chains = 0;
CRITICAL_SECTION g_owned_cs;
bool g_owned_cs_ready = false;
bool g_adopting = false;  // the import hooks are in

// The DXGI wrappers a player may have beside re2.exe - ReShade, Special K,
// OptiScaler, each a dxgi.dll in the game's folder - hand the game proxy
// objects of their own around DXGI's: a whole IDXGISwapChain4 in all three
// (41 slots), and each answers QueryInterface with an IID of its own (ReShade's
// IID_UnwrappedObject and Special K's IID_IUnwrappedDXGISwapChain for the
// object inside, OptiScaler's __uuidof(WrappedIDXGISwapChain4) for itself; all
// add the reference they should). Two things a proxy taught the mod on
// 2026-09-25, both from a player's 1.0.3 crash (OptiScaler, as it turned out):
//  - Never ask a proxy which swap chain interfaces it has. Special K's
//    QueryInterface for a higher one than the proxy was made with promotes the
//    inner object and hands the proxy back WITHOUT adding a reference
//    (IWrapDXGISwapChain::QueryInterface), so the Release that pairs with it
//    destroys the proxy and the swap chain under the game. A proxy is known by
//    its IID and taken as IDXGISwapChain4; DXGI's own object may be asked
//    (dxgi.dll counts right); anything else is sized by what made it and what
//    it is for (a D3D12 swap chain is IDXGISwapChain3 or the game could not use
//    it), and never asked.
//  - Copy more of the vtable than the interface has. OptiScaler's proxy has a
//    virtual destructor (`virtual ~WrappedIDXGISwapChain4()`), which MSVC puts
//    in the slot after the interface's 41; its Release at zero does `delete
//    this` through the object's vtable pointer - the copy - and a copy of 41
//    slots ended in whatever the heap held next: a call into it from the
//    wrapper's code, on the game's thread, the instant the game let go of a
//    swap chain (`call rax` to an unmapped address, this in rcx). So the copy
//    is 64 slots (what the read allows), the extra ones the class's own bytes,
//    for its own use; the mod touches only the four it hooks.
constexpr GUID kUnwrappedReShade = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
constexpr GUID kUnwrappedSpecialK = {0xe8a33b4a, 0x1405, 0x424c, {0xae, 0x88, 0x0d, 0x3e, 0x9d, 0x46, 0xc9, 0x14}};
constexpr GUID kOptiScalerSwapChain = {0x3af622a3, 0x82d0, 0x49cd, {0x99, 0x4f, 0xcc, 0xe0, 0x51, 0x22, 0xc2, 0x22}};
constexpr int kSwapChain4Slots = 41;
constexpr int kCopySlots = 64;

// The wrapper an object is a proxy of, or null for none known. (Special K's
// proxy forwards an IID it does not know to the object inside, so its IID is
// tried first: a ReShade proxy inside one would answer ReShade's.)
const char* wrapper_of(IUnknown* obj) {
  struct {
    const GUID* iid;
    const char* name;
  } const known[] = {{&kUnwrappedSpecialK, "Special K"}, {&kUnwrappedReShade, "ReShade"}, {&kOptiScalerSwapChain, "OptiScaler"}};
  for (const auto& k : known) {
    IUnknown* inner = nullptr;
    if (SUCCEEDED(obj->QueryInterface(*k.iid, reinterpret_cast<void**>(&inner))) && inner) {
      inner->Release();
      return k.name;
    }
  }
  return nullptr;
}

HMODULE module_of(const void* p) {
  HMODULE m = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCSTR>(p), &m);
  return m;
}

// Whether a module is Windows' own dxgi.dll - told by its file, not by
// loading it: OptiScaler, loaded as the game's dxgi.dll, answers every
// LoadLibrary of a dxgi.dll - System32's by its full path included - with
// itself (LibraryLoad_Hooks.cpp: "call, returning this dll!"), so the mod once
// took OptiScaler for Windows' DXGI and DXGI's own swap chains for objects of
// another kind (a player's log, 2026-09-26).
bool is_windows_dxgi(HMODULE m) {
  wchar_t path[MAX_PATH], own[MAX_PATH];
  const UINT n = GetSystemDirectoryW(own, MAX_PATH);
  if (!m || !n || n + 10 >= MAX_PATH || !GetModuleFileNameW(m, path, MAX_PATH)) return false;
  lstrcatW(own, L"\\dxgi.dll");
  return lstrcmpiW(path, own) == 0;
}

// Whether a swap chain made for this window is the game's: a window of this
// process made by the game's window thread (WinMain's, which loaded the mod
// on the game's first Steam call) or of the engine's own class. Not the
// throwaway ones of other tools (REFramework's: for composition - no window at
// all -, then a hidden window of its own thread's, then the desktop's).
bool game_window(HWND w) {
  DWORD pid = 0;
  const DWORD tid = w ? GetWindowThreadProcessId(w, &pid) : 0;
  if (!tid || pid != GetCurrentProcessId()) return false;
  if (tid == g_game_thread) return true;
  char cls[16] = {};
  return GetClassNameA(w, cls, sizeof(cls)) && std::strcmp(cls, "via") == 0;
}

struct OwnedLock {
  OwnedLock() { EnterCriticalSection(&g_owned_cs); }
  ~OwnedLock() { LeaveCriticalSection(&g_owned_cs); }
};

Owned* find_owned(Owned* list, int n, const void* obj) {  // under g_owned_cs
  for (int i = 0; i < n; ++i)
    if (list[i].obj == obj) return &list[i];
  return nullptr;
}

// The swap chain's entry, by value (the render thread reads it every frame).
Owned chain_of(const void* sc) {
  Owned none;
  if (!g_owned_cs_ready) return none;
  OwnedLock lock;
  const Owned* o = find_owned(g_chains, g_n_chains, sc);
  return o ? *o : none;
}

HWND make_dummy_window() {
  return CreateWindowExA(0, "STATIC", "RE2CabbyCodes dummy", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                         GetModuleHandleA(nullptr), nullptr);
}

void find_queue_offsets() {
  HMODULE d3d12 = LoadLibraryA("d3d12.dll");
  HMODULE dxgi = LoadLibraryA("dxgi.dll");
  auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
  using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
  auto create_factory = dxgi ? reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  if (!create_device || !create_factory) {
    logf("ERROR: dx12: D3D12CreateDevice/CreateDXGIFactory1 not found - no panel under DX12");
    InterlockedExchange(&g_queue_failed, 1);
    return;
  }
  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  IDXGIFactory2* factory = nullptr;
  IDXGISwapChain1* sc = nullptr;
  HWND wnd = make_dummy_window();
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = 64;
  sd.Height = 64;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  bool ok = wnd && SUCCEEDED(create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) &&
            SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) &&
            SUCCEEDED(create_factory(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))) &&
            SUCCEEDED(factory->CreateSwapChainForHwnd(queue, wnd, &sd, nullptr, nullptr, &sc));
  if (ok) {
    const uintptr_t q = reinterpret_cast<uintptr_t>(queue);
    const uintptr_t s = reinterpret_cast<uintptr_t>(sc);
    for (int i = 0; i < 512 * 8 && g_q_inner < 0; i += 8)
      if (mem::readable(s + i, 8) && mem::read<uintptr_t>(s + i) == q) g_q_inner = i;
    for (int base = 0; base < 512 * 8 && g_q_inner < 0; base += 8) {
      const uintptr_t inner = mem::readable(s + base, 8) ? mem::read<uintptr_t>(s + base) : 0;
      if (!mem::plausible(inner) || !mem::readable(inner, 8)) continue;
      for (int i = 0; i < 512 * 8; i += 8) {
        if (!mem::readable(inner + i, 8)) break;
        if (mem::read<uintptr_t>(inner + i) == q) {
          g_q_outer = base;
          g_q_inner = i;
          break;
        }
      }
    }
    g_queue_vt = mem::read<uintptr_t>(q);
  }
  if (sc) sc->Release();
  if (factory) factory->Release();
  if (queue) queue->Release();
  if (dev) dev->Release();
  if (wnd) DestroyWindow(wnd);
  if (!ok || g_q_inner < 0) {
    logf("ERROR: dx12: %s - no panel under DX12", ok ? "the queue pointer was not found in a swap chain" : "the dummy D3D12 swap chain could not be made");
    InterlockedExchange(&g_queue_failed, 1);
    return;
  }
  if (g_q_outer < 0) logf("dx12: a swap chain keeps its command queue at +0x%X", g_q_inner);
  else logf("dx12: a swap chain keeps its command queue at [+0x%X]+0x%X (a wrapped swap chain - Proton)", g_q_outer, g_q_inner);
  InterlockedExchange(&g_queue_known, 1);
}

ID3D12CommandQueue* queue_of(IDXGISwapChain* sc) {
  if (ID3D12CommandQueue* q = chain_of(sc).queue) return q;  // the game handed it over as it made the swap chain
  if (!g_queue_known) {
    if (!g_queue_failed && !InterlockedExchange(&g_need_queue, 1))
      logf("dx12: the game presents through Direct3D 12 - looking for its command queue");
    return nullptr;
  }
  uintptr_t base = reinterpret_cast<uintptr_t>(sc);
  if (g_q_outer >= 0) base = mem::read_ptr(base + g_q_outer);
  const uintptr_t q = base ? mem::read_ptr(base + g_q_inner) : 0;
  if (!q || mem::read_ptr(q) != g_queue_vt) return nullptr;
  return reinterpret_cast<ID3D12CommandQueue*>(q);
}

// --- DX11 -----------------------------------------------------------------------------------------
struct Dx11 {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  bool init = false;
  bool failed = false;
} g11;

void dx11_release_targets() {
  if (g11.rtv) {
    g11.rtv->Release();
    g11.rtv = nullptr;
  }
}

void dx11_frame(IDXGISwapChain* sc) {
  if (g11.failed) return;
  if (!g11.init) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g11.dev)))) {
      g11.failed = true;
      return;
    }
    g11.dev->GetImmediateContext(&g11.ctx);
    if (!ImGui_ImplDX11_Init(g11.dev, g11.ctx)) {
      logf("ERROR: dx11: ImGui's DX11 backend did not start");
      g11.failed = true;
      return;
    }
    g11.init = true;
    logf("dx11: panel renderer ready (device %p)", static_cast<void*>(g11.dev));
  }
  if (!g11.rtv) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = td.Format;
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    const HRESULT hr = g11.dev->CreateRenderTargetView(back, &rd, &g11.rtv);
    back->Release();
    if (FAILED(hr)) {
      logf("ERROR: dx11: no render target for the back buffer (format %d, hr 0x%08lX)", td.Format, static_cast<unsigned long>(hr));
      g11.failed = true;
      return;
    }
    logf("dx11: drawing into the back buffer (%ux%u, format %d)", td.Width, td.Height, td.Format);
  }
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  draw_panel();
  ImGui::Render();
  ID3D11RenderTargetView* old_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* old_dsv = nullptr;
  g11.ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, &old_dsv);
  g11.ctx->OMSetRenderTargets(1, &g11.rtv, nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  g11.ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, old_dsv);
  for (auto* r : old_rtv)
    if (r) r->Release();
  if (old_dsv) old_dsv->Release();
}

// --- DX12 -----------------------------------------------------------------------------------------
struct Frame {
  ID3D12CommandAllocator* alloc = nullptr;
  ID3D12Resource* back = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  UINT64 fence = 0;
};
struct Dx12 {
  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  ID3D12DescriptorHeap* srv_heap = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  UINT64 fence_value = 0;
  std::vector<Frame> frames;
  UINT rtv_step = 0, srv_step = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool init = false, targets = false, failed = false;
  std::vector<int> srv_free;
} g12;
constexpr int kSrvDescriptors = 64;

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
  if (g12.srv_free.empty()) {
    logf("ERROR: dx12: the panel ran out of texture descriptors");
    cpu->ptr = 0;
    gpu->ptr = 0;
    return;
  }
  const int i = g12.srv_free.back();
  g12.srv_free.pop_back();
  cpu->ptr = g12.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(i) * g12.srv_step;
  gpu->ptr = g12.srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(i) * g12.srv_step;
}

void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
  const SIZE_T start = g12.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
  if (cpu.ptr >= start && g12.srv_step) g12.srv_free.push_back(static_cast<int>((cpu.ptr - start) / g12.srv_step));
}

void dx12_wait_idle() {
  if (!g12.queue || !g12.fence) return;
  const UINT64 v = ++g12.fence_value;
  if (FAILED(g12.queue->Signal(g12.fence, v))) return;
  if (g12.fence->GetCompletedValue() < v && SUCCEEDED(g12.fence->SetEventOnCompletion(v, g12.event)))
    WaitForSingleObject(g12.event, 2000);
}

void dx12_release_targets() {
  if (!g12.init) return;
  dx12_wait_idle();
  for (Frame& f : g12.frames)
    if (f.back) {
      f.back->Release();
      f.back = nullptr;
    }
  g12.targets = false;
}

bool dx12_init(IDXGISwapChain* sc, ID3D12CommandQueue* queue) {
  if (g12.init) return true;
  if (g12.failed) return false;
  DXGI_SWAP_CHAIN_DESC sd{};
  if (FAILED(sc->GetDesc(&sd)) || FAILED(sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&g12.dev)))) {
    g12.failed = true;
    return false;
  }
  g12.queue = queue;
  g12.format = sd.BufferDesc.Format;
  const UINT n = sd.BufferCount;
  D3D12_DESCRIPTOR_HEAP_DESC rh{};
  rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rh.NumDescriptors = n;
  D3D12_DESCRIPTOR_HEAP_DESC sh{};
  sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  sh.NumDescriptors = kSrvDescriptors;
  sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  bool ok = SUCCEEDED(g12.dev->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g12.rtv_heap))) &&
            SUCCEEDED(g12.dev->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g12.srv_heap))) &&
            SUCCEEDED(g12.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g12.fence)));
  g12.frames.assign(n, Frame{});
  for (UINT i = 0; ok && i < n; ++i)
    ok = SUCCEEDED(g12.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                   reinterpret_cast<void**>(&g12.frames[i].alloc)));
  ok = ok && SUCCEEDED(g12.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g12.frames[0].alloc, nullptr,
                                                  __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g12.list)));
  if (ok) g12.list->Close();
  g12.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!ok || !g12.event) {
    logf("ERROR: dx12: the panel's device objects could not be made");
    g12.failed = true;
    return false;
  }
  g12.rtv_step = g12.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  g12.srv_step = g12.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  g12.srv_free.clear();
  for (int i = kSrvDescriptors - 1; i >= 0; --i) g12.srv_free.push_back(i);
  ImGui_ImplDX12_InitInfo ii;
  ii.Device = g12.dev;
  ii.CommandQueue = queue;
  ii.NumFramesInFlight = static_cast<int>(n);
  ii.RTVFormat = g12.format;
  ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
  ii.SrvDescriptorHeap = g12.srv_heap;
  ii.SrvDescriptorAllocFn = &srv_alloc;
  ii.SrvDescriptorFreeFn = &srv_free;
  if (!ImGui_ImplDX12_Init(&ii)) {
    logf("ERROR: dx12: ImGui's DX12 backend did not start");
    g12.failed = true;
    return false;
  }
  g12.init = true;
  logf("dx12: panel renderer ready (device %p, queue %p, %u back buffers, format %d)", static_cast<void*>(g12.dev),
       static_cast<void*>(queue), n, g12.format);
  return true;
}

bool dx12_targets(IDXGISwapChain* sc) {
  if (g12.targets) return true;
  DXGI_SWAP_CHAIN_DESC sd{};
  if (FAILED(sc->GetDesc(&sd))) return false;
  if (sd.BufferCount != g12.frames.size() || sd.BufferDesc.Format != g12.format) {
    logf("dx12: the swap chain changed to %u buffers, format %d (was %u, %d) - the panel stops until the game restarts",
         sd.BufferCount, sd.BufferDesc.Format, static_cast<unsigned>(g12.frames.size()), g12.format);
    g12.failed = true;
    return false;
  }
  for (UINT i = 0; i < sd.BufferCount; ++i) {
    Frame& f = g12.frames[i];
    if (FAILED(sc->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&f.back))) || !f.back) return false;
    f.rtv.ptr = g12.rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(i) * g12.rtv_step;
    g12.dev->CreateRenderTargetView(f.back, nullptr, f.rtv);
  }
  g12.targets = true;
  return true;
}

void barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  g12.list->ResourceBarrier(1, &b);
}

void dx12_frame(IDXGISwapChain* sc) {
  ID3D12CommandQueue* queue = queue_of(sc);
  if (!queue || !dx12_init(sc, queue) || !dx12_targets(sc)) return;
  // The back buffer index. An adopted swap chain is known to have the slot
  // (40 or more): a straight call, no QueryInterface - on a Special K proxy
  // the QueryInterface for IDXGISwapChain3 would not count the reference the
  // Release then takes away (see wrapper_of).
  UINT bi = 0;
  const Owned own = chain_of(sc);
  if (own.orig && own.iface >= 40) {
    bi = static_cast<IDXGISwapChain3*>(sc)->GetCurrentBackBufferIndex();
  } else {
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) || !sc3) return;
    bi = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
  }
  if (bi >= g12.frames.size()) return;
  Frame& fr = g12.frames[bi];
  if (g12.fence->GetCompletedValue() < fr.fence && SUCCEEDED(g12.fence->SetEventOnCompletion(fr.fence, g12.event)))
    WaitForSingleObject(g12.event, 2000);
  if (FAILED(fr.alloc->Reset()) || FAILED(g12.list->Reset(fr.alloc, nullptr))) return;
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  draw_panel();
  ImGui::Render();
  barrier(fr.back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  g12.list->OMSetRenderTargets(1, &fr.rtv, FALSE, nullptr);
  ID3D12DescriptorHeap* heaps[] = {g12.srv_heap};
  g12.list->SetDescriptorHeaps(1, heaps);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g12.list);
  barrier(fr.back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  if (FAILED(g12.list->Close())) return;
  ID3D12CommandList* lists[] = {g12.list};
  queue->ExecuteCommandLists(1, lists);
  queue->Signal(g12.fence, ++g12.fence_value);
  fr.fence = g12.fence_value;
}

// --- the hooks ------------------------------------------------------------------------------------
Api api_of(IDXGISwapChain* sc) {
  IUnknown* dev = nullptr;
  if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) && dev) {
    dev->Release();
    return Api::D3D12;
  }
  if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) {
    dev->Release();
    return Api::D3D11;
  }
  return Api::Other;
}

// The game's window as it is, for the log: its place and size, and the
// monitor's - in the game's view of the desktop (the mod's threads are as
// DPI aware as the game's).
void window_note(HWND w, char* out, size_t n) {
  RECT wr{}, cr{};
  if (!w || !GetWindowRect(w, &wr) || !GetClientRect(w, &cr)) {
    std::snprintf(out, n, "its window unknown");
    return;
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  const HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
  if (m && GetMonitorInfoW(m, &mi))
    std::snprintf(out, n, "its window %ldx%ld at %ld,%ld (inside %ldx%ld), its monitor %ldx%ld at %ld,%ld",
                  wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top, cr.right, cr.bottom,
                  mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left,
                  mi.rcMonitor.top);
  else
    std::snprintf(out, n, "its window %ldx%ld at %ld,%ld (inside %ldx%ld)", wr.right - wr.left, wr.bottom - wr.top, wr.left,
                  wr.top, cr.right, cr.bottom);
}

// The window's DPI and its DPI awareness (Windows 10 1607 and later), for the log.
void dpi_note(HWND w, char* out, size_t n) {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  using GetWindowContextFn = void*(WINAPI*)(HWND);
  using GetAwarenessFn = int(WINAPI*)(void*);
  const HMODULE user32 = GetModuleHandleA("user32.dll");
  const auto dpi = user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
  const auto ctx = user32 ? reinterpret_cast<GetWindowContextFn>(GetProcAddress(user32, "GetWindowDpiAwarenessContext")) : nullptr;
  const auto aware = user32 ? reinterpret_cast<GetAwarenessFn>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext")) : nullptr;
  if (!dpi || !ctx || !aware) {
    std::snprintf(out, n, "DPI not known");
    return;
  }
  const int a = aware(ctx(w));
  std::snprintf(out, n, "%u DPI, %s", dpi(w),
                a == 0 ? "not DPI aware" : a == 1 ? "system DPI aware" : a == 2 ? "per-monitor DPI aware" : "DPI awareness unknown");
}

HWND window_of(IDXGISwapChain* sc) {
  DXGI_SWAP_CHAIN_DESC sd{};
  return SUCCEEDED(sc->GetDesc(&sd)) ? sd.OutputWindow : nullptr;
}

void on_present(IDXGISwapChain* sc) {
  if (g_sc && sc != g_sc) return;  // another swap chain (a tool's, a launcher's): not the game's
  DXGI_SWAP_CHAIN_DESC sd{};
  if (FAILED(sc->GetDesc(&sd)) || !sd.OutputWindow || !IsWindowVisible(sd.OutputWindow)) return;
  if (!g_sc) {
    g_sc = sc;
    g_api = api_of(sc);
    char where[200], dpi[64];
    window_note(sd.OutputWindow, where, sizeof(where));
    dpi_note(sd.OutputWindow, dpi, sizeof(dpi));
    logf("overlay: the game presents through %s (swap chain %p%s, %ux%u, %u buffers, format %d, window %p) - %s, %s",
         g_api == Api::D3D12 ? "Direct3D 12" : g_api == Api::D3D11 ? "Direct3D 11" : "an unknown device",
         static_cast<void*>(sc), chain_of(sc).copy ? " - adopted as it was made" : " - the class hook", sd.BufferDesc.Width,
         sd.BufferDesc.Height, sd.BufferCount, sd.BufferDesc.Format, static_cast<void*>(sd.OutputWindow), where, dpi);
  }
  dispatch::set_game_window(sd.OutputWindow);
  if (g_api != Api::D3D11 && g_api != Api::D3D12) return;
  ImGuiLock guard;
  if (!ensure_context(sd.OutputWindow) || !wants_draw()) return;
  if (g_api == Api::D3D11) dx11_frame(sc);
  else dx12_frame(sc);
}

// What a hook calls on to. For an adopted swap chain, the class's function as
// the class's vtable has it NOW: a tool that hooks DXGI by writing its own
// function into the class's vtable after the game's swap chain was adopted
// must still get the game's frames - REFramework does, until the game's first
// frame through it (see Owned); with the function the class had when the swap
// chain was adopted, it never saw one. What the class had then is called
// instead when the slot holds a function of the mod's own (the class hook, the
// fallback, is in as well) and while a call on to the swap chain's own
// function is already under way on this thread - a hook in the class's vtable
// that calls the object's vtable again (REFramework's does, once, as it moves
// into the object) must not come back into itself.
template <typename Fn>
Fn original(IDXGISwapChain* sc, int slot, Fn fallback) {
  const Owned o = chain_of(sc);
  const int i = hooked_index(slot);
  if (!o.orig || i < 0) return fallback;
  void* now = nullptr;
  if (t_calls == 0 && slot < o.iface && mem::read_safe(reinterpret_cast<uintptr_t>(&o.orig[slot]), &now) && now &&
      !g_self.contains(reinterpret_cast<uintptr_t>(now)))
    return reinterpret_cast<Fn>(now);
  return o.real[i] ? reinterpret_cast<Fn>(o.real[i]) : fallback;
}

// Around a call on to a swap chain's own function (see original()).
struct CallingOn {
  CallingOn() { ++t_calls; }
  ~CallingOn() { --t_calls; }
  CallingOn(const CallingOn&) = delete;
  CallingOn& operator=(const CallingOn&) = delete;
};

// A Present that comes back in while one is on its way on to the class (see
// original()) is the same frame: the panel is drawn the first time through.
HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (t_calls == 0) InterlockedIncrement(&g_presents);
  if (t_depth == 0 && t_calls == 0 && !(flags & DXGI_PRESENT_TEST)) {
    ++t_depth;
    on_present(sc);
    --t_depth;
  }
  const PresentFn orig = original(sc, kPresent, g_orig_present);
  CallingOn calling;
  return orig ? orig(sc, sync, flags) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
  if (t_calls == 0) InterlockedIncrement(&g_presents);
  if (t_depth == 0 && t_calls == 0 && !(flags & DXGI_PRESENT_TEST)) {
    ++t_depth;
    on_present(sc);
    --t_depth;
  }
  const Present1Fn orig = original(sc, kPresent1, g_orig_present1);
  CallingOn calling;
  return orig ? orig(sc, sync, flags, p) : DXGI_ERROR_INVALID_CALL;
}

void before_resize(IDXGISwapChain* sc) {
  if (sc != g_sc) return;
  ImGuiLock guard;
  if (g_api == Api::D3D11) dx11_release_targets();
  else if (g_api == Api::D3D12) dx12_release_targets();
}

void log_resize(IDXGISwapChain* sc, UINT w, UINT h, HRESULT hr, const char* how) {
  char where[200];
  window_note(window_of(sc), where, sizeof(where));
  logf("overlay: the game resized its swap chain to %ux%u (%shr 0x%08lX) - %s", w, h, how, static_cast<unsigned long>(hr), where);
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
  before_resize(sc);
  const ResizeBuffersFn orig = original(sc, kResizeBuffers, g_orig_resize);
  HRESULT hr;
  {
    CallingOn calling;
    hr = orig ? orig(sc, n, w, h, f, flags) : DXGI_ERROR_INVALID_CALL;
  }
  if (sc == g_sc) log_resize(sc, w, h, hr, "");
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                     const UINT* masks, IUnknown* const* queues) {
  before_resize(sc);
  const ResizeBuffers1Fn orig = original(sc, kResizeBuffers1, g_orig_resize1);
  HRESULT hr;
  {
    CallingOn calling;
    hr = orig ? orig(sc, n, w, h, f, flags, masks, queues) : DXGI_ERROR_INVALID_CALL;
  }
  if (sc == g_sc) log_resize(sc, w, h, hr, "ResizeBuffers1, ");
  return hr;
}

// Only logged: whether the game's request for exclusive fullscreen took. In
// its Fullscreen mode the game moves its window to the monitor's corner,
// keeping its size, and then asks DXGI for fullscreen (re2.exe exe+0x2B30F4C:
// SetFullscreenState(FALSE), SetWindowPos with SWP_NOSIZE, SetFullscreenState
// (TRUE)), so a request that does not take leaves a small window in the top
// left corner - a player's report of 2026-09-26 (beside REFramework and
// OptiScaler) that the logs of that run could not explain.
HRESULT STDMETHODCALLTYPE hk_set_fullscreen(IDXGISwapChain* sc, BOOL fullscreen, IDXGIOutput* target) {
  const SetFullscreenStateFn orig = original(sc, kSetFullscreenState, static_cast<SetFullscreenStateFn>(nullptr));
  HRESULT hr;
  {
    CallingOn calling;
    hr = orig ? orig(sc, fullscreen, target) : DXGI_ERROR_INVALID_CALL;
  }
  static volatile LONG logged = 0;
  if (InterlockedIncrement(&logged) <= 20) {
    const HWND w = window_of(sc);
    char where[200], fg[200] = "";
    window_note(w, where, sizeof(where));
    if (FAILED(hr)) {
      const HWND f = GetForegroundWindow();
      char cls[64] = "?";
      DWORD pid = 0;
      if (f) {
        GetClassNameA(f, cls, sizeof(cls));
        GetWindowThreadProcessId(f, &pid);
      }
      std::snprintf(fg, sizeof(fg), "; the foreground window is %s (%p, class '%s')",
                    !f ? "none" : f == w ? "the game's" : pid == GetCurrentProcessId() ? "another of the game's" : "another program's",
                    static_cast<void*>(f), cls);
    }
    logf("overlay: the game asked DXGI for %s: hr 0x%08lX%s%s - %s", fullscreen ? "exclusive fullscreen" : "a window",
         static_cast<unsigned long>(hr), hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? " (not currently available)" : "", fg, where);
  }
  return hr;
}


// --- adopting ------------------------------------------------------------------------------------
struct Hook {
  int slot;
  void* fn;
};

// Points an object at a private copy of its vtable with `hooks` in it (the
// slots the copy has), and keeps the entry. An object already ours is left as
// it is; one the game freed and made again at the same address is adopted
// afresh (its entry reused, the old copy left to the leak it is).
Owned* adopt(Owned* list, int* n, int max, void* obj, int iface, const Hook* hooks, int nhooks, ID3D12CommandQueue* queue,
             bool* already) {
  void** vt = nullptr;
  *already = false;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(obj), &vt) || !vt || iface <= 0) return nullptr;
  OwnedLock lock;
  Owned* o = find_owned(list, *n, obj);
  if (o && o->copy == vt) {  // ours already (DXGI's CreateSwapChain makes its swap chain through CreateSwapChainForHwnd)
    *already = true;
    return o;
  }
  if (!o) {
    if (*n >= max) return nullptr;
    o = &list[*n];
  }
  // As many slots as the read allows, the interface's at least (a vtable at
  // the very end of what is mapped) - and the one before the first, where
  // MSVC keeps a class's type information: what typeid and dynamic_cast read
  // through the object (a wrapper built with it may ask), and what a vtable
  // hook of REFramework's copies with the rest.
  int slots = kCopySlots > iface ? kCopySlots : iface;
  void** block = new void*[slots + 1];
  void** copy = block + 1;
  for (;;) {
    if (mem::copy_from(block, reinterpret_cast<uintptr_t>(vt - 1), static_cast<size_t>(slots + 1) * sizeof(void*))) break;
    if (mem::copy_from(copy, reinterpret_cast<uintptr_t>(vt), static_cast<size_t>(slots) * sizeof(void*))) {
      block[0] = nullptr;
      break;
    }
    if (slots <= iface) {
      delete[] block;
      return nullptr;
    }
    slots = slots - 16 > iface ? slots - 16 : iface;
  }
  for (int i = 0; i < nhooks; ++i)
    if (hooks[i].slot < iface) copy[hooks[i].slot] = hooks[i].fn;
  *o = Owned{obj, vt, copy, slots, iface, queue, {}};
  if (o == &list[*n]) ++*n;
  InterlockedExchangePointer(reinterpret_cast<void**>(obj), copy);
  return o;
}

// How many slots an object's vtable has: the highest interface it answers
// with the same vtable (a QueryInterface that hands back another object with
// another vtable does not count).
struct Level {
  const IID* iid;
  int slots;
};
int slots_of(IUnknown* obj, void** vt, const Level* levels, int n, int least) {
  for (int i = 0; i < n; ++i) {
    IUnknown* p = nullptr;
    if (FAILED(obj->QueryInterface(*levels[i].iid, reinterpret_cast<void**>(&p))) || !p) continue;
    void** pvt = nullptr;
    const bool same = mem::read_safe(reinterpret_cast<uintptr_t>(p), &pvt) && pvt == vt;
    p->Release();
    if (same) return levels[i].slots;
  }
  return least;
}

// A swap chain made for no window or for another's (see Owned): left with its
// class's vtable. The first few are logged, with the code that asked for them.
void leave_alone(const char* how, HWND hwnd, void* caller) {
  static volatile LONG logged = 0;
  const LONG n = InterlockedIncrement(&logged);
  if (n > 8) return;
  char from[MAX_PATH + 32], what[160];
  describe_address(caller, from, sizeof(from));
  if (!hwnd) {
    std::snprintf(what, sizeof(what), "for no window");
  } else {
    char cls[64] = "?";
    DWORD pid = 0;
    GetClassNameA(hwnd, cls, sizeof(cls));
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
      std::snprintf(what, sizeof(what), "for window %p (class '%s', thread %lu)", static_cast<void*>(hwnd), cls, tid);
    else
      std::snprintf(what, sizeof(what), "for window %p (class '%s', another program's)", static_cast<void*>(hwnd), cls);
  }
  logf("overlay: a swap chain made %s (%s, called from %s) is not the game's - it keeps its class's vtable, where a tool "
       "that hooks DXGI through a swap chain of its own (REFramework) must find the game's frames%s",
       what, how, from, n == 8 ? " (no more of these logged)" : "");
}

void adopt_chain(IUnknown* device, IUnknown* made, const char* how, HWND hwnd, void* caller) {
  if (!game_window(hwnd)) {
    leave_alone(how, hwnd, caller);
    return;
  }
  auto* sc = static_cast<IDXGISwapChain*>(made);
  void** vt = nullptr;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(sc), &vt) || !vt) return;
  {
    // Ours already: DXGI's CreateSwapChain makes its swap chain through
    // CreateSwapChainForHwnd, so the object comes by here twice.
    OwnedLock lock;
    const Owned* o = find_owned(g_chains, g_n_chains, sc);
    if (o && o->copy == vt) return;
  }
  ID3D12CommandQueue* queue = nullptr;
  if (device && SUCCEEDED(device->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) && queue)
    queue->Release();  // the game holds it; the pointer is only compared and used for this swap chain's frames
  const char* wrapper = wrapper_of(sc);
  const char* sized_by = "";
  int iface;
  if (wrapper) {
    iface = kSwapChain4Slots;
    sized_by = "a proxy of ";
  } else if (is_windows_dxgi(module_of(vt))) {
    static const Level levels[] = {{&__uuidof(IDXGISwapChain4), 41}, {&__uuidof(IDXGISwapChain3), 40},
                                   {&__uuidof(IDXGISwapChain1), 29}};
    iface = slots_of(sc, vt, levels, 3, 18);
    sized_by = "DXGI's own, asked";
  } else {
    // Not asked (see the wrappers above): D3D12 needs IDXGISwapChain3, and
    // the creation call promises IDXGISwapChain1 or IDXGISwapChain.
    iface = queue ? 40 : std::strcmp(how, "CreateSwapChain") == 0 ? 18 : 29;
    sized_by = queue ? "an object of another kind, taken as IDXGISwapChain3 (D3D12)" : "an object of another kind, taken as what made it";
  }
  static const Hook hooks[] = {{kPresent, reinterpret_cast<void*>(&hk_present)},
                               {kResizeBuffers, reinterpret_cast<void*>(&hk_resize)},
                               {kPresent1, reinterpret_cast<void*>(&hk_present1)},
                               {kResizeBuffers1, reinterpret_cast<void*>(&hk_resize1)},
                               {kSetFullscreenState, reinterpret_cast<void*>(&hk_set_fullscreen)}};
  bool already = false;
  Owned* o = adopt(g_chains, &g_n_chains, kChainsMax, sc, iface, hooks, kHooked, queue, &already);
  if (already) return;
  if (o) {
    // What the hooks call on to when the class's slot will not do (original()).
    // Should the class hook (the fallback) be in as well, the class's slots hold
    // the mod's own entries: what those replaced then - never the entries,
    // which would call the hooks in a circle.
    OwnedLock lock;
    void* found[kHooked] = {reinterpret_cast<void*>(g_orig_present), reinterpret_cast<void*>(g_orig_resize),
                            reinterpret_cast<void*>(g_orig_present1), reinterpret_cast<void*>(g_orig_resize1), nullptr};
    for (int i = 0; i < kHooked; ++i) {
      void* fn = kHookedSlots[i] < iface ? vt[kHookedSlots[i]] : nullptr;
      if (fn && g_self.contains(reinterpret_cast<uintptr_t>(fn))) fn = found[i];
      o->real[i] = fn;
    }
  }
  char where[MAX_PATH + 32];
  describe_address(vt, where, sizeof(where));
  if (o)
    logf("overlay: the game made a swap chain (%s, %s%p; %s%s) - it has a vtable of its own now (%d of its slots the "
         "interface's, %d copied; the class's at %s)",
         how, queue ? "D3D12 queue " : "device ", queue ? static_cast<void*>(queue) : static_cast<void*>(device), sized_by,
         wrapper ? wrapper : "", iface, o->slots, where);
  else
    logf("overlay: the game made a swap chain (%s; %s%s) that could not be adopted (%d slots, the class's vtable at %s)", how,
         sized_by, wrapper ? wrapper : "", iface, where);
}

using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                    IDXGISwapChain1**);
using CreateForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                          IDXGIOutput*, IDXGISwapChain1**);
using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                           IDXGIOutput*, IDXGISwapChain1**);
constexpr int kQueryInterface = 0, kCreateSwapChain = 10, kCreateForHwnd = 15, kCreateForCoreWindow = 16,
              kCreateForComposition = 24;  // IDXGIFactory2's vtable

void adopt_factory(IUnknown* factory, const char* how);

// A factory's class function, from its vtable's entry (a hook sits in a
// vtable the mod hooked, so the object's vtable pointer names the entry).
template <typename Fn>
Fn factory_original(void* self, int slot) {
  void** vt = nullptr;
  if (!g_owned_cs_ready || !mem::read_safe(reinterpret_cast<uintptr_t>(self), &vt) || !vt) return nullptr;
  OwnedLock lock;
  for (int i = 0; i < g_n_factories; ++i)
    if (g_factories[i].vt == vt && slot < g_factories[i].slots) return reinterpret_cast<Fn>(g_factories[i].orig[slot]);
  return nullptr;
}

bool is_factory(REFIID riid) {
  for (const IID* iid : {&__uuidof(IDXGIFactory), &__uuidof(IDXGIFactory1), &__uuidof(IDXGIFactory2), &__uuidof(IDXGIFactory3),
                         &__uuidof(IDXGIFactory4), &__uuidof(IDXGIFactory5), &__uuidof(IDXGIFactory6), &__uuidof(IDXGIFactory7)})
    if (IsEqualIID(riid, *iid)) return true;
  return false;
}

HRESULT STDMETHODCALLTYPE hk_factory_query_interface(IUnknown* self, REFIID riid, void** out) {
  const auto orig = factory_original<QueryInterfaceFn>(self, kQueryInterface);
  if (!orig) return E_NOINTERFACE;
  const HRESULT hr = orig(self, riid, out);
  if (SUCCEEDED(hr) && out && *out && is_factory(riid)) adopt_factory(static_cast<IUnknown*>(*out), "QueryInterface");
  return hr;
}

// The factory's calls: a swap chain for the game's window is adopted; one for
// a CoreWindow or for composition never is (the game makes neither).
HRESULT STDMETHODCALLTYPE hk_create_swap_chain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                               IDXGISwapChain** out) {
  const auto orig = factory_original<CreateSwapChainFn>(self, kCreateSwapChain);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const HWND hwnd = desc ? desc->OutputWindow : nullptr;
  const HRESULT hr = orig(self, device, desc, out);
  if (SUCCEEDED(hr) && out && *out) adopt_chain(device, *out, "CreateSwapChain", hwnd, __builtin_return_address(0));
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_for_hwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict_to,
                                             IDXGISwapChain1** out) {
  const auto orig = factory_original<CreateForHwndFn>(self, kCreateForHwnd);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const HRESULT hr = orig(self, device, hwnd, desc, fs, restrict_to, out);
  if (SUCCEEDED(hr) && out && *out) adopt_chain(device, *out, "CreateSwapChainForHwnd", hwnd, __builtin_return_address(0));
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_for_core_window(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
                                                    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict_to,
                                                    IDXGISwapChain1** out) {
  const auto orig = factory_original<CreateForCoreWindowFn>(self, kCreateForCoreWindow);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const HRESULT hr = orig(self, device, window, desc, restrict_to, out);
  if (SUCCEEDED(hr) && out && *out) adopt_chain(device, *out, "CreateSwapChainForCoreWindow", nullptr, __builtin_return_address(0));
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_for_composition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    IDXGIOutput* restrict_to, IDXGISwapChain1** out) {
  const auto orig = factory_original<CreateForCompositionFn>(self, kCreateForComposition);
  if (!orig) return DXGI_ERROR_INVALID_CALL;
  const HRESULT hr = orig(self, device, desc, restrict_to, out);
  if (SUCCEEDED(hr) && out && *out) adopt_chain(device, *out, "CreateSwapChainForComposition", nullptr, __builtin_return_address(0));
  return hr;
}

void adopt_factory(IUnknown* factory, const char* how) {
  void** vt = nullptr;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(factory), &vt) || !vt) return;
  {
    OwnedLock lock;
    for (int i = 0; i < g_n_factories; ++i)
      if (g_factories[i].vt == vt) return;  // this class's vtable is hooked already
  }
  static const Level levels[] = {{&__uuidof(IDXGIFactory7), 32}, {&__uuidof(IDXGIFactory6), 30}, {&__uuidof(IDXGIFactory5), 29},
                                 {&__uuidof(IDXGIFactory4), 28}, {&__uuidof(IDXGIFactory3), 26}, {&__uuidof(IDXGIFactory2), 25},
                                 {&__uuidof(IDXGIFactory1), 14}};
  const int slots = slots_of(factory, vt, levels, 7, 12);
  static const Hook hooks[] = {{kQueryInterface, reinterpret_cast<void*>(&hk_factory_query_interface)},
                               {kCreateSwapChain, reinterpret_cast<void*>(&hk_create_swap_chain)},
                               {kCreateForHwnd, reinterpret_cast<void*>(&hk_create_for_hwnd)},
                               {kCreateForCoreWindow, reinterpret_cast<void*>(&hk_create_for_core_window)},
                               {kCreateForComposition, reinterpret_cast<void*>(&hk_create_for_composition)}};
  char where[MAX_PATH + 32];
  describe_address(vt[kCreateSwapChain], where, sizeof(where));
  OwnedLock lock;
  if (g_n_factories >= kFactoriesMax) return;
  FactoryVtable& f = g_factories[g_n_factories];
  f = FactoryVtable{vt, slots, {}};
  if (!mem::copy_from(f.orig, reinterpret_cast<uintptr_t>(vt), static_cast<size_t>(slots) * sizeof(void*))) return;
  ++g_n_factories;  // registered before the first slot is written: a call through a hooked slot finds its original
  int hooked = 0;
  for (const Hook& h : hooks)
    if (h.slot < slots && mem::write<void*>(reinterpret_cast<uintptr_t>(&vt[h.slot]), h.fn)) ++hooked;
  logf("overlay: the game's DXGI factory (from %s; %d slots, CreateSwapChain %s): %d slot(s) of its class vtable hooked in place",
       how, slots, where, hooked);
}

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
CreateFactoryFn g_real_create_factory = nullptr, g_real_create_factory1 = nullptr;
CreateFactory2Fn g_real_create_factory2 = nullptr;
uintptr_t g_slot_create_factory = 0, g_slot_create_factory1 = 0, g_slot_create_factory2 = 0;

HRESULT WINAPI hk_create_factory(REFIID riid, void** out) {
  const HRESULT hr = g_real_create_factory(riid, out);
  if (SUCCEEDED(hr) && out && *out) adopt_factory(static_cast<IUnknown*>(*out), "CreateDXGIFactory");
  return hr;
}
HRESULT WINAPI hk_create_factory1(REFIID riid, void** out) {
  const HRESULT hr = g_real_create_factory1(riid, out);
  if (SUCCEEDED(hr) && out && *out) adopt_factory(static_cast<IUnknown*>(*out), "CreateDXGIFactory1");
  return hr;
}
HRESULT WINAPI hk_create_factory2(UINT flags, REFIID riid, void** out) {
  const HRESULT hr = g_real_create_factory2(flags, riid, out);
  if (SUCCEEDED(hr) && out && *out) adopt_factory(static_cast<IUnknown*>(*out), "CreateDXGIFactory2");
  return hr;
}

}  // namespace

// From DllMain, before the game makes its factory (early in WinMain, ~1 s later).
void install_early() {
  if (!g_owned_cs_ready) {
    InitializeCriticalSection(&g_owned_cs);
    g_owned_cs_ready = true;
  }
  // The thread the game's delay load runs this on: WinMain's, the game
  // window's (game_window); and the mod's own image (original()).
  g_game_thread = GetCurrentThreadId();
  g_self = mem::module_range(module_of(reinterpret_cast<const void*>(&install_early)));
  // Windows only: the overlay's hooking is Windows', and under Proton (DXVK,
  // no such hooking) the class hook has been the panel's way all along. The
  // adoption test sets RE2CC_ADOPT to run this under Wine.
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version");
  const bool forced = GetEnvironmentVariableA("RE2CC_ADOPT", nullptr, 0) != 0;
  // Which DXGI the game has: Windows' own, or a wrapper beside re2.exe
  // (OptiScaler, ReShade, Special K - a dxgi.dll in the game's folder, which
  // loads Windows' own itself: the module DXGI's own objects have their
  // vtables in, is_windows_dxgi).
  {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    char path[MAX_PATH] = "";
    if (dxgi) GetModuleFileNameA(dxgi, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA file{};
    const unsigned long long size = path[0] && GetFileAttributesExA(path, GetFileExInfoStandard, &file)
                                        ? (static_cast<unsigned long long>(file.nFileSizeHigh) << 32) | file.nFileSizeLow
                                        : 0;
    logf("overlay: the game's DXGI is %s (%llu bytes)%s", path[0] ? path : "not loaded", size,
         dxgi && !is_windows_dxgi(dxgi) ? " - not Windows' own: a wrapper (OptiScaler, ReShade, Special K, ...) sits beside re2.exe"
                                        : "");
  }
  if (wine && !forced) {
    logf("overlay: Wine - the swap chain class's vtable will be hooked, as before");
    return;
  }
  HMODULE exe = GetModuleHandleA(nullptr);
  int n = 0;
  if (void* p = mem::iat_hook(exe, "dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&hk_create_factory),
                              &g_slot_create_factory)) {
    g_real_create_factory = reinterpret_cast<CreateFactoryFn>(p);
    ++n;
  }
  if (void* p = mem::iat_hook(exe, "dxgi.dll", "CreateDXGIFactory1", reinterpret_cast<void*>(&hk_create_factory1),
                              &g_slot_create_factory1)) {
    g_real_create_factory1 = reinterpret_cast<CreateFactoryFn>(p);
    ++n;
  }
  if (void* p = mem::iat_hook(exe, "dxgi.dll", "CreateDXGIFactory2", reinterpret_cast<void*>(&hk_create_factory2),
                              &g_slot_create_factory2)) {
    g_real_create_factory2 = reinterpret_cast<CreateFactory2Fn>(p);
    ++n;
  }
  g_adopting = n > 0;
  if (g_adopting)
    logf("overlay: %d CreateDXGIFactory import(s) of re2.exe hooked - the game's swap chains get vtables of their own as they "
         "are made",
         n);
  else
    logf("overlay: re2.exe imports no CreateDXGIFactory from dxgi.dll - the swap chain class's vtable will be hooked instead");
}

bool adopting() { return g_adopting; }
bool presenting() { return g_sc != nullptr; }

// The first design, now the fallback: the class's vtable, found through a
// throwaway swap chain. Not for a machine with the Steam overlay (above).
bool install() {
  if (g_owned_cs_ready) {
    OwnedLock lock;
    if (g_n_chains) {
      logf("overlay: a swap chain of the game's is adopted already - the class's vtable is left alone");
      return true;
    }
  }
  HMODULE d3d11 = LoadLibraryA("d3d11.dll");
  auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"))
                      : nullptr;
  if (!create) {
    logf("ERROR: overlay: d3d11.dll has no D3D11CreateDeviceAndSwapChain - no panel");
    return false;
  }
  HWND wnd = make_dummy_window();
  IDXGISwapChain* sc = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = E_FAIL;
  const DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_DISCARD};
  // WARP first: every DXGI swap chain is dxgi's own class whatever device made
  // it, so the software device gives the same vtable without touching the GPU
  // driver - a hardware one, made while the game was making its own, crashed
  // inside NVIDIA's D3D11 driver on Windows (2026-09-25, twice). DXVK (Proton)
  // either takes WARP as the GPU or fails it and gets the hardware device.
  const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE};
  for (D3D_DRIVER_TYPE drv : drivers) {
    for (DXGI_SWAP_EFFECT fx : effects) {
      DXGI_SWAP_CHAIN_DESC sd{};
      sd.BufferCount = fx == DXGI_SWAP_EFFECT_DISCARD ? 1 : 2;
      sd.BufferDesc.Width = 64;
      sd.BufferDesc.Height = 64;
      sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.OutputWindow = wnd;
      sd.SampleDesc.Count = 1;
      sd.Windowed = TRUE;
      sd.SwapEffect = fx;
      D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
      hr = create(nullptr, drv, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
      if (SUCCEEDED(hr)) break;
    }
    if (SUCCEEDED(hr)) break;
  }
  if (FAILED(hr) || !sc) {
    logf("ERROR: overlay: the dummy D3D11 swap chain could not be made (hr 0x%08lX) - no panel", static_cast<unsigned long>(hr));
    if (wnd) DestroyWindow(wnd);
    return false;
  }
  g_vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(sc));
  // Present1 and ResizeBuffers1 live further down the same vtable only if the
  // class implements IDXGISwapChain3 on one interface chain.
  IDXGISwapChain3* sc3 = nullptr;
  const bool has3 = SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3 &&
                    mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(sc3)) == g_vt;
  if (sc3) sc3->Release();
  // The slots get the entries above, which another hooker can patch in turn.
  re2cc_present_target = reinterpret_cast<void*>(&hk_present);
  re2cc_resize_target = reinterpret_cast<void*>(&hk_resize);
  re2cc_present1_target = reinterpret_cast<void*>(&hk_present1);
  re2cc_resize1_target = reinterpret_cast<void*>(&hk_resize1);
  g_orig_present = reinterpret_cast<PresentFn>(mem::hook_vtable(g_vt, 8, reinterpret_cast<void*>(&re2cc_present_entry)));
  g_orig_resize = reinterpret_cast<ResizeBuffersFn>(mem::hook_vtable(g_vt, 13, reinterpret_cast<void*>(&re2cc_resize_entry)));
  if (has3) {
    g_orig_present1 =
        reinterpret_cast<Present1Fn>(mem::hook_vtable(g_vt, 22, reinterpret_cast<void*>(&re2cc_present1_entry)));
    g_orig_resize1 =
        reinterpret_cast<ResizeBuffers1Fn>(mem::hook_vtable(g_vt, 39, reinterpret_cast<void*>(&re2cc_resize1_entry)));
  }
  char where[MAX_PATH + 32];
  describe_address(reinterpret_cast<void*>(g_orig_present), where, sizeof(where));
  logf("overlay: swap chain class hooked: Present, ResizeBuffers%s (Present was %s)", has3 ? ", Present1, ResizeBuffers1" : "",
       where);
  ctx->Release();
  dev->Release();
  sc->Release();
  DestroyWindow(wnd);
  return g_orig_present != nullptr;
}

void service() {
  if (g_need_queue && !g_queue_known && !g_queue_failed) {
    find_queue_offsets();
    InterlockedExchange(&g_need_queue, 0);
  }
}

void log_rates() {
  const unsigned p = static_cast<unsigned>(InterlockedExchange(&g_presents, 0));
  if (g_rate_logs < 5 && p) {
    ++g_rate_logs;
    logf("overlay: %u presents in the last second", p);
  }
}

void uninstall() {
  // The import slots back. Adopted objects keep their copies: the mod cannot
  // tell which of them the game still has, and the copies are never freed.
  mem::iat_restore(g_slot_create_factory, reinterpret_cast<void*>(&hk_create_factory),
                   reinterpret_cast<void*>(g_real_create_factory));
  mem::iat_restore(g_slot_create_factory1, reinterpret_cast<void*>(&hk_create_factory1),
                   reinterpret_cast<void*>(g_real_create_factory1));
  mem::iat_restore(g_slot_create_factory2, reinterpret_cast<void*>(&hk_create_factory2),
                   reinterpret_cast<void*>(g_real_create_factory2));
  // A slot is given back only while it still holds the mod's function: a tool
  // that hooked the same slot since (REFramework hooks the factory's
  // CreateSwapChainForHwnd) keeps its hook, and the mod's function it calls on
  // to stays in the image until the process is gone.
  auto give_back = [](void** slot, void* orig) {
    void* now = nullptr;
    if (orig && mem::read_safe(reinterpret_cast<uintptr_t>(slot), &now) && g_self.contains(reinterpret_cast<uintptr_t>(now)))
      mem::write<void*>(reinterpret_cast<uintptr_t>(slot), orig);
  };
  if (g_owned_cs_ready) {
    OwnedLock lock;
    for (int i = 0; i < g_n_factories; ++i)
      for (int slot : {kQueryInterface, kCreateSwapChain, kCreateForHwnd, kCreateForCoreWindow, kCreateForComposition})
        if (slot < g_factories[i].slots) give_back(&g_factories[i].vt[slot], g_factories[i].orig[slot]);
    g_n_factories = 0;
  }
  if (g_vt) {
    void** vt = reinterpret_cast<void**>(g_vt);
    give_back(&vt[kPresent], reinterpret_cast<void*>(g_orig_present));
    give_back(&vt[kResizeBuffers], reinterpret_cast<void*>(g_orig_resize));
    give_back(&vt[kPresent1], reinterpret_cast<void*>(g_orig_present1));
    give_back(&vt[kResizeBuffers1], reinterpret_cast<void*>(g_orig_resize1));
  }
  g_orig_present = nullptr;
  g_orig_present1 = nullptr;
  g_orig_resize = nullptr;
  g_orig_resize1 = nullptr;
  if (g11.init) ImGui_ImplDX11_Shutdown();
  if (g12.init) ImGui_ImplDX12_Shutdown();
  g11.init = g12.init = false;
}

}  // namespace re2cc::overlay::d3d
