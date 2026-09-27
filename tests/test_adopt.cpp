// The overlay's adoption of the game's DXGI objects (src/overlay_d3d.cpp), outside the game:
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_adopt.cpp src/mem.cpp
//       -ldxgi -ld3d11 -ldxguid -luser32 -o tests/build/test_adopt.exe
//   cp build/steam_api64.dll <dir>/; cp "$GAME_DIR/steam_api64_orig.dll" <dir>/
//   wine tests/build/test_adopt.exe <dir>
//
// This exe imports CreateDXGIFactory1 from dxgi.dll as re2.exe does, so the
// proxy, loaded the way the game's delay load loads it, hooks that import. A
// factory made after that must have the proxy's CreateSwapChain in its class
// vtable (hooked in place), and so must what QueryInterface hands out for
// another factory interface; a swap chain made from it (when a device can be
// made at all - WARP, then hardware; none under a headless Wine) must come
// back with a Present of the proxy's, and a Present through it must reach
// DXGI's. A swap chain for a window of another thread (another tool's
// throwaway: REFramework's) must keep its class's vtable, and a hook put into
// that class's vtable afterwards - REFramework's way, moving into the object
// on the first frame it gets - must get the game's frames through the proxy's
// hook (2026-09-26: it never did, and no REFramework mod worked); so must a
// class hook that calls the object's vtable again, once, without coming back
// into itself. Then the same through a wrapper the way Special K wraps (the exe's
// own CreateDXGIFactory1 import slot is pointed at a stand-in first, so the
// proxy takes the stand-in for the original): a factory and a swap chain
// proxy of the test's own, the swap chain one answering Special K's unwrap
// IID and, as Special K's does, handing itself back from a QueryInterface for
// a higher interface WITHOUT a reference - the proxy must adopt it without
// asking it any such thing (or the wrapper is destroyed under the game) - and
// with a virtual destructor, as OptiScaler's has, in the vtable slot after the
// interface's: the game's Release must still reach it through the copy (the
// 1.0.3 crash of 2026-09-25), and a Present through it must reach the real
// swap chain. The log the proxy writes into <dir> says the same.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../src/mem.h"

namespace {

int g_failures = 0;
HMODULE g_proxy = nullptr;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

bool in_proxy(const void* fn) {
  HMODULE owner = nullptr;
  return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(fn), &owner) &&
         owner == g_proxy;
}

void** vtable_of(const void* obj) { return *static_cast<void** const*>(obj); }

// --- a wrapper the way Special K wraps ---------------------------------------------------------
constexpr GUID kUnwrappedSpecialK = {0xe8a33b4a, 0x1405, 0x424c, {0xae, 0x88, 0x0d, 0x3e, 0x9d, 0x46, 0xc9, 0x14}};

struct FakeSwapChain final : IDXGISwapChain4 {
  IDXGISwapChain4* real;
  LONG refs = 1;
  UINT ver = 1;  // made through CreateSwapChain: IDXGISwapChain1, as Special K's constructor has it
  static inline void** class_vt = nullptr;
  static inline bool destroyed = false;
  static inline int promotions = 0;  // QueryInterfaces for a higher version: the ones that must not happen
  static inline int presents = 0;

  explicit FakeSwapChain(IDXGISwapChain4* r) : real(r) { class_vt = vtable_of(this); }
  // OptiScaler's proxy has one: the slot after the interface's 41, reached
  // through the object's vtable pointer by `delete this` - the copy must hold it.
  virtual ~FakeSwapChain() { destroyed = true; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, kUnwrappedSpecialK)) {
      real->AddRef();
      *out = real;
      return S_OK;
    }
    UINT want = 99;
    if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDXGIObject)) ||
        IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) || IsEqualIID(riid, __uuidof(IDXGISwapChain)))
      want = 0;
    else if (IsEqualIID(riid, __uuidof(IDXGISwapChain1))) want = 1;
    else if (IsEqualIID(riid, __uuidof(IDXGISwapChain2))) want = 2;
    else if (IsEqualIID(riid, __uuidof(IDXGISwapChain3))) want = 3;
    else if (IsEqualIID(riid, __uuidof(IDXGISwapChain4))) want = 4;
    if (want == 99) return real->QueryInterface(riid, out);
    if (want > ver) {
      ++promotions;
      ver = want;  // Special K: the inner object promoted, itself handed back - and no AddRef
    } else {
      AddRef();
    }
    *out = this;
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
      real->Release();
      delete this;  // the virtual destructor: through the vtable pointer, past the interface's slots
    }
    return static_cast<ULONG>(n);
  }
  // IDXGIObject
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override { return real->SetPrivateData(g, n, d); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return real->SetPrivateDataInterface(g, u); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override { return real->GetPrivateData(g, n, d); }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** p) override { return real->GetParent(riid, p); }
  // IDXGIDeviceSubObject
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** p) override { return real->GetDevice(riid, p); }
  // IDXGISwapChain
  HRESULT STDMETHODCALLTYPE Present(UINT s, UINT f) override {
    ++presents;
    return real->Present(s, f);
  }
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** p) override { return real->GetBuffer(i, riid, p); }
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* o) override { return real->SetFullscreenState(f, o); }
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override { return real->GetFullscreenState(f, o); }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { return real->GetDesc(d); }
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override { return real->ResizeBuffers(n, w, h, f, fl); }
  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* m) override { return real->ResizeTarget(m); }
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return real->GetContainingOutput(o); }
  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return real->GetFrameStatistics(s); }
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* n) override { return real->GetLastPresentCount(n); }
  // IDXGISwapChain1
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return real->GetDesc1(d); }
  HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return real->GetFullscreenDesc(d); }
  HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return real->GetHwnd(h); }
  HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** p) override { return real->GetCoreWindow(riid, p); }
  HRESULT STDMETHODCALLTYPE Present1(UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p) override { return real->Present1(s, f, p); }
  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return real->IsTemporaryMonoSupported(); }
  HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return real->GetRestrictToOutput(o); }
  HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return real->SetBackgroundColor(c); }
  HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return real->GetBackgroundColor(c); }
  HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return real->SetRotation(r); }
  HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return real->GetRotation(r); }
  // IDXGISwapChain2
  HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return real->SetSourceSize(w, h); }
  HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return real->GetSourceSize(w, h); }
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT n) override { return real->SetMaximumFrameLatency(n); }
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* n) override { return real->GetMaximumFrameLatency(n); }
  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return real->GetFrameLatencyWaitableObject(); }
  HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return real->SetMatrixTransform(m); }
  HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return real->GetMatrixTransform(m); }
  // IDXGISwapChain3
  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return real->GetCurrentBackBufferIndex(); }
  HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return real->CheckColorSpaceSupport(c, s); }
  HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override { return real->SetColorSpace1(c); }
  HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* m, IUnknown* const* q) override {
    return real->ResizeBuffers1(n, w, h, f, fl, m, q);
  }
  // IDXGISwapChain4
  HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT n, void* d) override { return real->SetHDRMetaData(t, n, d); }
};

struct FakeFactory final : IDXGIFactory2 {
  IDXGIFactory2* real;
  LONG refs = 1;
  explicit FakeFactory(IDXGIFactory2* r) : real(r) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    for (const IID* iid : {&__uuidof(IUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIFactory), &__uuidof(IDXGIFactory1),
                           &__uuidof(IDXGIFactory2)})
      if (IsEqualIID(riid, *iid)) {
        AddRef();
        *out = this;
        return S_OK;
      }
    return real->QueryInterface(riid, out);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
      real->Release();
      delete this;
    }
    return static_cast<ULONG>(n);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override { return real->SetPrivateData(g, n, d); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return real->SetPrivateDataInterface(g, u); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override { return real->GetPrivateData(g, n, d); }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** p) override { return real->GetParent(riid, p); }
  HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override { return real->EnumAdapters(i, a); }
  HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT f) override { return real->MakeWindowAssociation(h, f); }
  HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override { return real->GetWindowAssociation(h); }
  HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) override {
    IDXGISwapChain* sc = nullptr;
    const HRESULT hr = real->CreateSwapChain(dev, d, &sc);
    if (FAILED(hr) || !sc) return hr;
    IDXGISwapChain4* sc4 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void**>(&sc4))) || !sc4) {
      sc->Release();
      return E_NOINTERFACE;
    }
    sc->Release();
    *out = new FakeSwapChain(sc4);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { return real->CreateSoftwareAdapter(m, a); }
  HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override { return real->EnumAdapters1(i, a); }
  BOOL STDMETHODCALLTYPE IsCurrent() override { return real->IsCurrent(); }
  BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return real->IsWindowedStereoEnabled(); }
  HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f, IDXGIOutput* o,
                                                   IDXGISwapChain1** out) override {
    return real->CreateSwapChainForHwnd(dev, h, d, f, o, out);
  }
  HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* dev, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                         IDXGIOutput* o, IDXGISwapChain1** out) override {
    return real->CreateSwapChainForCoreWindow(dev, w, d, o, out);
  }
  HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override { return real->GetSharedResourceAdapterLuid(h, l); }
  HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND h, UINT m, DWORD* c) override { return real->RegisterStereoStatusWindow(h, m, c); }
  HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE e, DWORD* c) override { return real->RegisterStereoStatusEvent(e, c); }
  void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override { real->UnregisterStereoStatus(c); }
  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND h, UINT m, DWORD* c) override { return real->RegisterOcclusionStatusWindow(h, m, c); }
  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE e, DWORD* c) override { return real->RegisterOcclusionStatusEvent(e, c); }
  void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override { real->UnregisterOcclusionStatus(c); }
  HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o,
                                                          IDXGISwapChain1** out) override {
    return real->CreateSwapChainForComposition(dev, d, o, out);
  }
};

// The stand-in for CreateDXGIFactory1 the proxy takes for the original: DXGI's
// factory as it is, or - the wrapper part - a factory of the test's own around it.
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
CreateFactoryFn g_real_create_factory1 = nullptr;
bool g_wrap = false;
HRESULT WINAPI standin_create_factory1(REFIID riid, void** out) {
  if (!g_wrap) return g_real_create_factory1(riid, out);
  IDXGIFactory2* real = nullptr;
  const HRESULT hr = g_real_create_factory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&real));
  if (FAILED(hr) || !real) return FAILED(hr) ? hr : E_FAIL;
  *out = new FakeFactory(real);
  return S_OK;
}

ID3D11Device* make_device() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  for (D3D_DRIVER_TYPE drv : {D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE}) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, drv, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
      ctx->Release();
      return dev;
    }
  }
  return nullptr;
}

DXGI_SWAP_CHAIN_DESC chain_desc(HWND wnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 1;
  sd.BufferDesc.Width = 64;
  sd.BufferDesc.Height = 64;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = wnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  return sd;
}

HWND make_window() {
  return CreateWindowExA(0, "STATIC", "test_adopt", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                         GetModuleHandleA(nullptr), nullptr);
}

// A window of another thread of the process - where another tool's throwaway
// swap chain goes (REFramework's second try: a hidden window of its own thread).
struct OtherWindow {
  HWND hwnd = nullptr;
  HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE thread = nullptr;
};
DWORD WINAPI other_window_thread(LPVOID p) {
  auto* w = static_cast<OtherWindow*>(p);
  w->hwnd = make_window();
  SetEvent(w->ready);
  WaitForSingleObject(w->done, INFINITE);
  if (w->hwnd) DestroyWindow(w->hwnd);
  return 0;
}

// REFramework, the way its D3D12 hook finds the game's frames (D3D12Hook.cpp,
// 2026): Present hooked in the class vtable of a throwaway swap chain of its
// own - the class the game's swap chain shares -; the first Present through it
// gives the class its slot back, copies the vtable the object has (the entry
// before it too), puts its own Present in the copy, points the object at the
// copy and calls on through the old vtable's slot - the mod's hook, for an
// adopted swap chain. Its later frames come in through the copy.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
namespace ref {
void** class_vt = nullptr;
void* class_present = nullptr;  // what the class's slot held
void* object_present = nullptr; // what the object's own slot held: the mod's hook
void** old_vtable = nullptr;
int phase1 = 0, phase2 = 0, depth = 0, max_depth = 0;

HRESULT STDMETHODCALLTYPE present2(IDXGISwapChain* sc, UINT s, UINT f) {
  ++phase2;
  if (++depth > max_depth) max_depth = depth;
  const HRESULT hr = reinterpret_cast<PresentFn>(object_present)(sc, s, f);
  --depth;
  return hr;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain* sc, UINT s, UINT f) {
  ++phase1;
  if (++depth > max_depth) max_depth = depth;
  re2cc::mem::write<void*>(reinterpret_cast<uintptr_t>(&class_vt[8]), class_present);
  old_vtable = vtable_of(sc);
  auto** copy = new void*[65];
  std::memcpy(copy, old_vtable - 1, 65 * sizeof(void*));
  object_present = old_vtable[8];
  copy[1 + 8] = reinterpret_cast<void*>(&present2);
  *reinterpret_cast<void***>(sc) = copy + 1;
  const HRESULT hr = reinterpret_cast<PresentFn>(object_present)(sc, s, f);
  --depth;
  return hr;
}
}  // namespace ref

// A hook in the class's vtable that calls the object's own vtable again: the
// mod's hook, called back while its first call is on its way on, must go on to
// DXGI and not back into the class's slot (it would never end).
namespace redispatch {
void** class_vt = nullptr;
void* class_present = nullptr;
int calls = 0;
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* sc, UINT s, UINT f) {
  if (++calls > 3) return reinterpret_cast<PresentFn>(class_present)(sc, s, f);  // the test's own brake
  return sc->Present(s, f);
}
}  // namespace redispatch

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_adopt.exe <dir with steam_api64.dll and steam_api64_orig.dll>\n");
    return 2;
  }
  SetCurrentDirectoryA(argv[1]);
  SetEnvironmentVariableA("RE2CC_ADOPT", "1");  // the proxy adopts on Windows only, unless told to

  // This exe's own import slot for CreateDXGIFactory1, pointed at the stand-in
  // before the proxy looks: the proxy then calls the stand-in as the original.
  const uintptr_t slot = re2cc::mem::iat_slot(GetModuleHandleA(nullptr), "dxgi.dll", "CreateDXGIFactory1");
  check(slot != 0, "this exe imports CreateDXGIFactory1 from dxgi.dll");
  if (!slot) return 1;
  g_real_create_factory1 = re2cc::mem::read<CreateFactoryFn>(slot);
  re2cc::mem::write<void*>(slot, reinterpret_cast<void*>(&standin_create_factory1));

  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\steam_api64.dll", argv[1]);
  g_proxy = LoadLibraryA(path);
  std::printf("LoadLibrary(%s) = %p (error %lu)\n", path, static_cast<void*>(g_proxy), g_proxy ? 0 : GetLastError());
  if (!g_proxy) return 1;
  check(re2cc::mem::read<void*>(slot) != reinterpret_cast<void*>(&standin_create_factory1), "the proxy hooked the import slot");

  std::printf("DXGI's own factory\n");
  IDXGIFactory1* factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
  std::printf("  CreateDXGIFactory1 = 0x%08lX, %p\n", static_cast<unsigned long>(hr), static_cast<void*>(factory));
  check(SUCCEEDED(hr) && factory, "a factory is made");
  if (FAILED(hr) || !factory) return 1;
  void** fvt = vtable_of(factory);
  check(in_proxy(fvt[10]), "its CreateSwapChain (slot 10) is the proxy's");
  check(!in_proxy(fvt[7]), "its EnumAdapters (slot 7) is DXGI's");
  IDXGIFactory2* factory2 = nullptr;
  hr = factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2));
  std::printf("  QueryInterface(IDXGIFactory2) = 0x%08lX, %p\n", static_cast<unsigned long>(hr), static_cast<void*>(factory2));
  if (SUCCEEDED(hr) && factory2) {
    void** f2vt = vtable_of(factory2);
    check(in_proxy(f2vt[15]), "what it hands out for IDXGIFactory2 has the proxy's CreateSwapChainForHwnd (slot 15)");
    check(in_proxy(f2vt[0]), "and the proxy's QueryInterface (slot 0)");
    IDXGIAdapter* adapter = nullptr;
    hr = factory2->EnumAdapters(0, &adapter);
    check(SUCCEEDED(hr) || hr == DXGI_ERROR_NOT_FOUND, "a call through the hooked vtable reaches DXGI (EnumAdapters)");
    if (adapter) adapter->Release();
  }

  std::printf("DXGI's own swap chain (needs a device)\n");
  ID3D11Device* dev = make_device();
  std::printf("  D3D11CreateDevice: %s\n", dev ? "a device" : "none");
  HWND wnd = dev ? make_window() : nullptr;
  if (dev) {
    DXGI_SWAP_CHAIN_DESC sd = chain_desc(wnd);
    IDXGISwapChain* sc = nullptr;
    hr = factory->CreateSwapChain(dev, &sd, &sc);
    std::printf("  CreateSwapChain = 0x%08lX, %p\n", static_cast<unsigned long>(hr), static_cast<void*>(sc));
    check(SUCCEEDED(hr) && sc, "a swap chain is made through the hooked factory");
    if (SUCCEEDED(hr) && sc) {
      void** svt = vtable_of(sc);
      check(in_proxy(svt[8]), "its Present (slot 8) is the proxy's");
      check(in_proxy(svt[13]), "its ResizeBuffers (slot 13) is the proxy's");
      check(!in_proxy(svt[12]), "its GetDesc (slot 12) is DXGI's");
      DXGI_SWAP_CHAIN_DESC got{};
      check(SUCCEEDED(sc->GetDesc(&got)) && got.OutputWindow == wnd, "GetDesc through the copy answers");
      hr = sc->Present(0, DXGI_PRESENT_TEST);
      std::printf("  Present(DXGI_PRESENT_TEST) = 0x%08lX\n", static_cast<unsigned long>(hr));
      check(hr == S_OK || hr == DXGI_STATUS_OCCLUDED, "a Present through the proxy's hook reaches DXGI's");
      check(in_proxy(vtable_of(sc)[10]), "its SetFullscreenState (slot 10) is the proxy's (logged)");

      std::printf("another tool's swap chain in the game (REFramework's)\n");
      OtherWindow other;
      other.thread = CreateThread(nullptr, 0, other_window_thread, &other, 0, nullptr);
      WaitForSingleObject(other.ready, 5000);
      IDXGISwapChain* osc = nullptr;
      DXGI_SWAP_CHAIN_DESC od = chain_desc(other.hwnd);
      hr = other.hwnd ? factory->CreateSwapChain(dev, &od, &osc) : E_FAIL;
      std::printf("  CreateSwapChain for another thread's window = 0x%08lX, %p\n", static_cast<unsigned long>(hr), static_cast<void*>(osc));
      check(SUCCEEDED(hr) && osc, "a swap chain for a window of another thread is made through the hooked factory");
      if (SUCCEEDED(hr) && osc) {
        void** cvt = vtable_of(osc);
        check(!in_proxy(cvt[8]), "it keeps its class's vtable - not the game's, so not adopted");
        check(cvt != vtable_of(sc) && vtable_of(sc)[-1] == cvt[-1],
              "the game's copy has the entry before the class's vtable too (where MSVC keeps type information)");

        // REFramework's hook, into the class's vtable after the game's swap chain was adopted.
        ref::class_vt = cvt;
        ref::class_present = cvt[8];
        re2cc::mem::write<void*>(reinterpret_cast<uintptr_t>(&cvt[8]), reinterpret_cast<void*>(&ref::present1));
        hr = sc->Present(0, DXGI_PRESENT_TEST);
        check(ref::phase1 == 1, "the game's Present reaches a hook put into the class's vtable after the adoption (REFramework)");
        check(cvt[8] == ref::class_present && ref::old_vtable && in_proxy(ref::object_present),
              "it gave the class its slot back and moved into the object, calling on to the proxy's hook");
        check((hr == S_OK || hr == DXGI_STATUS_OCCLUDED) && ref::max_depth == 1,
              "and the proxy's hook, called again from inside it, went on to DXGI (not back into the class)");
        hr = sc->Present(0, DXGI_PRESENT_TEST);
        check(ref::phase2 == 1 && ref::phase1 == 1 && (hr == S_OK || hr == DXGI_STATUS_OCCLUDED),
              "the next Present comes through its hook in the object and on through the proxy's to DXGI");
        if (ref::old_vtable) *reinterpret_cast<void***>(sc) = ref::old_vtable;  // its unhook

        redispatch::class_vt = cvt;
        redispatch::class_present = cvt[8];
        re2cc::mem::write<void*>(reinterpret_cast<uintptr_t>(&cvt[8]), reinterpret_cast<void*>(&redispatch::present));
        hr = sc->Present(0, DXGI_PRESENT_TEST);
        re2cc::mem::write<void*>(reinterpret_cast<uintptr_t>(&cvt[8]), redispatch::class_present);
        check(redispatch::calls == 1 && (hr == S_OK || hr == DXGI_STATUS_OCCLUDED),
              "a class hook that calls the object's vtable again gets the frame once, and it reaches DXGI");
        osc->Release();
      }
      SetEvent(other.done);
      if (other.thread) {
        WaitForSingleObject(other.thread, 5000);
        CloseHandle(other.thread);
      }
      sc->Release();
    }
  } else {
    std::printf("  (no D3D11 device here - the swap chain parts are not tested)\n");
  }
  if (factory2) factory2->Release();
  factory->Release();

  std::printf("a wrapper's factory and swap chain (the way Special K wraps)\n");
  g_wrap = true;
  IDXGIFactory1* wf = nullptr;
  hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&wf));
  check(SUCCEEDED(hr) && wf, "the wrapper's factory comes back through the proxy's import hook");
  if (SUCCEEDED(hr) && wf) {
    void** wvt = vtable_of(wf);
    check(in_proxy(wvt[10]), "its CreateSwapChain (slot 10) is the proxy's, in the wrapper's own vtable");
    if (dev) {
      DXGI_SWAP_CHAIN_DESC sd = chain_desc(wnd);
      IDXGISwapChain* sc = nullptr;
      hr = wf->CreateSwapChain(dev, &sd, &sc);
      std::printf("  CreateSwapChain = 0x%08lX, %p\n", static_cast<unsigned long>(hr), static_cast<void*>(sc));
      check(SUCCEEDED(hr) && sc, "a swap chain proxy is made through it");
      if (SUCCEEDED(hr) && sc) {
        auto* fake = static_cast<FakeSwapChain*>(sc);
        check(vtable_of(sc) != FakeSwapChain::class_vt, "the proxy adopted it (its vtable is a copy)");
        check(in_proxy(vtable_of(sc)[8]), "its Present (slot 8) is the proxy's");
        check(!in_proxy(vtable_of(sc)[12]), "its GetDesc (slot 12) is the wrapper's");
        check(FakeSwapChain::promotions == 0, "the proxy asked it for no higher interface (Special K's reference bug)");
        check(!FakeSwapChain::destroyed && fake->refs == 1, "the wrapper is alive with its one reference");
        DXGI_SWAP_CHAIN_DESC got{};
        check(SUCCEEDED(sc->GetDesc(&got)) && got.OutputWindow == wnd, "GetDesc through the copy answers");
        hr = sc->Present(0, DXGI_PRESENT_TEST);
        check((hr == S_OK || hr == DXGI_STATUS_OCCLUDED) && FakeSwapChain::presents == 1,
              "a Present through the proxy's hook reaches the wrapper's, and DXGI's under it");
        sc->Release();
        check(FakeSwapChain::destroyed,
              "the game's Release still destroys it through the copy - its virtual destructor, past the interface's slots");
      }
    }
    wf->Release();
  }
  g_wrap = false;
  if (wnd) DestroyWindow(wnd);
  if (dev) dev->Release();

  std::printf("\nthe log (%s\\RE2CabbyCodes.log), the overlay's lines:\n", argv[1]);
  std::snprintf(path, sizeof(path), "%s\\RE2CabbyCodes.log", argv[1]);
  if (FILE* f = std::fopen(path, "r")) {
    char line[1024];
    while (std::fgets(line, sizeof(line), f))
      if (std::strstr(line, "overlay:")) std::printf("  %s", line);
    std::fclose(f);
  }
  std::printf("FreeLibrary = %d\n", FreeLibrary(g_proxy));
  std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "passed", g_failures);
  return g_failures ? 1 : 0;
}
