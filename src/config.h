#pragma once

namespace re2cc::config {

struct Settings {
  int toggle_key = 0x76;      // VK_F7: hide/show the panel while it is up
  // Initial state of each cheat when the game starts; the panel changes them
  // for the session.
  bool god_mode = false;
  bool one_hit_kills = false;
  bool infinite_ammo = false;
  bool save_without_ink = false;
  bool no_save_count = false;
  bool freeze_playtime = false;
  bool freeze_countdown = false;
  bool no_durability_loss = false;
  bool infinite_boards = false;
  bool no_item_box_count = false;
  bool no_recovery_item_count = false;
  bool freeze_steps = false;
  bool trace = false;         // verbose diagnostics
  bool always_show = false;   // debug: draw the panel on every screen, not only the pause menu
  bool dump_methods = false;  // write every managed method's code address beside the DLL once (reverse engineering)
  // "Disable = overlay,dispatch,game,events,input,cursor,keyboard" turns whole
  // subsystems off so a fault can be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
  bool disable_events = false;  // the game's events calling the mod (events.cpp): no hooks, the cheats do nothing
  bool disable_input = false;   // keeping the panel's clicks and keys from the game
  bool disable_cursor = false;  // showing and freeing the pointer while the panel is up
  bool disable_keyboard = false;  // the panel's own DirectInput keyboard (keyboard.h): it takes the window's key messages
};

const Settings& get();

// RE2CabbyCodes.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable.
void load(const char* dir);
const char* dir();  // the DLL's directory, with a trailing separator

}  // namespace re2cc::config
