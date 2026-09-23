#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "app.h"
#include "cheats.h"
#include "config.h"
#include "dispatch.h"
#include "events.h"
#include "game.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include "input.h"
#include "inventory.h"
#include "items.h"
#include "keyboard.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "records.h"
#include "savefiles.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace re2cc::overlay {
namespace {

HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
volatile LONG g_visible = 0;
// The toggle key (under the context lock): on the pause menu it hides the
// panel; anywhere else it opens it - over the game, or to show why the pause
// menu has none. Both are forgotten when the pause menu opens or closes.
bool g_user_hidden = false;
bool g_forced = false;
bool g_was_showable = false;
int g_was_records = -1;  // the records screen up at the last frame (records::Set, -1 none)
bool g_was_load = false;  // the Load Game screen up at the last frame (the save files)
volatile LONG g_legacy_keys = 0;  // the window gets WM_KEYDOWN: without the panel's own keyboard the toggle comes from there
volatile LONG g_legacy_chars = 0; // the window gets WM_CHAR (the log: whether typing could come from messages at all)
DWORD g_polled_toggle_at = 0;     // the render thread took a press of the toggle key (no WM_KEYDOWN seen yet)
UINT g_wake_msg = 0;              // posted to the window when the panel comes or goes: the pointer is the window thread's
// The cheats folded away to give the inventory and the item box the room; kept
// in the ImGui layout file ([RE2CabbyCodes][Panel]) with the panel's position.
bool g_cheats_open = true;

void cursor_update(bool now);

// The pause menu, a records screen or the Load Game screen is up, as of a recent
// tick (a tick that stopped coming is not a pause menu).
bool showable(const dispatch::Snapshot& s) {
  return (s.ticking && s.age_ms < 2000 && s.show_panel) || config::get().always_show;
}

void toggle(bool on_pause_menu, const char* how) {
  if (on_pause_menu) {
    g_user_hidden = !g_user_hidden;
    logf("panel %s by the toggle key%s", g_user_hidden ? "hidden" : "shown", how);
  } else {
    g_forced = !g_forced;
    logf("panel %s by the toggle key outside the pause menu%s", g_forced ? "opened" : "closed", how);
  }
}

// The context lock (see overlay.h). Created in install_early(), from DllMain,
// before either path that takes it exists; never destroyed, because a frame
// can be in progress while the process tears down.
CRITICAL_SECTION g_imgui_cs;
bool g_imgui_cs_ready = false;

// What the panel is taking this frame, published for the input guard.
volatile LONG g_capture_mouse = 0;
volatile LONG g_capture_keyboard = 0;

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (g_wake_msg && msg == g_wake_msg) {
    cursor_update(true);
    return 0;
  }
  cursor_update(false);
  // F-keys above F9 arrive as WM_SYSKEYDOWN; accept both, ignore auto-repeat.
  if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
    const bool first = !InterlockedExchange(&g_legacy_keys, 1);
    if (first) logf("overlay: the game's window gets key messages (WM_KEYDOWN)");
    if (!(lp & (1 << 30)) && static_cast<int>(wp) == config::get().toggle_key) {
      // With the panel's own keyboard the toggle comes from there (wants_draw);
      // the key is kept from the game either way.
      if (!keyboard::active()) {
        ImGuiLock guard;  // wants_draw reads and clears these
        // The render thread polls the key until the window is seen to get key
        // messages; a press it already took is not taken twice.
        if (!first || GetTickCount() - g_polled_toggle_at > 500) toggle(showable(dispatch::snapshot()), "");
      }
      return 0;
    }
  }
  if (msg == WM_CHAR && !InterlockedExchange(&g_legacy_chars, 1))
    logf("overlay: the game's window gets character messages (WM_CHAR)");
  // ImGui sees every message, visible or not: a button release that arrives
  // after the panel is hidden would otherwise never be delivered. Its keys come
  // from one place only: the panel's own keyboard (keyboard.h) when it has one,
  // these messages when it has not.
  if (g_context_ready) {
    bool swallow = false;
    {
      ImGuiLock guard;
      const bool key_message = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_UNICHAR;
      if (!(key_message && keyboard::active()) && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
      if (g_visible && !config::get().disable_input) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) swallow = true;
        if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE) swallow = true;
        // The game reads its keyboard and mouse as Raw Input: a packet for the
        // device the panel is using goes no further.
        if (msg == WM_INPUT) {
          const int type = input::raw_input_type(lp);
          if ((type == RIM_TYPEMOUSE && io.WantCaptureMouse) || (type == RIM_TYPEKEYBOARD && io.WantTextInput)) swallow = true;
        }
      }
    }
    if (g_visible && msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && !config::get().disable_cursor) {
      // The game hides its cursor in the window; the panel needs an arrow.
      SetCursor(LoadCursorA(nullptr, reinterpret_cast<LPCSTR>(IDC_ARROW)));
      return TRUE;
    }
    if (swallow) return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
  }
  // The game's own window procedure runs outside the lock.
  return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

// --- the pointer -------------------------------------------------------------------------------
// RE2 hides its cursor and, in a window, holds the pointer in it with
// ClipCursor. While the panel is up the cursor is shown - ShowCursor(TRUE)
// until the display count is back at 0, each call counted and taken back when
// the panel goes (under gamescope a hidden cursor also means the pointer is
// locked) - and the game's clip is answered with a release, its last clip put
// back when the panel goes.
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
ClipCursorFn g_real_clip = nullptr;
uintptr_t g_clip_slot = 0;
CRITICAL_SECTION g_cursor_cs;
bool g_game_clipping = false;
RECT g_game_clip{};
bool g_cursor_free = false;
int g_cursor_shows = 0;

BOOL WINAPI hk_clip_cursor(const RECT* r) {
  EnterCriticalSection(&g_cursor_cs);
  g_game_clipping = r != nullptr;
  if (r) g_game_clip = *r;
  const bool held_back = r && g_cursor_free;
  const BOOL ok = g_real_clip(held_back ? nullptr : r);
  LeaveCriticalSection(&g_cursor_cs);
  return ok;
}

void show_cursor(bool show) {
  if (show) {
    int count = ShowCursor(TRUE) - 1;  // read the display count without changing it
    ShowCursor(FALSE);
    const bool first = g_cursor_shows == 0;
    while (count < 0 && g_cursor_shows < 64) {
      count = ShowCursor(TRUE);
      ++g_cursor_shows;
    }
    if (first && g_cursor_shows) logf("pointer: the cursor is shown while the panel is up (%d ShowCursor call(s))", g_cursor_shows);
  } else if (g_cursor_shows) {
    for (; g_cursor_shows > 0; --g_cursor_shows) ShowCursor(FALSE);
  }
}

// --- panel helpers ----------------------------------------------------------------------------------
const ImVec4 kOrange(1.0f, 0.6f, 0.3f, 1.0f);

void help_marker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(420.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void cheat_row(cheats::Kind k, const char* label, const char* help, bool part_found, const char* not_found_text) {
  // A cheat works once the game's parts it needs were found and the game calls
  // the mod at its events (events.h).
  const bool available = part_found && events::ready(k);
  const char* unavailable_text = !part_found ? not_found_text : events::why_not(k);
  bool v = cheats::enabled(k);
  if (!available) ImGui::BeginDisabled();
  if (ImGui::Checkbox(label, &v)) {
    cheats::set_enabled(k, v);
    logf("%s %s (panel)", cheats::name(k), v ? "ON" : "OFF");
  }
  if (!available) ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(400.0f);
    ImGui::TextUnformatted(help);
    if (!available && unavailable_text) {
      ImGui::Separator();
      ImGui::TextUnformatted(unavailable_text);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (!available && unavailable_text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", unavailable_text);
  }
}

// The difficulty: not a switch the mod holds but the game's own state, shown as
// it is and changed through the game's own switch (cheats.h) while the pause
// menu is up.
void draw_difficulty(const cheats::Status& st, bool paused) {
  ImGui::SeparatorText("Difficulty");
  help_marker(
      "Switches the game you are playing between Assisted, Standard and Hardcore on the spot - nothing is reloaded. "
      "It is the game's own switch, the one a new game and every load make. What the game checks as it goes follows "
      "at once: Assisted's aim assist and slow healing, the typewriters' ink ribbons on Hardcore, and the game's "
      "rank, held to the new difficulty's range from its next change. What the game set up for the difficulty when "
      "it loaded the area you are in stays as it is until that area loads again.\n\n"
      "Your next save records the new difficulty, and loading a save made before the switch brings that save's "
      "difficulty back. A clear counts for the difficulty you finish on. If you took the game over screen's offer "
      "to continue on Assisted, switching to Standard or Hardcore ends it (the game would otherwise put every load "
      "back on Assisted until you return to the title).");
  const game::Difficulty kChoices[] = {game::Difficulty::kAssisted, game::Difficulty::kStandard,
                                       game::Difficulty::kHardcore};
  const auto cur = static_cast<game::Difficulty>(st.difficulty);
  float widest = 0.0f;
  for (const game::Difficulty d : kChoices) widest = std::max(widest, ImGui::CalcTextSize(game::difficulty_name(d)).x);
  ImGui::BeginDisabled(!st.difficulty_ok || !paused || cur == game::Difficulty::kUnknown);
  ImGui::SetNextItemWidth(widest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
  if (ImGui::BeginCombo("##difficulty", game::difficulty_name(cur))) {
    for (const game::Difficulty d : kChoices) {
      const bool sel = d == cur;
      if (ImGui::Selectable(game::difficulty_name(d), sel) && !sel) {
        cheats::request_difficulty(static_cast<int>(d));
        logf("difficulty: %s asked for (panel)", game::difficulty_name(d));
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (!st.difficulty_ok && st.difficulty_why[0]) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", st.difficulty_why);
  } else if (!paused) {
    ImGui::SameLine();
    ImGui::TextDisabled("(pause the game to switch)");
  }
}

void hms(double seconds, char* out, size_t n) {
  if (seconds < 0.0) {
    std::snprintf(out, n, "?");
    return;
  }
  const long long t = static_cast<long long>(seconds);
  std::snprintf(out, n, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
}

// Case-insensitive substring match against "<id> <code> <name> <examined>", so a
// list can be narrowed by a name, the name an item shows before it is
// examined, its number or its code.
bool matches(const items::Def& d, const char* needle) {
  if (!needle || !needle[0]) return true;
  char hay[192];
  std::snprintf(hay, sizeof(hay), "%d %s %s %s", d.id, d.code, d.name, d.examined ? d.examined : "");
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (needle[i] && lower(p[i]) == lower(needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return false;
}

// The same over a weapon's "<id> <code> <name>": the pickers narrow weapons
// and items alike.
bool matches_weapon(const items::WeaponDef& w, const char* needle) {
  items::Def d{w.id, w.code, w.name, nullptr, w.name, items::kBankWeapon, w.listed};
  return matches(d, needle);
}

// Weapon rows in the slot picker are pushed under an id of their own, clear of
// the items' Item.ID values.
constexpr int kWeaponPickerId = 100000;

const items::Def* item_def(int id) {
  for (const auto& d : items::kItems)
    if (d.id == id) return &d;
  return nullptr;
}

const items::WeaponDef* weapon_def(int id) {
  if (id <= 0) return nullptr;
  for (const auto& w : items::kWeapons)
    if (w.id == id) return &w;
  return nullptr;
}

int bank_of(const game::Item& it) {
  if (game::is_weapon(it)) return items::kBankWeapon;
  const items::Def* d = item_def(it.item_id);
  return d ? d->bank : items::kBankOther;
}

const char* bank_name(int b) {
  static const char* const kNames[items::kBankCount] = {"Key items", "Weapons", "Ammo", "Heals", "Other"};
  return b >= 0 && b < items::kBankCount ? kNames[b] : "?";
}

// --- the inventory ------------------------------------------------------------------------------------
// A row being edited. A dirty row stops following the live inventory - that
// is the point of it - so it must not outlive the panel it was typed into.
// g_edit_wid is the WeaponType picked for the row (-1: none, an item or empty);
// the two are never both set, as a stock holds one or the other.
int g_edit_id[inventory::kBagMax], g_edit_wid[inventory::kBagMax], g_edit_cnt[inventory::kBagMax];
bool g_edit_dirty[inventory::kBagMax];
int g_ask_slot = -1;
int g_drop_index = -1;
inventory::Entry g_drop_seen;

// The records panel's "all" confirmation (draw_records, below): which (all on 1,
// all off 0) and for which set's window (records::Set).
int g_records_ask_all = -1;
int g_records_ask_set = -1;

// The save file a Delete or Copy asked about, and what the panel saw of the
// slots when it was clicked (draw_save_files, below). Like RE1's, it belongs to
// the screen it was clicked on: a Load Game screen that closed and came back is
// asked again.
enum { kAskNone = 0, kAskDelete, kAskCopy };
int g_save_ask = kAskNone;
int g_save_from = -1, g_save_to = -1;  // rows of the game's list
savefiles::Row g_save_from_seen, g_save_to_seen;

void forget_save_dialogs() {
  g_save_ask = kAskNone;
  g_save_from = g_save_to = -1;
}

void forget_dialogs() {
  std::memset(g_edit_dirty, 0, sizeof(g_edit_dirty));
  g_ask_slot = -1;
  g_drop_index = -1;
  g_records_ask_all = -1;  // the records' "all" confirmation (records panel, below)
  forget_save_dialogs();
}

void draw_inventory(const inventory::View& v) {
  char nb[96];
  bool ask_apply = false;
  ImGui::SeparatorText("Inventory");
  help_marker(
      "The inventory of the character you are playing, straight from the game. Pick an item or a weapon and a count "
      "for a slot and press Apply; Store puts whatever the slot holds - weapons and two-slot items too - into the "
      "item box below.\n\n"
      "Weapons, the grenades and knives among them, go in and out through the game's own inventory functions, so one "
      "is armed and put away properly - which needs those functions and an empty item box entry to build it in. An "
      "item that takes two slots still comes in through the item box.\n\n"
      "A weapon comes with no parts fitted, full of the ammo the game says it takes. The infinite weapons are the "
      "game's rewards: unless the record that gives one is earned (the records panel), the game takes it back out "
      "of the inventory at the next load.\n\n"
      "A change shows in the game's inventory screen the next time it opens.");
  if (!game::parts().bag) {
    ImGui::TextColored(kOrange, "The inventory was not found in this build - see the log.");
    return;
  }
  if (!v.known) {
    ImGui::TextDisabled("(the inventory cannot be read right now)");
    return;
  }
  int used = 0;
  for (int s = 0; s < v.size && s < v.slots; ++s)
    used += !game::is_blank(v.bag[s].item) || v.bag[s].dead || v.bag[s].force_blank;
  ImGui::Text("%d of %d slots used", used, v.size);
  if (!v.open) {
    ImGui::SameLine();
    ImGui::TextDisabled("(pause the game to change it)");
  }
  // A weapon is put in and taken out by the game's own inventory functions, and
  // it is built in an empty item box entry (inventory.cpp): without either, the
  // picker still shows the weapons but will not take one.
  const bool weapons_ok = v.calls && v.box_known && v.box_blank > 0;
  const char* weapons_why = !v.calls          ? v.calls_status
                            : !v.box_known    ? "the item box cannot be read right now, and a weapon is made in it"
                                              : "the item box has no empty entry, and a weapon is made in one";
  ImGui::BeginDisabled(!v.open);
  if (ImGui::BeginTable("bag", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("");
    for (int s = 0; s < v.size && s < v.slots && s < inventory::kBagMax; ++s) {
      const inventory::Entry& e = v.bag[s];
      const bool weapon = game::is_weapon(e.item);
      if (!g_edit_dirty[s]) {
        g_edit_id[s] = weapon ? 0 : e.item.item_id;
        g_edit_wid[s] = weapon ? e.item.weapon_id : -1;
        g_edit_cnt[s] = e.item.count;
      }
      ImGui::PushID(s);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%d", s + 1);
      ImGui::TableNextColumn();
      if (e.dead && !game::is_blank(e.item)) {
        // An item under the two-slot item to its left: nothing belongs here, so Store is offered to get it out.
        ImGui::TextColored(kOrange, "%s (under slot %d)", inventory::item_name(e.item, nb, sizeof(nb)), s);
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          char left[96];
          ImGui::PushTextWrapPos(420.0f);
          ImGui::Text("Slot %d is the second half of %s in slot %d, yet it holds this item as well. Store puts it in "
                      "the item box.", s + 1, inventory::item_name(v.bag[s - 1].item, left, sizeof(left)), s);
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
      } else if (e.dead) {
        ImGui::TextDisabled("(second half of a two-slot item)");
      } else if (e.force_blank) {
        ImGui::TextDisabled("(kept blank by the game)");
      } else {
        game::Item shown;
        shown.item_id = g_edit_id[s];
        shown.weapon_id = g_edit_wid[s];
        ImGui::SetNextItemWidth(270.0f);
        if (ImGui::BeginCombo("##item", inventory::item_name(shown, nb, sizeof(nb)), ImGuiComboFlags_HeightLarge)) {
          static char filter[48] = {};
          if (ImGui::IsWindowAppearing()) {
            filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
          }
          ImGui::SetNextItemWidth(-FLT_MIN);
          const bool enter = ImGui::InputTextWithHint("##filter", "type to narrow the list", filter, sizeof(filter),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
          ImGui::Separator();
          auto pick = [&](int id) {
            if (id == g_edit_id[s] && g_edit_wid[s] < 0) return;
            g_edit_id[s] = id;
            g_edit_wid[s] = -1;
            g_edit_cnt[s] = id ? (!weapon && id == e.item.item_id ? e.item.count : 1) : 0;
            g_edit_dirty[s] = true;
          };
          // A weapon starts at what the game says it holds full: a gun's
          // magazine, the grenades a stack of them is, a knife's durability.
          auto pick_weapon = [&](const items::WeaponDef& w) {
            if (w.id == g_edit_wid[s]) return;
            const int full = w.id > 0 && w.id < inventory::kWeaponIds ? v.weapon_full[w.id] : 0;
            g_edit_id[s] = 0;
            g_edit_wid[s] = w.id;
            g_edit_cnt[s] = weapon && w.id == e.item.weapon_id ? e.item.count : (full > 0 ? full : 1);
            g_edit_dirty[s] = true;
          };
          int first = -2;
          const items::WeaponDef* first_weapon = nullptr;
          if (!filter[0] && ImGui::Selectable("(empty)", g_edit_id[s] == 0 && g_edit_wid[s] < 0)) pick(0);
          for (const auto& d : items::kItems) {
            if (!d.listed || !matches(d, filter)) continue;
            if (first == -2) first = d.id;
            const bool sel = d.id == g_edit_id[s] && g_edit_wid[s] < 0;
            ImGui::PushID(d.id);  // labels are unique (gen_items.py), but the id is what the row is
            if (ImGui::Selectable(d.label, sel)) pick(d.id);
            ImGui::PopID();
            if (sel && !filter[0]) ImGui::SetItemDefaultFocus();
          }
          // The weapons - the grenades and knives among them - which the game
          // itself has to add and take away, so they are offered only while it can.
          bool headed = false;
          for (const auto& w : items::kWeapons) {
            if (!w.listed || !matches_weapon(w, filter)) continue;
            if (!headed) {
              headed = true;
              ImGui::SeparatorText("Weapons");
              if (!weapons_ok) ImGui::TextDisabled("%s", weapons_why);
            }
            if (!first_weapon) first_weapon = &w;
            const bool sel = w.id == g_edit_wid[s];
            ImGui::PushID(kWeaponPickerId + w.id);  // apart from the items' ids
            ImGui::BeginDisabled(!weapons_ok && !sel);
            if (ImGui::Selectable(w.name, sel)) pick_weapon(w);
            ImGui::EndDisabled();
            ImGui::PopID();
            if (sel && !filter[0]) ImGui::SetItemDefaultFocus();
          }
          if (first == -2 && !first_weapon && filter[0]) ImGui::TextDisabled("nothing matches \"%s\"", filter);
          if (enter && (first >= 0 || (first_weapon && weapons_ok))) {
            if (first >= 0) pick(first);
            else pick_weapon(*first_weapon);
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndCombo();
        }
      }
      ImGui::TableNextColumn();
      const items::WeaponDef* picked = weapon_def(g_edit_wid[s]);
      if (e.dead && !game::is_blank(e.item)) {
        ImGui::Text("%d", e.item.count);
      } else if (e.dead || e.force_blank || (!picked && g_edit_id[s] == 0)) {
        ImGui::TextDisabled("-");
      } else {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt("##cnt", &g_edit_cnt[s])) {
          g_edit_cnt[s] = std::clamp(g_edit_cnt[s], picked ? 0 : 1, 9999);
          g_edit_dirty[s] = true;
        }
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          if (picked && picked->sub) ImGui::TextUnformatted("How many of them (a knife: how much it has left)");
          else if (picked) ImGui::TextUnformatted("Rounds loaded");
          else ImGui::TextUnformatted("How many are in the slot");
          ImGui::EndTooltip();
        }
      }
      ImGui::TableNextColumn();
      if (g_edit_dirty[s]) {
        if (ImGui::SmallButton("Apply")) {
          g_ask_slot = s;
          ask_apply = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) g_edit_dirty[s] = false;
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          ImGui::TextUnformatted("Throw the edit away");
          ImGui::EndTooltip();
        }
      }
      ImGui::TableNextColumn();
      if (!e.force_blank && !game::is_blank(e.item)) {
        const int room = v.stack_room[s];  // on the box's stacks of this item
        const bool can = v.box_known && (v.box_blank > 0 || room > 0) && (!weapon || v.calls);
        ImGui::BeginDisabled(!can);
        if (ImGui::SmallButton("Store")) inventory::request_store(s, e);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
          ImGui::PushTextWrapPos(420.0f);
          const char* name = inventory::item_name(e.item, nb, sizeof(nb));
          if (!v.box_known) ImGui::TextUnformatted("The item box cannot be read right now");
          else if (v.box_blank <= 0 && room <= 0) ImGui::TextUnformatted("The item box is full");
          else if (weapon && !v.calls) ImGui::Text("Weapons need the game's own inventory functions: %s", v.calls_status);
          else if (room >= e.item.count) ImGui::Text("Put %s x%d on the item box's stacks of it", name, e.item.count);
          else if (room > 0 && v.box_blank > 0)
            ImGui::Text("Put %s x%d in the item box: %d on its stacks of it, %d in a new stack", name, e.item.count, room,
                        e.item.count - room);
          else if (room > 0)
            ImGui::Text("Put %d of %s x%d on the item box's stacks of it - the box has no room for the other %d", room,
                        name, e.item.count, e.item.count - room);
          else ImGui::Text("Put %s x%d in the item box", name, e.item.count);
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();

  if (ask_apply) ImGui::OpenPopup("apply this change?");
  if (ImGui::BeginPopupModal("apply this change?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const int as = g_ask_slot;
    if (as < 0 || as >= v.size || !g_edit_dirty[as] || !v.open) {
      g_ask_slot = -1;
      ImGui::CloseCurrentPopup();
    } else {
      const inventory::Entry& live = v.bag[as];
      game::Item want;
      want.item_id = g_edit_id[as];
      want.weapon_id = g_edit_wid[as];
      want.count = g_edit_cnt[as];
      // The weapon already in the slot keeps its parts and its ammo: only the
      // rounds change, and the tick writes them into the stock the game made.
      if (game::is_weapon(want) && game::is_weapon(live.item) && want.weapon_id == live.item.weapon_id) {
        const int count = want.count;
        want = live.item;
        want.count = count;
      }
      char was[96];
      std::snprintf(was, sizeof(was), "%s", inventory::item_name(live.item, nb, sizeof(nb)));
      if (want.item_id <= 0 && !game::is_weapon(want)) ImGui::Text("Empty slot %d (%s x%d)?", as + 1, was, live.item.count);
      else ImGui::Text("Set slot %d to %s x%d?", as + 1, inventory::item_name(want, nb, sizeof(nb)), want.count);
      const bool by_game = (game::is_weapon(want) || game::is_weapon(live.item)) &&
                           !(game::is_weapon(want) && game::is_weapon(live.item) && want.weapon_id == live.item.weapon_id);
      ImGui::TextDisabled(by_game ? "The game's own inventory functions do this, as they do at an item box."
                                  : "This writes straight into the game's inventory.");
      if (ImGui::Button("Apply")) {
        inventory::request_set(as, want, live);
        g_edit_dirty[as] = false;
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_ask_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// The item box list takes the height the screen has left below it - most of
// the screen with the cheats folded away - with 12 rows at the least (the
// panel scrolls then) and no more than it has. Measured from where the list
// starts with the panel unscrolled, so scrolling the panel does not grow it.
float box_rows(int shown) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float row = ImGui::GetTextLineHeightWithSpacing();
  const float top = ImGui::GetCursorScreenPos().y + ImGui::GetScrollY();
  const float after = row * 3.0f + style.WindowPadding.y + style.DisplaySafeAreaPadding.y;  // a note, the footer
  const float fit = std::floor((ImGui::GetIO().DisplaySize.y - top - after) / row) - 0.5f;
  return std::min(static_cast<float>(shown) + 0.5f, std::max(12.5f, fit));
}

void draw_item_box(const inventory::View& v) {
  char nb[96];
  bool ask_drop = false;
  ImGui::SeparatorText("Item box");
  help_marker(
      "The game's own item box - the one every item box in the game opens, saved with your game. Take puts an item "
      "into the first free slot of the inventory (an item that takes two slots into the first two free slots side by "
      "side), x throws it away, and Store beside an inventory slot puts one in.\n\n"
      "Anything moves, weapons included: Take and Store run the game's own inventory functions, so a weapon is armed "
      "or put away, and unequipped first, just as at a real item box. Store stacks like the game's box: ammo and "
      "the other items the game stacks top up the stacks the box already holds, up to the game's own maximum (60 "
      "handgun rounds a stack), and the rest becomes a new stack. Take moves a stack whole.\n\n"
      "The list is filed under five banks and sorted by name; # is the entry's place in the game's box.");
  if (!game::parts().box) {
    ImGui::TextColored(kOrange, "The item box was not found in this build - see the log.");
    return;
  }
  if (!v.box_known) {
    ImGui::TextDisabled("(the item box cannot be read right now)");
    return;
  }
  static int order[inventory::kBoxMax];
  int n = 0;
  for (int i = 0; i < v.box_count; ++i)
    if (v.box[i].prim && !game::is_blank(v.box[i].item)) order[n++] = i;
  std::sort(order, order + n, [&](int a, int b) {
    const game::Item &x = v.box[a].item, &y = v.box[b].item;
    const int bx = bank_of(x), by = bank_of(y);
    if (bx != by) return bx < by;
    char na[96], nb2[96];
    const int c = std::strcmp(inventory::item_name(x, na, sizeof(na)), inventory::item_name(y, nb2, sizeof(nb2)));
    if (c) return c < 0;
    if (x.count != y.count) return x.count > y.count;
    return a < b;
  });
  ImGui::Text("%d item%s", n, n == 1 ? "" : "s");
  static int shown_bank = items::kBankKey;
  static char filter[48] = {};
  int held[items::kBankCount] = {}, matched[items::kBankCount] = {};
  auto hits = [&](const game::Item& it) {
    if (!filter[0]) return true;
    if (game::is_weapon(it)) return std::strstr(inventory::item_name(it, nb, sizeof(nb)), filter) != nullptr;
    const items::Def* d = item_def(it.item_id);
    return d && matches(*d, filter);
  };
  for (int k = 0; k < n; ++k) {
    const game::Item& it = v.box[order[k]].item;
    ++held[bank_of(it)];
    if (hits(it)) ++matched[bank_of(it)];
  }
  if (ImGui::BeginTabBar("banks")) {
    for (int k = 0; k < items::kBankCount; ++k) {
      char tab[48];
      std::snprintf(tab, sizeof(tab), "%s %d###bank%d", bank_name(k), held[k], k);
      if (ImGui::BeginTabItem(tab)) {
        shown_bank = k;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  const int bank = shown_bank;
  if (n > 0) {
    ImGui::SetNextItemWidth(230.0f);
    ImGui::InputTextWithHint("##box filter", "type to narrow the list", filter, sizeof(filter));
    if (filter[0]) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) filter[0] = '\0';
      ImGui::SameLine();
      ImGui::TextDisabled("%d of %d shown", matched[bank], held[bank]);
    }
  }
  char table_id[16];
  std::snprintf(table_id, sizeof(table_id), "box%d", bank);
  const int shown = matched[bank];
  if (n == 0) {
    ImGui::TextDisabled("The item box is empty.");
  } else if (held[bank] == 0) {
    ImGui::TextDisabled("Nothing in the box is filed under %s.", bank_name(bank));
  } else if (shown == 0) {
    ImGui::TextDisabled("nothing under %s matches \"%s\"", bank_name(bank), filter);
  } else if (ImGui::BeginTable(table_id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                               ImVec2(0.0f, box_rows(shown) * ImGui::GetTextLineHeightWithSpacing()))) {
    ImGui::TableSetupColumn("item");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("");
    for (int k = 0; k < n; ++k) {
      const int i = order[k];
      const inventory::Entry& e = v.box[i];
      if (bank_of(e.item) != bank || !hits(e.item)) continue;
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(inventory::item_name(e.item, nb, sizeof(nb)));
      ImGui::TableNextColumn();
      ImGui::Text("%d", e.item.count);
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%d", i + 1);
      ImGui::TableNextColumn();
      const bool weapon = game::is_weapon(e.item);
      const bool room = e.fat ? v.free_pairs > 0 : v.free_slots > 0;
      ImGui::BeginDisabled(!v.open || !room || (weapon && !v.calls));
      if (ImGui::SmallButton("Take")) inventory::request_take(i, e);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(420.0f);
        if (!v.open) ImGui::TextUnformatted("Pause the game to take it");
        else if (weapon && !v.calls) ImGui::Text("Weapons need the game's own inventory functions: %s", v.calls_status);
        else if (!room && e.fat) ImGui::TextUnformatted("It takes two slots side by side, and the inventory has no such room");
        else if (!room) ImGui::TextUnformatted("The inventory has no free slot");
        else if (e.fat) ImGui::Text("Put %s into the first two free slots side by side", inventory::item_name(e.item, nb, sizeof(nb)));
        else ImGui::Text("Put %s x%d into the first free slot", inventory::item_name(e.item, nb, sizeof(nb)), e.item.count);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!v.open);
      if (ImGui::SmallButton("x")) {
        g_drop_index = i;
        g_drop_seen = e;
        ask_drop = true;
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("Throw this item away");
        ImGui::EndTooltip();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (ask_drop) ImGui::OpenPopup("throw item away?");
  if (ImGui::BeginPopupModal("throw item away?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_drop_index < 0 || !v.open) {
      g_drop_index = -1;
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::Text("Throw away %s x%d from the item box?", inventory::item_name(g_drop_seen.item, nb, sizeof(nb)),
                  g_drop_seen.item.count);
      ImGui::TextDisabled("It is gone for good once the game is saved.");
      if (ImGui::Button("Throw away")) {
        inventory::request_drop(g_drop_index, g_drop_seen);
        g_drop_index = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_drop_index = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// --- the records panel ----------------------------------------------------------------------------------------
// Beside the game's records screens (records.h): every record of the screen's set
// with a switch, what it gives, and what switching it does. A click is a request
// the next tick carries out; until the records read back that way the row shows
// what was asked (pending).
char g_records_filter[64] = {};
bool g_records_hidden = true;  // the main game's hidden records (not on the game's screen)
struct Pending {
  int set = -1, id = -1;
  bool on = false;
  DWORD at = 0;
};
Pending g_records_pending;

void condition_text(const records::Row& r, int special_limit, char* out, size_t n) {
  const char* c = r.condition ? r.condition : "";
  const char* at = std::strstr(c, "{0}");
  if (!at) {
    std::snprintf(out, n, "%s", c);
    return;
  }
  const int v = r.kind == 2 && special_limit > 0 ? special_limit : r.goal;  // "Gunslinger" counts shots, not its goal
  std::snprintf(out, n, "%.*s%d%s", static_cast<int>(at - c), c, v, at + 3);
}

void record_name(const records::Row& r, char* out, size_t n) {
  if (r.name) {
    std::snprintf(out, n, "%s", r.name);
    return;
  }
  size_t used = static_cast<size_t>(std::snprintf(out, n, "Hidden record"));
  for (int k = 0; k < r.n_rewards && used < n; ++k)
    used += static_cast<size_t>(std::snprintf(out + used, n - used, "%s%s", k ? ", " : ": ",
                                              r.reward[k].name ? r.reward[k].name : "?"));
}

bool record_matches(const records::Row& r, int special_limit, const char* needle) {
  if (!needle || !needle[0]) return true;
  char hay[640], name[160], cond[256];
  record_name(r, name, sizeof(name));
  condition_text(r, special_limit, cond, sizeof(cond));
  size_t used = static_cast<size_t>(std::snprintf(hay, sizeof(hay), "%s %s", name, cond));
  for (int k = 0; k < r.n_rewards && used < sizeof(hay); ++k)
    used += static_cast<size_t>(
        std::snprintf(hay + used, sizeof(hay) - used, " %s", r.reward[k].name ? r.reward[k].name : ""));
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (needle[i] && lower(p[i]) == lower(needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return false;
}

void draw_records(records::Set set) {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.50f, io.DisplaySize.y * 0.05f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.47f, io.DisplaySize.y * 0.86f), ImGuiCond_FirstUseEver);
  const bool rogue = set == records::kRogue;
  // Collapsible (its title bar's arrow, or a double-click on it), to see the
  // game's screen behind it; the "###" id keeps where it was left across versions.
  if (!ImGui::Begin(rogue ? "Records: The Ghost Survivors - Cabby Codes  v" RE2CC_VERSION "###records_rogue"
                          : "Records - Cabby Codes  v" RE2CC_VERSION "###records")) {
    ImGui::End();
    return;
  }
  const records::View* v = new records::View(records::view(set));
  const bool usable = records::ready(set) && v->known;
  if (!records::ready(set)) {
    ImGui::TextColored(kOrange, "%s", records::status(set));
  } else if (!v->known) {
    ImGui::TextDisabled("%s", v->status[0] ? v->status : "Reading the records...");
  } else {
    int hidden = 0, hidden_cleared = 0, achievements = 0;
    for (int i = 0; i < v->count; ++i) {
      hidden += v->row[i].hidden;
      hidden_cleared += v->row[i].hidden && v->row[i].cleared;
      achievements += v->row[i].achievement && !v->row[i].cleared;
    }
    ImGui::Text("%d of %d records complete", v->cleared, v->listed);
    if (hidden) {
      ImGui::SameLine();
      ImGui::TextDisabled("(and %d of %d hidden ones)", hidden_cleared, hidden);
    }
    help_marker(rogue
                    ? "Switch a record on and it is complete, as if you had earned it: the game's records screen shows "
                      "it (move its cursor to see a change), its accessory unlocks, and what earns it is written into "
                      "your save the way the game writes it - a scenario cleared, a play count. \"Hell of a Sheriff\" "
                      "and \"Got 'Em\" unlock their Steam achievements.\n\n"
                      "Switch one off and it is incomplete again: its accessory is locked (and taken off whoever wears "
                      "it), and what earned it is taken back so you can earn it again - one Mr. Raccoon comes back, the "
                      "play count drops just short, the special condition is forgotten. Nothing is locked away: No Way "
                      "Out stays open. It opens once Forgotten Soldier, Runaway and No Time To Mourn are all cleared, "
                      "so while it is open, \"Mission Complete?\", \"Reunited\" and \"Getting Over It\" are held off by "
                      "the mod instead of un-clearing their scenario, and the no-training records leave their scenario "
                      "cleared as a training clear.\n\n"
                      "Every change is saved at once."
                    : "Switch a record on and it is complete, as if you had earned it: the game's records screen shows "
                      "it (move its cursor to see a change), its rewards unlock - game modes, costumes, infinite "
                      "weapons, concept art, figures - and its progress is set the way the game sets it. A record "
                      "with a Steam achievement unlocks it: Steam keeps those for good.\n\n"
                      "Switch one off and it is incomplete again: its progress drops just short of its goal and its "
                      "rewards are locked again, unless another complete record gives the same one. The menus and "
                      "the costume, figure and concept art screens follow the next time they open; the infinite "
                      "weapons leave your item box and inventory the next time a game is loaded (and come into the "
                      "item box when switched on). A costume you are wearing stays on until you change it.\n\n"
                      "Every change is saved at once.");
    ImGui::BeginDisabled(!usable);
    if (ImGui::SmallButton("Complete all")) g_records_ask_all = 1, g_records_ask_set = set;
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear all")) g_records_ask_all = 0, g_records_ask_set = set;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputTextWithHint("##records_filter", "filter", g_records_filter, sizeof(g_records_filter));
    if (!rogue && hidden) {
      ImGui::SameLine();
      ImGui::Checkbox("Hidden records", &g_records_hidden);
      if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted("Records the game keeps off its records screen - they give the infinite ATM-4 and "
                               "Minigun, and The Tofu Survivor's other characters.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
    if (v->all_rewards)
      ImGui::TextColored(kOrange, "The all-rewards unlock is active: every reward shows unlocked whatever the records say.");
    if (!v->achievements)
      ImGui::TextColored(kOrange, "The game's achievement calls were not found - records switched on unlock none.");

    // The confirmations for all on / all off.
    if (g_records_ask_all >= 0 && g_records_ask_set != static_cast<int>(set)) g_records_ask_all = -1;
    if (g_records_ask_all >= 0) ImGui::OpenPopup("records: all");
    if (ImGui::BeginPopupModal("records: all", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
      if (g_records_ask_all == 1) {
        ImGui::Text("Complete every record%s?", rogue ? " of The Ghost Survivors" : ", hidden ones too");
        if (!v->achievements_known)
          ImGui::TextColored(kOrange, "Every record that carries a Steam achievement unlocks it - Steam keeps them for good.");
        else if (achievements)
          ImGui::TextColored(kOrange, "%d Steam achievement(s) will be unlocked - Steam keeps them for good.", achievements);
      } else {
        ImGui::Text("Clear every record%s?", rogue ? " of The Ghost Survivors" : ", hidden ones too");
        ImGui::TextUnformatted(rogue ? "Their accessories are locked. While No Way Out is open, the three records "
                                       "for clearing its scenarios are held off instead, so it stays open."
                                     : "Their rewards are locked again: game modes, costumes, figures, concept art, "
                                       "and the infinite weapons at the next load.");
      }
      ImGui::PopTextWrapPos();
      if (ImGui::Button(g_records_ask_all == 1 ? "Complete all" : "Clear all")) {
        records::request_all(set, g_records_ask_all == 1);
        logf("records: %s asked for (panel)", g_records_ask_all == 1 ? "complete all" : "clear all");
        g_records_ask_all = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        g_records_ask_all = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    if (ImGui::BeginTable("records", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, -footer))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthStretch, 0.52f);
      ImGui::TableSetupColumn(rogue ? "Accessory" : "Rewards", ImGuiTableColumnFlags_WidthStretch, 0.36f);
      ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 0.12f);
      ImGui::TableHeadersRow();
      const DWORD now = GetTickCount();
      for (int i = 0; i < v->count; ++i) {
        const records::Row& r = v->row[i];
        if (r.hidden && !g_records_hidden) continue;
        if (!record_matches(r, v->special_limit, g_records_filter)) continue;
        char name[160], cond[256];
        record_name(r, name, sizeof(name));
        condition_text(r, v->special_limit, cond, sizeof(cond));
        ImGui::TableNextRow();
        ImGui::PushID(r.id);
        ImGui::TableNextColumn();
        bool on = r.cleared;
        const bool pending = g_records_pending.set == set && g_records_pending.id == r.id &&
                             g_records_pending.on != r.cleared && now - g_records_pending.at < 1500;
        if (pending) on = g_records_pending.on;
        ImGui::BeginDisabled(!usable);
        if (ImGui::Checkbox("##on", &on)) {
          records::request(set, r.id, on);
          g_records_pending = Pending{static_cast<int>(set), r.id, on, now};
          logf("records: %s \"%s\" %s (panel)", rogue ? "Ghost Survivors" : "main", name, on ? "on" : "off");
        }
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(name);
        if (r.fresh && r.cleared) {
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "NEW");
        }
        if (r.achievement) {
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "[achievement]");
        }
        if (r.held) {
          ImGui::SameLine();
          ImGui::TextColored(kOrange, "(held off)");
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", cond);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        // What switching it does beyond the obvious, over the name and the condition.
        if ((r.note || r.achievement) && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
          ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
          if (r.achievement) ImGui::TextUnformatted("Switching this on unlocks its Steam achievement (for good).");
          if (r.note) ImGui::TextUnformatted(r.note);
          ImGui::PopTextWrapPos();
          ImGui::EndTooltip();
        }
        ImGui::TableNextColumn();
        for (int k = 0; k < r.n_rewards; ++k) {
          const records::Reward& w = r.reward[k];
          char line[160];
          if (rogue) std::snprintf(line, sizeof(line), "%s", w.name ? w.name : "?");
          else std::snprintf(line, sizeof(line), "%s: %s", w.type >= 0 ? records::reward_kind(w.type) : "Reward",
                             w.name ? w.name : "?");
          ImGui::PushTextWrapPos(0.0f);
          if (w.open) ImGui::TextUnformatted(line);
          else ImGui::TextDisabled("%s", line);
          ImGui::PopTextWrapPos();
          if (w.shared && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            ImGui::TextUnformatted("Another complete record gives this too: it stays unlocked while that one does.");
            ImGui::EndTooltip();
          }
        }
        ImGui::TableNextColumn();
        if (r.progress >= 0 && r.goal > 0) {
          if (rogue || r.kind == 0) {
            if (r.goal > 1) ImGui::Text("%d / %d", r.progress, r.goal);
          } else if (r.kind == 1) {
            if (r.progress > 0) ImGui::Text("%d (%d or less)", r.progress, r.goal);
            else ImGui::TextDisabled("- (%d or less)", r.goal);
          }
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  if (v->status[0]) {
    if (v->status_error) ImGui::TextColored(kOrange, "%s", v->status);
    else ImGui::TextWrapped("%s", v->status);
  }
  if (v->saving) ImGui::TextDisabled("Saving...");
  ImGui::TextDisabled("The game's list shows a change once you move its cursor.  F%d hides this panel.",
                      config::get().toggle_key - 0x6F);
  delete v;
  ImGui::End();
}

// --- the save files ------------------------------------------------------------------------------------
// "Auto-save", "Slot 3": a row of the game's list by its slot.
const char* slot_label(const savefiles::Row& r, char* out, size_t n) {
  if (r.slot == 0) std::snprintf(out, n, "Auto-save");
  else if (r.slot > 0) std::snprintf(out, n, "Slot %d", r.slot);
  else std::snprintf(out, n, "?");
  return out;
}

// One save in a line, from the game's own strings for its row - the scenario,
// its difficulty, where it was made (SaveDataManager.getStringFromDetailString:
// scenario, difficulty, saves, location, area, ...) - and when: "Leon A ·
// Standard · Police Station - Main Hall · 2026-09-19 14:48", or "No data".
void describe_save(const savefiles::Row& r, char* out, size_t n) {
  if (!r.used) {
    std::snprintf(out, n, "No data");
    return;
  }
  size_t k = 0;
  auto add = [&](const char* sep, const char* text) {
    if (!text || !text[0] || k + 1 >= n) return;
    k += static_cast<size_t>(std::snprintf(out + k, n - k, "%s%s", k ? sep : "", text));
    if (k >= n) k = n - 1;
  };
  out[0] = '\0';
  add("", r.n_texts > 0 && r.texts[0][0] ? r.texts[0] : r.subtitle);
  if (r.n_texts > 1) add(" \xC2\xB7 ", r.texts[1]);
  if (r.n_texts > 3) add(" \xC2\xB7 ", r.texts[3]);
  if (r.n_texts > 4) add(" - ", r.texts[4]);
  if (r.unix_time > 0) {
    const time_t t = static_cast<time_t>(r.unix_time);
    tm local{};
    char when[32];
    if (localtime_s(&local, &t) == 0 && std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local))
      add(" \xC2\xB7 ", when);
  }
  if (!out[0]) std::snprintf(out, n, "a save");
}

// Every string the game's row has for the save, for the tooltip.
void save_tooltip(const savefiles::Row& r) {
  if (!r.used || !ImGui::IsItemHovered() || !ImGui::BeginTooltip()) return;
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
  for (int i = 0; i < r.n_texts; ++i)
    if (r.texts[i][0]) ImGui::TextUnformatted(r.texts[i]);
  if (r.size > 0) ImGui::TextDisabled("%lld KB", (r.size + 1023) / 1024);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

// The Load Game screen's slots - the auto-save and slots 1-20, as the game lists
// them - with a Delete and a Copy to... for each save. Both change the saves the
// moment they are confirmed, so both ask first, and the request carries what was
// seen, so a slot that changed in between is left alone (savefiles.cpp).
void draw_save_files() {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.60f, io.DisplaySize.y * 0.06f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.38f, io.DisplaySize.y * 0.80f), ImGuiCond_FirstUseEver);
  // Collapsible (its title bar's arrow), to see the game's list behind it; the
  // "###" id keeps where it was left across versions.
  if (!ImGui::Begin("Save files - Cabby Codes  v" RE2CC_VERSION "###savefiles")) {
    ImGui::End();
    return;
  }
  const auto* v = new savefiles::View(savefiles::view());
  const bool hooked = events::load_screen_hooked();
  const bool can = v->ready && v->up && hooked && !v->busy && v->consistent;
  bool open_delete = false, open_copy = false;
  char label[32], d[256];

  int used = 0;
  for (int i = 0; i < v->count; ++i) used += v->row[i].used;
  ImGui::Text("%d save%s in %d slots", used, used == 1 ? "" : "s", v->count);
  help_marker(
      "The game's own save slots, as its Load Game list shows them: the auto-save and slots 1-20.\n\n"
      "Delete removes a save: the game deletes it the way it deletes anything from its saves, and the slot reads "
      "No Data from then on - your next save can go there. Copy to... puts a copy of a save into another slot, empty "
      "or not (a save already there is replaced). The copy is written where the game keeps its saves, in Steam Cloud, "
      "and loads like the original.\n\n"
      "Both change your saves straight away, so neither can be undone - back up your saves first if in doubt "
      "(Steam/userdata/<your id>/883710/remote/win64_save/). The game's list beside this one is read again after each "
      "change; its cursor goes back to the last save you loaded.");
  // (Why nothing can change when it cannot is the status line, below.)
  if (v->ready && !hooked) ImGui::TextColored(kOrange, "The Load Game screen is not heard yet (see the log) - nothing can change.");
  else if (v->ready && v->count > 0 && !v->consistent)
    ImGui::TextColored(kOrange, "The game's list is not laid out as expected (see the log) - nothing can change.");
  else if (!v->copy_ok) ImGui::TextColored(kOrange, "Steam's cloud storage was not found - saves can be deleted, not copied.");

  const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
  if (v->count > 0 &&
      ImGui::BeginTable("saves", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0.0f, -footer))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Save", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int i = 0; i < v->count; ++i) {
      const savefiles::Row& r = v->row[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(slot_label(r, label, sizeof(label)));
      ImGui::TableNextColumn();
      describe_save(r, d, sizeof(d));
      ImGui::PushTextWrapPos(0.0f);
      if (r.used) ImGui::TextUnformatted(d);
      else ImGui::TextDisabled("%s", d);
      ImGui::PopTextWrapPos();
      save_tooltip(r);
      ImGui::TableNextColumn();
      if (r.used) {
        ImGui::BeginDisabled(!can || !v->copy_ok);
        if (ImGui::SmallButton("Copy to...")) ImGui::OpenPopup("copy to");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!can);
        if (ImGui::SmallButton("Delete")) {
          g_save_ask = kAskDelete;
          g_save_to = i;
          g_save_to_seen = r;
          open_delete = true;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("copy to")) {
          ImGui::TextDisabled("Copy %s to:", slot_label(r, label, sizeof(label)));
          ImGui::Separator();
          for (int j = 0; j < v->count; ++j) {
            if (j == i) continue;
            char dj[256], lj[32], item[320];
            describe_save(v->row[j], dj, sizeof(dj));
            std::snprintf(item, sizeof(item), "%s - %s", slot_label(v->row[j], lj, sizeof(lj)), dj);
            if (ImGui::Selectable(item)) {
              g_save_ask = kAskCopy;
              g_save_from = i;
              g_save_to = j;
              g_save_from_seen = r;
              g_save_to_seen = v->row[j];
              open_copy = true;
            }
          }
          ImGui::EndPopup();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  } else if (v->ready && v->count == 0) {
    ImGui::TextDisabled("Reading the game's list...");
  }
  if (v->status[0]) {
    ImGui::PushTextWrapPos(0.0f);
    if (v->status_error) ImGui::TextColored(kOrange, "%s", v->status);
    else ImGui::TextUnformatted(v->status);
    ImGui::PopTextWrapPos();
  } else if (v->busy && v->ready) {
    ImGui::TextDisabled("The game is busy with its saves...");
  }
  ImGui::TextDisabled("Changes cannot be undone.  F%d hides this panel.", config::get().toggle_key - 0x6F);

  // Asked outside the table, so neither dialog belongs to a row.
  if (open_delete) ImGui::OpenPopup("delete this save?");
  if (open_copy) ImGui::OpenPopup("copy this save?");
  const auto row_ok = [&](int i) { return i >= 0 && i < v->count; };
  if (ImGui::BeginPopupModal("delete this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_save_ask != kAskDelete || !row_ok(g_save_to) || !can) {
      forget_save_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      describe_save(g_save_to_seen, d, sizeof(d));
      ImGui::Text("Delete the save in %s?", slot_label(g_save_to_seen, label, sizeof(label)));
      ImGui::TextUnformatted(d);
      ImGui::TextDisabled("It is removed from your saves (and Steam Cloud) straight away, so this cannot be undone.");
      if (ImGui::Button("Delete")) {
        savefiles::request_delete(g_save_to, g_save_to_seen);
        logf("save files: delete %s asked for (panel)", label);
        forget_save_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_save_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("copy this save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (g_save_ask != kAskCopy || !row_ok(g_save_from) || !row_ok(g_save_to) || !can) {
      forget_save_dialogs();
      ImGui::CloseCurrentPopup();
    } else {
      char to[32];
      slot_label(g_save_from_seen, label, sizeof(label));
      slot_label(g_save_to_seen, to, sizeof(to));
      describe_save(g_save_from_seen, d, sizeof(d));
      if (g_save_to_seen.used) ImGui::Text("Copy %s over %s?", label, to);
      else ImGui::Text("Copy %s to the empty %s?", label, to);
      ImGui::TextUnformatted(d);
      if (g_save_to_seen.used) {
        describe_save(g_save_to_seen, d, sizeof(d));
        ImGui::TextColored(kOrange, "The save in %s is replaced: %s", to, d);
      }
      ImGui::TextDisabled("The copy is written to your saves (and Steam Cloud) straight away, so this cannot be undone.");
      if (ImGui::Button("Copy")) {
        savefiles::request_copy(g_save_from, g_save_to, g_save_from_seen, g_save_to_seen);
        logf("save files: copy %s to %s asked for (panel)", label, to);
        forget_save_dialogs();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        forget_save_dialogs();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  delete v;
  ImGui::End();
}

}  // namespace

// --- context --------------------------------------------------------------------------------------------
bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sRE2CabbyCodes.imgui.ini", config::dir());
  io.IniFilename = ini_path;
  // The panel's own line in that file: whether the cheats are folded away.
  // Registered before the first frame, which is when ImGui reads the file.
  ImGuiSettingsHandler panel{};
  panel.TypeName = "RE2CabbyCodes";
  panel.TypeHash = ImHashStr("RE2CabbyCodes");
  panel.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
    return std::strcmp(name, "Panel") == 0 ? &g_cheats_open : nullptr;
  };
  panel.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    int v = 0;
    if (std::sscanf(line, "CheatsOpen=%d", &v) == 1) g_cheats_open = v != 0;
  };
  panel.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out) {
    out->appendf("[%s][Panel]\nCheatsOpen=%d\n\n", h->TypeName, g_cheats_open ? 1 : 0);
  };
  ImGui::AddSettingsHandler(&panel);
  // No keyboard navigation: the game's menus keep their keys. Dragging is
  // limited to the title bar so a click on the panel's background cannot pick
  // the window up by accident. The render thread must not set the window's
  // cursor (it belongs to the window's thread): WM_SETCURSOR does that.
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  // The game runs at anything from 720p to 4K: scale the panel with the window.
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const float scale = std::clamp(static_cast<float>(rc.bottom - rc.top) / 900.0f, 1.0f, 2.5f);
  style.ScaleAllSizes(scale);
  style.FontScaleMain = scale;
  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_wake_msg = RegisterWindowMessageA("RE2CabbyCodes.PanelVisibility");
  g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_context_ready = true;
  keyboard::start(hwnd);
  char cls[64] = "?";
  GetClassNameA(hwnd, cls, sizeof(cls));
  logf("overlay ready (hwnd=%p class '%s' %ldx%ld, scale %.2f, render thread %lu, window thread %lu)",
       static_cast<void*>(hwnd), cls, rc.right - rc.left, rc.bottom - rc.top, scale, GetCurrentThreadId(),
       GetWindowThreadProcessId(hwnd, nullptr));
  return true;
}

bool capturing_mouse() { return g_capture_mouse != 0; }
bool capturing_keyboard() { return g_capture_keyboard != 0; }
bool visible() { return g_visible != 0; }

void lock_imgui() {
  if (g_imgui_cs_ready) EnterCriticalSection(&g_imgui_cs);
}
void unlock_imgui() {
  if (g_imgui_cs_ready) LeaveCriticalSection(&g_imgui_cs);
}

bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool on_pause_menu = showable(s);
  const int records_screen = s.ticking && s.age_ms < 2000 ? s.records_screen : -1;
  const bool load_screen = s.ticking && s.age_ms < 2000 && s.load_screen;
  if (on_pause_menu != g_was_showable || records_screen != g_was_records || load_screen != g_was_load) {
    if (records_screen != g_was_records) g_records_ask_all = -1;  // a confirmation belongs to the screen it was asked on
    if (load_screen != g_was_load) forget_save_dialogs();
    g_was_showable = on_pause_menu;
    g_was_records = records_screen;
    g_was_load = load_screen;
    g_user_hidden = false;  // the next time the pause menu (or a records screen) opens the panel is back
    g_forced = false;
  }
  const bool focused = GetForegroundWindow() == g_hwnd;
  if (keyboard::active()) {
    // The panel's own keyboard: what was typed since the last frame goes to
    // the panel while it is up and the game has the focus, and the toggle key
    // is read from it (only while the game has the focus: the device hears
    // keys in the background too).
    const int presses = keyboard::pump(g_visible != 0 && focused, config::get().toggle_key);
    for (int i = 0; i < presses && focused; ++i) toggle(on_pause_menu, "");
  } else if (!g_legacy_keys && g_context_ready) {
    // No WM_KEYDOWN has reached the window (the game's DirectInput keyboard
    // taken exclusively?): the toggle key is read here instead.
    static bool was_down = false;
    const bool down = focused && (GetAsyncKeyState(config::get().toggle_key) & 0x8000) != 0;
    if (down && !was_down) {
      g_polled_toggle_at = GetTickCount();
      toggle(on_pause_menu, " (read from the keyboard)");
    }
    was_down = down;
  }
  const bool vis = g_context_ready && (on_pause_menu ? !g_user_hidden : g_forced);
  if (InterlockedExchange(&g_visible, vis ? 1 : 0) != (vis ? 1 : 0) && g_hwnd) {
    if (!vis) forget_dialogs();
    if (g_wake_msg) PostMessageW(g_hwnd, g_wake_msg, vis ? 1 : 0, 0);
  }
  if (!vis) {
    InterlockedExchange(&g_capture_mouse, 0);
    InterlockedExchange(&g_capture_keyboard, 0);
  }
  ImGuiIO& io = ImGui::GetIO();
  io.MouseDrawCursor = vis && config::get().disable_cursor;
  if (vis) {
    // The physical buttons are the truth: a press or release the window never
    // got as a message (Raw Input can take them) is still the panel's.
    static const int kVk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b) {
      const bool down = focused && (GetAsyncKeyState(kVk[b]) & 0x8000) != 0;
      if (io.MouseDown[b] != down) io.AddMouseButtonEvent(b, down);
    }
  } else if (g_context_ready) {
    // Input made while the panel is away is not the panel's (RE0).
    io.ClearEventsQueue();
    io.ClearInputMouse();
    io.ClearInputKeys();
  }
  return vis;
}

void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const ImGuiIO& io = ImGui::GetIO();
  InterlockedExchange(&g_capture_mouse, io.WantCaptureMouse ? 1 : 0);
  InterlockedExchange(&g_capture_keyboard, io.WantTextInput ? 1 : 0);
  // The Load Game screen is up (the title's, the pause menu's, the game over
  // screen's): the save files instead of the cheats.
  if (s.load_screen && s.ticking && s.age_ms < 2000) {
    draw_save_files();
    return;
  }
  // A records screen is up (the title's Bonus menu, or the pause menu's Records):
  // its records instead of the cheats.
  if (s.records_screen >= 0 && s.ticking && s.age_ms < 2000) {
    draw_records(static_cast<records::Set>(s.records_screen));
    return;
  }
  const cheats::Status st = cheats::status();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.55f, io.DisplaySize.y * 0.06f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Resident Evil 2 - Cabby Codes  v" RE2CC_VERSION, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  if (!s.show_panel && !config::get().always_show) {
    ImGui::TextDisabled("Opened outside the pause menu - F%d closes it", config::get().toggle_key - 0x6F);
    ImGui::Separator();
  }
  if (!re::ready()) {
    ImGui::TextColored(kOrange, "Game hooks: %s", re::status());
    ImGui::End();
    return;
  }
  if (!s.ticking) {
    ImGui::TextColored(kOrange, "Game tick: %s", app::status());
    ImGui::End();
    return;
  }
  if (s.age_ms >= 2000) {
    ImGui::TextColored(kOrange, "Game tick: the game has not called the mod for %lu s", s.age_ms / 1000);
    ImGui::End();
    return;
  }
  if (!s.game_ready) {
    ImGui::TextColored(kOrange, "Game hooks: %s", game::status_text());
    ImGui::End();
    return;
  }
  if (!s.in_game) {
    ImGui::TextDisabled("Waiting for a game (start a new game or load a save)...");
    ImGui::Separator();
  }
  const game::Parts& p = game::parts();
  {
    int on = 0;
    for (int k = 0; k < cheats::kCount; ++k) on += cheats::enabled(static_cast<cheats::Kind>(k));
    char head[48];
    std::snprintf(head, sizeof(head), "Cheats  (%d on)###cheats", on);
    ImGui::SetNextItemOpen(g_cheats_open, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(head);
    if (open != g_cheats_open) {
      g_cheats_open = open;
      ImGui::MarkIniSettingsDirty();
    }
  }
  ImGui::BeginDisabled(!s.in_game);
  if (g_cheats_open) {
    ImGui::SeparatorText("Player");
    cheat_row(cheats::kGodMode, "God mode",
              "Every hit is healed as it lands, and poison is cured at once. Hits still knock you about, and an attack "
              "that kills outright - a blow worth more than all your health, or being eaten - still does.",
              p.players, "player not found");
    if (p.players && st.players > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%d / %d HP)", st.hp, st.max_hp);
    }
    cheat_row(cheats::kOneHitKills, "One hit kills",
              "Any hit that does damage kills: the enemy's health drops to the hit's damage, so the game's own code "
              "kills it - deaths and drops as usual. Mr. X cannot be killed: the first hit puts him down on his "
              "knees. A plant burns up on any hit that does damage - no glands to shoot, no fire to bring, and no "
              "knocking it down first. An enemy "
              "in a scripted moment (climbing in through a window) dies when the moment ends. Enemies the game holds "
              "at their last point of health (Mr. X on a ladder) are left to it, and a few special hits (G2 clinging "
              "on, some breakable parts) do their normal damage.",
              p.enemies, "enemy list not found");
    if (p.enemies) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%d enemies)", st.enemies);
    }
    cheat_row(cheats::kInfiniteAmmo, "Infinite ammo",
              "Firing uses no ammo: the gun in your hands keeps its loaded rounds, the flamethrower and the spark shot "
              "their charge. Pickups and reloads still work as usual; knives and grenades are left alone.",
              p.equipped, "inventory not found");
    cheat_row(cheats::kNoDurabilityLoss, "No durability loss",
              "Knives never wear down: the knife you hold is kept full through every hit, so it never breaks (a worn "
              "one is made full when this goes on). A knife used to break free of a grab still stays in the enemy "
              "until you pick it up. Knives are the only things in the game that wear.",
              p.bag && st.durability_ok, "knives not found");
    cheat_row(cheats::kInfiniteBoards, "Infinite wooden boards",
              "Boarding up a window takes no Wooden Boards: the window is boarded as usual and your boards stay in the "
              "inventory. You still need to carry boards to board a window. Nothing else is kept - other items are used "
              "up as usual.",
              p.boards, "use-item triggers not found");
    if (p.bag && st.boards >= 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%d board%s)", st.boards, st.boards == 1 ? "" : "s");
    }

    ImGui::SeparatorText("Saves & time");
    cheat_row(cheats::kSaveWithoutInk, "Save without ink ribbons",
              "Hardcore: typewriters save without asking for an ink ribbon - the typewriter goes straight to the save "
              "menu, as it does on Standard. If you carry ribbons, the save still uses one: the game's save menu takes "
              "it on Hardcore.",
              p.typewriter, "typewriters not understood");
    if (p.bag && st.ink >= 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%d ribbon%s)", st.ink, st.ink == 1 ? "" : "s");
    }
    cheat_row(cheats::kNoSaveCount, "Save without counting",
              "Saves add nothing to the save count the results screen shows and ranks you on: each save records the "
              "count it had when this went on (or the one set below).",
              p.header, "records not found");
    {
      static int edit_saves = -1;
      if (edit_saves < 0 && st.saves >= 0) edit_saves = st.saves;
      ImGui::BeginDisabled(!(p.records || p.header) || st.saves < 0);
      ImGui::Text("Saves: %d", st.saves);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.0f);
      ImGui::InputInt("##saves", &edit_saves);
      ImGui::SameLine();
      if (ImGui::SmallButton("Apply##saves")) cheats::request_saves(edit_saves < 0 ? 0 : edit_saves);
      ImGui::EndDisabled();
    }
    {
      char t[32];
      hms(st.play_seconds, t, sizeof(t));
      cheat_row(cheats::kFreezePlaytime, "Freeze play time",
                "The clear time the results screen ranks you on stops where it is: the time your saves and your "
                "clear record stops counting while this is on, in the main game and in The Ghost Survivors, The "
                "4th Survivor and The Tofu Survivor. The Ghost Survivors' run timer on screen stands still with "
                "it. The game's own clock keeps running underneath - the enemies' timers are on it, and stopping "
                "it leaves them standing about - so the pause menu's own play time keeps counting. The figure "
                "below is the one that will be recorded.",
                p.clock, "clock not found");
      static int eh = 0, em = 0, es = 0;
      ImGui::BeginDisabled(!p.clock || st.play_seconds < 0.0);
      ImGui::Text("Play time: %s%s", t, st.play_held ? " (held)" : "");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.0f);
      ImGui::InputInt("h", &eh, 0, 0);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.0f);
      ImGui::InputInt("m", &em, 0, 0);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.0f);
      ImGui::InputInt("s", &es, 0, 0);
      ImGui::SameLine();
      if (ImGui::SmallButton("Set##pt")) {
        eh = std::max(eh, 0);
        em = std::clamp(em, 0, 59);
        es = std::clamp(es, 0, 59);
        cheats::request_playtime_seconds(eh * 3600 + em * 60 + es);
      }
      ImGui::EndDisabled();
    }
    cheat_row(cheats::kFreezeCountdown, "Freeze countdown timer",
              "Stops the on-screen countdown (the facility's self-destruct) where it is: its frame runs as usual "
              "- alarms, clearing it by getting out - but the time does not go down.",
              p.countdown, "countdown not found");

    ImGui::SeparatorText("Records");
    cheat_row(cheats::kNoItemBoxCount, "Item box without counting",
              "Opening the item box is not counted, for the record for finishing the game without opening it: the "
              "game's count goes to 0 when this goes on and stays there while it is on - in your saves too. Turned "
              "off, openings count again from there.",
              p.item_box_count, "item box count not found");
    if (p.item_box_count && st.item_box_opens >= 0) {
      ImGui::SameLine();
      if (st.item_box_opens == 1) ImGui::TextDisabled("(opened once)");
      else ImGui::TextDisabled("(opened %d times)", st.item_box_opens);
    }
    cheat_row(cheats::kNoHealCount, "Recovery items without counting",
              "Using a recovery item (a first aid spray, a herb or a mix) is not counted, for the record for finishing "
              "the game without using one: the game's count goes to 0 when this goes on and stays there while it is "
              "on - in your saves too. The item still heals you and is used up. Turned off, uses count again from "
              "there.",
              p.heal_count, "recovery item count not found");
    if (p.heal_count && st.heals_used >= 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%d used)", st.heals_used);
    }
    {
      char help[320], limit[48] = "a set number of";
      if (st.steps_limit > 0) std::snprintf(limit, sizeof(limit), "%d", st.steps_limit);
      std::snprintf(help, sizeof(help),
                    "The game counts your steps, for the record for finishing the game in %s steps or fewer. This "
                    "stops the count where it is: steps you take while it is on are not counted - in your saves too. "
                    "Set a count below and it stays at that one.",
                    limit);
      cheat_row(cheats::kFreezeSteps, "Freeze step count", help, p.steps, "step count not found");
      static int edit_steps = -1;
      if (edit_steps < 0 && st.steps >= 0) edit_steps = st.steps;
      ImGui::BeginDisabled(!p.steps || st.steps < 0);
      ImGui::Text("Steps: %d", st.steps);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.0f);
      ImGui::InputInt("##steps", &edit_steps, 100, 1000);
      ImGui::SameLine();
      if (ImGui::SmallButton("Apply##steps")) cheats::request_steps(edit_steps < 0 ? 0 : edit_steps);
      ImGui::EndDisabled();
      if (st.steps_limit > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(the record: %d or fewer)", st.steps_limit);
      }
    }
    draw_difficulty(st, s.paused);
    if (st.last_action[0]) ImGui::TextWrapped("%s", st.last_action);
  }
  {
    const inventory::View* iv = new inventory::View(inventory::view());
    draw_inventory(*iv);
    draw_item_box(*iv);
    if (iv->note_error && iv->note[0]) {
      ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
      ImGui::TextWrapped("%s", iv->note);
      ImGui::PopStyleColor();
    }
    delete iv;
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::TextDisabled("F%d hides this panel", config::get().toggle_key - 0x6F);
  ImGui::End();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  InterlockedExchange(&g_visible, 0);
}

void install_early() {
  if (!g_imgui_cs_ready) {
    InitializeCriticalSection(&g_imgui_cs);
    InitializeCriticalSection(&g_cursor_cs);
    g_imgui_cs_ready = true;
  }
  if (config::get().disable_cursor || config::get().disable_overlay) return;
  if (void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "ClipCursor",
                                 reinterpret_cast<void*>(&hk_clip_cursor), &g_clip_slot)) {
    g_real_clip = reinterpret_cast<ClipCursorFn>(prev);
    logf("overlay: re2.exe's ClipCursor import hooked (the pointer is let go while the panel is up)");
  } else {
    logf("overlay: re2.exe's ClipCursor import not found - the pointer stays where the game holds it");
  }
}

bool install() { return d3d::install(); }

void service() {
  d3d::service();
  static DWORD last = 0;
  const DWORD now = GetTickCount();
  if (now - last >= 1000) {
    last = now;
    d3d::log_rates();
  }
}

namespace {

// The window's thread (ShowCursor counts per thread): when the panel comes or
// goes (posted from the render thread), and on its messages - the show is
// re-asserted a few times a second while the panel is up, in case the game hid
// the cursor again.
void cursor_update(bool now) {
  static bool was_visible = false;
  static DWORD checked = 0;
  if (config::get().disable_cursor || !g_context_ready) return;
  const bool vis = g_visible != 0;
  const DWORD t = GetTickCount();
  if (!now && vis == was_visible && (!vis || t - checked < 250)) return;
  was_visible = vis;
  checked = t;
  show_cursor(vis);
  if (!g_real_clip) return;
  EnterCriticalSection(&g_cursor_cs);
  if (vis != g_cursor_free) {
    g_cursor_free = vis;
    if (vis) g_real_clip(nullptr);
    else if (g_game_clipping) g_real_clip(&g_game_clip);
  }
  LeaveCriticalSection(&g_cursor_cs);
}

}  // namespace

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  d3d::uninstall();
  keyboard::stop();
  shutdown_imgui();
  mem::iat_restore(g_clip_slot, reinterpret_cast<void*>(&hk_clip_cursor), reinterpret_cast<void*>(g_real_clip));
  logf("overlay hooks removed");
}

}  // namespace re2cc::overlay
