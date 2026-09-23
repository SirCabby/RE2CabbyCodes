// Offline test of src/re.cpp against the real type database, run under Wine:
//
//   make -C tests re && wine tests/build/test_re.exe "Z:\...\re2.exe"
//
// re2.exe keeps its TDB in .data with array offsets instead of pointers; the
// VM rebases them as it starts. This does the same to a copy read from the file
// and then asks re:: for what the game layer asks for - class names, field
// offsets up the class chain, enum literals, generic instance names - checking
// each answer against what tools/tdb_dump.py reads out of the same file.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/log.h"
#include "../src/re.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

void check_field(const char* type, const char* field, uint32_t offset, bool is_static = false) {
  const uint32_t t = re2cc::re::find_type(type);
  const re2cc::re::Field f = re2cc::re::find_field(t, field);
  char what[256];
  std::snprintf(what, sizeof(what), "%s.%s at +0x%X%s (got %s+0x%X, declared by %s)", type, field, offset,
                is_static ? " static" : "", f.valid() ? "" : "nothing ", f.offset,
                f.valid() ? re2cc::re::full_name(f.declaring).c_str() : "-");
  check(t && f.valid() && f.offset == offset && f.is_static() == is_static, what);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_re.exe <re2.exe>\n");
    return 2;
  }
  re2cc::log_init(".\\");
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::printf("cannot open %s\n", argv[1]);
    return 2;
  }
  // Find the header in .data the way the mod does: magic, version 70.
  IMAGE_DOS_HEADER dos;
  std::fread(&dos, sizeof(dos), 1, f);
  IMAGE_NT_HEADERS64 nt;
  std::fseek(f, dos.e_lfanew, SEEK_SET);
  std::fread(&nt, sizeof(nt), 1, f);
  std::vector<IMAGE_SECTION_HEADER> secs(nt.FileHeader.NumberOfSections);
  std::fseek(f, dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader,
             SEEK_SET);
  std::fread(secs.data(), sizeof(IMAGE_SECTION_HEADER), secs.size(), f);
  const IMAGE_SECTION_HEADER* data = nullptr;
  for (const auto& s : secs)
    if (!std::strncmp(reinterpret_cast<const char*>(s.Name), ".data", 8)) data = &s;
  check(data != nullptr, ".data section present");
  if (!data) return 1;
  std::vector<uint8_t> sec(data->SizeOfRawData);
  std::fseek(f, data->PointerToRawData, SEEK_SET);
  std::fread(sec.data(), 1, sec.size(), f);
  std::fclose(f);
  size_t at = 0;
  for (; at + 8 < sec.size(); at += 8)
    if (!std::memcmp(&sec[at], "TDB\0", 4) && *reinterpret_cast<uint32_t*>(&sec[at + 4]) == 70) break;
  check(at + 8 < sec.size(), "TDB v70 header found in .data");
  // Copy from the header to the end of the section and rebase the 18 array
  // offsets into pointers, as the VM does.
  const size_t len = sec.size() - at;
  auto* tdb = static_cast<uint8_t*>(VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  std::memcpy(tdb, &sec[at], len);
  for (int i = 0; i < 18; ++i) {
    auto* p = reinterpret_cast<uint64_t*>(tdb + 0x58 + 8 * i);
    if (*p) *p += reinterpret_cast<uint64_t>(tdb);
  }
  *reinterpret_cast<uint32_t*>(tdb + 8) = 1;
  check(re2cc::re::attach_tdb(reinterpret_cast<uintptr_t>(tdb)), "attach_tdb");
  std::printf("status: %s, %u types\n", re2cc::re::status(), re2cc::re::num_types());

  using namespace re2cc;
  check(re::find_type("via.GameObject") == 62370, "via.GameObject is type 62370");
  check(re::find_type("app.ropeway.gamemastering.InventoryManager") == 45819, "InventoryManager is type 45819");
  check(re::find_type("app.ropeway.gamemastering.InventoryManager.PrimitiveItem") == 12194, "nested PrimitiveItem");
  check(re::find_type("System.Collections.Generic.List`1<app.ropeway.inventory.Slot>") == 36696, "List<Slot> by name");
  check(re::full_name(re::parent(45819)) ==
            "app.ropeway.RopewaySingletonBehaviorRoot`1<app.ropeway.gamemastering.InventoryManager>",
        "InventoryManager's parent is its singleton root");
  check_field("app.ropeway.gamemastering.InventoryManager", "<CurrentInventory>k__BackingField", 0x0);
  check_field("app.ropeway.gamemastering.InventoryManager", "_Instance", 0x0, true);
  check_field("app.ropeway.gamemastering.InventoryManager", "FatItemUserData", 0x8, true);
  check_field("app.ropeway.HitPointController", "<CurrentHitPoint>k__BackingField", 0x8);
  check_field("app.ropeway.HitPointController", "<DefaultHitPoint>k__BackingField", 0x4);
  check_field("app.ropeway.EnemyHitPointController", "<CurrentHitPoint>k__BackingField", 0x8);
  check_field("app.ropeway.survivor.player.PlayerCondition", "<HitPointController>k__BackingField", 0x1E0);
  check_field("app.ropeway.survivor.player.PlayerCondition", "_IsPoison", 0x208);
  check_field("app.ropeway.PlayerManager", "PlayerList", 0x0);
  check_field("app.ropeway.EnemyManager", "<ActiveEnemyList>k__BackingField", 0x8);
  check_field("app.ropeway.EnemyController", "<HitPoint>k__BackingField", 0x238);
  check_field("app.ropeway.survivor.Inventory", "_Slots", 0x48);
  check_field("app.ropeway.survivor.Inventory", "_CurrentSlotSize", 0x40);
  check_field("app.ropeway.inventory.Slot", "_Stock", 0x8);
  check_field("app.ropeway.gamemastering.InventoryManager.StockItem", "DefaultItem", 0x0);
  check_field("app.ropeway.gamemastering.InventoryManager.StockItem", "Index", 0x18);
  check_field("app.ropeway.gamemastering.InventoryManager.PrimitiveItem", "Count", 0x10);
  check_field("app.ropeway.gamemastering.InventoryManager.PrimitiveItem", "WeaponId", 0x4);
  check_field("app.ropeway.GameClock", "_GameSaveData", 0x10);
  check_field("app.ropeway.GameClock.GameSaveData", "_GameElapsedTime", 0x8);
  check_field("app.ropeway.GameClock.GameSaveData", "_PauseSpendingTime", 0x20);
  check_field("app.ropeway.gamemastering.RecordManager", "<CurrentSaveCount>k__BackingField", 0x8);
  check_field("app.ropeway.gamemastering.MainFlowManager", "_CurrentMainState", 0x8C);
  check_field("app.ropeway.gamemastering.MainFlowManager", "gameHeaderSaveData", 0x150);
  check_field("app.ropeway.gamemastering.MainFlowManager.GameHeaderSaveData", "SaveTimes", 0x14);
  check_field("app.ropeway.gamemastering.ItemLockerManager", "gameSaveData", 0x18);
  check_field("app.ropeway.gimmick.action.GimmickItemLockerControl.ItemLockerSaveData", "_Items", 0x0);
  check_field("System.Collections.Generic.List`1<app.ropeway.inventory.Slot>", "mItems", 0x0);
  check_field("System.Collections.Generic.List`1<app.ropeway.inventory.Slot>", "mSize", 0x8);
  check_field("app.ropeway.gui.CountDownBehavior", "<CurrentTimerFrame>k__BackingField", 0x28);
  // The plants (One hit kills): the health only fire takes, and the knockdown's.
  check_field("app.ropeway.enemy.em5000.Em5000Think", "<FlameOnlyHitPoint>k__BackingField", 0x178);
  check_field("app.ropeway.enemy.em5000.FlameOnlyHitPoint", "<CurrentHitPoint>k__BackingField", 0x4);
  check_field("app.ropeway.HitPointController", "<NoDamage>k__BackingField", 0xD);

  const uint32_t state = re::find_type("app.ropeway.gamemastering.MainFlowManager.MainState");
  check(re::enum_value(state, "PAUSE", -1) == 9, "MainState.PAUSE = 9");
  check(re::enum_value(state, "IN_GAME", -1) == 6, "MainState.IN_GAME = 6");
  check(re::enum_value(state, "TITLE", -1) == 3, "MainState.TITLE = 3");
  const uint32_t item = re::find_type("app.ropeway.gamemastering.Item.ID");
  check(re::enum_value(item, "sm70_201", -1) == 32, "Item.ID.sm70_201 (Ink Ribbon) = 32");
  const uint32_t wp = re::find_type("app.ropeway.EquipmentDefine.WeaponType");
  check(re::enum_value(wp, "Invalid", 0) == -1, "WeaponType.Invalid = -1");
  check(re::enum_value(wp, "WP8700", -1) == 252, "WeaponType.WP8700 = 252");
  check(re::enum_value(wp, "WP0000", -1) == 1, "WeaponType.WP0000 = 1");

  // The overloads call.cpp calls, by parameter types (indices as tools/tdb_dump.py prints them).
  const uint32_t im = re::find_type("app.ropeway.gamemastering.InventoryManager");
  const uint32_t inv = re::find_type("app.ropeway.survivor.Inventory");
  const uint32_t item_data = re::find_type("app.ropeway.gamemastering.InventoryManager.ItemData");
  check(re::find_method(im, "setStock", {"System.Int32", "app.ropeway.gamemastering.InventoryManager.StockItem"}) == 121814,
        "InventoryManager.setStock(Int32, StockItem) is method 121814");
  check(re::find_method(im, "removeStock", {"System.Int32"}) == 121819, "InventoryManager.removeStock(Int32) is method 121819");
  check(re::find_method(im, "putShortcutWeapon", {"System.Int32"}) == 121876,
        "InventoryManager.putShortcutWeapon(Int32) is method 121876");
  check(re::find_method(im, "updateInventoryFlags", {}) == 121850, "InventoryManager.updateInventoryFlags() is method 121850");
  check(re::find_method(inv, "unequipSlot", {"System.Int32"}) == 121054, "Inventory.unequipSlot(Int32) is method 121054");
  check(re::find_method(item_data, "setData", {"app.ropeway.gamemastering.InventoryManager.ItemData"}) == 5865,
        "ItemData.setData(ItemData) is method 5865, not its 3-parameter overload");
  check(re::find_method(item_data, "setBlank", {}) == 5861, "ItemData.setBlank() is method 5861");
  check(re::find_method(item_data, "setData", {"System.Int32"}) == 0, "no ItemData.setData(Int32)");
  check(re::find_method(im, "setStock", {"System.Int32", "app.ropeway.gamemastering.InventoryManager.StockItem"}, true) == 0,
        "setStock is not static");
  // Two-slot weapons (game.cpp reads the parts out of its code).
  check(re::find_method(im, "isFatWeapon",
                        {"app.ropeway.EquipmentDefine.WeaponType", "app.ropeway.EquipmentDefine.WeaponParts"}, true) == 121908,
        "InventoryManager.isFatWeapon(WeaponType, WeaponParts) is static method 121908");
  check(re::enum_value(re::find_type("app.ropeway.EquipmentDefine.WeaponParts"), "A", -1) == 1, "WeaponParts.A = 1");

  // Stacks (game.cpp): the item table as a Dictionary<Item.ID, ItemElement> - its
  // generic instance, the nested Entry struct - and the maximum's method, whose
  // id sits in r8 because it is an instance method.
  check_field("app.ropeway.gamemastering.ItemManager", "<ItemElementTable>k__BackingField", 0xA0);
  const char* const kItemTable =
      "System.Collections.Generic.Dictionary`2<app.ropeway.gamemastering.Item.ID,app.ropeway.gamemastering.item.ItemElement>";
  const char* const kItemEntry =
      "System.Collections.Generic.Dictionary`2.Entry<app.ropeway.gamemastering.Item.ID,app.ropeway.gamemastering.item.ItemElement>";
  check_field(kItemTable, "_entries", 0x8);
  check_field(kItemTable, "_count", 0x10);
  check_field(kItemEntry, "hashCode", 0x0);
  check_field(kItemEntry, "key", 0x8);
  check_field(kItemEntry, "value", 0x10);
  check(re::is_value_type(re::find_type(kItemEntry)) && re::value_size(re::find_type(kItemEntry)) == 0x18,
        "the dictionary's Entry is a 0x18-byte value type");
  check_field("app.ropeway.gamemastering.item.ItemElement", "Disposable", 0x4);
  check(re::enum_value(re::find_type("app.ropeway.gamemastering.item.Disposable"), "MultipleUse", -1) == 2,
        "item.Disposable.MultipleUse = 2");
  check(re::enum_value(item, "MAX", -1) == 297, "Item.ID.MAX = 297");
  const uint32_t item_mgr = re::find_type("app.ropeway.gamemastering.ItemManager");
  check(re::find_method(item_mgr, "getItemMultipleUseMax", {"app.ropeway.gamemastering.Item.ID"}) == 354617,
        "ItemManager.getItemMultipleUseMax(Item.ID) is instance method 354617");

  // Wooden boards (game.cpp, events.cpp): the callbacks TriggerUseItem.registerUseMode
  // makes - closures holding the ItemWork - and the item data onUsedBase reads.
  check(re::enum_value(item, "sm70_202", -1) == 33, "Item.ID.sm70_202 (Wooden Boards) = 33");
  check(re::enum_value(re::find_type("app.ropeway.gamemastering.InventoryManager.STOCK_TYPE"), "ITEM", -1) == 1,
        "InventoryManager.STOCK_TYPE.ITEM = 1");
  const char* const kUseClosure0 = "app.ropeway.gimmick.action.TriggerUseItem.<>c__DisplayClass33_0";
  const char* const kUseClosure1 = "app.ropeway.gimmick.action.TriggerUseItem.<>c__DisplayClass33_1";
  check_field(kUseClosure0, "wk", 0x0);
  check_field(kUseClosure1, "wk", 0x0);
  check_field("app.ropeway.gimmick.action.TriggerUseItem.ItemWork", "ItData", 0x0);
  check_field("app.ropeway.gimmick.action.TriggerUseItem.ItemData", "Stock", 0x8);
  check_field("app.ropeway.gimmick.action.TriggerUseItem.ItemData", "ItemId", 0xC);
  check_field("app.ropeway.gimmick.action.TriggerUseItem.ItemData", "CanReuse", 0x1C);
  check(re::find_method(re::find_type(kUseClosure0), "<registerUseMode>b__0", {}) == 380469,
        "TriggerUseItem's use-item callback <registerUseMode>b__0() is method 380469");
  check(re::find_method(re::find_type(kUseClosure1), "<registerUseMode>b__1", {}) == 380471,
        "TriggerUseItem's useless-item callback <registerUseMode>b__1() is method 380471");

  // The difficulty switch (game.cpp, call.cpp): the header's difficulty, the game
  // over screen's continue-on-Assisted hold, the manager whose flags
  // setDifficulty sets, and setDifficulty itself.
  const uint32_t diff = re::find_type("app.ropeway.gamemastering.MainFlowManager.Difficulty");
  check(re::enum_value(diff, "EASY", -1) == 0 && re::enum_value(diff, "NORMAL", -1) == 1 &&
            re::enum_value(diff, "HARD", -1) == 2,
        "MainFlowManager.Difficulty EASY = 0, NORMAL = 1, HARD = 2");
  check_field("app.ropeway.gamemastering.MainFlowManager.GameHeaderSaveData", "CurrentDifficulty", 0x10);
  check_field("app.ropeway.gamemastering.MainFlowManager", "<ForceEasyContinue>k__BackingField", 0x68);
  check_field("app.ropeway.gamemastering.GlobalUserDataManager", "_Instance", 0x0, true);
  const uint32_t main_flow = re::find_type("app.ropeway.gamemastering.MainFlowManager");
  check(re::find_method(main_flow, "setDifficulty", {"app.ropeway.gamemastering.MainFlowManager.Difficulty"}) == 99149,
        "MainFlowManager.setDifficulty(Difficulty) is instance method 99149");
  check(re::find_method(main_flow, "setDifficulty", {"app.ropeway.gamemastering.MainFlowManager.Difficulty"}, true) == 0,
        "setDifficulty is not static");

  // The records' counters (game.cpp, events.cpp): what the results screen checks
  // for three of its records, the step record's limit, and the three virtual
  // methods the counts move in (hooked in their classes' vtables).
  check_field("app.ropeway.gamemastering.RecordManager", "gameSaveData", 0x20);
  check_field("app.ropeway.gamemastering.RecordManager.GameSaveData", "OpenItemBox", 0x8);
  check_field("app.ropeway.gamemastering.RecordManager.GameSaveData", "UseHealItem", 0xC);
  check_field("app.ropeway.PlayerManager", "<Pedometer>k__BackingField", 0x14);
  check_field("app.ropeway.gamemastering.RecordManager", "RecordDataList", 0x98);
  check_field("app.ropeway.gamemastering.RecordManager.RecordNode", "ProgressType", 0x30);
  check_field("app.ropeway.gamemastering.RecordManager.RecordNode", "ClearCount", 0x34);
  const uint32_t progress = re::find_type("app.ropeway.gamemastering.RecordManager.ProgressType");
  check(re::enum_value(progress, "ITEMBOX", -1) == 4 && re::enum_value(progress, "CURE", -1) == 5 &&
            re::enum_value(progress, "WALK", -1) == 6,
        "RecordManager.ProgressType ITEMBOX = 4, CURE = 5, WALK = 6");
  {
    const uint32_t box = re::find_method(re::find_type("app.ropeway.fsmv2.SwitchItemToInventory"), "update",
                                         {"via.behaviortree.ActionArg"});
    check(box == 369956 && re::method_vt_index(box) == 9,
          "SwitchItemToInventory.update(ActionArg) is method 369956, virtual[9]");
    const uint32_t screen = re::find_method(re::find_type("app.ropeway.gui.NewInventorySlotBehavior"), "update", {});
    check(screen == 96410 && re::method_vt_index(screen) == 15,
          "NewInventorySlotBehavior.update() is method 96410, virtual[15]");
    const uint32_t land = re::find_method(re::find_type("app.ropeway.PlayerFootEffectController"), "onLand",
                                          {"via.motion.script.FootEffectController.JointPartsType",
                                           "via.motion.script.FootEffectController.JointSideType", "via.vec3"});
    check(land == 52305 && re::method_vt_index(land) == 19,
          "PlayerFootEffectController.onLand(JointPartsType, JointSideType, vec3) is method 52305, virtual[19]");
  }

  // Freeze play time, the extra modes: the FSM action that ends a run's measuring
  // and records its clear time while the main state is still IN_GAME - The Ghost
  // Survivors' four missions and The 4th Survivor / The Tofu Survivor. Its own
  // start, not the base class's (via.behaviortree.Action declares one too).
  {
    const uint32_t action = re::find_type("app.ropeway.fsmv2.EndMeasureAndRecordGameElapsedTimeForExtra");
    const uint32_t start = re::find_method(action, "start", {"via.behaviortree.ActionArg"});
    check(start == 369564 && re::method_vt_index(start) == 8,
          "EndMeasureAndRecordGameElapsedTimeForExtra.start(ActionArg) is method 369564, virtual[8]");
    check(re::find_method(re::find_type("via.behaviortree.Action"), "start", {"via.behaviortree.ActionArg"}) == 65295,
          "the base via.behaviortree.Action.start is a different method (65295): the action's own is hooked");
  }

  // Freeze play time, The Ghost Survivors' run timer on the HUD: the same behaviour
  // whose updateCountDown is Freeze countdown timer's counts *up* in lateUpdate,
  // straight from the clock. The two countdown behaviours are separate classes with
  // separate lateUpdates - the main game's is Freeze countdown timer's own hook.
  {
    const uint32_t rogue = re::find_type("app.ropeway.gui.RogueCountDownBehavior");
    const uint32_t late = re::find_method(rogue, "lateUpdate", {});
    check(late == 248804 && re::method_vt_index(late) == 6,
          "RogueCountDownBehavior.lateUpdate() is method 248804, virtual[6]");
    const uint32_t main_late = re::find_method(re::find_type("app.ropeway.gui.CountDownBehavior"), "lateUpdate", {});
    check(main_late && main_late != late,
          "CountDownBehavior.lateUpdate() is a different method (Freeze countdown timer's own hook)");
    check(re::find_method(rogue, "updateCountUp", {}) != 0 && re::find_method(rogue, "updateCountDown", {}) != 0,
          "RogueCountDownBehavior has both updateCountUp (the HUD timer) and updateCountDown");
  }

  // The records panel (records.cpp, events.cpp, call.cpp): both record sets, what
  // earns them, their rewards, the raccoons, the accessories worn, the save request,
  // the two screens' update methods and the achievement calls.
  {
    const char* const kRm = "app.ropeway.gamemastering.RecordManager";
    const char* const kRmSsd = "app.ropeway.gamemastering.RecordManager.SystemSaveData";
    check_field(kRm, "systemSaveData", 0x18);
    check_field(kRm, "RecordIdList", 0x90);
    check_field(kRm, "RewardDataList", 0xA0);
    check_field(kRm, "<Naomi>k__BackingField", 0x11);
    check_field(kRmSsd, "RecordProgressNumber", 0x48);
    check_field(kRmSsd, "IsClearedRecord", 0x50);
    check_field(kRmSsd, "IsNewClearedRecord", 0x58);
    check_field(kRmSsd, "IsOpenedReward", 0x60);
    check_field(kRmSsd, "IsNewReward", 0x68);
    check_field(kRmSsd, "IsOpenedPopUp_TofuCharacter", 0x79);
    check_field("app.ropeway.gamemastering.RecordManager.RecordNode", "RewardList", 0x28);
    check_field("app.ropeway.gamemastering.RecordManager.RecordNode", "Type", 0x38);
    check_field("app.ropeway.gamemastering.RecordManager.RewardNode", "Type", 0x0);
    check_field("app.ropeway.gamemastering.RecordManager.RewardNode", "NameGuid", 0x8);
    check_field("app.ropeway.gamemastering.RecordManager.RecordIds", "Id", 0x0);
    const char* const kRr = "app.ropeway.gamemastering.RogueRecordManager";
    const char* const kRrSsd = "app.ropeway.gamemastering.RogueRecordManager.RogueSystemSaveData";
    check_field(kRr, "rogueSystemSaveData", 0x10);
    check_field(kRr, "RogueRecordOrderList", 0x30);
    check_field(kRr, "RogueRecordDataList", 0x38);
    check_field(kRr, "D_COURCE_SPECIAL_CONDITION_NUM", 0x8, true);
    check_field(kRrSsd, "IsClearedRogueGame_Normal", 0x8);
    check_field(kRrSsd, "IsClearedRogueGame_Training", 0x10);
    check_field(kRrSsd, "RoguePlayCount", 0x38);
    check_field(kRrSsd, "IsSpecialClearedRogue", 0x40);
    check_field(kRrSsd, "IsClearedTypeScenarioC", 0x48);
    check_field(kRrSsd, "RogueRaccoonFigureCount", 0x50);
    check_field(kRrSsd, "IsClearedRogueRecord", 0x60);
    check_field(kRrSsd, "IsNewClearedRogueRecord", 0x68);
    check_field(kRrSsd, "IsOpenedRogueReward", 0x70);
    check_field(kRrSsd, "IsNewRogueReward", 0x78);
    check_field("app.ropeway.gamemastering.RogueRecordManager.RogueRecordNode", "RogueReward", 0x0);
    check_field("app.ropeway.gamemastering.RogueRecordManager.RogueRecordNode", "RecordType", 0x4);
    check_field("app.ropeway.gamemastering.RogueRecordManager.RogueRecordNode", "LostType", 0x8);
    check_field("app.ropeway.gamemastering.RogueRecordManager.RogueRecordNode", "ClearCount", 0xC);
    check_field("app.ropeway.GimmickRaccoonFigureManager", "_SystemSaveData", 0x0);
    check_field("app.ropeway.GimmickRaccoonFigureManager.SystemSaveData", "RaccoonFigureDataList", 0x8);
    check_field("app.ropeway.rogue.GimmickRogueRaccoonFigureManager", "_RogueSystemSaveData", 0x0);
    check_field("app.ropeway.rogue.GimmickRogueRaccoonFigureManager.RogueSystemSaveData", "RaccoonFigureDataList", 0x8);
    check_field("app.ropeway.GimmickDialLockManager", "_SystemSaveData", 0x0);
    check_field("app.ropeway.GimmickDialLockManager.SystemSaveData", "LockRecordList", 0x8);
    check(re::value_size(re::find_type("app.ropeway.GimmickDialLockManager.LockData")) == 1,
          "a lock's LockData is one byte (the bool records.cpp clears)");
    check(re::find_method(re::find_type("app.ropeway.GimmickDialLockManager"), "SetUnlockRecord",
                          {"app.ropeway.GimmickDialLockManager.RecordDialLock"}) == 258510,
          "GimmickDialLockManager.SetUnlockRecord(RecordDialLock) is method 258510");
    check_field("app.ropeway.gamemastering.RogueAccessoryManager", "rogueSystemSaveData", 0x0);
    check_field("app.ropeway.gamemastering.RogueAccessoryManager.RogueSystemSaveData", "AccessoryEquipSettings", 0x8);
    check_field("app.ropeway.gamemastering.RogueAccessoryManager.AccessoryData", "AccessoryID", 0x4);
    const char* const kSdm = "app.ropeway.gamemastering.SaveDataManager";
    check_field(kSdm, "SaveRequest", 0x38);
    check_field(kSdm, "LoadRequest", 0x39);
    check_field(kSdm, "RemoveRequest", 0x3A);
    check_field(kSdm, "RemoveAllRequest", 0x3B);
    check_field(kSdm, "SlotId", 0x3C);
    check_field(kSdm, "<saveLoadStep>k__BackingField", 0x5C);
    const uint32_t step = re::find_type("app.ropeway.gamemastering.SaveDataManager.SaveLoadStep");
    check(re::enum_value(step, "INITIALIZE", -1) == 0 && re::enum_value(step, "REQUEST_WAIT", -1) == 1 &&
              re::enum_value(step, "PS5_CROSSSAVE_DIALOG", -1) == 15,
          "SaveLoadStep INITIALIZE 0, REQUEST_WAIT 1, PS5_CROSSSAVE_DIALOG 15 (get_IsBusy's idle steps)");
    check(re::enum_value(re::find_type("app.ropeway.gamemastering.RecordManager.RecordId"), "MAX", -1) == 91,
          "RecordId.MAX = 91");
    const uint32_t reward = re::find_type("app.ropeway.gamemastering.RecordManager.RewardId");
    check(re::enum_value(reward, "MAX", -1) == 133 && re::enum_value(reward, "OPEN_MODE_06", -1) == 7 &&
              re::enum_value(reward, "OPEN_MODE_09", -1) == 10,
          "RewardId.MAX = 133, OPEN_MODE_06..09 = 7..10 (the tofu survivors)");
    check(re::enum_value(re::find_type("app.ropeway.gamemastering.RecordManager.RecordType"), "HIDDEN", -1) == 3,
          "RecordType.HIDDEN = 3");
    const uint32_t rtype = re::find_type("app.ropeway.gamemastering.RogueRecordManager.RogueRecordType");
    check(re::enum_value(rtype, "CLEAR_NORMAL", -1) == 0 && re::enum_value(rtype, "CLEAR_TRAINING", -1) == 1 &&
              re::enum_value(rtype, "SPECIAL_CONDITION", -1) == 2 && re::enum_value(rtype, "COUNT_UP", -1) == 3,
          "RogueRecordType CLEAR_NORMAL 0, CLEAR_TRAINING 1, SPECIAL_CONDITION 2, COUNT_UP 3");
    check(re::enum_value(re::find_type("app.ropeway.gamemastering.RogueRecordManager.RogueRecordId"), "MAX", -1) == 15,
          "RogueRecordId.MAX = 15");
    check(re::enum_value(re::find_type("app.ropeway.SurvivorDefine.Accessory"), "Invalid", 0) == -1,
          "SurvivorDefine.Accessory.Invalid = -1");
    const uint32_t screen = re::find_method(re::find_type("app.ropeway.gui.RecordBehavior"), "update", {});
    check(screen == 196123 && re::method_vt_index(screen) == 15, "RecordBehavior.update() is method 196123, virtual[15]");
    const uint32_t rscreen = re::find_method(re::find_type("app.ropeway.gui.RogueRecordBehavior"), "update", {});
    check(rscreen == 159620 && re::method_vt_index(rscreen) == 15,
          "RogueRecordBehavior.update() is method 159620, virtual[15]");
    const uint32_t am = re::find_type("app.ropeway.gamemastering.AchievementManager");
    check(re::find_method(am, "unlockRecord", {"app.ropeway.gamemastering.RecordManager.RecordId", "System.Boolean"}) ==
              356242,
          "AchievementManager.unlockRecord(RecordId, Boolean) is instance method 356242");
    check(re::find_method(am, "unlockRogueClearRecord", {"app.ropeway.gamemastering.RogueRecordManager.RogueRecordId"}) ==
              356240,
          "AchievementManager.unlockRogueClearRecord(RogueRecordId) is instance method 356240");
    check(re::find_method(re::find_type("app.ropeway.gamemastering.RogueAccessoryManager"),
                          "convertRewardIdToAccessoryDefine",
                          {"app.ropeway.gamemastering.RogueRecordManager.RogueRewardId"}) == 247714,
          "RogueAccessoryManager.convertRewardIdToAccessoryDefine(RogueRewardId) is instance method 247714");
    const char* const kAchDict =
        "System.Collections.Generic.Dictionary`2<app.ropeway.gamemastering.RecordManager.RecordId,"
        "app.ropeway.gamemastering.AchievementDefine.ID>";
    const char* const kAchEntry =
        "System.Collections.Generic.Dictionary`2.Entry<app.ropeway.gamemastering.RecordManager.RecordId,"
        "app.ropeway.gamemastering.AchievementDefine.ID>";
    check_field("app.ropeway.gamemastering.AchievementDefine", "Record2AchievementID", 0x0, true);
    check_field("app.ropeway.gamemastering.AchievementDefine", "RogueRecord2AchievementID", 0x10, true);
    check_field(kAchDict, "_entries", 0x8);
    check_field(kAchEntry, "key", 0x8);
    check(re::is_value_type(re::find_type(kAchEntry)) && re::value_size(re::find_type(kAchEntry)) == 0x10,
          "the achievement dictionary's Entry is a 0x10-byte value type");
  }

  // The save files (savefiles.cpp, events.cpp): the Load Game screen and the list
  // it shows, the game's delete request, the engine's table of the slots and its
  // refresh, and the getters SaveFileDetail's and String's layout is read from.
  {
    const uint32_t load = re::find_type("app.ropeway.gui.LoadBehavior");
    const uint32_t base = re::find_type("app.ropeway.gui.SaveLoadBaseBehavior");
    check(load && base && re::parent(load) == base, "LoadBehavior's parent is SaveLoadBaseBehavior");
    const uint32_t upd = re::find_method(base, "update", {});
    check(upd == 194697 && re::method_vt_index(upd) == 15, "SaveLoadBaseBehavior.update() is method 194697, virtual[15]");
    check(re::find_method(load, "update", {}) == 0, "LoadBehavior has no update of its own (its vtable holds its base's)");
    check_field("app.ropeway.gui.LoadBehavior", "<SaveFileDetailList>k__BackingField", 0x20);
    check_field("app.ropeway.gui.LoadBehavior", "<SaveFileDetailTextList>k__BackingField", 0x28);
    check_field("app.ropeway.gui.LoadBehavior", "<KeepRequest>k__BackingField", 0x3C);
    check_field("app.ropeway.gui.LoadBehavior", "SaveModeValue", 0x40);
    check_field("app.ropeway.gui.LoadBehavior", "IsLoaded", 0x90);
    check_field("app.ropeway.gui.LoadBehavior", "IsSaved", 0x91);
    check(re::find_method(base, "set_SaveFileDetailList",
                          {"System.Collections.Generic.List`1<via.storage.saveService.SaveFileDetail>"}) == 194681,
          "SaveLoadBaseBehavior.set_SaveFileDetailList(List<SaveFileDetail>) is method 194681");
    check(re::enum_value(re::find_type("app.ropeway.gamemastering.SaveDataManager.SaveMode"), "SCENARIO", -1) == 1,
          "SaveDataManager.SaveMode.SCENARIO = 1");
    check_field("app.ropeway.gamemastering.SaveDataManager", "RemoveRequest", 0x3A);
    check_field("app.ropeway.gamemastering.SaveDataManager", "SlotId", 0x3C);
    {
      // The delete is armed with one compare-exchange when the four request flags and SlotId are one qword.
      const uint32_t sdm = re::find_type("app.ropeway.gamemastering.SaveDataManager");
      const char* const kFields[5] = {"SaveRequest", "LoadRequest", "RemoveRequest", "RemoveAllRequest", "SlotId"};
      bool packed = true;
      for (int i = 0; i < 5; ++i) packed = packed && re::find_field(sdm, kFields[i]).offset == 0x38u + (i < 4 ? i : 4);
      check(packed && (0x38 % 8) == 0,
            "SaveDataManager's four request flags and SlotId are one qword (+0x38..+0x3F): the delete's compare-exchange");
    }
    check_field("app.ropeway.gamemastering.SaveDataManager", "<LastDetailUserIndex>k__BackingField", 0x58);
    check_field("app.ropeway.gamemastering.SaveDataManager", "<saveLoadStep>k__BackingField", 0x5C);
    const uint32_t svc = re::find_type("via.storage.saveService.SaveService");
    check(re::find_method(svc, "updateSaveFileDetailTbl", {"via.UserIndex"}, true) == 299023,
          "SaveService.updateSaveFileDetailTbl(UserIndex) is static method 299023");
    check(re::find_method(svc, "updateSaveFileDetailTbl", {"via.UserIndex"}) == 0, "(and no instance method of the name)");
    check(re::enum_value(re::find_type("via.UserIndex"), "Reserved", -1) == 16, "via.UserIndex: User0-15, Reserved = 16");
    const uint32_t detail = re::find_type("via.storage.saveService.SaveFileDetail");
    check(re::find_method(detail, "get_Slot", {}) == 88627 && re::find_method(detail, "get_Title", {}) == 88621 &&
              re::find_method(detail, "get_SubTitle", {}) == 88623 && re::find_method(detail, "get_Detail", {}) == 88625 &&
              re::find_method(detail, "get_LastUpdateTimeStamp", {}) == 88628 &&
              re::find_method(detail, "get_UseSize", {}) == 88630,
          "SaveFileDetail's getters: Slot 88627, Title 88621, SubTitle 88623, Detail 88625, LastUpdateTimeStamp 88628, "
          "UseSize 88630");
    check(re::find_method(detail, "get_InvalidSlot", {}) == 88633, "SaveFileDetail.get_InvalidSlot() is method 88633");
    check(re::find_type("System.Collections.Generic.List`1<via.storage.saveService.SaveFileDetail>") &&
              re::find_type("System.Collections.Generic.List`1<System.Collections.Generic.List`1<System.String>>") &&
              re::find_type("System.Collections.Generic.List`1<System.String>"),
          "List<SaveFileDetail>, List<List<String>> and List<String> are named");
    const uint32_t str = re::find_type("System.String");
    check(re::find_method(str, "get_Chars", {"System.Int32"}) == 270891 && re::find_method(str, "get_Length", {}) == 270886,
          "String.get_Chars(Int32) is method 270891, get_Length() 270886");
  }

  // Method hooks: the entry's code is exchanged only while it holds game code
  // (here: code of this test's own image, as the file's entries are empty until
  // the VM fills them) and put back only while it holds the replacement.
  {
    const uint32_t ec = re::find_type("app.ropeway.EnemyController");
    const uint32_t idx = re::find_method(ec, "HitController_OnHitDamage", {"app.Collision.HitController.DamageInfo"});
    check(idx == 113641, "EnemyController.HitController_OnHitDamage(DamageInfo) is method 113641");
    const uintptr_t rec = re::method_record_at(idx);
    check(rec && re::method_at(rec) == "app.ropeway.EnemyController.HitController_OnHitDamage", "its entry names it");
    struct Fn {
      static void original() {}
    };
    // The replacement lives outside the image, as the mod's does (never called here).
    const auto orig = reinterpret_cast<uintptr_t>(&Fn::original);
    const auto repl = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(re::hook_method(rec, repl) == 0, "an entry with no code is not hooked");
    *reinterpret_cast<uintptr_t*>(rec + 8) = orig;
    check(re::hook_method(rec, repl) == orig && *reinterpret_cast<uintptr_t*>(rec + 8) == repl,
          "hook_method exchanges the code and returns the original");
    check(re::hook_method(rec, repl) == 0, "an entry holding a non-game pointer (the replacement) is not hooked again");
    check(!re::unhook_method(rec, orig, repl), "unhook refuses when the entry does not hold that replacement");
    check(re::unhook_method(rec, repl, orig) && *reinterpret_cast<uintptr_t*>(rec + 8) == orig, "unhook puts it back");
    check(re::hook_method(rec + 8, repl) == 0 && re::hook_method(0, repl) == 0, "only method entries are hooked");
    // In the game the entries are in re2.exe's .data, copy-on-write pages - or
    // pages the runtime may protect: the exchange must get through either.
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(rec + 8), 8, PAGE_READONLY, &old);
    unsigned long protect = 0;
    check(re::hook_method(rec, repl, &protect) == orig && *reinterpret_cast<uintptr_t*>(rec + 8) == repl &&
              protect == PAGE_READONLY,
          "hook_method gets through a read-only page (and says so)");
    check(re::unhook_method(rec, repl, orig) && *reinterpret_cast<uintptr_t*>(rec + 8) == orig, "unhook too");
    VirtualProtect(reinterpret_cast<void*>(rec + 8), 8, old, &old);
    *reinterpret_cast<uintptr_t*>(rec + 8) = 0;
  }

  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
