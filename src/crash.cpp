#include "crash.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "log.h"
#include "mem.h"

namespace re2cc {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilterFn g_orig_set_filter = nullptr;

const char* code_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
  }
}

void describe(const void* addr, char* out, size_t n) {
  HMODULE owner = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &owner) &&
      owner) {
    GetModuleFileNameA(owner, path, MAX_PATH);
    const char* leaf = std::strrchr(path, '\\');
    std::snprintf(out, n, "%s+0x%llX", leaf ? leaf + 1 : path,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(owner)));
  } else {
    std::snprintf(out, n, "%p (no module)", addr);
  }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    void* at = info->ExceptionRecord->ExceptionAddress;
    char where[MAX_PATH + 32];
    describe(at, where, sizeof(where));
    logf("CRASH: %s (0x%08lX) at %s (thread %lu)", code_name(info->ExceptionRecord->ExceptionCode),
         info->ExceptionRecord->ExceptionCode, where, GetCurrentThreadId());
    // For an access violation the record carries what was touched and how; a
    // null or freed address there is the difference between a bad pointer and
    // a bad instruction, and it is not in the register dump below.
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
      const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
      logf("       %s address %016llX", kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
           static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    if (const CONTEXT* c = info->ContextRecord) {
      logf("       rip=%016llX rsp=%016llX rbp=%016llX", c->Rip, c->Rsp, c->Rbp);
      logf("       rax=%016llX rcx=%016llX rdx=%016llX r8=%016llX r9=%016llX", c->Rax, c->Rcx, c->Rdx, c->R8, c->R9);
      // A few return addresses off the stack, for the ones that land in a module.
      auto* sp = reinterpret_cast<void**>(c->Rsp);
      for (int i = 0, shown = 0; i < 256 && shown < 10; ++i) {
        if (!mem::readable(sp + i, sizeof(void*))) break;
        void* v = sp[i];
        HMODULE owner = nullptr;
        if (v && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCSTR>(v), &owner) &&
            owner) {
          describe(v, where, sizeof(where));
          logf("       stack[%03d] %s", i, where);
          ++shown;
        }
      }
    }
  }
  if (!g_previous) return EXCEPTION_CONTINUE_SEARCH;
  const LONG r = g_previous(info);
  if (r == EXCEPTION_CONTINUE_EXECUTION) logf("       (the filter we chain to handled it and the program went on - not a crash)");
  return r;
}

// SetUnhandledExceptionFilter has one global slot, and the game (RE Engine
// installs its own crash reporter) takes it after our DllMain has run. Its
// import is pointed here from DllMain, so the game's filter becomes the one we
// chain to and ours stays on top; reassert_crash_filter() covers every path we
// do not see.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_set_filter(LPTOP_LEVEL_EXCEPTION_FILTER next) {
  LPTOP_LEVEL_EXCEPTION_FILTER previous = g_previous;
  g_previous = next;
  if (g_orig_set_filter) g_orig_set_filter(&on_exception);
  logf("crash: the game set its top-level exception filter to %p; ours stays on top (chaining to it)",
       reinterpret_cast<void*>(next));
  return previous;
}

}  // namespace

void describe_address(const void* addr, char* out, unsigned n) { describe(addr, out, n); }

void install_crash_logger() {
  g_orig_set_filter = &SetUnhandledExceptionFilter;
  g_previous = SetUnhandledExceptionFilter(&on_exception);
  logf("crash logger installed (the game's own filter is chained once it sets one)");
}

void* crash_filter_hook() { return reinterpret_cast<void*>(&hk_set_filter); }
void* crash_real_set_filter() { return reinterpret_cast<void*>(g_orig_set_filter); }

void reassert_crash_filter() {
  LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(&on_exception);
  if (cur == &on_exception) return;
  if (cur) g_previous = cur;
  logf("crash: the top-level filter had been replaced (%p) - ours is back on top%s", reinterpret_cast<void*>(cur),
       cur ? ", chaining to it" : "");
}

}  // namespace re2cc
