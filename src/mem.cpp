#include "mem.h"

// Guarded copies. A read of game memory must never crash the game, and asking
// VirtualQuery before every read (the old way) is ruinous under Wine: its
// NtQueryVirtualMemory scans the page table to the end of the committed run,
// about 15 µs per GB of heap after the address, and a tick's reads missed a
// one-region cache on nearly every object - thousands of queries a frame, the
// game at 30 fps instead of 120. So a read is now a plain copy, and a fault in
// it is caught instead: the copy loop below touches no stack and no callee-saved
// register, a vectored exception handler (installed first, once) sends a fault
// raised inside it to the `failed` exit, and the copy returns false. The game
// installs no vectored handler of its own (re2.exe imports none), so the mod's
// sees its own faults first; every other exception is passed on untouched.

extern "C" {
// rcx = destination, rdx = source, r8 = bytes. Either side may be the game's
// memory, so one loop serves reads and stores. Returns 1, or 0 after a fault.
int re2cc_guarded_copy(void* dst, const void* src, size_t n);
extern const char re2cc_guarded_begin[], re2cc_guarded_failed[], re2cc_guarded_end[];
}

asm(R"(
    .text
    .p2align 4
    .globl re2cc_guarded_begin
re2cc_guarded_begin:
    .globl re2cc_guarded_copy
re2cc_guarded_copy:
    cmpq $8, %r8
    jb 2f
1:  movq (%rdx), %rax
    movq %rax, (%rcx)
    addq $8, %rdx
    addq $8, %rcx
    subq $8, %r8
    cmpq $8, %r8
    jae 1b
2:  testq %r8, %r8
    jz 4f
3:  movzbl (%rdx), %eax
    movb %al, (%rcx)
    incq %rdx
    incq %rcx
    decq %r8
    jnz 3b
4:  movl $1, %eax
    ret
    .globl re2cc_guarded_failed
re2cc_guarded_failed:
    xorl %eax, %eax
    ret
    .globl re2cc_guarded_end
re2cc_guarded_end:
)");

namespace re2cc::mem {
namespace {

PVOID g_handler = nullptr;
volatile LONG g_installing = 0;
volatile LONG g_faults = 0;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != STATUS_GUARD_PAGE_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR)
    return EXCEPTION_CONTINUE_SEARCH;
  const DWORD64 rip = ep->ContextRecord->Rip;
  if (rip < static_cast<DWORD64>(reinterpret_cast<uintptr_t>(re2cc_guarded_begin)) ||
      rip >= static_cast<DWORD64>(reinterpret_cast<uintptr_t>(re2cc_guarded_failed)))
    return EXCEPTION_CONTINUE_SEARCH;
  ep->ContextRecord->Rip = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(re2cc_guarded_failed));
  InterlockedIncrement(&g_faults);
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool guard_install() {
  if (g_handler) return true;
  // Once, whichever thread gets here first; the others wait for it.
  while (InterlockedCompareExchange(&g_installing, 1, 0) != 0) {
    if (g_handler) return true;
    Sleep(0);
  }
  if (!g_handler) g_handler = AddVectoredExceptionHandler(1, &on_exception);
  InterlockedExchange(&g_installing, 0);
  return g_handler != nullptr;
}

long guard_faults() { return g_faults; }

bool copy_from(void* dst, uintptr_t src, size_t n) {
  if (!plausible(src) || n > (1ull << 32) || src + n < src) return false;
  if (!g_handler && !guard_install()) return false;
  return re2cc_guarded_copy(dst, reinterpret_cast<const void*>(src), n) != 0;
}

bool copy_to(uintptr_t dst, const void* src, size_t n) {
  if (!plausible(dst) || n > (1ull << 32) || dst + n < dst) return false;
  if (!g_handler && !guard_install()) return false;
  return re2cc_guarded_copy(reinterpret_cast<void*>(dst), src, n) != 0;
}

bool readable(uintptr_t a, size_t n) {
  if (!plausible(a) || n > (1ull << 32) || a + n < a) return false;
  if (n == 0) return true;
  // One byte of every page the range touches: access is granted per page.
  uint8_t b;
  const uintptr_t last_page = (a + n - 1) & ~static_cast<uintptr_t>(0xFFF);
  for (uintptr_t page = a & ~static_cast<uintptr_t>(0xFFF);; page += 0x1000) {
    if (!copy_from(&b, page < a ? a : page, 1)) return false;
    if (page >= last_page) return true;
  }
}

}  // namespace re2cc::mem
