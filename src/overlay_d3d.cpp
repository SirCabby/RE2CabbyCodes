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
#include <dxgi1_4.h>

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

namespace re2cc::overlay::d3d {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);

PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
ResizeBuffersFn g_orig_resize = nullptr;
ResizeBuffers1Fn g_orig_resize1 = nullptr;
uintptr_t g_vt = 0;  // the swap chain class's vtable, patched

enum class Api { Unknown, D3D11, D3D12, Other };
IDXGISwapChain* g_sc = nullptr;  // the game's swap chain (the first one that presents a window)
Api g_api = Api::Unknown;
thread_local int t_depth = 0;

volatile LONG g_presents = 0;
int g_rate_logs = 0;

// --- DX12 queue discovery (mod thread) ------------------------------------------------------
volatile LONG g_need_queue = 0;      // Present saw a D3D12 swap chain
volatile LONG g_queue_known = 0;     // the offsets below are set
volatile LONG g_queue_failed = 0;
int g_q_outer = -1;                  // -1: the queue pointer is in the swap chain itself
int g_q_inner = -1;
uintptr_t g_queue_vt = 0;            // what a real ID3D12CommandQueue's vtable is

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
  IDXGISwapChain3* sc3 = nullptr;
  if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) || !sc3) return;
  const UINT bi = sc3->GetCurrentBackBufferIndex();
  sc3->Release();
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

void on_present(IDXGISwapChain* sc) {
  if (g_sc && sc != g_sc) return;  // another swap chain (a tool's, a launcher's): not the game's
  DXGI_SWAP_CHAIN_DESC sd{};
  if (FAILED(sc->GetDesc(&sd)) || !sd.OutputWindow || !IsWindowVisible(sd.OutputWindow)) return;
  if (!g_sc) {
    g_sc = sc;
    g_api = api_of(sc);
    logf("overlay: the game presents through %s (swap chain %p, %ux%u, %u buffers, format %d, window %p)",
         g_api == Api::D3D12 ? "Direct3D 12" : g_api == Api::D3D11 ? "Direct3D 11" : "an unknown device",
         static_cast<void*>(sc), sd.BufferDesc.Width, sd.BufferDesc.Height, sd.BufferCount, sd.BufferDesc.Format,
         static_cast<void*>(sd.OutputWindow));
  }
  dispatch::set_game_window(sd.OutputWindow);
  if (g_api != Api::D3D11 && g_api != Api::D3D12) return;
  ImGuiLock guard;
  if (!ensure_context(sd.OutputWindow) || !wants_draw()) return;
  if (g_api == Api::D3D11) dx11_frame(sc);
  else dx12_frame(sc);
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  InterlockedIncrement(&g_presents);
  if (t_depth == 0 && !(flags & DXGI_PRESENT_TEST)) {
    ++t_depth;
    on_present(sc);
    --t_depth;
  }
  return g_orig_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
  InterlockedIncrement(&g_presents);
  if (t_depth == 0 && !(flags & DXGI_PRESENT_TEST)) {
    ++t_depth;
    on_present(sc);
    --t_depth;
  }
  return g_orig_present1(sc, sync, flags, p);
}

void before_resize(IDXGISwapChain* sc) {
  if (sc != g_sc) return;
  ImGuiLock guard;
  if (g_api == Api::D3D11) dx11_release_targets();
  else if (g_api == Api::D3D12) dx12_release_targets();
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
  before_resize(sc);
  const HRESULT hr = g_orig_resize(sc, n, w, h, f, flags);
  if (sc == g_sc) logf("overlay: the game resized its swap chain to %ux%u (hr 0x%08lX)", w, h, static_cast<unsigned long>(hr));
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                     const UINT* masks, IUnknown* const* queues) {
  before_resize(sc);
  const HRESULT hr = g_orig_resize1(sc, n, w, h, f, flags, masks, queues);
  if (sc == g_sc) logf("overlay: the game resized its swap chain to %ux%u (ResizeBuffers1, hr 0x%08lX)", w, h, static_cast<unsigned long>(hr));
  return hr;
}

}  // namespace

bool install() {
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
  const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
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
  g_orig_present = reinterpret_cast<PresentFn>(mem::hook_vtable(g_vt, 8, reinterpret_cast<void*>(&hk_present)));
  g_orig_resize = reinterpret_cast<ResizeBuffersFn>(mem::hook_vtable(g_vt, 13, reinterpret_cast<void*>(&hk_resize)));
  if (has3) {
    g_orig_present1 = reinterpret_cast<Present1Fn>(mem::hook_vtable(g_vt, 22, reinterpret_cast<void*>(&hk_present1)));
    g_orig_resize1 = reinterpret_cast<ResizeBuffers1Fn>(mem::hook_vtable(g_vt, 39, reinterpret_cast<void*>(&hk_resize1)));
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
  if (g_vt && mem::readable(g_vt, 40 * sizeof(void*))) {
    if (g_orig_present) mem::write<void*>(g_vt + 8 * sizeof(void*), reinterpret_cast<void*>(g_orig_present));
    if (g_orig_resize) mem::write<void*>(g_vt + 13 * sizeof(void*), reinterpret_cast<void*>(g_orig_resize));
    if (g_orig_present1) mem::write<void*>(g_vt + 22 * sizeof(void*), reinterpret_cast<void*>(g_orig_present1));
    if (g_orig_resize1) mem::write<void*>(g_vt + 39 * sizeof(void*), reinterpret_cast<void*>(g_orig_resize1));
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
