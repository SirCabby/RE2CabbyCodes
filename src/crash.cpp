#include "crash.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "mem.h"

namespace re2cc {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;  // the filter ours passes an exception on to
using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilterFn g_orig_set_filter = nullptr;
bool g_inert_said = false;                               // SetUnhandledExceptionFilter made to do nothing: logged
bool g_replacer_logged = false;                          // a filter found over ours has been logged,
LPTOP_LEVEL_EXCEPTION_FILTER g_replacer_said = nullptr;  // and the last one was this

// An exception as the filter tells one from another.
struct Seen {
  DWORD code = 0;
  void* at = nullptr;
  DWORD thread = 0;
  DWORD64 rsp = 0;
};
bool same(const Seen& a, const Seen& b) {
  return a.code == b.code && a.at == b.at && a.thread == b.thread && a.rsp == b.rsp;
}

// The exception written out last: the same one given to ours again gets a line,
// not a second dump.
Seen g_last;
unsigned g_again = 0;

// The threads in on_exception now, each with the exceptions ours is passing on
// there, outermost first. Ours given one of those again from further down its
// own call is a loop (on_exception). A new one is an exception raised while the
// filter ours passed the last one on to ran - a packer's handler may raise one
// on purpose - and is passed on as any other, but not past kMaxDepth.
constexpr int kInsideSlots = 8;
constexpr int kMaxDepth = 3;
struct Inside {
  volatile LONG thread;  // 0: free; only its thread touches the rest
  int depth;
  Seen seen[kMaxDepth];
};
Inside g_inside[kInsideSlots] = {};

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

void describe_filter(LPTOP_LEVEL_EXCEPTION_FILTER f, char* out, size_t n) {
  if (f) describe(reinterpret_cast<const void*>(f), out, n);
  else std::snprintf(out, n, "none");
}

// Code that can be called: what ours chains to must be. A value that only
// claims to be the previous filter (below) is not. (A VirtualQuery, but only
// when a filter turns up and at a crash - never per read.)
bool is_code(LPTOP_LEVEL_EXCEPTION_FILTER f) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!f || !VirtualQuery(reinterpret_cast<const void*>(f), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// The first instruction a call of `fn` runs, past the jumps in front of it
// (kernel32's export jumps into kernelbase's on newer Windows; a hook's patch is
// a jump too: jmp [rip+disp32] - with a REX.W or not, and with disp32 0 the
// address right after it - jmp rel32, jmp rel8, mov rax,imm64 + jmp rax). Its
// first 16 bytes go to `code`; false when they cannot be read.
bool first_code(const void* fn, uintptr_t* at, uint8_t code[16]) {
  auto p = reinterpret_cast<uintptr_t>(fn);
  for (int hop = 0; hop < 5; ++hop) {
    if (!p || !mem::copy_from(code, p, 16)) return false;
    int32_t disp = 0;
    if ((code[0] == 0xFF && code[1] == 0x25) || (code[0] == 0x48 && code[1] == 0xFF && code[2] == 0x25)) {
      const int len = code[0] == 0x48 ? 7 : 6;
      std::memcpy(&disp, code + len - 4, sizeof(disp));
      uintptr_t to = 0;
      if (!mem::read_safe(p + len + static_cast<intptr_t>(disp), &to)) return false;
      p = to;
    } else if (code[0] == 0xE9) {
      std::memcpy(&disp, code + 1, sizeof(disp));
      p += 5 + static_cast<intptr_t>(disp);
    } else if (code[0] == 0xEB) {
      p += 2 + static_cast<int8_t>(code[1]);
    } else if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
      std::memcpy(&p, code + 2, sizeof(p));
    } else {
      *at = p;
      return true;
    }
  }
  return false;
}

// Some crash handlers keep the slot for themselves: they set their filter, then
// write a ret over SetUnhandledExceptionFilter's first bytes so that nobody can
// set another (re4_tweaks does it to RE4 (2005) when a CrashDumps folder is
// beside the game; ASI loaders' crash dumpers do the same). A call then changes
// nothing and "returns" whatever rax held - no filter at all, which ours must
// neither take for the previous one nor chain to. Their handler reports the
// crash; this log does not see it.
bool slot_taken() {
  uintptr_t at = 0;
  uint8_t c[16];
  if (!first_code(reinterpret_cast<const void*>(g_orig_set_filter), &at, c)) return false;
  const auto is_ret = [](const uint8_t* q) { return q[0] == 0xC3 || q[0] == 0xC2; };
  const int xor_len = ((c[0] == 0x33 || c[0] == 0x31) && c[1] == 0xC0)                    ? 2
                      : (c[0] == 0x48 && (c[1] == 0x31 || c[1] == 0x33) && c[2] == 0xC0) ? 3
                                                                                          : 0;  // xor eax/rax first
  const bool inert = is_ret(c) || (xor_len && is_ret(c + xor_len));
  if (inert && !g_inert_said) {
    g_inert_said = true;
    char where[MAX_PATH + 32];
    describe(reinterpret_cast<const void*>(at), where, sizeof(where));
    logf("crash: SetUnhandledExceptionFilter has been made to do nothing (a ret at %s) - another crash handler keeps "
         "the slot for itself; a crash is its to report, not this log's",
         where);
  }
  return inert;
}

Seen seen_of(const EXCEPTION_POINTERS* info) {
  Seen s;
  s.thread = GetCurrentThreadId();
  if (info && info->ExceptionRecord) {
    s.code = info->ExceptionRecord->ExceptionCode;
    s.at = info->ExceptionRecord->ExceptionAddress;
  }
  if (info && info->ContextRecord) s.rsp = info->ContextRecord->Rsp;
  return s;
}

// One exception, as the filter sees it. True when it is the one written out
// last, given to ours again (it then gets a line, not a second dump).
bool write_out(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord) {
    logf("CRASH: an exception without its record");
    return false;
  }
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  const CONTEXT* c = info->ContextRecord;
  const Seen seen = seen_of(info);
  char where[MAX_PATH + 32];
  describe(rec->ExceptionAddress, where, sizeof(where));
  if (same(seen, g_last)) {
    logf("CRASH: the same %s at %s again (%u) - our filter was given it once more", code_name(seen.code), where,
         ++g_again);
    return true;
  }
  g_last = seen;
  g_again = 0;

  logf("CRASH: %s (0x%08lX) at %s (thread %lu)", code_name(rec->ExceptionCode), rec->ExceptionCode, where,
       GetCurrentThreadId());
  // For an access violation the record carries what was touched and how; a
  // null or freed address there is the difference between a bad pointer and
  // a bad instruction, and it is not in the register dump below.
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const ULONG_PTR kind = rec->ExceptionInformation[0];
    logf("       %s address %016llX", kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
         static_cast<unsigned long long>(rec->ExceptionInformation[1]));
  }
  if (!c) return false;
  logf("       rip=%016llX rsp=%016llX rbp=%016llX", c->Rip, c->Rsp, c->Rbp);
  logf("       rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX rsi=%016llX rdi=%016llX", c->Rax, c->Rbx, c->Rcx,
       c->Rdx, c->Rsi, c->Rdi);
  logf("       r8=%016llX r9=%016llX r10=%016llX r11=%016llX r12=%016llX r13=%016llX r14=%016llX r15=%016llX", c->R8,
       c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
  // The top of the stack as it is: the faulting function's frame and its
  // callers' locals and spilled registers - what it was working on, which the
  // register dump does not hold any more.
  for (int row = 0; row < 6; ++row) {
    unsigned long long q[4];
    if (!mem::copy_from(q, c->Rsp + row * sizeof(q), sizeof(q))) break;
    logf("       rsp+%03X: %016llX %016llX %016llX %016llX", static_cast<unsigned>(row * sizeof(q)), q[0], q[1], q[2],
         q[3]);
  }
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
  return false;
}

// This thread's entry in g_inside (taken when it has none), or null when every
// entry is another thread's (ours then goes unguarded).
Inside* inside_entry() {
  const LONG me = static_cast<LONG>(GetCurrentThreadId());
  for (Inside& e : g_inside)
    if (e.thread == me) return &e;
  for (Inside& e : g_inside)
    if (InterlockedCompareExchange(&e.thread, me, 0) == 0) {
      e.depth = 0;
      return &e;
    }
  return nullptr;
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  const Seen now = seen_of(info);
  Inside* in = inside_entry();
  if (in && in->depth > 0) {
    // Ours is already running on this thread, further up, passing an exception
    // on. The same one again is the filter it went to handing it back - it
    // chains to the filter it replaced, and that was ours (one set over ours
    // that reassert put ours back over): passing it on again would go round for
    // ever. Another is one raised while that filter ran, passed on as any other
    // - but not past kMaxDepth.
    char where[MAX_PATH + 32] = "?";
    if (info && info->ExceptionRecord) describe(info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    for (int i = 0; i < in->depth; ++i)
      if (same(in->seen[i], now)) {
        logf("       (back in our filter with the same exception, from the filter it passed it on to - %s at %s; not "
             "passed on again)",
             code_name(now.code), where);
        return EXCEPTION_CONTINUE_SEARCH;
      }
    if (in->depth >= kMaxDepth) {
      logf("       (another exception, %s at %s, inside the filters ours has passed %d on to - not passed on)",
           code_name(now.code), where, in->depth);
      return EXCEPTION_CONTINUE_SEARCH;
    }
    logf("       (another exception, raised while the filter ours passed the last one on to ran:)");
  }
  const bool again = write_out(info);
  LONG r = EXCEPTION_CONTINUE_SEARCH;
  if (const LPTOP_LEVEL_EXCEPTION_FILTER next = g_previous) {
    if (!again) {
      char who[MAX_PATH + 32];
      describe_filter(next, who, sizeof(who));
      logf("       passed on to the filter at %s", who);
    }
    if (in) in->seen[in->depth++] = now;
    r = next(info);
    if (in) --in->depth;
    if (r == EXCEPTION_CONTINUE_EXECUTION)
      logf("       (the filter we chain to handled it and the program went on - not a crash)");
  }
  if (in && in->depth == 0) InterlockedExchange(&in->thread, 0);
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
  char who[MAX_PATH + 32];
  describe_filter(next, who, sizeof(who));
  if (slot_taken()) {
    logf("crash: the game set its top-level exception filter (%s) - it did not go in: the slot is another crash "
         "handler's",
         who);
    return previous;
  }
  if (g_orig_set_filter) g_orig_set_filter(&on_exception);
  logf("crash: the game set its top-level exception filter (%s); ours stays on top, chaining to it", who);
  return previous;
}

}  // namespace

void describe_address(const void* addr, char* out, unsigned n) { describe(addr, out, n); }

void install_crash_logger() {
  g_orig_set_filter = &SetUnhandledExceptionFilter;
  if (slot_taken()) return;
  const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(&on_exception);
  g_previous = is_code(previous) ? previous : nullptr;
  logf("crash logger installed (the game's own filter is chained once it sets one)");
}

void* crash_filter_hook() { return reinterpret_cast<void*>(&hk_set_filter); }
void* crash_real_set_filter() { return reinterpret_cast<void*>(g_orig_set_filter); }

void reassert_crash_filter() {
  if (slot_taken()) return;
  const LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(&on_exception);
  if (cur == &on_exception) return;
  // Another filter went over ours without the game's import. Ours goes back on
  // top and passes exceptions on to it - unless it is no code at all. A filter
  // that chains to the one it replaced comes back to ours (on_exception stops
  // it there). One that keeps putting itself back is logged once.
  const bool code = is_code(cur);
  if (code) g_previous = cur;
  if (g_replacer_logged && cur == g_replacer_said) return;
  g_replacer_logged = true;
  g_replacer_said = cur;
  char who[MAX_PATH + 32];
  describe_filter(cur, who, sizeof(who));
  if (cur && !code)
    logf("crash: the top-level filter had been replaced by %s, which is no code - ours is back on top, not chaining to "
         "it",
         who);
  else
    logf("crash: the top-level filter had been replaced (%s) - ours is back on top%s", who,
         cur ? ", chaining to it" : "");
}

}  // namespace re2cc
