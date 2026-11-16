// The crash logger (src/crash.cpp) outside the game, under Wine:
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_crash.cpp
//       src/crash.cpp src/mem.cpp -o tests/build/test_crash.exe
//   WINEPREFIX=<scratch> wine tests/build/test_crash.exe        # "PASSED"; `c2` makes the last phase's stub ret 4
//
// The filter is called as UnhandledExceptionFilter calls the top-level one, with
// made-up access violations. In turn: the game's own filter set through its
// import (the hook) and chained to; the same exception given to ours again (a
// line, not a second dump, still chained); a filter set over ours that chains to
// the one it replaced - ours - which reassert puts ours back over and chains to
// (the same exception back in ours must stop there, not go round); a filter
// that raises another exception into ours while it runs and handles that one
// (ours must pass the new one on); one that raises a new one every time (passed
// on three deep, then not); a "previous filter" that is no code (not chained
// to); a crash on another thread while one is inside the chain (its own dump);
// and SetUnhandledExceptionFilter made to do nothing - a ret over its first
// byte, or `c2`: ret 4 - as some crash handlers do to keep the slot (said once,
// the API not called, nothing taken for a filter, no line a second). Each is
// checked in the log lines. Found reading an RE4 (2005) mod user's crash report,
// 2026-11-10: the old file goes round the chain-back phase until the stack runs
// out.
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/crash.h"
#include "../src/log.h"

namespace {

std::vector<std::string> g_lines;
CRITICAL_SECTION g_lock;
int g_failures = 0;

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

size_t count(const char* needle, size_t from = 0) {
  size_t n = 0;
  EnterCriticalSection(&g_lock);
  for (size_t i = from; i < g_lines.size(); ++i)
    if (g_lines[i].find(needle) != std::string::npos) ++n;
  LeaveCriticalSection(&g_lock);
  return n;
}

size_t mark() {
  EnterCriticalSection(&g_lock);
  const size_t n = g_lines.size();
  LeaveCriticalSection(&g_lock);
  return n;
}

using Filter = LPTOP_LEVEL_EXCEPTION_FILTER;
using SetFilterFn = Filter(WINAPI*)(Filter);

// The filter on top now, the slot left as it was.
Filter top() {
  const Filter t = SetUnhandledExceptionFilter(nullptr);
  SetUnhandledExceptionFilter(t);
  return t;
}

// A made-up access violation: reading address 0 at `at`, with a stack of its own.
struct Fake {
  EXCEPTION_RECORD rec;
  CONTEXT ctx;
  EXCEPTION_POINTERS ptrs;
  uint64_t stack[160];
};
void make(Fake* f, void* at, uint64_t rdi) {
  std::memset(f, 0, sizeof(*f));
  f->rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  f->rec.ExceptionAddress = at;
  f->rec.NumberParameters = 2;
  for (int i = 0; i < 160; ++i) f->stack[i] = 0x0100000000000000ull + i;
  f->stack[3] = reinterpret_cast<uint64_t>(&make);  // a return address in this exe
  f->ctx.ContextFlags = CONTEXT_FULL;
  f->ctx.Rip = reinterpret_cast<DWORD64>(at);
  f->ctx.Rsp = reinterpret_cast<DWORD64>(f->stack);
  f->ctx.Rax = 0x10;
  f->ctx.Rcx = 4;
  f->ctx.Rsi = 0;
  f->ctx.Rdi = rdi;
  f->ctx.R15 = 0x1515151515151515ull;
  f->ptrs.ExceptionRecord = &f->rec;
  f->ptrs.ContextRecord = &f->ctx;
}
void* code_at(int n) { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&top) + n); }

Filter g_ours = nullptr;

// The game's own filter (RE Engine's crash reporter, here: nothing).
int g_game_calls = 0;
LONG WINAPI game_filter(EXCEPTION_POINTERS*) {
  ++g_game_calls;
  return EXCEPTION_CONTINUE_SEARCH;
}

// A filter set over ours with the real API, that passes every exception on to
// the one it replaced (as many crash loggers do).
Filter g_x_prev = nullptr;
int g_x_calls = 0;
LONG WINAPI x_filter(EXCEPTION_POINTERS* p) {
  ++g_x_calls;
  return g_x_prev ? g_x_prev(p) : EXCEPTION_CONTINUE_SEARCH;
}

// A filter that raises another exception into ours while it runs (a packer's
// handler may), and handles that one: the program goes on.
Fake g_inner;
int g_y_calls = 0;
LONG WINAPI y_filter(EXCEPTION_POINTERS*) {
  if (++g_y_calls == 1) {
    make(&g_inner, code_at(0x40), 0x1111);
    g_ours(&g_inner.ptrs);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

// A filter that raises a new exception into ours every time it runs.
Fake g_runaway[8];
int g_z_calls = 0;
LONG WINAPI z_filter(EXCEPTION_POINTERS*) {
  const int i = g_z_calls++;
  if (i >= 8) return EXCEPTION_CONTINUE_SEARCH;
  make(&g_runaway[i], code_at(0x80 + i), 0x2000 + i);
  return g_ours(&g_runaway[i].ptrs);
}

// For the two threads: the first, inside x_waiting_filter, waits for the second's crash.
HANDLE g_inside_x = nullptr, g_second_done = nullptr;
DWORD g_waiting_thread = 0;
LONG WINAPI x_waiting_filter(EXCEPTION_POINTERS* p) {
  ++g_x_calls;
  if (GetCurrentThreadId() == g_waiting_thread) {
    SetEvent(g_inside_x);
    WaitForSingleObject(g_second_done, 5000);
  }
  return g_x_prev ? g_x_prev(p) : EXCEPTION_CONTINUE_SEARCH;
}

struct Fire {
  Filter f;
  Fake* fake;
  LONG r;
};
DWORD WINAPI fire_thread(void* arg) {
  auto* a = static_cast<Fire*>(arg);
  a->r = a->f(&a->fake->ptrs);
  return 0;
}

}  // namespace

namespace re2cc {
void log_init(const char*) {}
void log_shutdown() {}
void logf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  EnterCriticalSection(&g_lock);
  g_lines.push_back(buf);
  std::printf("    | %s\n", buf);
  LeaveCriticalSection(&g_lock);
}
}  // namespace re2cc

int main(int argc, char** argv) {
  InitializeCriticalSection(&g_lock);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  using namespace re2cc;
  const bool ret4 = argc > 1 && std::strcmp(argv[1], "c2") == 0;

  std::printf("install, and the game's filter set through its import:\n");
  install_crash_logger();
  const Filter ours = g_ours = top();
  check(ours != nullptr && ours != &game_filter && ours != &x_filter, "ours is on top after install");
  const auto hook = reinterpret_cast<SetFilterFn>(crash_filter_hook());
  hook(&game_filter);
  check(top() == ours, "ours stays on top when the game sets its filter");

  std::printf("a crash:\n");
  Fake f1;
  make(&f1, code_at(0), 0x0C5D77C);
  size_t m = mark();
  LONG r = ours(&f1.ptrs);
  check(r == EXCEPTION_CONTINUE_SEARCH, "the search goes on");
  check(count("CRASH: ACCESS_VIOLATION", m) == 1, "one dump");
  check(count("reading address 0000000000000000", m) == 1, "what was touched");
  check(count("rsi=0000000000000000 rdi=0000000000C5D77C", m) == 1 && count("r15=1515151515151515", m) == 1,
        "every register");
  check(count("rsp+000: 0100000000000000 0100000000000001 0100000000000002", m) == 1 && count("rsp+0A0:", m) == 1,
        "the top of the stack, raw (six rows)");
  check(count("stack[003] test_crash.exe+", m) == 1, "a return address named by its module");
  check(count("passed on to the filter at test_crash.exe+", m) == 1, "the filter it chains to, named");
  check(g_game_calls == 1, "the game's filter ran once");

  std::printf("the same exception again:\n");
  m = mark();
  ours(&f1.ptrs);
  check(count("CRASH: the same ACCESS_VIOLATION", m) == 1 && count("rip=", m) == 0, "a line, not a second dump");
  check(g_game_calls == 2, "still chained");

  std::printf("a filter set over ours that chains to the one it replaced:\n");
  g_x_prev = SetUnhandledExceptionFilter(&x_filter);
  check(g_x_prev == ours, "it saved ours as the one to chain to");
  m = mark();
  reassert_crash_filter();
  check(top() == ours, "reassert puts ours back on top");
  check(count("replaced (test_crash.exe+", m) == 1 && count("chaining to it", m) == 1, "and says over whose");
  Fake f2;
  make(&f2, code_at(0x10), 0x12345678);
  m = mark();
  r = ours(&f2.ptrs);
  check(g_x_calls == 1, "the other filter ran once - no loop");
  check(count("back in our filter with the same exception", m) == 1, "the way back into ours is logged");
  check(count("CRASH: ACCESS_VIOLATION", m) == 1, "one dump for it");
  check(r == EXCEPTION_CONTINUE_SEARCH, "the search goes on");
  m = mark();
  SetUnhandledExceptionFilter(&x_filter);
  reassert_crash_filter();
  check(count("replaced", m) == 0, "a filter that puts itself back again is not logged again");

  std::printf("a filter that raises another exception into ours, and handles it:\n");
  SetUnhandledExceptionFilter(&y_filter);
  reassert_crash_filter();
  Fake f3;
  make(&f3, code_at(0x20), 0x3333);
  m = mark();
  r = ours(&f3.ptrs);
  check(g_y_calls == 2, "the new exception was passed on to it too");
  check(count("another exception, raised while", m) == 1, "said so");
  check(count("rdi=0000000000003333", m) == 1 && count("rdi=0000000000001111", m) == 1, "each with its own dump");
  check(count("handled it and the program went on", m) == 1, "its answer for the new one given back");
  check(r == EXCEPTION_CONTINUE_SEARCH, "the first one's search goes on");

  std::printf("a filter that raises a new exception into ours every time:\n");
  SetUnhandledExceptionFilter(&z_filter);
  reassert_crash_filter();
  Fake f4;
  make(&f4, code_at(0x30), 0x4444);
  m = mark();
  r = ours(&f4.ptrs);
  check(g_z_calls == 3, "passed on three deep, then not");
  check(count("CRASH: ACCESS_VIOLATION", m) == 3, "a dump for each passed on");
  check(count("inside the filters ours has passed 3 on to - not passed on", m) == 1, "the fourth stopped, said so");
  check(r == EXCEPTION_CONTINUE_SEARCH, "the search goes on");

  std::printf("a previous filter that is no code:\n");
  SetUnhandledExceptionFilter(&x_filter);
  reassert_crash_filter();
  SetUnhandledExceptionFilter(reinterpret_cast<Filter>(static_cast<uintptr_t>(0x3EA)));
  m = mark();
  reassert_crash_filter();
  check(top() == ours, "ours is back on top");
  check(count("which is no code", m) == 1, "said so");
  Fake f5;
  make(&f5, code_at(0x50), 1);
  const int x_before = g_x_calls;
  r = ours(&f5.ptrs);  // would jump to 0x3EA if it were chained to
  check(g_x_calls == x_before + 1, "still chained to the last filter that is code");

  std::printf("two threads:\n");
  g_x_prev = SetUnhandledExceptionFilter(&x_waiting_filter);
  reassert_crash_filter();
  g_inside_x = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  g_second_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  Fake fa, fb;
  make(&fa, code_at(0x60), 0xAAAA);
  make(&fb, code_at(0x70), 0xBBBB);
  Fire a{ours, &fa, -9}, b{ours, &fb, -9};
  m = mark();
  HANDLE ta = CreateThread(nullptr, 0, fire_thread, &a, CREATE_SUSPENDED, &g_waiting_thread);
  ResumeThread(ta);
  check(WaitForSingleObject(g_inside_x, 5000) == WAIT_OBJECT_0, "the first thread is inside the chain");
  HANDLE tb = CreateThread(nullptr, 0, fire_thread, &b, 0, nullptr);
  WaitForSingleObject(tb, 5000);
  check(count("rdi=000000000000BBBB", m) == 1, "the second thread's crash gets its own dump");
  SetEvent(g_second_done);
  WaitForSingleObject(ta, 5000);
  check(count("rdi=000000000000AAAA", m) == 1, "and the first's has its own");
  check(count("back in our filter with the same exception", m) == 2, "each stopped on its own thread");
  check(a.r == EXCEPTION_CONTINUE_SEARCH && b.r == EXCEPTION_CONTINUE_SEARCH, "both searches go on");
  CloseHandle(ta);
  CloseHandle(tb);

  std::printf("SetUnhandledExceptionFilter made to do nothing (%s over its first bytes):\n", ret4 ? "ret 4" : "ret");
  auto* api = reinterpret_cast<uint8_t*>(&SetUnhandledExceptionFilter);
  uint8_t saved[3];
  std::memcpy(saved, api, 3);
  DWORD old = 0;
  VirtualProtect(api, 3, PAGE_EXECUTE_READWRITE, &old);
  const uint8_t stub_ret[3] = {0xC3, 0xCC, 0xCC}, stub_ret4[3] = {0xC2, 0x04, 0x00};
  std::memcpy(api, ret4 ? stub_ret4 : stub_ret, 3);
  FlushInstructionCache(GetCurrentProcess(), api, 3);
  // (From here until the bytes are back, nothing may call the API: a ret 4 would
  // leave the stack 4 bytes off.)
  m = mark();
  for (int i = 0; i < 5; ++i) reassert_crash_filter();  // five seconds of ticks
  check(count("made to do nothing", m) == 1, "said once");
  check(count("replaced", m) == 0, "no garbage taken for a filter, no line a second");
  hook(&game_filter);
  check(count("it did not go in", m) == 1 && count("ours stays on top", m) == 0,
        "the game's call is not claimed to leave ours on top");
  install_crash_logger();
  check(count("crash logger installed", m) == 0, "install stands aside too");
  std::memcpy(api, saved, 3);
  VirtualProtect(api, 3, old, &old);
  FlushInstructionCache(GetCurrentProcess(), api, 3);
  check(top() == ours, "the API back as it was, ours is still on top");

  std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
  return g_failures ? 1 : 0;
}
