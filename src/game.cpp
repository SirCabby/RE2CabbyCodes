#include "game.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "items.h"
#include "log.h"
#include "mem.h"
#include "re.h"
#include "switch_eval.h"

namespace re2cc::game {
namespace {

std::atomic<bool> g_ready{false};  // set last by discover() on the mod thread
char g_status[200] = "not started";
Parts g_parts;

// Classes.
uint32_t t_main_flow, t_player_mgr, t_enemy_mgr, t_inv_mgr, t_locker_mgr, t_clock, t_record;
uint32_t t_survivor_cond, t_hpc, t_enemy_ctl, t_inventory, t_slot, t_item_data, t_stock_item, t_primitive;
uint32_t t_locker_gsd, t_locker_save, t_clock_gsd, t_header, t_fat;
uint32_t t_typewriter, t_player_cond, t_reaction, t_damage_info, t_equipment, t_weapon, t_bitflag2;
uint32_t t_countdown, t_rogue_countdown;
uint32_t t_equip_mgr, t_bullet_ud, t_weapon_combo, t_parts_combo;
// The singletons' `_Instance` fields.
re::Field i_main_flow, i_player_mgr, i_enemy_mgr, i_inv_mgr, i_locker_mgr, i_clock, i_record, i_equip_mgr;
// Fields.
re::Field f_state, f_header, f_save_times;
re::Field f_player_list, f_cond_hpc, f_cond_poison, f_cond_survivor;
re::Field f_hp_cur, f_hp_def;
re::Field f_active_enemies, f_enemy_hp;
re::Field f_save_count, f_clear_time;
re::Field f_clock_data, f_elapsed, f_demo, f_inv_time, f_pause_time;
re::Field f_cur_inventory, f_fat_data, f_fat_items, f_fat_weapons;
re::Field f_inv_slots, f_inv_size;
re::Field f_slot_stock, f_slot_force_blank;
re::Field f_default_item, f_stock_index;
re::Field f_p_item, f_p_weapon, f_p_parts, f_p_bullet, f_p_count;
re::Field f_locker_gsd, f_locker_save, f_locker_items;
re::Field f_difficulty;
// The difficulty switch: the continue-on-Assisted hold, and the manager setDifficulty sets its flags on.
re::Field f_force_easy;
uint32_t t_user_data_mgr;
re::Field i_user_data_mgr;
// The records' counters: RecordManager's save data (OpenItemBox, UseHealItem), PlayerManager's
// <Pedometer>, and the record table the step record's limit is read from.
uint32_t t_record_gsd, t_record_node;
re::Field f_record_gsd, f_open_item_box, f_use_heal, f_pedometer, f_record_nodes, f_node_progress, f_node_clear;
// What the event hooks touch (events.cpp).
re::Field f_hp_nodamage, f_di_damage, f_di_kill, f_enemy_flags2, f_bitflag_flag, f_reaction_hp, f_reaction_enemy;
uint32_t t_em7100_think;               // G2: its own stagger counter, not its HP (stun_g2)
re::Field f_enemy_think, f_tired_hp;
uint32_t t_em5000_think, t_flame_hp;   // the plants: the health only fire takes (wither_plant)
re::Field f_think_flame, f_flame_cur;
re::Field f_eq_condition, f_weapon_condition, f_cond_inventory, f_inv_main_slot, f_inv_sub_slot, f_clock_measure;
re::Field f_skip_restrict;
re::Field f_cd_timer, f_rogue_timer;
// Infinite wooden boards: a use-item trigger's callback closures, their ItemWork and its item data.
uint32_t t_use_closure[2], t_item_work, t_use_data;
re::Field f_closure_work[2], f_work_data, f_use_stock, f_use_item, f_use_reuse;
re::Field f_bullet_ud, f_bullet_combos, f_wc_type, f_wc_combos, f_pc_parts, f_pc_priority, f_pc_overwrite, f_pc_infinity,
          f_pc_number, f_pc_overwrite_kind, f_pc_kind;
// EquipmentDefine.getItemID(Bullet): the ammo item a Bullet kind is, run for
// each weapon's kind (weapon_bullet_id). 0 when the method is not a switch the
// mod can run - a weapon is then made with no ammo recorded, as an empty one is.
uintptr_t g_bullet_item_code = 0;
int8_t g_weapon_category[512];  // EquipmentDefine.getCategory by WeaponType (-1: none; -2: not read)
// Stacks: ItemManager.<ItemElementTable> (a Dictionary<Item.ID, ItemElement>) and
// getItemMultipleUseMax by Item.ID (0: not read).
uint32_t t_item_mgr, t_item_table, t_item_entry, t_item_element;
re::Field i_item_mgr, f_item_table, f_table_entries, f_table_count, f_entry_hash, f_entry_key, f_entry_value,
          f_elem_disposable;
constexpr int kItemIds = 1024;
int32_t g_item_max[kItemIds];
EventTargets g_events;
uint32_t g_keep_alive = 0;  // EnemyDefine.ConditionStateBitFlag2 STOP_DAMAGE | GUTS_MODE, as bits of BitFlag.Flag
uint32_t g_no_death = 0;    // NO_DEATH, for the trace only (make_lethal does not hold it off)
uint32_t g_stop_damage = 0;  // STOP_DAMAGE alone (wither_plant)
SRWLOCK g_full_lock = SRWLOCK_INIT;  // weapon_full_count's cache (the tick and the game's threads)
// Enum values, read out of the database (the defaults are build 11636119's).
int k_in_game = 6, k_pause = 9, k_title = 3, k_load = 4, k_game_over = 7;
// The end of a run, where the game records the clear (result_state_is): the
// ending, the staff roll and the results screens, the Ghost Survivors' too.
// Any the enum does not name stays -1 and matches nothing.
int k_result[6] = {14, 15, 30, 13, 10, 12};
int k_ink = 32;
int k_columns = 4;  // slots per inventory row, read from Inventory.isRightEdgeSlot's code
int k_fat_parts = 1;  // WeaponParts that make a weapon take two slots (A), read from InventoryManager.isFatWeapon's code
int k_easy = 0, k_normal = 1, k_hard = 2;  // MainFlowManager.Difficulty EASY, NORMAL, HARD
int k_knife = 9;    // EquipmentDefine.WeaponCategory.Knife
int k_multiple_use = 2;  // item.Disposable.MultipleUse
int k_boards = 33;       // Item.ID sm70_202 (Wooden Boards)
int k_stock_item = 1;    // InventoryManager.STOCK_TYPE.ITEM
int k_walk = 6;          // RecordManager.ProgressType.WALK (the step record)

uintptr_t instance(const re::Field& inst, uint32_t t) {
  const uintptr_t at = re::static_addr(inst);
  const uintptr_t obj = at ? mem::read_ptr(at) : 0;
  return obj && re::is_a(obj, t) ? obj : 0;
}

template <typename T>
bool get(uintptr_t obj, const re::Field& f, T* out) {
  const uintptr_t a = re::field_addr(obj, f);
  return a && mem::read_safe(a, out);
}

template <typename T>
bool put(uintptr_t obj, const re::Field& f, const T& v) {
  const uintptr_t a = re::field_addr(obj, f);
  return a && mem::store(a, v);
}

uintptr_t ref(uintptr_t obj, const re::Field& f) {
  const uintptr_t a = re::field_addr(obj, f);
  return a ? mem::read_ptr(a) : 0;
}

int g_missing = 0;
uint32_t need_type(const char* name) {
  const uint32_t t = re::find_type(name);
  if (!t) {
    logf("game: class %s not found", name);
    ++g_missing;
  }
  return t;
}
re::Field need_field(uint32_t t, const char* name) {
  if (!t) {
    ++g_missing;
    return {};
  }
  const re::Field f = re::find_field(t, name);
  if (!f.valid()) {
    logf("game: %s.%s not found", re::full_name(t).c_str(), name);
    ++g_missing;
  }
  return f;
}

bool read_int_list(uintptr_t list, int* out, int max, int* n) {
  re::List l;
  *n = 0;
  if (!list || !re::read_list(list, &l)) return false;
  // A List<enum>: the elements are 4-byte values.
  if (l.items.elem_size != 4) return false;
  for (int i = 0; i < l.count && *n < max; ++i) out[(*n)++] = mem::read<int32_t>(l.items.data + static_cast<uintptr_t>(i) * 4);
  return true;
}

}  // namespace

// --- discovery ---------------------------------------------------------------------------
bool discover() {
  g_missing = 0;
  t_main_flow = need_type("app.ropeway.gamemastering.MainFlowManager");
  t_player_mgr = need_type("app.ropeway.PlayerManager");
  t_enemy_mgr = need_type("app.ropeway.EnemyManager");
  t_inv_mgr = need_type("app.ropeway.gamemastering.InventoryManager");
  t_locker_mgr = need_type("app.ropeway.gamemastering.ItemLockerManager");
  t_clock = need_type("app.ropeway.GameClock");
  t_record = need_type("app.ropeway.gamemastering.RecordManager");
  t_survivor_cond = need_type("app.ropeway.survivor.SurvivorCondition");
  t_hpc = need_type("app.ropeway.HitPointController");
  t_enemy_ctl = need_type("app.ropeway.EnemyController");
  t_inventory = need_type("app.ropeway.survivor.Inventory");
  t_slot = need_type("app.ropeway.inventory.Slot");
  t_item_data = need_type("app.ropeway.gamemastering.InventoryManager.ItemData");
  t_stock_item = need_type("app.ropeway.gamemastering.InventoryManager.StockItem");
  t_primitive = need_type("app.ropeway.gamemastering.InventoryManager.PrimitiveItem");
  t_locker_gsd = need_type("app.ropeway.gamemastering.ItemLockerManager.GameSaveData");
  t_locker_save = need_type("app.ropeway.gimmick.action.GimmickItemLockerControl.ItemLockerSaveData");
  t_clock_gsd = need_type("app.ropeway.GameClock.GameSaveData");
  t_header = need_type("app.ropeway.gamemastering.MainFlowManager.GameHeaderSaveData");
  t_fat = need_type("app.ropeway.FatItemUserData");

  i_main_flow = need_field(t_main_flow, "_Instance");
  i_player_mgr = need_field(t_player_mgr, "_Instance");
  i_enemy_mgr = need_field(t_enemy_mgr, "_Instance");
  i_inv_mgr = need_field(t_inv_mgr, "_Instance");
  i_locker_mgr = need_field(t_locker_mgr, "_Instance");
  i_clock = need_field(t_clock, "_Instance");
  i_record = need_field(t_record, "_Instance");

  f_state = need_field(t_main_flow, "_CurrentMainState");
  f_header = need_field(t_main_flow, "gameHeaderSaveData");
  f_save_times = need_field(t_header, "SaveTimes");
  f_player_list = need_field(t_player_mgr, "PlayerList");
  f_cond_hpc = need_field(t_survivor_cond, "<HitPointController>k__BackingField");
  f_cond_poison = need_field(t_survivor_cond, "_IsPoison");
  f_cond_survivor = need_field(t_survivor_cond, "<SurvivorType>k__BackingField");
  f_hp_cur = need_field(t_hpc, "<CurrentHitPoint>k__BackingField");
  f_hp_def = need_field(t_hpc, "<DefaultHitPoint>k__BackingField");
  f_active_enemies = need_field(t_enemy_mgr, "<ActiveEnemyList>k__BackingField");
  f_enemy_hp = need_field(t_enemy_ctl, "<HitPoint>k__BackingField");
  f_save_count = need_field(t_record, "<CurrentSaveCount>k__BackingField");
  // The clear time the results screen shows (setClearedGame writes it from the clock).
  f_clear_time = need_field(t_record, "<CurrentClearTime>k__BackingField");
  f_clock_data = need_field(t_clock, "_GameSaveData");
  f_elapsed = need_field(t_clock_gsd, "_GameElapsedTime");
  f_demo = need_field(t_clock_gsd, "_DemoSpendingTime");
  f_inv_time = need_field(t_clock_gsd, "_InventorySpendingTime");
  f_pause_time = need_field(t_clock_gsd, "_PauseSpendingTime");
  f_cur_inventory = need_field(t_inv_mgr, "<CurrentInventory>k__BackingField");
  f_fat_data = need_field(t_inv_mgr, "FatItemUserData");
  f_fat_items = need_field(t_fat, "FatItemList");
  f_fat_weapons = need_field(t_fat, "FatWeaponList");
  f_inv_slots = need_field(t_inventory, "_Slots");
  f_inv_size = need_field(t_inventory, "_CurrentSlotSize");
  f_slot_stock = need_field(t_slot, "_Stock");
  f_slot_force_blank = need_field(t_slot, "<IsForceBlank>k__BackingField");
  f_default_item = need_field(t_item_data, "DefaultItem");
  f_stock_index = need_field(t_stock_item, "Index");
  f_p_item = need_field(t_primitive, "ItemId");
  f_p_weapon = need_field(t_primitive, "WeaponId");
  f_p_parts = need_field(t_primitive, "WeaponParts");
  f_p_bullet = need_field(t_primitive, "BulletId");
  f_p_count = need_field(t_primitive, "Count");
  f_locker_gsd = need_field(t_locker_mgr, "gameSaveData");
  f_locker_save = need_field(t_locker_gsd, "SaveData");
  f_locker_items = need_field(t_locker_save, "_Items");

  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.MainFlowManager.MainState")) {
    k_in_game = re::enum_value(e, "IN_GAME", k_in_game);
    k_pause = re::enum_value(e, "PAUSE", k_pause);
    k_title = re::enum_value(e, "TITLE", k_title);
    k_load = re::enum_value(e, "LOAD_GAME_DATA", k_load);
    k_game_over = re::enum_value(e, "GAME_OVER", k_game_over);
    static const char* const kResultNames[6] = {"RESULT",  "RESULT_EXTRA", "ROGUE_RESULT",
                                                "ENDING",  "STAFFROLL",    "WAIT_STAFFROLL"};
    for (int i = 0; i < 6; ++i) k_result[i] = re::enum_value(e, kResultNames[i], k_result[i]);
  }
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.Item.ID")) {
    k_ink = re::enum_value(e, "sm70_201", k_ink);
    k_boards = re::enum_value(e, "sm70_202", k_boards);
  }
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.MainFlowManager.Difficulty")) {
    k_easy = re::enum_value(e, "EASY", k_easy);
    k_normal = re::enum_value(e, "NORMAL", k_normal);
    k_hard = re::enum_value(e, "HARD", k_hard);
  }

  // Durability (No durability loss): the knives, from EquipmentDefine.getCategory's
  // jump table - `dec edx; cmp edx,N; ja; movsxd rax,edx; lea rdx,[image]; movzx eax,
  // byte [rdx+rax+cases]; mov ecx,[rdx+rax*4+targets]; add rcx,rdx; jmp rcx`, each
  // case `xor eax,eax; ret` or `mov eax,imm32; ret` - and their full counts from
  // EquipmentManager's WeaponBulletUserData.
  if (const uint32_t e = re::find_type("app.ropeway.EquipmentDefine.WeaponCategory")) k_knife = re::enum_value(e, "Knife", k_knife);
  std::memset(g_weapon_category, -2, sizeof(g_weapon_category));
  int knives = 0;
  const uint32_t t_equip_define = need_type("app.ropeway.EquipmentDefine");
  const uintptr_t cat_code = t_equip_define ? re::method_code(t_equip_define, "getCategory", 1) : 0;
  const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const mem::Range text = mem::section(GetModuleHandleA(nullptr), ".text");
  if (cat_code && mem::matches(cat_code, "FF CA 81 FA ?? ?? ?? ?? 77 ?? 48 63 C2 48 8D 15 ?? ?? ?? ?? 0F B6 84 02 ?? ?? ?? ?? "
                                         "8B 8C 82 ?? ?? ?? ?? 48 03 CA FF E1") &&
      mem::rip_target(cat_code + 13, 3, 7) == exe) {
    const uint32_t last = mem::read<uint32_t>(cat_code + 4);
    const uintptr_t cases = exe + mem::read<uint32_t>(cat_code + 24), targets = exe + mem::read<uint32_t>(cat_code + 31);
    for (uint32_t id = 1; id <= last + 1 && id < sizeof(g_weapon_category); ++id) {
      const uintptr_t target = exe + mem::read<uint32_t>(targets + mem::read<uint8_t>(cases + id - 1) * 4);
      int8_t cat = -2;
      if (text.contains(target) && (mem::matches(target, "31 C0 C3") || mem::matches(target, "33 C0 C3"))) cat = 0;
      else if (text.contains(target) && mem::matches(target, "B8 ?? ?? ?? ?? C3")) cat = static_cast<int8_t>(mem::read<int32_t>(target + 1));
      g_weapon_category[id] = cat;
      knives += cat == k_knife;
    }
  } else {
    logf("game: EquipmentDefine.getCategory is not the code expected - no knife is known");
  }
  t_equip_mgr = need_type("app.ropeway.EquipmentManager");
  i_equip_mgr = need_field(t_equip_mgr, "_Instance");
  f_bullet_ud = need_field(t_equip_mgr, "_WeaponBulletUserdata");
  t_bullet_ud = need_type("app.ropeway.WeaponBulletUserData");
  f_bullet_combos = need_field(t_bullet_ud, "_LoadingPartsCombos");
  t_weapon_combo = need_type("app.ropeway.WeaponBulletUserData.WeaponLoadingpartsCombination");
  f_wc_type = need_field(t_weapon_combo, "_WeaponType");
  f_wc_combos = need_field(t_weapon_combo, "_LoadingPartsCombos");
  t_parts_combo = need_type("app.ropeway.WeaponBulletUserData.LoadingPartsCombination");
  f_pc_parts = need_field(t_parts_combo, "_Parts");
  f_pc_priority = need_field(t_parts_combo, "_Priority");
  f_pc_overwrite = need_field(t_parts_combo, "_OverwriteNumber");
  f_pc_infinity = need_field(t_parts_combo, "_Infinity");
  f_pc_number = need_field(t_parts_combo, "_Number");
  // The kind of ammo a weapon takes, the same entry's `_Kind` (an
  // EquipmentDefine.Bullet), and EquipmentDefine.getItemID to turn it into the
  // Item.ID a stock's BulletId holds - what the game's own setWeapon is given
  // when it makes a weapon (ItemLockerManager.AddWeaponToStrage passes the
  // AddItemData entry's ammType). Optional: without them a weapon the editor
  // puts in a slot records no ammo, as an empty weapon does.
  f_pc_overwrite_kind = t_parts_combo ? re::find_field(t_parts_combo, "_OverwriteKind") : re::Field{};
  f_pc_kind = t_parts_combo ? re::find_field(t_parts_combo, "_Kind") : re::Field{};
  g_bullet_item_code =
      t_equip_define ? re::method_code_typed(t_equip_define, "getItemID", {"app.ropeway.EquipmentDefine.Bullet"}) : 0;

  // Stacks (the item box's Store): which items stack - ItemManager.<ItemElementTable>,
  // filled as the game starts and read when an item is stored - and how far:
  // ItemManager.getItemMultipleUseMax(Item.ID), a compiled switch run here for
  // every id (an instance method: the id is in r8, after the context and `this`).
  t_item_mgr = need_type("app.ropeway.gamemastering.ItemManager");
  i_item_mgr = need_field(t_item_mgr, "_Instance");
  f_item_table = need_field(t_item_mgr, "<ItemElementTable>k__BackingField");
  t_item_table = need_type("System.Collections.Generic.Dictionary`2<app.ropeway.gamemastering.Item.ID,"
                           "app.ropeway.gamemastering.item.ItemElement>");
  f_table_entries = need_field(t_item_table, "_entries");
  f_table_count = need_field(t_item_table, "_count");
  t_item_entry = need_type("System.Collections.Generic.Dictionary`2.Entry<app.ropeway.gamemastering.Item.ID,"
                           "app.ropeway.gamemastering.item.ItemElement>");
  f_entry_hash = need_field(t_item_entry, "hashCode");
  f_entry_key = need_field(t_item_entry, "key");
  f_entry_value = need_field(t_item_entry, "value");
  t_item_element = need_type("app.ropeway.gamemastering.item.ItemElement");
  f_elem_disposable = need_field(t_item_element, "Disposable");
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.item.Disposable"))
    k_multiple_use = re::enum_value(e, "MultipleUse", k_multiple_use);
  int item_ids = 297;
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.Item.ID")) item_ids = re::enum_value(e, "MAX", item_ids);
  std::memset(g_item_max, 0, sizeof(g_item_max));
  const uintptr_t max_code =
      t_item_mgr ? re::method_code_typed(t_item_mgr, "getItemMultipleUseMax", {"app.ropeway.gamemastering.Item.ID"}) : 0;
  const mem::Range image = mem::module_range(GetModuleHandleA(nullptr));
  int stacking = 0, unread = 0;
  for (int id = 1; max_code && id < item_ids && id < kItemIds && !unread; ++id) {
    int32_t max = 0;
    if (!code::eval_switch(
            max_code, 8, static_cast<uint64_t>(id),
            [](uintptr_t at, uint8_t* out, size_t n) { return mem::copy_from(out, at, n); },
            [&](uintptr_t at) { return text.contains(at); }, [&](uintptr_t at) { return image.contains(at); }, &max) ||
        max <= 0) {
      ++unread;
      continue;
    }
    g_item_max[id] = max;
    stacking += max > 1;
  }
  if (!max_code || unread) {
    std::memset(g_item_max, 0, sizeof(g_item_max));
    stacking = 0;
    logf("game: ItemManager.getItemMultipleUseMax %s - Store puts items in the item box without stacking them",
         max_code ? "is not a switch the mod can run" : "was not found");
  }

  // Typewriters (Save without ink ribbons on Hardcore). Which lambda is which is read
  // out of their code: `cmp ecx,HARD; je` answers false on Hardcore, `jne` true.
  t_typewriter = need_type("app.ropeway.gimmick.action.GimmickTypeWriter");
  f_difficulty = need_field(t_header, "CurrentDifficulty");
  bool tw_checks_known = t_typewriter != 0;
  for (int w = 0; w < 2 && t_typewriter; ++w) {
    char name[64];
    std::snprintf(name, sizeof(name), "<getTriggerFuncCheckValid>b__8_%d", w);
    const uint32_t m = re::find_method(t_typewriter, name, {"app.ropeway.gimmick.action.Trigger"});
    const uintptr_t code = m ? re::method_code_at(m) : 0;
    bool seen = false;
    for (uintptr_t p = code; code && p < code + 0x80 && !seen; ++p)
      seen = mem::read<uint8_t>(p) == 0x83 && mem::read<uint8_t>(p + 1) == 0xF9 &&
             mem::read<int8_t>(p + 2) == k_hard && mem::read<uint8_t>(p + 3) == (w == 0 ? 0x74 : 0x75);
    if (!seen) {
      logf("game: GimmickTypeWriter.%s is not the %s check expected - typewriters are left alone", name,
           w == 0 ? "not-HARD" : "HARD");
      tw_checks_known = false;
    }
    g_events.tw_check[w] = m;
  }

  // The difficulty switch: MainFlowManager.setDifficulty writes the header's
  // difficulty unless the game over screen's continue on Assisted holds it
  // (<ForceEasyContinue>), then sets GlobalUserDataManager's flags - and throws
  // when that manager is not up.
  f_force_easy = need_field(t_main_flow, "<ForceEasyContinue>k__BackingField");
  t_user_data_mgr = need_type("app.ropeway.gamemastering.GlobalUserDataManager");
  i_user_data_mgr = need_field(t_user_data_mgr, "_Instance");

  // The records' counters (Counter): the item box opened and recovery items used
  // in RecordManager's save data, the player's steps in PlayerManager, and the
  // step record's limit in the record table - the RecordNode in
  // RecordManager.RecordDataList whose ProgressType is WALK.
  t_record_gsd = need_type("app.ropeway.gamemastering.RecordManager.GameSaveData");
  f_record_gsd = need_field(t_record, "gameSaveData");
  f_open_item_box = need_field(t_record_gsd, "OpenItemBox");
  f_use_heal = need_field(t_record_gsd, "UseHealItem");
  f_pedometer = need_field(t_player_mgr, "<Pedometer>k__BackingField");
  f_record_nodes = need_field(t_record, "RecordDataList");
  t_record_node = need_type("app.ropeway.gamemastering.RecordManager.RecordNode");
  f_node_progress = need_field(t_record_node, "ProgressType");
  f_node_clear = need_field(t_record_node, "ClearCount");
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.RecordManager.ProgressType"))
    k_walk = re::enum_value(e, "WALK", k_walk);

  // The game's events (events.cpp): the handlers and virtual methods the game
  // calls the mod through, and what their hooks read and write.
  const char* const kDamageInfo = "app.Collision.HitController.DamageInfo";
  t_damage_info = need_type(kDamageInfo);
  f_di_damage = need_field(t_damage_info, "<Damage>k__BackingField");
  f_di_kill = need_field(t_damage_info, "<IsKill>k__BackingField");
  f_hp_nodamage = need_field(t_hpc, "<NoDamage>k__BackingField");
  f_enemy_flags2 = need_field(t_enemy_ctl, "<ConditionFlagsField2>k__BackingField");
  t_bitflag2 = need_type("app.ropeway.BitFlag`1<app.ropeway.EnemyDefine.ConditionStateBitFlag2>");
  f_bitflag_flag = need_field(t_bitflag2, "Flag");
  if (const uint32_t e = re::find_type("app.ropeway.EnemyDefine.ConditionStateBitFlag2")) {
    // STOP_DAMAGE is kept on its own too: the plants' knockdown is written while
    // GUTS_MODE is on, which is the state they spend their whole life in.
    const int nd = re::enum_value(e, "NO_DEATH", -1);  // trace only
    g_no_death = nd >= 0 && nd < 32 ? 1u << nd : 0;
    const struct { const char* bit; uint32_t* own; } kBits[] = {{"STOP_DAMAGE", &g_stop_damage}, {"GUTS_MODE", nullptr}};
    for (const auto& b : kBits) {
      const int v = re::enum_value(e, b.bit, -1);
      if (v < 0 || v >= 32) {
        logf("game: EnemyDefine.ConditionStateBitFlag2.%s not found", b.bit);
        continue;
      }
      g_keep_alive |= 1u << v;
      if (b.own) *b.own = 1u << v;
    }
  }
  f_enemy_think = need_field(t_enemy_ctl, "<Think>k__BackingField");
  // Trace only (never missing-counted): what EnemyController.canRestrictMove compares
  // with GameClock.get_ActualPlayingTime - the enemies' own clock (game.h, enemy_time).
  f_skip_restrict = re::find_field(t_enemy_ctl, "SkipRestrictMoveFrame");
  t_em7100_think = need_type("app.ropeway.enemy.em7100.Em7100Think");
  f_tired_hp = need_field(t_em7100_think, "TiredHp");
  t_em5000_think = need_type("app.ropeway.enemy.em5000.Em5000Think");
  f_think_flame = need_field(t_em5000_think, "<FlameOnlyHitPoint>k__BackingField");
  t_flame_hp = need_type("app.ropeway.enemy.em5000.FlameOnlyHitPoint");
  f_flame_cur = need_field(t_flame_hp, "<CurrentHitPoint>k__BackingField");
  t_reaction = need_type("app.ropeway.EnemyReactionController");
  f_reaction_hp = need_field(t_reaction, "<HitPoint>k__BackingField");
  f_reaction_enemy = need_field(t_reaction, "<EnemyController>k__BackingField");
  t_player_cond = need_type("app.ropeway.survivor.player.PlayerCondition");
  t_equipment = need_type("app.ropeway.survivor.Equipment");
  f_eq_condition = need_field(t_equipment, "<Condition>k__BackingField");
  t_weapon = need_type("app.ropeway.Weapon");
  f_weapon_condition = need_field(t_weapon, "<Condition>k__BackingField");
  f_cond_inventory = need_field(t_survivor_cond, "<Inventory>k__BackingField");
  f_inv_main_slot = need_field(t_inventory, "_MainSlot");
  f_inv_sub_slot = need_field(t_inventory, "_SubSlot");
  f_clock_measure = need_field(t_clock, "_MeasureGameElapsedTime");
  auto need_method = [](uint32_t t, const char* name, std::initializer_list<const char*> params) -> uint32_t {
    const uint32_t m = t ? re::find_method(t, name, params) : 0;
    if (t && !m) {
      logf("game: method %s.%s not found", re::full_name(t).c_str(), name);
      ++g_missing;
    }
    return m;
  };
  EventTargets& ev = g_events;
  ev.enemy_controller = t_enemy_ctl;
  ev.reaction = t_reaction;
  ev.survivor = t_survivor_cond;
  ev.player = t_player_cond;
  ev.typewriter = t_typewriter;
  ev.weapon = t_weapon;
  ev.equipment = t_equipment;
  ev.main_flow = t_main_flow;
  ev.clock = t_clock;
  ev.enemy_damage = need_method(t_enemy_ctl, "HitController_OnHitDamage", {kDamageInfo});
  ev.reaction_damage = need_method(t_reaction, "onHitDamage", {kDamageInfo});
  ev.player_check = need_method(t_survivor_cond, "checkHitDamage", {kDamageInfo});
  ev.player_damage = need_method(t_player_cond, "onHitDamage", {kDamageInfo});
  ev.player_hp = need_method(t_player_cond, "<doSurvivorStart>b__36_0", {"System.Int32"});
  ev.weapon_hit = need_method(t_weapon, "onHitAttack", {kDamageInfo});
  ev.defend_category = need_method(t_equipment, "defend", {"app.ropeway.EquipmentDefine.EquipCategory", "System.Int32"});
  ev.defend_type = need_method(t_equipment, "defend", {"app.ropeway.EquipmentDefine.WeaponType", "System.Int32"});
  ev.fire = need_method(t_equipment, "executeFire", {"app.ropeway.EquipmentDefine.WeaponType", "System.Int32"});
  ev.melee = need_method(t_equipment, "onHitMeleeAttack",
                         {kDamageInfo, "app.ropeway.implement.Melee.Attack", "System.Int32"});
  ev.save_data = need_method(t_main_flow, "saveGameSaveData", {});
  // Freeze play time: a save captures the clock's own save data here (IGameSaveData
  // slot 1, the same pass as the header's). The held time goes in for that capture -
  // the clock is never stopped, see hold_record_time.
  ev.clock_save = need_method(t_clock, "saveGameSaveData", {});
  // Countdowns (Freeze countdown timer): the self-destruct's CountDownBehavior counts
  // <CurrentTimerFrame> down by the frame's time in lateUpdate -> updateCountDown (and
  // calls the game over at 0); the Ghost Survivors' RogueCountDownBehavior likewise in
  // its updateCountDown, which only reflection reaches.
  t_countdown = need_type("app.ropeway.gui.CountDownBehavior");
  f_cd_timer = need_field(t_countdown, "<CurrentTimerFrame>k__BackingField");
  t_rogue_countdown = need_type("app.ropeway.gui.RogueCountDownBehavior");
  f_rogue_timer = need_field(t_rogue_countdown, "<CurrentTimerFrame>k__BackingField");
  ev.countdown = t_countdown;
  ev.rogue_countdown = t_rogue_countdown;
  ev.countdown_update = need_method(t_countdown, "lateUpdate", {});
  ev.rogue_countdown_update = need_method(t_rogue_countdown, "updateCountDown", {});
  // Freeze play time, The Ghost Survivors' HUD run timer: the same behaviour counts
  // *up* in lateUpdate -> updateCountUp, which reads GameClock.get_ActualRecordTime
  // itself every frame into <CurrentTimerFrame> for dispCountUp to draw. The held
  // time goes in for the length of that call (events.cpp).
  ev.rogue_timer_late = need_method(t_rogue_countdown, "lateUpdate", {});
  // Wooden boards (Infinite wooden boards): a window takes them through a use-item trigger.
  // TriggerUseItem.registerUseMode makes a callback for each item the trigger wants (a closure
  // holding the ItemWork, <registerUseMode>b__0) and each it lists as useless (b__1); the inventory
  // calls it for the item picked, and TriggerUseItem.onUsedBase then takes the work's item data -
  // Count of ItemId, when Stock is ITEM - from the inventory unless CanReuse is set.
  const char* const kClosures[2] = {"app.ropeway.gimmick.action.TriggerUseItem.<>c__DisplayClass33_0",
                                    "app.ropeway.gimmick.action.TriggerUseItem.<>c__DisplayClass33_1"};
  for (int i = 0; i < 2; ++i) {
    t_use_closure[i] = need_type(kClosures[i]);
    f_closure_work[i] = need_field(t_use_closure[i], "wk");
  }
  ev.use_item = need_method(t_use_closure[0], "<registerUseMode>b__0", {});
  ev.use_useless = need_method(t_use_closure[1], "<registerUseMode>b__1", {});
  t_item_work = need_type("app.ropeway.gimmick.action.TriggerUseItem.ItemWork");
  f_work_data = need_field(t_item_work, "ItData");
  t_use_data = need_type("app.ropeway.gimmick.action.TriggerUseItem.ItemData");
  f_use_stock = need_field(t_use_data, "Stock");
  f_use_item = need_field(t_use_data, "ItemId");
  f_use_reuse = need_field(t_use_data, "CanReuse");
  if (const uint32_t e = re::find_type("app.ropeway.gamemastering.InventoryManager.STOCK_TYPE"))
    k_stock_item = re::enum_value(e, "ITEM", k_stock_item);
  // The records' counters move inside three of the game's calls, all virtual: the
  // item box's FSM action opening it (SwitchItemToInventory.update, the only caller
  // of GUIMaster.openInventoryItemBoxMode), the inventory screen's frame
  // (NewInventorySlotBehavior.update: a recovery item is used from inside it -
  // executeCommand -> InventoryManager.useStock -> useItem -> useHealItem, direct
  // calls all) and a footstep (PlayerFootEffectController.onLand -> addPedometer).
  ev.item_box_action = need_type("app.ropeway.fsmv2.SwitchItemToInventory");
  ev.item_box_open = need_method(ev.item_box_action, "update", {"via.behaviortree.ActionArg"});
  ev.inventory_screen = need_type("app.ropeway.gui.NewInventorySlotBehavior");
  ev.inventory_update = need_method(ev.inventory_screen, "update", {});
  ev.foot_effects = need_type("app.ropeway.PlayerFootEffectController");
  ev.footstep = need_method(ev.foot_effects, "onLand",
                            {"via.motion.script.FootEffectController.JointPartsType",
                             "via.motion.script.FootEffectController.JointSideType", "via.vec3"});
  // Freeze play time, the extra modes: the main game's clear time is read by
  // ResultFlow.update -> RecordManager.setClearedGame, in the RESULT state the tick
  // holds the time for - but The Ghost Survivors and The 4th Survivor / The Tofu
  // Survivor record theirs a state earlier, while the run is still IN_GAME. This FSM
  // action's start is where: it clears GameClock._MeasureGameElapsedTime and then,
  // by the header's ScenarioType, calls RecordManager.backupGameElapsedTimeForExtra
  // (the 4th Survivor, Tofu) or RogueRecordManager.backupRoguePlayData(LostType,
  // true) (the four Ghost Survivors missions), each reading get_ActualRecordTime for
  // the time it writes into the save. Its only method, no subclasses.
  ev.extra_record_action = need_type("app.ropeway.fsmv2.EndMeasureAndRecordGameElapsedTimeForExtra");
  ev.extra_record = need_method(ev.extra_record_action, "start", {"via.behaviortree.ActionArg"});
  // The inventory's rows: Inventory.isRightEdgeSlot(Index) is `and r8d,N; cmp r8b,N; sete al; ret` - a slot is at
  // the right edge when (Index & N) == N, so a row holds N + 1.
  if (const uintptr_t code = t_inventory ? re::method_code(t_inventory, "isRightEdgeSlot", 1) : 0) {
    if (mem::matches(code, "41 83 E0 ?? 41 80 F8 ?? 0F 94 C0 C3") && mem::read<uint8_t>(code + 3) == mem::read<uint8_t>(code + 7) &&
        mem::read<uint8_t>(code + 3) > 0 && mem::read<uint8_t>(code + 3) < 16)
      k_columns = mem::read<uint8_t>(code + 3) + 1;
    else
      logf("game: Inventory.isRightEdgeSlot is not the code expected - taking %d slots a row", k_columns);
  }
  // Two-slot weapons: InventoryManager.isFatWeapon(WeaponType, WeaponParts) opens with `test r8b,N; je; mov al,1` -
  // a weapon fitted with any of the parts N takes two slots before FatWeaponList is asked.
  if (const uintptr_t code = t_inv_mgr ? re::method_code_typed(t_inv_mgr, "isFatWeapon",
                                                               {"app.ropeway.EquipmentDefine.WeaponType",
                                                                "app.ropeway.EquipmentDefine.WeaponParts"},
                                                               true)
                                       : 0) {
    int parts = 0;
    for (uintptr_t p = code; p < code + 0x20 && !parts; ++p)
      if (mem::matches(p, "41 F6 C0 ?? 74 ?? B0 01")) parts = mem::read<uint8_t>(p + 3);
    if (parts) k_fat_parts = parts;
    else logf("game: InventoryManager.isFatWeapon is not the code expected - taking weapons with parts 0x%X for two-slot ones", k_fat_parts);
  }

  Parts& p = g_parts;
  p.flow = t_main_flow && i_main_flow.valid() && f_state.valid();
  p.players = t_player_mgr && i_player_mgr.valid() && f_player_list.valid() && f_cond_hpc.valid() && f_hp_cur.valid() &&
              f_hp_def.valid();
  p.enemies = t_enemy_mgr && i_enemy_mgr.valid() && f_active_enemies.valid() && f_enemy_hp.valid() && f_hp_cur.valid();
  p.records = t_record && i_record.valid() && f_save_count.valid();
  p.header = p.flow && f_header.valid() && f_save_times.valid();
  p.clock = t_clock && i_clock.valid() && f_clock_data.valid() && f_elapsed.valid() && f_demo.valid() &&
            f_pause_time.valid() && f_inv_time.valid();
  p.bag = t_inv_mgr && i_inv_mgr.valid() && f_cur_inventory.valid() && f_inv_slots.valid() && f_inv_size.valid() &&
          f_slot_stock.valid() && f_stock_index.valid() && f_default_item.valid() && f_p_item.valid() && f_p_weapon.valid() &&
          f_p_count.valid() && f_p_parts.valid() && f_p_bullet.valid();
  p.box = p.bag && t_locker_mgr && i_locker_mgr.valid() && f_locker_gsd.valid() && f_locker_save.valid() &&
          f_locker_items.valid();
  p.fat = f_fat_data.valid() && f_fat_items.valid() && f_fat_weapons.valid();
  p.countdown = t_countdown && f_cd_timer.valid();
  p.durability = knives > 0;
  p.typewriter = tw_checks_known && p.header && f_difficulty.valid();
  p.difficulty = p.header && f_difficulty.valid() && f_force_easy.valid() && t_user_data_mgr && i_user_data_mgr.valid();
  p.damage = t_damage_info && f_di_damage.valid() && f_di_kill.valid() && f_hp_nodamage.valid() && f_hp_cur.valid();
  p.g2_stun = t_em7100_think && f_enemy_think.valid() && f_tired_hp.valid();
  p.plants = p.damage && f_enemy_think.valid() && t_em5000_think && f_think_flame.valid() && t_flame_hp &&
             f_flame_cur.valid();
  p.equipped = p.bag && t_equipment && f_eq_condition.valid() && f_cond_inventory.valid() && f_inv_main_slot.valid() &&
               f_inv_sub_slot.valid();
  p.measure = p.clock && f_clock_measure.valid();
  p.stacks = stacking > 0 && t_item_mgr && i_item_mgr.valid() && f_item_table.valid() && t_item_table &&
             f_table_entries.valid() && f_table_count.valid() && t_item_entry && f_entry_hash.valid() &&
             f_entry_key.valid() && f_entry_value.valid() && t_item_element && f_elem_disposable.valid();
  p.boards = t_use_closure[0] && f_closure_work[0].valid() && t_item_work && f_work_data.valid() && t_use_data &&
             f_use_stock.valid() && f_use_item.valid() && f_use_reuse.valid();
  const bool record_data = t_record && i_record.valid() && t_record_gsd && f_record_gsd.valid();
  p.item_box_count = record_data && f_open_item_box.valid();
  p.heal_count = record_data && f_use_heal.valid();
  p.steps = t_player_mgr && i_player_mgr.valid() && f_pedometer.valid();
  logf("game: discovery %s (%d name(s) missing): flow %d, players %d, enemies %d, records %d, header %d, clock %d, "
       "inventory %d, item box %d, two-slot items %d (and weapons with parts 0x%X), typewriters %d, knives %d, "
       "damage %d, plants %d, equipped weapons %d, play-time flag %d, countdown %d, stacks %d (%d items stack above 1), "
       "use-item callbacks %d, difficulty %d, record counters %d%d%d (item box, recovery items, steps); MainState "
       "IN_GAME=%d PAUSE=%d TITLE=%d, ink ribbon id %d, wooden boards id %d, %d inventory slots a row, Difficulty "
       "EASY=%d NORMAL=%d HARD=%d, ProgressType WALK=%d",
       p.flow ? "done" : "FAILED", g_missing, p.flow, p.players, p.enemies, p.records, p.header, p.clock, p.bag, p.box,
       p.fat, k_fat_parts, p.typewriter, knives, p.damage, p.plants, p.equipped, p.measure, p.countdown, p.stacks,
       stacking, p.boards, p.difficulty, p.item_box_count, p.heal_count, p.steps, k_in_game, k_pause, k_title, k_ink,
       k_boards, k_columns, k_easy, k_normal, k_hard, k_walk);
  g_ready = p.flow;
  std::snprintf(g_status, sizeof(g_status), "%s", g_ready ? "ready" : "the game's flow manager was not found (see the log)");
  return g_ready;
}

bool ready() { return g_ready; }
const char* status_text() { return g_status; }
const Parts& parts() { return g_parts; }

// --- flow ------------------------------------------------------------------------------------
int main_state() {
  if (!g_parts.flow) return -1;
  const uintptr_t mf = instance(i_main_flow, t_main_flow);
  int32_t s = -1;
  return mf && get(mf, f_state, &s) ? s : -1;
}

bool in_game_state(int state) { return state == k_in_game || state == k_pause; }
bool pause_state_is(int state) { return state == k_pause; }
bool title_state_is(int state) { return state == k_title; }
bool result_state_is(int state) {
  for (int s : k_result)
    if (s >= 0 && state == s) return true;
  return false;
}


// The log's name for a main state. MainState in full, in the order this build
// declares it (the states the mod acts on are read from the database by name -
// k_in_game, k_pause, k_result - and never from this table); the whole enum is
// here because an end-of-run path the log could only call "other" is one nobody
// can read afterwards - the Ghost Survivors' IN_GAME -> RESET_TO_RESULT_ROGUE ->
// ROGUE_RESULT took a disassembly to follow (2026-09-23).
const char* state_name(int s) {
  static const char* const kNames[] = {"INVALID", "INITIALIZE", "WAKE_UP", "TITLE", "LOAD_GAME_DATA",
                                       "IN_GAME_INITIALIZE", "IN_GAME", "GAME_OVER", "GAME_OVER_TO_RESET_TO_WAKE_UP",
                                       "PAUSE", "STAFFROLL", "STAFFROLL_TO_RESET_TO_WAIT", "WAIT_STAFFROLL", "ENDING",
                                       "RESULT", "RESULT_EXTRA", "RESET_TO_WAKE_UP", "RESET_TO_TITLE", "RESET_TO_GAME",
                                       "RESET_TO_STAFFROLL", "RESET_TO_WAIT_STAFFROLL", "RESET_TO_RESULT",
                                       "RESET_TO_RESULT_EXTRA", "RESET_TO_EXTRAMENU", "RESET_TO_EXTRA_CONTINUE_4",
                                       "RESET_TO_EXTRA_CONTINUE_T", "RESET_TITLE_TO_WAKE_UP", "RESET_TO_ROGUE_BASE",
                                       "RESET_TO_ROGUE_IN_GAME", "ROGUE_BASEAREA", "ROGUE_RESULT", "LMODE_SETUP",
                                       "RESET_TO_LMODE_INGAME", "RESET_TO_RESULT_ROGUE", "FORCE_TITLE_VS",
                                       "DEVELOP_INITIALIZE", "DEVELOP_LOAD_WAIT", "DEVELOP_FORCE_RESET_INGAME",
                                       "DEVELOP_SCENARIO_JUMP"};
  if (s >= 0 && s < static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return kNames[s];
  return s < 0 ? "unknown" : "other";
}

// --- players and enemies ---------------------------------------------------------------------------
int players(Player* out, int max) {
  if (!g_parts.players) return 0;
  const uintptr_t pm = instance(i_player_mgr, t_player_mgr);
  re::List l;
  if (!pm || !re::read_list(ref(pm, f_player_list), &l)) return 0;
  int n = 0;
  for (int i = 0; i < l.count && n < max; ++i) {
    const uintptr_t cond = re::list_ref(l, i);
    if (!cond || !re::is_a(cond, t_survivor_cond)) continue;
    Player p;
    p.cond = cond;
    p.hpc = ref(cond, f_cond_hpc);
    if (!p.hpc || !re::is_a(p.hpc, t_hpc)) continue;
    int32_t v = 0;
    if (!get(p.hpc, f_hp_cur, &v)) continue;
    p.hp = v;
    if (get(p.hpc, f_hp_def, &v)) p.max_hp = v;
    uint8_t b = 0;
    if (f_cond_poison.valid() && get(cond, f_cond_poison, &b)) p.poisoned = b != 0;
    if (f_cond_survivor.valid() && get(cond, f_cond_survivor, &v)) p.survivor = v;
    out[n++] = p;
  }
  return n;
}

int enemies(Enemy* out, int max) {
  if (!g_parts.enemies) return 0;
  const uintptr_t em = instance(i_enemy_mgr, t_enemy_mgr);
  re::List l;
  if (!em || !re::read_list(ref(em, f_active_enemies), &l)) return 0;
  int n = 0;
  for (int i = 0; i < l.count && n < max; ++i) {
    const uintptr_t ctl = re::list_ref(l, i);
    if (!ctl || !re::is_a(ctl, t_enemy_ctl)) continue;
    Enemy e;
    e.ctl = ctl;
    e.hpc = ref(ctl, f_enemy_hp);
    if (!e.hpc || !re::is_a(e.hpc, t_hpc)) continue;
    int32_t v = 0;
    if (!get(e.hpc, f_hp_cur, &v)) continue;
    e.hp = v;
    if (get(e.hpc, f_hp_def, &v)) e.max_hp = v;
    uint64_t stamp = 0;
    if (f_skip_restrict.valid() && get(ctl, f_skip_restrict, &stamp)) e.skip_restrict = stamp;
    uint8_t nd = 0;
    if (get(e.hpc, f_hp_nodamage, &nd)) e.no_damage = nd != 0;
    const uintptr_t flags = f_enemy_flags2.valid() ? ref(ctl, f_enemy_flags2) : 0;
    uint32_t set = 0;
    if (flags && re::is_a(flags, t_bitflag2) && get(flags, f_bitflag_flag, &set)) e.flags2 = set;
    out[n++] = e;
  }
  return n;
}

// --- records ------------------------------------------------------------------------------------------
int save_count() {
  if (!g_parts.records) return -1;
  const uintptr_t rm = instance(i_record, t_record);
  int32_t v = -1;
  return rm && get(rm, f_save_count, &v) ? v : -1;
}

int header_save_times() {
  if (!g_parts.header) return -1;
  const uintptr_t mf = instance(i_main_flow, t_main_flow);
  const uintptr_t h = mf ? ref(mf, f_header) : 0;
  int32_t v = -1;
  return h && re::is_a(h, t_header) && get(h, f_save_times, &v) ? v : -1;
}

bool set_save_count(int n) {
  bool ok = false;
  if (g_parts.records)
    if (const uintptr_t rm = instance(i_record, t_record)) ok = put<int32_t>(rm, f_save_count, n);
  if (g_parts.header) {
    const uintptr_t mf = instance(i_main_flow, t_main_flow);
    const uintptr_t h = mf ? ref(mf, f_header) : 0;
    if (h && re::is_a(h, t_header)) put<int32_t>(h, f_save_times, n);
  }
  return ok;
}

// Where each counter is. The event hooks ask on the game's threads - the
// inventory screen's every frame while its cheat is on - so the answer is kept:
// the manager and the object holding the counter are read again on each call
// (a static and a field), and the counter's address - re::field_addr, which
// takes re.cpp's critical section - is worked out again only when one of them
// changed. A new game or a load gives RecordManager new save data.
namespace {
struct CounterSite {
  uintptr_t manager = 0;   // the singleton it was worked out from
  uintptr_t holder_at = 0; // where the manager keeps the object holding the counter (0: the manager holds it)
  uintptr_t holder = 0;    // that object
  uintptr_t at = 0;        // the counter
};
SRWLOCK g_counter_lock = SRWLOCK_INIT;
CounterSite g_counter_site[kCounters];
}  // namespace

static uintptr_t counter_at(Counter c) {
  const int i = static_cast<int>(c);
  const bool steps = c == Counter::kSteps;
  if (i < 0 || i >= kCounters) return 0;
  if (!(c == Counter::kItemBox ? g_parts.item_box_count : c == Counter::kHeals ? g_parts.heal_count : g_parts.steps))
    return 0;
  const uintptr_t mgr = steps ? instance(i_player_mgr, t_player_mgr) : instance(i_record, t_record);
  if (!mgr) return 0;
  AcquireSRWLockShared(&g_counter_lock);
  const CounterSite known = g_counter_site[i];
  ReleaseSRWLockShared(&g_counter_lock);
  uintptr_t holder = 0;
  if (known.manager == mgr && known.at &&
      (!known.holder_at || (mem::read_safe(known.holder_at, &holder) && holder == known.holder)))
    return known.at;
  CounterSite s;
  s.manager = mgr;
  if (steps) {
    s.at = re::field_addr(mgr, f_pedometer);
  } else {
    s.holder_at = re::field_addr(mgr, f_record_gsd);
    s.holder = s.holder_at ? mem::read_ptr(s.holder_at) : 0;
    if (!s.holder || !re::is_a(s.holder, t_record_gsd)) return 0;
    s.at = re::field_addr(s.holder, c == Counter::kItemBox ? f_open_item_box : f_use_heal);
  }
  if (!s.at) return 0;
  AcquireSRWLockExclusive(&g_counter_lock);
  g_counter_site[i] = s;
  ReleaseSRWLockExclusive(&g_counter_lock);
  return s.at;
}

int counter(Counter c) {
  const uintptr_t at = counter_at(c);
  int32_t v = -1;
  return at && mem::read_safe(at, &v) ? v : -1;
}

bool set_counter(Counter c, int v) {
  const uintptr_t at = v >= 0 ? counter_at(c) : 0;
  return at && mem::store<int32_t>(at, v);
}

int hold_counter(Counter c, int held) {
  const uintptr_t at = held >= 0 ? counter_at(c) : 0;
  int32_t v = -1;
  if (!at || !mem::read_safe(at, &v) || v == held) return -1;
  return mem::store<int32_t>(at, held) ? v : -1;
}

int steps_limit() {
  // The record table is the game's user data, built once as RecordManager wakes:
  // read until it answers, then kept (the game tick's).
  static int s_limit = -1;
  if (s_limit > 0 || !g_parts.steps || !f_record_nodes.valid() || !f_node_progress.valid() || !f_node_clear.valid())
    return s_limit;
  const uintptr_t rm = instance(i_record, t_record);
  re::List l;
  if (!rm || !re::read_list(ref(rm, f_record_nodes), &l)) return -1;
  for (int i = 0; i < l.count && i < 1024; ++i) {
    const uintptr_t node = re::list_ref(l, i);
    int32_t type = -1, limit = -1;
    if (node && re::is_a(node, t_record_node) && get(node, f_node_progress, &type) && type == k_walk &&
        get(node, f_node_clear, &limit) && limit > 0) {
      s_limit = limit;
      logf("game: the step record allows %d steps (RecordManager's record %d)", limit, i);
      break;
    }
  }
  return s_limit;
}

// --- the clock ----------------------------------------------------------------------------------------
static uintptr_t clock_data() {
  const uintptr_t c = instance(i_clock, t_clock);
  const uintptr_t d = c ? ref(c, f_clock_data) : 0;
  return d && re::is_a(d, t_clock_gsd) ? d : 0;
}

bool read_clock(Clock* out) {
  if (!g_parts.clock) return false;
  const uintptr_t d = clock_data();
  Clock c;
  if (!d || !get(d, f_elapsed, &c.elapsed) || !get(d, f_demo, &c.demo) || !get(d, f_inv_time, &c.inventory) ||
      !get(d, f_pause_time, &c.pause))
    return false;
  *out = c;
  return true;
}

bool set_elapsed(uint64_t elapsed) {
  const uintptr_t d = g_parts.clock ? clock_data() : 0;
  return d && put<uint64_t>(d, f_elapsed, elapsed);
}

bool set_inventory_time(uint64_t us) {
  const uintptr_t d = g_parts.clock ? clock_data() : 0;
  return d && put<uint64_t>(d, f_inv_time, us);
}

// Freeze play time: the clock is lowered so get_ActualRecordTime reads `held_us`,
// and only for as long as the game is reading it for the record. What is taken
// out is given back by release_record_time - never left in, because the enemies
// run on this clock (enemy_time).
//   The inventory's time moves with it. The enemies' clock is
// elapsed - (demo + inventory + pause), so taking T out of the elapsed time alone
// takes T off their clock as well - and the save the hold is for carries the pair,
// which is how a loaded game began with every enemy frozen (game.h, Held). Taking
// the same T out of the inventory's time leaves their clock exactly where it was,
// live and in the file. It is clamped at the inventory's own time: a hold far
// below the clock then leaves the enemies the clock a game just begun has, which
// is a clock that moves.
Held hold_record_time(uint64_t held_us) {
  Clock c;
  if (!read_clock(&c)) return {};
  const Held want = hold_by(c, held_us);  // the arithmetic: game.h, tests/test_clock.cpp
  if (!want.any() || !set_elapsed(less_by(c.elapsed, want.elapsed))) return {};
  Held h;
  h.elapsed = want.elapsed;
  // The elapsed time is the hold; the inventory's only keeps the enemies' clock
  // where it was, so the hold stands even if that write does not.
  if (want.inventory && set_inventory_time(less_by(c.inventory, want.inventory))) h.inventory = want.inventory;
  return h;
}

bool release_record_time(const Held& taken) {
  if (!taken.any()) return true;
  Clock c;
  // Read again: the clock kept counting while it was held, and that time is kept.
  if (!read_clock(&c)) return false;
  bool ok = true;
  if (taken.elapsed) ok = set_elapsed(less_by(c.elapsed, -taken.elapsed));
  if (taken.inventory) ok = set_inventory_time(less_by(c.inventory, -taken.inventory)) && ok;
  return ok;
}

// A clock the enemies cannot run on, healed (game.h, repaired).
uint64_t repair_enemy_clock() {
  Clock c;
  if (!read_clock(&c)) return 0;
  if (!enemies_stuck(c)) return 0;  // their clock moves
  const uint64_t behind = c.demo + c.inventory + c.pause - c.elapsed;
  const Clock r = repaired(c);
  if (!set_inventory_time(r.inventory)) {
    logf("ERROR: the enemies' clock is %.1f s behind (the inventory's time is above the elapsed time) "
         "and could not be put right",
         static_cast<double>(behind) / 1e6);
    return 0;
  }
  if (r.elapsed != c.elapsed) set_elapsed(r.elapsed);
  return behind;
}

uint64_t clear_time() {
  const uintptr_t r = instance(i_record, t_record);
  uint64_t us = 0;
  return r && f_clear_time.valid() && get(r, f_clear_time, &us) ? us : 0;
}

bool set_clear_time(uint64_t us) {
  const uintptr_t r = instance(i_record, t_record);
  return r && f_clear_time.valid() && put<uint64_t>(r, f_clear_time, us);
}

// --- items --------------------------------------------------------------------------------------------------
bool read_stock(uintptr_t data_obj, Stock* out) {
  if (!data_obj || !re::is_a(data_obj, t_item_data)) return false;
  const uintptr_t prim = ref(data_obj, f_default_item);
  if (!prim || !re::is_a(prim, t_primitive)) return false;
  Stock s;
  s.obj = data_obj;
  s.prim = prim;
  int32_t v = 0;
  if (!get(prim, f_p_item, &v)) return false;
  s.item.item_id = v;
  if (!get(prim, f_p_weapon, &v)) return false;
  s.item.weapon_id = v;
  if (!get(prim, f_p_parts, &v)) return false;
  s.item.parts = v;
  if (!get(prim, f_p_bullet, &v)) return false;
  s.item.bullet_id = v;
  if (!get(prim, f_p_count, &v)) return false;
  s.item.count = v;
  if (re::is_a(data_obj, t_stock_item) && get(data_obj, f_stock_index, &v)) s.index = v;
  *out = s;
  return true;
}

bool write_item(const Stock& s, const Item& v) {
  // The stock must still hold this very DefaultItem: a slot the game refilled
  // since it was read is left alone.
  if (!s.obj || !re::is_a(s.obj, t_item_data) || ref(s.obj, f_default_item) != s.prim || !re::is_a(s.prim, t_primitive))
    return false;
  return put<int32_t>(s.prim, f_p_item, v.item_id) && put<int32_t>(s.prim, f_p_weapon, v.weapon_id) &&
         put<int32_t>(s.prim, f_p_parts, v.parts) && put<int32_t>(s.prim, f_p_bullet, v.bullet_id) &&
         put<int32_t>(s.prim, f_p_count, v.count);
}

void read_fat_rule(FatRule* out) {
  out->n_items = out->n_weapons = 0;
  out->parts = k_fat_parts;
  const uintptr_t at = g_parts.fat ? re::static_addr(f_fat_data) : 0;
  const uintptr_t fat = at ? mem::read_ptr(at) : 0;
  if (!fat || !re::is_a(fat, t_fat)) return;
  const int max = static_cast<int>(sizeof(out->items) / sizeof(out->items[0]));
  read_int_list(ref(fat, f_fat_items), out->items, max, &out->n_items);
  read_int_list(ref(fat, f_fat_weapons), out->weapons, max, &out->n_weapons);
}

bool read_bag(Bag* out) {
  if (!g_parts.bag) return false;
  const uintptr_t im = instance(i_inv_mgr, t_inv_mgr);
  const uintptr_t inv = im ? ref(im, f_cur_inventory) : 0;
  if (!inv || !re::is_a(inv, t_inventory)) return false;
  int32_t size = 0;
  re::List l;
  if (!get(inv, f_inv_size, &size) || size < 0 || size > kMaxSlots || !re::read_list(ref(inv, f_inv_slots), &l)) return false;
  Bag b;
  b.manager = im;
  b.inventory = inv;
  b.size = size;
  b.slots = l.count < kMaxSlots ? l.count : kMaxSlots;
  // Read in list order, then filed under the game's own numbers: the slots in
  // play must be numbered 0..size-1, once each (see Bag); the slots the player
  // has not got yet follow in list order.
  Stock listed[kMaxSlots];
  bool fb_listed[kMaxSlots] = {};
  for (int i = 0; i < b.slots; ++i) {
    const uintptr_t slot = re::list_ref(l, i);
    if (!slot || !re::is_a(slot, t_slot)) continue;
    uint8_t fb = 0;
    if (f_slot_force_blank.valid() && get(slot, f_slot_force_blank, &fb)) fb_listed[i] = fb != 0;
    read_stock(ref(slot, f_slot_stock), &listed[i]);
  }
  int number[kMaxSlots], pos[kMaxSlots];
  for (int i = 0; i < b.slots; ++i) number[i] = listed[i].prim ? listed[i].index : -1;
  b.numbered = file_slots(number, b.slots, size, pos);
  for (int i = 0; i < b.slots; ++i) {
    const int at = pos[i];
    if (at < 0 || at >= kMaxSlots) continue;
    b.slot[at] = listed[i];
    b.force_blank[at] = fb_listed[i];
    b.list_pos[at] = i;
  }
  static bool s_logged_unnumbered = false;
  if (!b.numbered && !s_logged_unnumbered) {
    s_logged_unnumbered = true;
    logf("game: the inventory's slots in play are not numbered 0..%d - slots are taken in the list's order (the "
         "game's item box calls are refused while that lasts)", size - 1);
  }
  static FatRule fat;  // the game tick's alone
  read_fat_rule(&fat);
  for (int i = 0; i < b.slots; ++i) b.fat[i] = b.slot[i].prim && fat_by(fat, b.slot[i].item);
  dead_slots(b.fat, b.slots, size, k_columns, b.dead);
  *out = b;
  return true;
}

int inventory_columns() { return k_columns; }

static uintptr_t box_list() {
  const uintptr_t lm = instance(i_locker_mgr, t_locker_mgr);
  const uintptr_t gsd = lm ? ref(lm, f_locker_gsd) : 0;
  const uintptr_t save = gsd && re::is_a(gsd, t_locker_gsd) ? ref(gsd, f_locker_save) : 0;
  return save && re::is_a(save, t_locker_save) ? ref(save, f_locker_items) : 0;
}

bool read_box(Box* out) {
  if (!g_parts.box) return false;
  re::List l;
  const uintptr_t list = box_list();
  if (!list || !re::read_list(list, &l)) return false;
  out->list = list;
  out->count = l.count < kMaxBox ? l.count : kMaxBox;
  out->capacity = l.items.count;
  for (int i = 0; i < out->count; ++i) {
    out->entry[i] = Stock{};
    read_stock(re::list_ref(l, i), &out->entry[i]);
  }
  return true;
}

bool box_remove(int index, uintptr_t expect_obj) {
  re::List l;
  const uintptr_t list = box_list();
  if (!list || !re::read_list(list, &l) || index < 0 || index >= l.count || l.items.elem_size != 8) return false;
  if (re::list_ref(l, index) != expect_obj) return false;
  const re::Field size_f = re::find_field(re::type_of(list), "mSize");
  const uintptr_t size_at = re::field_addr(list, size_f);
  if (!size_at) return false;
  // Exactly what List<T>.RemoveAt does: shift down, clear the tail, shrink.
  for (int i = index; i + 1 < l.count; ++i)
    if (!mem::store<uintptr_t>(l.items.data + static_cast<uintptr_t>(i) * 8,
                               mem::read<uintptr_t>(l.items.data + static_cast<uintptr_t>(i + 1) * 8)))
      return false;
  mem::store<uintptr_t>(l.items.data + static_cast<uintptr_t>(l.count - 1) * 8, 0);
  return mem::store<int32_t>(size_at, l.count - 1);
}

bool is_fat(const Item& it) {
  static FatRule fat;  // the game tick's alone
  read_fat_rule(&fat);
  return fat_by(fat, it);
}

// --- stacks -------------------------------------------------------------------------------------------------------------
// The item's ItemElement in ItemManager.<ItemElementTable>, 0 when absent: the
// dictionary's entries in order, the removed ones (a negative hash code) skipped.
static uintptr_t item_element(int item_id) {
  const uintptr_t im = instance(i_item_mgr, t_item_mgr);
  const uintptr_t table = im ? ref(im, f_item_table) : 0;
  int32_t count = 0;
  re::Array entries;
  if (!table || !re::is_a(table, t_item_table) || !get(table, f_table_count, &count) ||
      !re::read_array(ref(table, f_table_entries), &entries) || entries.elem_size < f_entry_value.offset + 8) {
    static bool s_logged = false;  // the game tick's
    if (!s_logged) {
      s_logged = true;
      logf("game: ItemManager.<ItemElementTable> could not be read (manager %p, table %p, entries of %u bytes) - "
           "Store does not stack items", reinterpret_cast<void*>(im), reinterpret_cast<void*>(table), entries.elem_size);
    }
    return 0;
  }
  for (int i = 0; i < count && i < entries.count && i < 4096; ++i) {
    const uintptr_t e = entries.data + static_cast<uintptr_t>(i) * entries.elem_size;
    int32_t hash = -1, key = 0;
    if (!mem::read_safe(re::value_field_addr(e, f_entry_hash), &hash) || hash < 0 ||
        !mem::read_safe(re::value_field_addr(e, f_entry_key), &key) || key != item_id)
      continue;
    const uintptr_t el = mem::read_ptr(re::value_field_addr(e, f_entry_value));
    return el && re::is_a(el, t_item_element) ? el : 0;
  }
  return 0;
}

int stack_max(int item_id) {
  if (!g_parts.stacks || item_id <= 0 || item_id >= kItemIds || g_item_max[item_id] <= 0) return 0;
  const uintptr_t el = item_element(item_id);
  int32_t disposable = -1;
  return el && get(el, f_elem_disposable, &disposable) && disposable == k_multiple_use ? g_item_max[item_id] : 0;
}

// --- durability -------------------------------------------------------------------------------------------------------
bool is_melee_weapon(int weapon_id) {
  return weapon_id > 0 && weapon_id < static_cast<int>(sizeof(g_weapon_category)) && g_weapon_category[weapon_id] == k_knife;
}

// What one weapon's entry in WeaponBulletUserData says for the parts it has:
// how many rounds it holds (`count`, -1 unlimited) and which ammo it takes
// (`kind`, an EquipmentDefine.Bullet; 0 when the entry names none - a knife, a
// grenade). Among the entries whose parts the weapon has, the lowest _Priority
// that overwrites the figure wins, each figure on its own
// (WeaponLoadingpartsCombination.getNumber and getKind).
static void read_combo(int weapon_id, int parts, int* count, uint32_t* kind) {
  *count = 0;
  *kind = 0;
  const uintptr_t em = instance(i_equip_mgr, t_equip_mgr);
  const uintptr_t ud = em ? ref(em, f_bullet_ud) : 0;
  re::Array weapons;
  if (!ud || !re::is_a(ud, t_bullet_ud) || !re::read_array(ref(ud, f_bullet_combos), &weapons) || weapons.elem_size != 8)
    return;
  for (int i = 0; i < weapons.count && i < 1024; ++i) {
    const uintptr_t wc = mem::read_ptr(weapons.data + static_cast<uintptr_t>(i) * 8);
    int32_t type = -1;
    if (!wc || !re::is_a(wc, t_weapon_combo) || !get(wc, f_wc_type, &type) || type != weapon_id) continue;
    re::Array combos;
    if (!re::read_array(ref(wc, f_wc_combos), &combos) || combos.elem_size != 8) return;
    int32_t best = INT32_MAX, best_kind = INT32_MAX;
    for (int k = 0; k < combos.count && k < 256; ++k) {
      const uintptr_t pc = mem::read_ptr(combos.data + static_cast<uintptr_t>(k) * 8);
      uint32_t cparts = 0;
      int32_t prio = 0, number = 0;
      uint8_t overwrite = 0, infinity = 0;
      if (!pc || !re::is_a(pc, t_parts_combo) || !get(pc, f_pc_parts, &cparts) || !get(pc, f_pc_priority, &prio) ||
          !get(pc, f_pc_overwrite, &overwrite) || !get(pc, f_pc_infinity, &infinity) || !get(pc, f_pc_number, &number))
        continue;
      if ((static_cast<uint32_t>(parts) & cparts) != cparts) continue;
      if (overwrite && prio < best) {
        best = prio;
        *count = infinity ? -1 : number;
      }
      uint8_t overwrite_kind = 0;
      uint32_t this_kind = 0;
      if (f_pc_overwrite_kind.valid() && f_pc_kind.valid() && get(pc, f_pc_overwrite_kind, &overwrite_kind) &&
          overwrite_kind && prio < best_kind && get(pc, f_pc_kind, &this_kind)) {
        best_kind = prio;
        *kind = this_kind;
      }
    }
    return;
  }
}

// Both figures are read together and kept: the user data does not change. The
// tick and the game's threads (a knife's hit) both ask.
struct KnownWeapon {
  int weapon, parts, count, bullet;
};
static KnownWeapon g_known[64];
static int g_known_n = 0;

static KnownWeapon weapon_info(int weapon_id, int parts) {
  AcquireSRWLockShared(&g_full_lock);
  KnownWeapon k{weapon_id, parts, 0, 0};
  bool found = false;
  for (int i = 0; i < g_known_n && !found; ++i)
    if (g_known[i].weapon == weapon_id && g_known[i].parts == parts) k = g_known[i], found = true;
  ReleaseSRWLockShared(&g_full_lock);
  if (found) return k;
  uint32_t kind = 0;
  read_combo(weapon_id, parts, &k.count, &kind);
  // The ammo item of that kind, through the game's own EquipmentDefine.getItemID.
  // A weapon that takes more than one kind has them as one set of flags, which
  // that method does not name: the lowest kind it does name is the one a fresh
  // weapon is given, as the game's own weapon data lists its plain ammo first.
  if (kind && g_bullet_item_code) {
    const mem::Range text = mem::section(GetModuleHandleA(nullptr), ".text");
    const mem::Range image = mem::module_range(GetModuleHandleA(nullptr));
    auto item_of = [&](uint32_t k2) {
      int32_t id = 0;
      return code::eval_switch(
                 g_bullet_item_code, 2, static_cast<uint64_t>(k2),
                 [](uintptr_t at, uint8_t* out, size_t n) { return mem::copy_from(out, at, n); },
                 [&](uintptr_t at) { return text.contains(at); }, [&](uintptr_t at) { return image.contains(at); }, &id) &&
                     id > 0
                 ? id
                 : 0;
    };
    k.bullet = item_of(kind);
    for (uint32_t bit = 1; !k.bullet && bit; bit <<= 1)
      if (kind & bit) k.bullet = item_of(bit);
  }
  if (k.count == 0 && k.bullet == 0) return k;  // nothing read: ask again later
  AcquireSRWLockExclusive(&g_full_lock);
  bool again = false;
  for (int i = 0; i < g_known_n && !again; ++i) again = g_known[i].weapon == weapon_id && g_known[i].parts == parts;
  if (!again && g_known_n < static_cast<int>(sizeof(g_known) / sizeof(g_known[0]))) g_known[g_known_n++] = k;
  ReleaseSRWLockExclusive(&g_full_lock);
  if (!again) logf("game: weapon %d (parts 0x%X) is full at %d, loaded with item %d", weapon_id, parts, k.count, k.bullet);
  return k;
}

int weapon_full_count(int weapon_id, int parts) { return weapon_info(weapon_id, parts).count; }

int weapon_bullet_id(int weapon_id, int parts) { return weapon_info(weapon_id, parts).bullet; }

// --- difficulty -------------------------------------------------------------------------------------------------------
static uintptr_t header() {
  const uintptr_t mf = g_parts.header ? instance(i_main_flow, t_main_flow) : 0;
  const uintptr_t h = mf ? ref(mf, f_header) : 0;
  return h && re::is_a(h, t_header) ? h : 0;
}

Difficulty difficulty() {
  const uintptr_t h = f_difficulty.valid() ? header() : 0;
  int32_t d = -1;
  if (!h || !get(h, f_difficulty, &d)) return Difficulty::kUnknown;
  return d == k_easy     ? Difficulty::kAssisted
         : d == k_normal ? Difficulty::kStandard
         : d == k_hard   ? Difficulty::kHardcore
                         : Difficulty::kUnknown;
}

const char* difficulty_name(Difficulty d) {
  switch (d) {
    case Difficulty::kAssisted: return "Assisted";
    case Difficulty::kStandard: return "Standard";
    case Difficulty::kHardcore: return "Hardcore";
    default: return "?";
  }
}

int difficulty_value(Difficulty d) {
  switch (d) {
    case Difficulty::kAssisted: return k_easy;
    case Difficulty::kStandard: return k_normal;
    case Difficulty::kHardcore: return k_hard;
    default: return -1;
  }
}

uintptr_t difficulty_owner() {
  if (!g_parts.difficulty || !header()) return 0;
  const uintptr_t mf = instance(i_main_flow, t_main_flow);
  return mf && instance(i_user_data_mgr, t_user_data_mgr) ? mf : 0;
}

int force_easy_continue() {
  const uintptr_t mf = g_parts.difficulty ? instance(i_main_flow, t_main_flow) : 0;
  int32_t v = -1;
  return mf && get(mf, f_force_easy, &v) ? v : -1;
}

bool set_force_easy_continue(int v) {
  const uintptr_t mf = g_parts.difficulty ? instance(i_main_flow, t_main_flow) : 0;
  return mf && put<int32_t>(mf, f_force_easy, v);
}

// --- typewriters ------------------------------------------------------------------------------------------------------
bool hardcore() { return difficulty() == Difficulty::kHardcore; }


int ink_ribbon_id() { return k_ink; }
int wooden_boards_id() { return k_boards; }

// --- diagnostics ----------------------------------------------------------------------------------------------------
static void describe_item(const Item& it, char* out, size_t n) {
  const char* name = nullptr;
  if (it.weapon_id > 0) {
    for (const auto& w : items::kWeapons)
      if (w.id == it.weapon_id) name = w.name;
  } else if (it.item_id > 0) {
    for (const auto& d : items::kItems)
      if (d.id == it.item_id) name = d.label;
  }
  std::snprintf(out, n, "item %d weapon %d parts %d bullet %d count %d%s%s%s", it.item_id, it.weapon_id, it.parts,
                it.bullet_id, it.count, name ? " (" : "", name ? name : "", name ? ")" : "");
}

void log_bag(const char* why) {
  Bag* b = new Bag;
  if (!read_bag(b)) {
    logf("inventory (%s): not readable", why);
    delete b;
    return;
  }
  logf("inventory (%s): %d of %d slot(s) in play, list at %p, inventory object fields from +0x%X", why, b->size,
       b->slots, reinterpret_cast<void*>(b->inventory), re::fieldptr_offset(b->inventory));
  for (int i = 0; i < b->slots; ++i) {
    char d[160];
    describe_item(b->slot[i].item, d, sizeof(d));
    logf("  slot %2d (list %2d, Index %2d)%s%s%s: stock %p (%s) prim %p: %s", i + 1, b->list_pos[i] + 1,
         b->slot[i].index, b->force_blank[i] ? " [force blank]" : "", b->fat[i] ? " [two slots]" : "",
         b->dead[i] ? " [second half]" : "",
         reinterpret_cast<void*>(b->slot[i].obj), b->slot[i].obj ? re::full_name(re::type_of(b->slot[i].obj)).c_str() : "-",
         reinterpret_cast<void*>(b->slot[i].prim), d);
  }
  delete b;
}

void log_box(const char* why) {
  Box* b = new Box;
  if (!read_box(b)) {
    logf("item box (%s): not readable", why);
    delete b;
    return;
  }
  int blank = 0;
  for (int i = 0; i < b->count; ++i) blank += is_blank(b->entry[i].item);
  logf("item box (%s): %d entr%s (%d blank), array capacity %d", why, b->count, b->count == 1 ? "y" : "ies", blank,
       b->capacity);
  for (int i = 0; i < b->count && i < 60; ++i) {
    char d[160];
    describe_item(b->entry[i].item, d, sizeof(d));
    logf("  box %3d: %s %p: %s", i, b->entry[i].obj ? re::full_name(re::type_of(b->entry[i].obj)).c_str() : "-",
         reinterpret_cast<void*>(b->entry[i].obj), d);
  }
  delete b;
}

// Trace: the enemies against the clock they run on. EnemyController.canRestrictMove
// holds an enemy still while its SkipRestrictMoveFrame is *equal* to
// GameClock.get_ActualPlayingTime, so a clock that stopped (Freeze play time, or the
// editor set backwards) pins every enemy stamped in that instant - what this prints.
void log_enemies(const Clock& c, bool detail) {
  Enemy e[32];
  const int n = enemies(e, 32);
  const uint64_t now = enemy_time(c);
  uint8_t measuring = 2;
  if (const uintptr_t f = clock_measure_flag(instance(i_clock, t_clock))) mem::read_safe(f, &measuring);
  // The flag is cleared only for the clock's own update, so a tick almost always
  // reads it back at 1: what tells a stopped clock is that it did not move since
  // the last dump (which is also how a play time set backwards shows).
  static uint64_t s_was = 0, s_at = 0;
  const uint64_t at = GetTickCount64();
  int held_still = 0;
  for (int i = 0; i < n; ++i) held_still += f_skip_restrict.valid() && e[i].skip_restrict == now;
  logf("enemies: %d active, %d held still by canRestrictMove; their clock (elapsed less cutscenes, inventory "
       "and pauses) is %.3f s, _MeasureGameElapsedTime %d; it moved %.3f s in the %.1f s since the last dump "
       "(elapsed %.1f s, cutscenes %.1f, inventory %.1f, pauses %.1f)",
       n, held_still, static_cast<double>(now) / 1e6, measuring,
       s_at ? static_cast<double>(now - s_was) / 1e6 : 0.0, s_at ? (at - s_at) / 1000.0 : 0.0,
       static_cast<double>(c.elapsed) / 1e6, static_cast<double>(c.demo) / 1e6,
       static_cast<double>(c.inventory) / 1e6, static_cast<double>(c.pause) / 1e6);
  s_was = now;
  s_at = at;
  for (int i = 0; detail && i < n; ++i) {
    char flags[96];
    std::snprintf(flags, sizeof(flags), "%s%s%s%s", e[i].flags2 & g_stop_damage ? " STOP_DAMAGE" : "",
                  e[i].flags2 & g_keep_alive & ~g_stop_damage ? " GUTS_MODE" : "",
                  e[i].flags2 & g_no_death ? " NO_DEATH" : "", e[i].no_damage ? " NoDamage" : "");
    const bool held = f_skip_restrict.valid() && e[i].skip_restrict == now;
    logf("  enemy %d: %s hp %d/%d, flags 0x%X%s, SkipRestrictMoveFrame %.3f s%s", i,
         re::full_name(re::type_of(e[i].ctl)).c_str(), e[i].hp, e[i].max_hp, e[i].flags2, flags,
         static_cast<double>(e[i].skip_restrict) / 1e6,
         held ? "  <- EQUAL to their clock: canRestrictMove holds this one still" : "");
  }
}

void trace_tick(int state) {
  static int s_state = -2;
  if (state == s_state) return;
  logf("game: main state %d (%s) -> %d (%s)", s_state, state_name(s_state), state, state_name(state));
  const bool entering_pause = state == k_pause && s_state != k_pause;
  s_state = state;
  if (!entering_pause) return;
  // The enemies against the clock they run on: one line every time the pause menu
  // opens, whatever Trace says, so a session that goes wrong is on the record.
  Clock ec;
  if (read_clock(&ec)) log_enemies(ec, config::get().trace);
  if (!config::get().trace) return;
  log_bag("pause menu opened");
  log_box("pause menu opened");
  Player p[4];
  const int n = players(p, 4);
  for (int i = 0; i < n; ++i)
    logf("player %d: survivor %d hp %d/%d poisoned %d (condition fields from +0x%X, hp from +0x%X)", i, p[i].survivor,
         p[i].hp, p[i].max_hp, p[i].poisoned, re::fieldptr_offset(p[i].cond), re::fieldptr_offset(p[i].hpc));
  Clock c;
  if (read_clock(&c))
    logf("clock: elapsed %llu demo %llu inventory %llu pause %llu -> play time %.1f s",
         static_cast<unsigned long long>(c.elapsed), static_cast<unsigned long long>(c.demo),
         static_cast<unsigned long long>(c.inventory), static_cast<unsigned long long>(c.pause),
         static_cast<double>(play_time(c)) / 1e6);
  logf("save count: RecordManager %d, the game header's SaveTimes %d", save_count(), header_save_times());
  logf("records: item box opened %d, recovery items used %d, steps %d (the step record allows %d)",
       counter(Counter::kItemBox), counter(Counter::kHeals), counter(Counter::kSteps), steps_limit());
  logf("difficulty: %s, continue on Assisted held %d", difficulty_name(difficulty()), force_easy_continue());
}

// --- the event hooks' work (events.cpp) ----------------------------------------------------------------------------
const EventTargets& event_targets() { return g_events; }
int pause_state() { return k_pause; }

uintptr_t enemy_hit_points(uintptr_t enemy) {
  const uintptr_t hpc = g_parts.enemies && enemy ? ref(enemy, f_enemy_hp) : 0;
  return hpc && re::is_a(hpc, t_hpc) ? hpc : 0;
}

bool reaction_targets(uintptr_t reaction, uintptr_t* hpc, uintptr_t* enemy) {
  *hpc = reaction && f_reaction_hp.valid() ? ref(reaction, f_reaction_hp) : 0;
  *enemy = reaction && f_reaction_enemy.valid() ? ref(reaction, f_reaction_enemy) : 0;
  if (*hpc && !re::is_a(*hpc, t_hpc)) *hpc = 0;
  return *hpc != 0;
}

// Any of `bits` set in the enemy's EnemyDefine.ConditionStateBitFlag2: its state
// says the game is holding this hit off for now.
bool enemy_flagged(uintptr_t enemy, uint32_t bits) {
  if (!enemy || !bits || !f_enemy_flags2.valid() || !f_bitflag_flag.valid()) return false;
  const uintptr_t flags = ref(enemy, f_enemy_flags2);
  uint32_t set = 0;
  return flags && re::is_a(flags, t_bitflag2) && get(flags, f_bitflag_flag, &set) && (set & bits) != 0;
}

int make_lethal(uintptr_t hpc, uintptr_t enemy, uintptr_t info) {
  if (!g_parts.damage || !hpc || !info || !re::is_a(info, t_damage_info)) return -1;
  int32_t damage = 0, hp = 0;
  uint8_t no_damage = 0, kill = 0;
  if (!get(info, f_di_damage, &damage) || damage <= 0 || !get(hpc, f_hp_cur, &hp) || hp <= 0) return -1;
  if (get(hpc, f_hp_nodamage, &no_damage) && no_damage) return -1;
  // No hit takes the HP of one the game holds (STOP_DAMAGE, or GUTS_MODE: never below 1). NO_DEATH
  // does not hold it: the HP goes as to any hit, and the game decides what 0 means (Mr. X kneels; a
  // set piece's enemy dies when the set piece lets it go).
  if (enemy_flagged(enemy, g_keep_alive)) return -1;
  if (get(info, f_di_kill, &kill) && kill && hp <= damage) return -1;  // lethal already (or made so by the other handler)
  if (hp > damage && !put<int32_t>(hpc, f_hp_cur, damage)) return -1;
  put<uint8_t>(info, f_di_kill, 1);
  return hp;
}

int wither_plant(uintptr_t enemy, uintptr_t hpc, uintptr_t info) {
  if (!g_parts.plants || !enemy || !hpc || !info || !re::is_a(info, t_damage_info)) return kNotAPlant;
  const uintptr_t think = ref(enemy, f_enemy_think);
  if (!think || !re::is_a(think, t_em5000_think)) return kNotAPlant;  // any other enemy
  // From here the enemy is a plant, and is the game's to kill however this goes.
  // A hit the game is holding off (a grapple, a set piece) is left to play out.
  if (enemy_flagged(enemy, g_stop_damage)) return kPlantLeftAlone;
  int32_t damage = 0, hp = 0;
  if (!get(info, f_di_damage, &damage) || damage <= 0 || damage >= INT32_MAX - 1) return kPlantLeftAlone;
  if (!get(hpc, f_hp_cur, &hp) || hp <= 0) return kPlantLeftAlone;  // dead already: its burn-up is running
  const uintptr_t flame = ref(think, f_think_flame);
  int32_t left = 0;
  if (!flame || !re::is_a(flame, t_flame_hp) || !get(flame, f_flame_cur, &left)) return kPlantLeftAlone;
  // The plant's own onHitDamage tests the flame-only health before the
  // knockdown, so emptying it here burns the plant up on this hit, whatever hit
  // it and wherever - no glands, no fire, no lying there first.
  if (left > 0 && !put<int32_t>(flame, f_flame_cur, 0)) return kPlantLeftAlone;
  // That test is only reached while the plant is alive, and GUTS_MODE - what
  // normally holds a plant off 0 - is off once it has been down once
  // (setupFakeDead clears it and revival does not put it back). So the health
  // goes where the game's own addDamage lands it on 1, whatever the hit was
  // worth. forceDead takes it to 0 itself a moment later.
  if (!put<int32_t>(hpc, f_hp_cur, damage + 1)) return kPlantLeftAlone;
  return left > 0 ? kPlantBurned : kPlantLeftAlone;  // an empty one already: this hit started nothing
}

int stun_g2(uintptr_t enemy) {
  if (!g_parts.g2_stun || !enemy) return 0;
  const uintptr_t think = ref(enemy, f_enemy_think);
  if (!think || !re::is_a(think, t_em7100_think)) return 0;  // any other enemy: not this fight
  constexpr int32_t kNearlyTired = INT32_MAX / 2;
  int32_t tired = 0;
  if (!get(think, f_tired_hp, &tired) || tired >= kNearlyTired) return 0;
  return put<int32_t>(think, f_tired_hp, kNearlyTired) ? 1 : 0;
}

bool is_player(uintptr_t cond) { return t_player_cond && cond && re::is_a(cond, t_player_cond); }

int heal_player(uintptr_t cond) {
  if (!g_parts.players || !is_player(cond)) return -1;
  const uintptr_t hpc = ref(cond, f_cond_hpc);
  int32_t hp = 0, full = 0;
  if (!hpc || !re::is_a(hpc, t_hpc) || !get(hpc, f_hp_cur, &hp) || !get(hpc, f_hp_def, &full)) return -1;
  // A living player only: a blow that already took everything is the game's to play out.
  if (hp > 0 && full > 0 && hp < full && put<int32_t>(hpc, f_hp_cur, full)) hp = full;
  return hp;
}

bool cure_poison(uintptr_t cond) {
  uint8_t poisoned = 0;
  return f_cond_poison.valid() && is_player(cond) && get(cond, f_cond_poison, &poisoned) && poisoned &&
         put<uint8_t>(cond, f_cond_poison, 0);
}

uintptr_t survivor_of(uintptr_t equipment_or_weapon) {
  if (!g_parts.equipped || !equipment_or_weapon) return 0;
  uintptr_t cond = 0;
  if (t_equipment && re::is_a(equipment_or_weapon, t_equipment)) cond = ref(equipment_or_weapon, f_eq_condition);
  else if (t_weapon && f_weapon_condition.valid() && re::is_a(equipment_or_weapon, t_weapon))
    cond = ref(equipment_or_weapon, f_weapon_condition);
  return cond && re::is_a(cond, t_survivor_cond) ? cond : 0;
}

bool equipped_stock(uintptr_t survivor, bool sub, Stock* out) {
  if (!g_parts.equipped || !survivor) return false;
  const uintptr_t inv = ref(survivor, f_cond_inventory);
  const uintptr_t slot = inv && re::is_a(inv, t_inventory) ? ref(inv, sub ? f_inv_sub_slot : f_inv_main_slot) : 0;
  return slot && re::is_a(slot, t_slot) && read_stock(ref(slot, f_slot_stock), out);
}

bool keep_knife_full(const Stock& s) {
  if (!s.prim || !is_weapon(s.item) || !is_melee_weapon(s.item.weapon_id)) return false;
  const int full = weapon_full_count(s.item.weapon_id, s.item.parts);
  if (full <= 0 || s.item.count >= full) return false;
  Item v = s.item;
  v.count = full;
  return write_item(s, v);
}

bool restore_count(const Stock& before) {
  Stock now;
  if (!before.obj || !read_stock(before.obj, &now) || now.prim != before.prim || now.item.weapon_id != before.item.weapon_id ||
      now.item.bullet_id != before.item.bullet_id || now.item.count < 0 || now.item.count >= before.item.count)
    return false;
  Item v = now.item;
  v.count = before.item.count;
  return write_item(now, v);
}

int cap_save_times(uintptr_t main_flow, int held) {
  if (!g_parts.header || held < 0 || !main_flow || !re::is_a(main_flow, t_main_flow)) return -1;
  const uintptr_t h = ref(main_flow, f_header);
  int32_t v = -1;
  if (!h || !re::is_a(h, t_header) || !get(h, f_save_times, &v) || v <= held) return -1;
  return put<int32_t>(h, f_save_times, held) ? v : -1;
}

uintptr_t countdown_timer(uintptr_t behaviour) {
  if (!g_parts.countdown || !behaviour) return 0;
  if (re::is_a(behaviour, t_countdown)) return re::field_addr(behaviour, f_cd_timer);
  if (t_rogue_countdown && f_rogue_timer.valid() && re::is_a(behaviour, t_rogue_countdown))
    return re::field_addr(behaviour, f_rogue_timer);
  return 0;
}

uintptr_t clock_measure_flag(uintptr_t clock) {
  return g_parts.measure && clock && re::is_a(clock, t_clock) ? re::field_addr(clock, f_clock_measure) : 0;
}

namespace {
// The boards item data this marked reusable, so that only those are ever
// unmarked (a window's item data is the scene's, and may be freed with its
// area: a remembered address is only compared, never read). Any thread.
constexpr int kBoardMarks = 64;
std::atomic<uintptr_t> g_board_marks[kBoardMarks];
std::atomic<unsigned> g_board_mark_next{0};
}  // namespace

int mark_boards_reusable(uintptr_t closure, bool keep) {
  if (!g_parts.boards || !closure) return 0;
  uintptr_t work = 0;
  for (int i = 0; i < 2 && !work; ++i)
    if (t_use_closure[i] && f_closure_work[i].valid() && re::is_a(closure, t_use_closure[i]))
      work = ref(closure, f_closure_work[i]);
  const uintptr_t data = work && re::is_a(work, t_item_work) ? ref(work, f_work_data) : 0;
  int32_t stock = -1, item = 0;
  uint8_t reuse = 0;
  if (!data || !re::is_a(data, t_use_data) || !get(data, f_use_stock, &stock) || stock != k_stock_item ||
      !get(data, f_use_item, &item) || item != k_boards || !get(data, f_use_reuse, &reuse))
    return 0;
  if (keep) {
    // Already reusable: the game's own (nothing to do), or marked at an earlier use.
    if (reuse || !put<uint8_t>(data, f_use_reuse, 1)) return 0;
    bool known = false;
    for (const auto& m : g_board_marks) known = known || m.load(std::memory_order_relaxed) == data;
    if (!known) g_board_marks[g_board_mark_next.fetch_add(1, std::memory_order_relaxed) % kBoardMarks].store(data);
    return 1;
  }
  if (!reuse) return 0;
  for (auto& m : g_board_marks) {
    uintptr_t expect = data;
    if (m.compare_exchange_strong(expect, 0)) return put<uint8_t>(data, f_use_reuse, 0) ? -1 : 0;
  }
  return 0;  // reusable as the game made it
}

void forget_board_marks() {
  for (auto& m : g_board_marks) m.store(0, std::memory_order_relaxed);
  g_board_mark_next.store(0, std::memory_order_relaxed);
}

}  // namespace re2cc::game
