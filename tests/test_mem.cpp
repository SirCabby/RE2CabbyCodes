// src/mem.cpp's guarded copies under Wine (or Windows): reads and stores of bad
// addresses fail instead of faulting, good ones cost nanoseconds, and faults
// that are not the mod's reach the handlers after it.
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_mem.cpp src/mem.cpp -o tests/build/test_mem.exe
//   WINEPREFIX=<scratch> wine tests/build/test_mem.exe
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/mem.h"

using namespace re2cc;

static int g_failed = 0;
#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                             \
    }                                                         \
  } while (0)

static double now_ns() {
  static LARGE_INTEGER f;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(f.QuadPart);
}

static std::atomic<int> g_passed_on{0};
static LONG CALLBACK later_handler(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode == 0xE0DE0001) {
    ++g_passed_on;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
  CHECK(mem::guard_install());
  const uintptr_t page = 0x1000;

  // Good memory: the stack, the heap, this image.
  uint64_t local = 0x1122334455667788ull, got = 0;
  CHECK(mem::read_safe(reinterpret_cast<uintptr_t>(&local), &got) && got == local);
  std::vector<uint8_t> heap(10000);
  for (size_t i = 0; i < heap.size(); ++i) heap[i] = static_cast<uint8_t>(i * 7);
  std::vector<uint8_t> copy(heap.size());
  CHECK(mem::copy_from(copy.data(), reinterpret_cast<uintptr_t>(heap.data()), heap.size()) && copy == heap);
  CHECK(mem::readable(reinterpret_cast<uintptr_t>(&main), 64));
  CHECK(mem::readable(reinterpret_cast<uintptr_t>(heap.data()), heap.size()));
  for (size_t n = 0; n < 20; ++n) {  // every tail length of the word + byte loop
    std::vector<uint8_t> dst(20, 0xEE);
    CHECK(mem::copy_from(dst.data(), reinterpret_cast<uintptr_t>(heap.data()) + 3, n));
    for (size_t i = 0; i < 20; ++i) CHECK(dst[i] == (i < n ? heap[3 + i] : 0xEE));
  }

  // Reserved but never committed, released, no access, guard, read-only.
  auto* res = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4 * page, MEM_RESERVE, PAGE_NOACCESS));
  VirtualAlloc(res, page, MEM_COMMIT, PAGE_READWRITE);  // first page usable, the rest reserved
  res[page - 1] = 0xAB;
  const uintptr_t r = reinterpret_cast<uintptr_t>(res);
  uint8_t b = 0;
  CHECK(mem::read_safe(r + page - 1, &b) && b == 0xAB);
  CHECK(!mem::read_safe(r + page, &b));
  uint16_t w = 0;
  CHECK(!mem::read_safe(r + page - 1, &w));  // straddles into the reserved page
  CHECK(mem::readable(r, page) && !mem::readable(r, page + 1) && !mem::readable(r + 2 * page, 1));
  CHECK(mem::store<uint8_t>(r + 5, 0x5A) && res[5] == 0x5A);
  CHECK(!mem::store<uint8_t>(r + page, 1));

  auto* gone = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  VirtualFree(gone, 0, MEM_RELEASE);
  CHECK(!mem::read_safe(reinterpret_cast<uintptr_t>(gone), &b) && !mem::readable(gone, 1));

  auto* noacc = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
  CHECK(!mem::read_safe(reinterpret_cast<uintptr_t>(noacc), &b));

  auto* ro = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  ro[9] = 0x42;
  DWORD old = 0;
  VirtualProtect(ro, page, PAGE_READONLY, &old);
  CHECK(mem::read_safe(reinterpret_cast<uintptr_t>(ro) + 9, &b) && b == 0x42);
  CHECK(!mem::store<uint8_t>(reinterpret_cast<uintptr_t>(ro) + 9, 0x77) && ro[9] == 0x42);
  CHECK(!mem::writable(reinterpret_cast<uintptr_t>(ro), 1) && mem::writable(r, 1));

  auto* guard = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD));
  CHECK(!mem::read_safe(reinterpret_cast<uintptr_t>(guard), &b));

  CHECK(!mem::read_safe(0, &b) && !mem::read_safe(8, &b) && !mem::readable(0x7FFFFFFFFFFF0000ull, 1));
  CHECK(mem::guard_faults() >= 6);

  // exchange_ptr: in place on read-write and copy-on-write pages (an image's
  // .data - the type database - reports PAGE_WRITECOPY under Wine), through a
  // protection lifted and put back on a read-only one, never on no-access, and
  // only while the value is the one expected.
  {
    auto protect_of = [](void* p) {
      MEMORY_BASIC_INFORMATION mbi{};
      VirtualQuery(p, &mbi, sizeof(mbi));
      return mbi.Protect;
    };
    auto* rw = static_cast<uintptr_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    rw[3] = 0x1111;
    DWORD seen = 0;
    CHECK(mem::exchange_ptr(reinterpret_cast<uintptr_t>(&rw[3]), 0x1111, 0x2222, &seen) && rw[3] == 0x2222 &&
          seen == PAGE_READWRITE);
    CHECK(!mem::exchange_ptr(reinterpret_cast<uintptr_t>(&rw[3]), 0x1111, 0x3333) && rw[3] == 0x2222);

    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page), nullptr);
    auto* cow = section ? static_cast<uintptr_t*>(MapViewOfFile(section, FILE_MAP_COPY, 0, 0, page)) : nullptr;
    CHECK(cow != nullptr);
    if (cow) {
      std::printf("a copy-on-write view reports protection 0x%lX\n", protect_of(cow));
      cow[5] = 0x4444;  // written once, as the runtime writes the database
      CHECK(mem::writable(reinterpret_cast<uintptr_t>(&cow[5]), 8));
      CHECK(mem::exchange_ptr(reinterpret_cast<uintptr_t>(&cow[5]), 0x4444, 0x5555, &seen) && cow[5] == 0x5555);
      std::printf("  after a write it reports 0x%lX\n", seen);
      UnmapViewOfFile(cow);
    }
    if (section) CloseHandle(section);

    auto* ro_ptr = static_cast<uintptr_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ro_ptr[7] = 0x6666;
    DWORD was = 0;
    VirtualProtect(ro_ptr, page, PAGE_READONLY, &was);
    CHECK(!mem::writable(reinterpret_cast<uintptr_t>(&ro_ptr[7]), 8));
    CHECK(mem::exchange_ptr(reinterpret_cast<uintptr_t>(&ro_ptr[7]), 0x6666, 0x7777, &seen) && ro_ptr[7] == 0x7777 &&
          seen == PAGE_READONLY && protect_of(ro_ptr) == PAGE_READONLY);

    auto* none = static_cast<uintptr_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    CHECK(!mem::exchange_ptr(reinterpret_cast<uintptr_t>(none), 0, 1));
    CHECK(!mem::exchange_ptr(0, 0, 1));
  }

  // Someone else's exception passes the mod's handler by.
  AddVectoredExceptionHandler(0, &later_handler);
  RaiseException(0xE0DE0001, 0, 0, nullptr);
  CHECK(g_passed_on == 1);

  // Threads reading good and bad memory at once.
  std::atomic<long> good{0}, bad{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 8; ++t)
    th.emplace_back([&, t] {
      uint64_t v = 0;
      for (int i = 0; i < 20000; ++i) {
        if (mem::read_safe(reinterpret_cast<uintptr_t>(&local), &v) && v == local) ++good;
        if (!mem::read_safe(r + page + static_cast<uintptr_t>((i + t) % 3000), &v)) ++bad;
      }
    });
  for (auto& x : th) x.join();
  CHECK(good == 160000 && bad == 160000);

  // What a good read costs.
  const int n = 20000000;
  uint64_t sum = 0;
  double t0 = now_ns();
  for (int i = 0; i < n; ++i) {
    mem::read_safe(reinterpret_cast<uintptr_t>(heap.data()) + static_cast<uintptr_t>(i % 9000), &got);
    sum += got;
  }
  const double per_read = (now_ns() - t0) / n;
  t0 = now_ns();
  for (int i = 0; i < 2000; ++i) mem::read_safe(r + page, &got);
  const double per_fault = (now_ns() - t0) / 2000;
  std::printf("a good 8-byte read: %.1f ns; a faulting read: %.0f ns; faults caught: %ld (checksum %llu)\n", per_read,
              per_fault, mem::guard_faults(), static_cast<unsigned long long>(sum));
  CHECK(per_read < 1000.0);

  std::printf(g_failed ? "%d check(s) FAILED\n" : "all checks passed\n", g_failed);
  return g_failed ? 1 : 0;
}
