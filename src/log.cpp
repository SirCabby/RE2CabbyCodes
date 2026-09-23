#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace re2cc {
namespace {

char             g_path[MAX_PATH] = {};
CRITICAL_SECTION g_lock;
bool             g_ready = false;

}  // namespace

void log_init(const char* dir) {
  if (g_ready) return;
  InitializeCriticalSection(&g_lock);
  g_ready = true;

  snprintf(g_path, sizeof(g_path), "%sRE2CabbyCodes.log", dir);

  // Truncate on each launch so the log always describes the current run - but
  // keep the run before it, because the first thing that happens after a crash
  // is a relaunch, and that would otherwise wipe the only record of it.
  char previous[MAX_PATH];
  snprintf(previous, sizeof(previous), "%sRE2CabbyCodes.prev.log", dir);
  MoveFileExA(g_path, previous, MOVEFILE_REPLACE_EXISTING);
  if (FILE* f = fopen(g_path, "w")) fclose(f);
}

void log_shutdown() {
  if (!g_ready) return;
  g_ready = false;
  DeleteCriticalSection(&g_lock);
}

void logf(const char* fmt, ...) {
  if (!g_ready || !g_path[0]) return;

  EnterCriticalSection(&g_lock);
  if (FILE* f = fopen(g_path, "a")) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fputc('\n', f);
    fclose(f);  // flush-per-line: a crash must not lose the last message
  }
  LeaveCriticalSection(&g_lock);
}

}  // namespace re2cc
