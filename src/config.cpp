#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace re2cc::config {
namespace {

Settings g_settings;
char g_path[MAX_PATH] = {};
char g_dir[MAX_PATH] = {};

bool truthy(const char* v) {
  return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes");
}

void write_defaults() {
  FILE* f = std::fopen(g_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# RE2CabbyCodes\n"
               "#\n"
               "#   ToggleKey             = 0x76  ; virtual-key code that hides/shows the panel (0x76 = F7)\n"
               "#   GodMode               = 0     ; cheats switched on when the game starts (the panel changes\n"
               "#   OneHitKills           = 0     ; them for the session)\n"
               "#   InfiniteAmmo          = 0\n"
               "#   SaveWithoutInkRibbons = 0     ; Hardcore typewriters save without asking for a ribbon\n"
               "#   NoSaveCount           = 0     ; save without incrementing the save counter\n"
               "#   FreezePlaytime        = 0\n"
               "#   FreezeCountdown       = 0     ; stop the on-screen countdowns\n"
               "#   NoDurabilityLoss      = 0     ; knives never wear down\n"
               "#   InfiniteWoodenBoards  = 0     ; boarding up a window uses no wooden boards\n"
               "#   NoItemBoxCount        = 0     ; opening the item box is not counted (its count stays 0)\n"
               "#   NoRecoveryItemCount   = 0     ; using a recovery item is not counted (its count stays 0)\n"
               "#   FreezeSteps           = 0     ; the step count stays where it is\n"
               "#   AlwaysShow            = 0     ; debug: draw the panel on every screen, not only the pause menu\n"
               "#   Trace                 = 0     ; verbose diagnostics in RE2CabbyCodes.log\n"
               "#   DumpMethods           = 0     ; write every game method's code address beside the DLL once\n"
               "#   Disable               =       ; comma list of subsystems to turn off: overlay,dispatch,game,events,input,cursor,keyboard\n"
               "ToggleKey = 0x76\n"
               "GodMode = 0\n"
               "OneHitKills = 0\n"
               "InfiniteAmmo = 0\n"
               "SaveWithoutInkRibbons = 0\n"
               "NoSaveCount = 0\n"
               "FreezePlaytime = 0\n"
               "FreezeCountdown = 0\n"
               "NoDurabilityLoss = 0\n"
               "InfiniteWoodenBoards = 0\n"
               "NoItemBoxCount = 0\n"
               "NoRecoveryItemCount = 0\n"
               "FreezeSteps = 0\n"
               "AlwaysShow = 0\n"
               "Trace = 0\n"
               "DumpMethods = 0\n"
               "Disable =\n");
  std::fclose(f);
}

}  // namespace

const Settings& get() { return g_settings; }
const char* dir() { return g_dir; }

void load(const char* dir) {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
  std::snprintf(g_path, sizeof(g_path), "%sRE2CabbyCodes.ini", dir);
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    write_defaults();
    logf("config: no %s - wrote one with the defaults", g_path);
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    // A trailing "; comment" is the file's own documentation style.
    if (char* c = std::strchr(val, ';')) *c = '\0';
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
    } else if (!_stricmp(key, "GodMode")) {
      g_settings.god_mode = truthy(val);
    } else if (!_stricmp(key, "OneHitKills")) {
      g_settings.one_hit_kills = truthy(val);
    } else if (!_stricmp(key, "InfiniteAmmo")) {
      g_settings.infinite_ammo = truthy(val);
    } else if (!_stricmp(key, "SaveWithoutInkRibbons") || !_stricmp(key, "InfiniteInkRibbons")) {
      // InfiniteInkRibbons: the cheat's earlier name, still in ini files written before the rename.
      g_settings.save_without_ink = truthy(val);
    } else if (!_stricmp(key, "NoSaveCount")) {
      g_settings.no_save_count = truthy(val);
    } else if (!_stricmp(key, "FreezePlaytime")) {
      g_settings.freeze_playtime = truthy(val);
    } else if (!_stricmp(key, "FreezeCountdown")) {
      g_settings.freeze_countdown = truthy(val);
    } else if (!_stricmp(key, "NoDurabilityLoss")) {
      g_settings.no_durability_loss = truthy(val);
    } else if (!_stricmp(key, "InfiniteWoodenBoards")) {
      g_settings.infinite_boards = truthy(val);
    } else if (!_stricmp(key, "NoItemBoxCount")) {
      g_settings.no_item_box_count = truthy(val);
    } else if (!_stricmp(key, "NoRecoveryItemCount")) {
      g_settings.no_recovery_item_count = truthy(val);
    } else if (!_stricmp(key, "FreezeSteps")) {
      g_settings.freeze_steps = truthy(val);
    } else if (!_stricmp(key, "AlwaysShow")) {
      g_settings.always_show = truthy(val);
    } else if (!_stricmp(key, "Trace")) {
      g_settings.trace = truthy(val);
    } else if (!_stricmp(key, "DumpMethods")) {
      g_settings.dump_methods = truthy(val);
    } else if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_dispatch = std::strstr(val, "dispatch") != nullptr;
      g_settings.disable_game = std::strstr(val, "game") != nullptr;
      g_settings.disable_events = std::strstr(val, "events") != nullptr;
      g_settings.disable_input = std::strstr(val, "input") != nullptr;
      g_settings.disable_cursor = std::strstr(val, "cursor") != nullptr;
      g_settings.disable_keyboard = std::strstr(val, "keyboard") != nullptr;
    } else {
      logf("config: unknown key '%s' ignored", key);
    }
  }
  std::fclose(f);
  logf("config: ToggleKey=0x%02X GodMode=%d OneHitKills=%d InfiniteAmmo=%d SaveWithoutInkRibbons=%d NoSaveCount=%d "
       "FreezePlaytime=%d FreezeCountdown=%d NoDurabilityLoss=%d InfiniteWoodenBoards=%d NoItemBoxCount=%d "
       "NoRecoveryItemCount=%d FreezeSteps=%d AlwaysShow=%d Trace=%d DumpMethods=%d Disable=%s%s%s%s%s%s%s",
       g_settings.toggle_key, g_settings.god_mode, g_settings.one_hit_kills, g_settings.infinite_ammo,
       g_settings.save_without_ink, g_settings.no_save_count, g_settings.freeze_playtime, g_settings.freeze_countdown,
       g_settings.no_durability_loss, g_settings.infinite_boards, g_settings.no_item_box_count,
       g_settings.no_recovery_item_count, g_settings.freeze_steps, g_settings.always_show, g_settings.trace, g_settings.dump_methods, g_settings.disable_overlay ? "overlay " : "",
       g_settings.disable_dispatch ? "dispatch " : "", g_settings.disable_game ? "game " : "",
       g_settings.disable_events ? "events " : "",
       g_settings.disable_input ? "input " : "", g_settings.disable_cursor ? "cursor " : "",
       g_settings.disable_keyboard ? "keyboard" : "");
}

}  // namespace re2cc::config
