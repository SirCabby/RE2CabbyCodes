# RE2CabbyCodes — project guide

A client-side mod for **Resident Evil 2 (2019)** — Steam app 883710, 64-bit RE Engine `re2.exe`
(SteamStub-wrapped, build 11636119, Direct3D 11 or 12 by the game's own option) — cross-built on
Linux with mingw-w64. It draws a Dear ImGui panel beside the game's **pause menu** with God mode, One
hit kills, Infinite ammo, No durability loss, Infinite wooden boards, Save without ink ribbons, Save without counting, save-count and play-time
editors, Freeze play time, Item box without counting, Recovery items without counting, a step-count editor and Freeze step count
(the records' counters), an Assisted/Standard/Hardcore **difficulty switch**, an **inventory editor** for the character in play and the game's own
**item box** (take, store - weapons and two-slot items included - and throw away) - and, beside the game's two **records
screens** (the main game's and The Ghost Survivors', from the title's Bonus menu or the pause menu), a **records panel**
that switches each record on or off, its rewards and Steam achievement following - and, beside the game's **Load Game**
screen (the title's, the pause menu's, the game over screen's), a **save-file manager** that deletes a save or copies one
into another slot (RE1CabbyCodes' feature, done through the game's save manager and Steam's remote storage). `README.md` is the user-facing doc; this file records **what
was reverse-engineered**. It is a sibling of `../RE1-1996CabbyCodes` and `../RE0CabbyCodes` (same
architecture: proxy DLL, IAT + vtable hooks in `src/mem.h`, a game tick, ImGui from the renderer's
present) - but where those found the game by byte signature and ticked from its message pump, this one
finds everything by name through RE Engine's own reflection, ticks from inside the engine's frame
(via.Application's entry table), and runs its cheats on the game's own events (method entries delegates
are made from, a few vtable slots).

## Build & deploy

```sh
make            # -> build/steam_api64.dll   (config.mk sets GAME_DIR; gitignored)
make install    # rename stock steam_api64.dll -> steam_api64_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make version X.Y.Z   # set the version;  make package -> dist/RE2CabbyCodes_vX.Y.Z.zip
make proxy      # regenerate steam_api64.def + src/proxy_exports.inc from the stock DLL's export table
python3 tools/gen_items.py --game "$GAME_DIR" --out src/items.h   # item/weapon ids + the game's English names
python3 tools/gen_records.py --game "$GAME_DIR" --out src/records_text.h   # the records panel's names (records, rewards, accessories)
# reverse engineering, all from the files on disk:
python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --dump tdb.txt          # every class, field, method, enum (~75 MB)
python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --type app.ropeway.GameClock   # one class, inherited fields too
python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --enum app.ropeway.gamemastering.Item.ID
python3 tools/tdb_dump.py --exe "$GAME_DIR/re2.exe" --methods "$GAME_DIR/RE2CabbyCodes.methods.bin" --type app.ropeway.survivor.Inventory
                                                                              # method indices + code RVAs (DumpMethods = 1)
python3 tools/pak_msg.py --game "$GAME_DIR" --grep '^ITEM_NAME_70'           # the game's text
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140XXXXXX --stop-address=... "$GAME_DIR/re2.exe"
# tests (Wine, scratch prefix; see Tooling notes):
x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_re.cpp src/re.cpp src/log.cpp src/mem.cpp -o tests/build/test_re.exe
WINEPREFIX=<scratch> wine tests/build/test_re.exe 'Z:\...\re2.exe'   # src/re.cpp against the real type database; method hooks
x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static tests/test_mem.cpp src/mem.cpp -o tests/build/test_mem.exe
WINEPREFIX=<scratch> wine tests/build/test_mem.exe                   # guarded reads: bad addresses fail, a good read ~2 ns
x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_records.cpp -o tests/build/test_records.exe
WINEPREFIX=<scratch> wine tests/build/test_records.exe                # the records panel's rules and text
x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_savefiles.cpp -o tests/build/test_savefiles.exe
WINEPREFIX=<scratch> wine tests/build/test_savefiles.exe 'Z:\...\re2.exe'   # the save files' names, rows and getters' code
x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_switch.cpp -o tests/build/test_switch.exe
WINEPREFIX=<scratch> wine tests/build/test_switch.exe 'Z:\...\re2.exe'   # src/switch_eval.h on the game's compiled switches
x86_64-w64-mingw32-g++ -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Icontrib/imgui -static tests/test_keyboard.cpp src/keyboard.cpp src/log.cpp src/mem.cpp src/config.cpp contrib/imgui/imgui.cpp contrib/imgui/imgui_draw.cpp contrib/imgui/imgui_tables.cpp contrib/imgui/imgui_widgets.cpp -ldinput8 -ldxguid -luser32 -o tests/build/test_keyboard.exe
WINEPREFIX=<scratch> wine tests/build/test_keyboard.exe exclusive   # (and `shared`) the panel's keyboard beside a game's; null graphics driver
```

The log (`RE2CabbyCodes.log`, the run before it in `.prev.log`), the ini (`RE2CabbyCodes.ini`) and the
ImGui layout file sit beside the DLL in the game folder. The log always has each event hook (`events:`)
as it is installed and when the game first calls it. `Trace = 1` logs the inventory, the item box, the
players, the clock and the save counts each time the pause menu opens, and each event a cheat acts on (a
kill, a round kept, a knife topped up) - from the game's threads, so it slows them; `AlwaysShow = 1` draws
the panel outside the pause menu; `DumpMethods = 1` writes `RE2CabbyCodes.methods.bin` (the code RVA of
every method, see The type database); `Disable = overlay,dispatch,game,events,input,cursor,keyboard` bisects a
fault. Keep them on only for a diagnostic run.

## ⛔ Rules

- **Never patch `re2.exe` on disk.**
- **Never use absolute addresses or hard-coded field offsets in code.** Every class, field, enum value
  and singleton is looked up by name in the game's type database at run time (`src/re.cpp`,
  `src/game.cpp`); the offsets in this file document build 11636119 only. The two code patterns the
  mod scans for (the VM global, see below) are REFramework's and are cross-checked against the
  database the VM points to.
- **Game memory only from the game tick or an event hook.** The tick runs inside via.Application's
  `UpdateBehavior` entry (or `EndRendering` on a frame without one), serialised in `dispatch.cpp`; the
  panel (render thread) flips atomics and posts requests, the tick applies them. The entries are jobs on
  the engine's thread pool, so the tick is not one fixed thread: nothing in it may rely on thread
  identity. An event hook (`src/events.cpp`) runs where the game calls it - any thread, at the same time
  as the tick: it touches only the objects the game handed it (and what they hold), reads switches as
  atomics, forwards to the game's code unchanged, and returns at once when that code left an exception
  pending. **Nothing reads the game every frame** but the tick's one main-state read; the panel's
  figures are read while the pause menu is up.
- **Game functions only where the user approved them.** Two approvals, both 2026-09-16: the item box's
  **Take and Store** call the game's inventory functions (`src/call.cpp`, see Game calls below), so that
  weapons and two-slot items move like at a real item box; and the cheats are **event hooks** - the game
  calls the mod through pointers exchanged in its data (method entries delegates are made from, vtable
  slots; see Events below), each hook forwarding to the original it replaced (Save without ink ribbons on
  Hardcore runs the typewriter's other check lambda instead); Freeze countdown timer's two countdown hooks and
  Infinite wooden boards' two use-item callback hooks were asked for the same day (the user narrowed that cheat
  from all consumables to wooden boards). A third, 2026-09-18: the **difficulty switch** calls
  `MainFlowManager.setDifficulty` (the call a new game and every load make; see Difficulty below), from the
  tick while the pause menu is up - the four global flags it sets live in the engine's native variables,
  where no field write reaches. A fourth, 2026-09-19: the **records' counters'** three vtable hooks -
  `SwitchItemToInventory.update`, `NewInventorySlotBehavior.update`, `PlayerFootEffectController.onLand` - for
  Item box without counting, Recovery items without counting and Freeze step count (the user chose them over
  putting the counts back only when the pause menu opens, at saves and at the ending). A fifth, the same day:
  the **records panel** - vtable hooks on `RecordBehavior.update` and `RogueRecordBehavior.update` (a records
  screen is up), the calls `AchievementManager.unlockRecord` / `unlockRogueClearRecord` when a record is switched
  on (the user chose "flags and achievements"), and a system save the mod requests itself right after a change
  (the two fields `requestSaveSystemDataNoSaveIcon` sets - "trigger immediately"); switching off resets what the
  game re-checks, but must lock nothing out (Records and their rewards). A sixth, the same day: the **save files** -
  a vtable hook on `LoadBehavior.update` (the Load Game screen is up; its list read), Delete as the game's own
  `RemoveRequest`/`SlotId` request (what its unused `requestRemoveGameData` writes), Copy through **Steam's remote
  storage** (the original steam_api64.dll's `SteamAPI_ISteamRemoteStorage_FileRead/FileWrite/...`, from a thread of the
  mod's own), and the game's list read again with two calls from inside that hook - `SaveService.updateSaveFileDetailTbl`
  and the screen's `set_SaveFileDetailList(null)` (the user chose "Hook + calls" over field writes that would leak the
  old list, and no backups of what is deleted or replaced, as in RE1). A seventh, 2026-09-20: **Freeze play time
  rebuilt** - a vtable hook on `GameClock.saveGameSaveData` (virtual[32], `IGameSaveData` slot 1), replacing the
  `GameClock.update` hook that stopped the clock, because the enemies run on that clock and a stopped one leaves
  them standing about and stalls scripted set pieces (The enemies run on that same clock). The user chose it over
  keeping the clock-stopping freeze; the clear side needed no hook. An eighth, 2026-09-20: the **inventory
  editor's weapon picker** - the item box's already-approved `setStock` / `removeStock` / `unequipSlot` /
  `putShortcutWeapon` / `updateInventoryFlags` used in a new place, to put a weapon the player picks into a
  slot and take out what was there (the user asked for the missing entries - the Flash Grenade - in the
  picker itself rather than a new control on the item box). No new call and no new hook: the weapon is
  built by field writes in an empty item box entry, which is what `setStock` is handed. A ninth, 2026-09-23:
  **Freeze play time for the extra modes** - a vtable hook on
  `fsmv2.EndMeasureAndRecordGameElapsedTimeForExtra.start` (virtual[8]), which records The Ghost Survivors'
  and The 4th Survivor's / The Tofu Survivor's clear time while the run is still IN_GAME, a state before the
  tick's hold goes in (The extra modes record their clear time early). The user chose the hook over patching
  the saved record afterwards by field writes. A tenth, the same day: **The Ghost Survivors' run timer on the
  HUD** - a vtable hook on `RogueCountDownBehavior.lateUpdate` (virtual[6]), the count-*up* side of the
  behaviour whose `updateCountDown` Freeze countdown timer already hooks: it reads the clock itself every
  frame, so the held time goes in for that call (The Ghost Survivors' run timer is the clock itself). The user
  reported the timer still running with the freeze on and chose it over also covering the pause menu's play
  time, which is not hookable (`PauseBehavior.open` is a direct call). There are no code
  patches; ask the user before adding any other call or hook.
- **Never `VirtualQuery` per read.** Reads and stores of game memory are guarded copies (`src/mem.cpp`: a
  copy loop whose faults a first vectored exception handler turns into `false`; ~2 ns a read, a fault
  ~1.5 µs). Under Wine `NtQueryVirtualMemory` scans the page table to the end of the committed run (~15 µs
  per GB of heap after the address, plus two syscalls): the first version asked it before every read and
  ran the game at 30 fps instead of 120. `mem::writable` still asks - rare checks only.
- **Never add, remove or move a weapon by field writes.** A weapon in the inventory is also a model
  the `Equipment` component spawns when the slot is filled (`Inventory.OnAddSlot` →
  `Equipment.onAddWeapon`); a weapon written into a slot would be a slot with no gun. A weapon moves
  only through the game's own inventory calls - the item box's Take and Store, and the inventory
  editor's weapon picker (2026-09-20, Weapons in the inventory editor below), which builds the weapon
  in an empty **item box** entry (data alone: no model hangs off it) and hands that entry to
  `setStock`. All of them are refused when those calls are unavailable. Only a weapon's loaded rounds
  are a field write, into the stock the game itself made.
- Back up `Steam/userdata/<id>/883710/remote/win64_save/` into `.save-backup-<date>/` (gitignored)
  before experiments that write saves.
- The user commits every repo themselves — do not `git commit`/`push` unless asked.

## Why steam_api64.dll

`re2.exe` imports d3d11, dxgi, dinput8, xinput1_3, version, winmm (all Wine builtins: under Proton
they would need `WINEDLLOVERRIDES`) and **delay-loads** `steam_api64.dll` and `openvr_api.dll`. The
Steam DLL ships with the game and is not a builtin, so Proton loads ours with no launch options, and
`dinput8.dll` stays free for REFramework. The delay load happens on the game's first Steam call
(`SteamAPI_RestartAppIfNecessary`, early in WinMain) - after SteamStub has run, with the exe's import
table bound. The original (1018 function exports, SDK of 2020-12-20) is proxied whole:
`tools/gen_proxy.py` reads its export table (PE32+), checks it covers the 12 names the exe's
delay-load table asks for, and emits one `jmp [rip+slot]` thunk per function plus a PE forwarder for
the one data export, `g_pSteamClientGameServer`.

## What the game does

### Executable
- PE32+, image base 0x140000000 (ASLR). Sections `.text` 0x1000 (0x58ABC07), `.rdata` 0x58AD000,
  `.data` 0x6C7D000 (0x245F000), `.pdata`, `_RDATA`, `.rsrc`, `.reloc`, and `.bind` 0x9B87000 - the
  SteamStub stub, which holds the entry point. **`.text` is plain on disk** (no encryption flag set),
  so `objdump` on the file is the disassembly. The 354 exports are Wwise's (`AK::SoundEngine`); the
  export directory names the image `runtime_il2cpp.exe`.
- User32 imports of note: `GetMessageW` (the window thread's loop, exe+0x2009170: `GetMessageW`,
  `TranslateMessage`, `DispatchMessageW`, blocking - **no per-frame pump**; `PeekMessageW` is only used by a
  worker), `GetRawInputData` / `RegisterRawInputDevices` (**the mouse only** is Raw Input: usage page 1 usage 2,
  `RIDEV_INPUTSINK`, exe+0x2467260; the keyboard is DirectInput - The keyboard below), `GetAsyncKeyState`,
  `GetKeyState`, `ShowCursor`, `ClipCursor`, `SetCursorPos`, `GetCursorPos`, `SetWindowLongPtrW`.

### The type database (TDB v70, src/re.cpp)
RE Engine keeps every managed class, field, method and property in one database - REFramework's
`sdk::RETypeDB`, version 70 for RE2's ray-tracing-era build - and it sits **in `.data`**: header at
RVA 0x73DE9D0 (file offset 0x73DD9D0), `initialized` 0. On disk its 18 array pointers are **offsets
from the header** (types +0x300, methods +0x610830, fields +0xE25440, typesImpl +0xF08F00, methodsImpl
+0xFBBF10, fieldsImpl +0x105FEF0, properties +0x114DF40, params +0x125C970, initData +0x134DD20,
stringPool +0x135A880, bytePool +0x15ED0B0, ...); every array ends exactly where the next begins,
which is what confirmed the layout. The VM's loader (exe+0x1F7F870, its rebase exe+0x1F54590)
rewrites them to addresses in place as it starts and **never sets `initialized`** (it stays 0). Counts: 79 479 types, 529 601 methods, 116 567 fields, 82 339 params, 2.7 MB of strings,
6.8 MB of byte pool.

| record | size | layout (REFramework tdb69/70) |
| --- | --- | --- |
| header | 0xE8 | counts at 0x0C.. (`numTypes` 0x0C, `numMethods` 0x10, `numFields` 0x14, `numTypeImpl` 0x18, `numFieldImpl` 0x1C, `numMethodImpl` 0x20, `numParams` 0x30, `numInitData` 0x38, `numStringPool` 0x50, `numBytePool` 0x54), arrays from 0x58 (types 0x60, typesImpl 0x68, methods 0x70, methodsImpl 0x78, fields 0x80, fieldsImpl 0x88, params 0xA8, initData 0xB8, stringPool 0xD0, bytePool 0xD8) |
| typedef | 0x50 | u64 {index:18 parent:18 declaring:18 underlying:7 object_type:3}, u64 {array:18 element:18 impl:18 system:10}, flags, size, fqn, crc, ctor, vt, `member_method` +0x28, `member_field` +0x2C, prop {num:12 start:18}, event, interfaces, generics (byte pool: {definition:18 num:14} + type ids), `type` +0x40, **`managed_vt` +0x48** (the REObjectInfo of its instances; runtime) |
| typeImpl | 0x30 | name, namespace (string pool), field_size, static_size, module, rank, `num_member_methods` u16 +0x12, `num_member_fields` i32 +0x14, ... |
| method | 0x10 | u64 {declaring:18 impl:20 params:26 (byte-pool ParamList)}, **code pointer - 0 on disk, set by the VM** |
| methodImpl | 12 | attrs, vtable index, flags (0x10 static, 0x40 virtual, 0x400 abstract), impl flags (0x1000 internal call), name |
| field | 8 | u64 {declaring:18 impl:20 offset:26} |
| fieldImpl | 12 | attrs, flags (0x10 static, 0x40 literal), {type:18 init_lo:14}, {name:30 init_hi:2} |

- **Names**: namespace + name from the typeImpl; a nested class is its declaring class's full name +
  `.` + name; a generic instance appends `<arg,...>` from the byte pool
  (`System.Collections.Generic.List`1<app.ropeway.inventory.Slot>`). `re::find_type` builds the map of
  all 79 479 once. `tests/test_re.cpp` checks names, field offsets and enum literals against
  `tools/tdb_dump.py`'s reading of the same file.
- **Finding it at run time** (`re::init`): the TDB header in `.data` by magic `TDB\0` + version 70; the
  VM global REFramework's way - the RIP-relative target of `48 8B 0D ?? ?? ?? ?? BA FF FF FF FF E8`
  (`mov rcx,[vm]; mov edx,-1; call get_thread_context`) that occurs more than ten times; in the VM
  object, the qword pointing at that very header (the mod requires they agree); **the VM's static
  table 0x30 bytes before it** - `{u8** elements; u32 size}`, one static block per type index, 0
  until the class's static constructor ran. The mod waits until none of the 18 array fields is still an
  offset (every one non-zero is above the header's own address) before reading; if the static table is
  not 0x30 before the database pointer it looks for the `{elements; size}` pair nearby.
- **Instance fields** sit at `object + fieldptr + field.offset`, where `fieldptr` is the int32 just
  before the object's REObjectInfo (`*(i32*)(obj->info - 8)`; REFramework's `get_fieldptr_offset`).
  An object is `{REObjectInfo* info; u32 refcount; ...}` (0x10); `info->classInfo` is its typedef, so
  `(classInfo - types) / 0x50` is its class index - the mod's type check for every object it touches.
  Value-type fields sit at `value + field.offset`.
- **Statics and singletons**: `static_block(declaring class) + field.offset`. RE2's managers derive
  from `app.ropeway.RopewaySingletonBehaviorRoot`1<T>`, whose static `_Instance` (offset 0 of the
  generic instance's block) is the singleton; `re::singleton` walks the class chain for it.
- **Arrays**: `{header 0x10, element class +0x10, count +0x1C, elements +0x20}`; element size 8 for
  references, the value type's field size otherwise. **`List<T>`**: `mItems` (array) at +0, `mSize` at
  +8 of its fields. **`Dictionary<K,V>`**: `_buckets` +0, `_entries` +8 (Entry {hashCode, next, key,
  value}, 0x18 for an enum key and a reference), `_count` +0x10.
- **Enums**: literal fields of the enum class, the value in the byte pool through `initData[index]`
  (a negative offset points into the string pool). `re::enum_value`.
- **Method code** only exists at run time. `DumpMethods = 1` writes `RE2CabbyCodes.methods.bin`
  ("RE2M", version, count, then one u32 RVA per method index); join it with `tools/tdb_dump.py`'s
  indices and disassemble the file. (A 288 772-pointer run into `.text` sits at RVA 0x70B0CB0 in
  `.data`, but it is not one pointer per method - 321 677 methods have bodies - and was not pursued.)

### Classes the mod uses (all found by name; offsets are from the field pointer)

| class | field | what |
| --- | --- | --- |
| `app.ropeway.gamemastering.MainFlowManager` (singleton) | `_CurrentMainState` +0x8C | `MainState`: TITLE 3, LOAD_GAME_DATA 4, IN_GAME 6, GAME_OVER 7, **PAUSE 9**, ... (read from the enum) |
| | `gameHeaderSaveData` +0x150 | `GameHeaderSaveData`: `SaveTimes` +0x14 (the save count a save records), difficulty +0x10, survivor +0x8 |
| `app.ropeway.PlayerManager` (singleton) | `PlayerList` +0x0, `<Pedometer>` +0x14 | `List<survivor.player.PlayerCondition>`; the player's steps (the step record's count) |
| `app.ropeway.survivor.SurvivorCondition` | `<HitPointController>k__BackingField` +0x1E0, `_IsPoison` +0x208, `<SurvivorType>` +0x4, `<Inventory>` +0xD0, `<Equipment>` +0xD8 | |
| `app.ropeway.HitPointController` | `<DefaultHitPoint>` +0x4, `<CurrentHitPoint>` +0x8, `<Invincible>` +0xC, `<NoDamage>` +0xD | players' and enemies' HP |
| `app.ropeway.EnemyManager` (singleton) | `<ActiveEnemyList>` +0x8 | `List<EnemyController>`; `EnemyController.<HitPoint>` +0x238 is its `EnemyHitPointController` |
| `app.ropeway.gamemastering.RecordManager` (singleton) | `<CurrentSaveCount>` +0x8, `<CurrentClearTime>` +0x0 | the results screen's save count |
| | `gameSaveData` +0x20, `RecordDataList` +0x98 | `RecordManager.GameSaveData`: `OpenItemBox` +0x8, `UseHealItem` +0xC (the records' counters); `List<RecordNode>`: `ProgressType` +0x30, `ClearCount` +0x34 |
| `app.ropeway.GameClock` (singleton) | `_GameSaveData` +0x10 | `GameClock.GameSaveData`: `_GameElapsedTime` +0x8, `_DemoSpendingTime` +0x10, `_InventorySpendingTime` +0x18, `_PauseSpendingTime` +0x20 (u64, microseconds) |
| `app.ropeway.gamemastering.InventoryManager` (singleton) | `<CurrentInventory>` +0x0, static `FatItemUserData` +0x8 | `survivor.Inventory`: `_CurrentSlotSize` +0x40, `_Slots` +0x48 (`List<inventory.Slot>`); `FatItemUserData`: `FatItemList`, `FatWeaponList` (two-slot items; weapons with part A too, see Two-slot items) |
| `app.ropeway.inventory.Slot` | `<IsForceBlank>` +0x0, `_Stock` +0x8 | `StockItem` < `ItemData`: `DefaultItem` +0x0, `AdditionalItem` +0x8, `ExData` +0x10, `Index` +0x18; `IsForceBlank` makes `Slot.get_Type` answer Blank whatever the stock holds |
| `...InventoryManager.PrimitiveItem` | `ItemId` +0x0, `WeaponId` +0x4, `WeaponParts` +0x8, `BulletId` +0xC, `Count` +0x10 | a weapon's `Count` is its loaded rounds |
| `app.ropeway.gamemastering.ItemLockerManager` (singleton, `BOXSLOT_MAX` 400) | `gameSaveData` +0x18 | `.SaveData` → `GimmickItemLockerControl.ItemLockerSaveData._Items` (`List<ItemData>`): **the item box** |
| `app.ropeway.gimmick.action.TriggerUseItem.ItemData` | `Stock` +0x8, `ItemId` +0xC, `CanReuse` +0x1C | an item a use-item trigger takes; reached from a use-item callback's closure: `wk` +0x0 → `ItemWork.ItData` +0x0 |

- **Enemy HP and death** (One hit kills): `HitManager.hitSetting` (exe+0x9E7F20) sets a hit up before any
  `DamageHitHandler` runs: `<IsKill>` = `HitPointController.checkDead(Damage)`; then, when the attack's
  `AttackUserData.AttackAttributeValue` has NOT_KILL (4) or the target `EnemyController.get_GutsMode`, a lethal
  `Damage` is cut to HP - 1; then `<IsKill>` = `HitController.checkKill(Damage)` (the same check). NO_DEATH is never
  asked. `EnemyController.HitController_OnHitDamage` (exe+0x14C48D0) takes the HP (`addDamage`, clamped at 0;
  skipped under STOP_DAMAGE), calls `<Think>.onHitDamage` (`EnemyThinkBehavior` virtual[44]), and `dead()` when the
  enemy was live and is not. `get_IsLive` is `!AUTOPSIED && (NO_DEATH || HP > 0)`: HP 0 under NO_DEATH is a live
  enemy, and `EnemyController.lateUpdate` calls `dead()` in the frame NO_DEATH goes off with HP ≤ 0 (`<PrevNoDead>`).
  **Mr. X** (`enemy.em6200.Em6200Think`): `onThinkAwake` sets NO_DEATH for good. His `onHitDamage`, at HP ≤ 0,
  requests action 5000 (not when his action category already is 5000; under `ExternalJackedControlMode` or
  CONTROLED_FSM (`ConditionFlagsField` bit 24) it sets HP back to 1 instead) and sets `Em6200Param.FirstDeadFlag`:
  the kneel. `startSleep` turns on STOP_DAMAGE, NoAddReaction, SensorOff, HideBGM; `endSleep` turns them off and
  sets `CurrentHitPoint = DefaultHitPoint`. His motion tree (pak entry (0x7C2C464E, 0xE10F28C3)) switches GUTS_MODE
  on only in LADDER_UP, LADDER_DOWN, SPT_KNIFE.5350_Grapple_EM_Knife and SPT_GRENADE.5302_Down_End. NO_DEATH
  elsewhere: `SwitchNoDead`/`SwitchNoDeadByTrack` in the zombie tree (SET_CAPTURE, DMG_SHOCK, SET_WINDOW_IN.*), the
  Ivy's (em5000: SET_HANG, SET_DEAD_IDLE, ACT_WAKEUP.STANDUP; `Em5000Think.checkDamageHit` clears it, `onThinkAwake`
  sets GUTS_MODE) and the licker's (SET_FIRST_APPEARANCE, SET_FALL, SET_DOG_EAT, SET_HOLD_CEILING,
  SET_FALL_TOPBOARD, SET_INTRUSION_TOPBOARD); `EventTemporaryStorage` (jacked motions); `Em6000FsmAction_Hide`; G2
  (`Em7100Think.onThinkStart`, with GUTS_MODE, for `SetType_WasteWater` only). `SwitchGutsMode` also runs in the
  G-adult's (em6000) appearances. No other tree uses the three switches. So One hit kills skips STOP_DAMAGE and
  GUTS_MODE, where the game holds the HP, but not NO_DEATH: the HP goes as it would to enough damage, and the game
  decides what 0 means. (Until 2026-09-16 it skipped NO_DEATH too, and Mr. X took hits as usual.) The trees were
  found by type id: every pak entry up to 8 MB holding `RSZ\0` blocks (9 149), their instance tables. A BHVT node
  (RE_RSZ.bt's layout, RE2 RT: four transition lists) names its actions by id; each id is the first field of its
  action's RSZ instance, so the ids sorted by where they sit in the data follow the object table.
- **G2's stagger is not its health** (One hit kills, the crane fight in the sewers - `app.ropeway.enemy.em7100`,
  whose `Em7100Think` holds `<Crane>` `GimmickG2BattleCrane` and `<Shutter>` `GimmickShutterG2Break`). `Em7100Think`
  keeps its own counter, **`TiredHp` +0x2A8**, and `onHitDamage` (virtual[44], exe+0x1475BF0) does, at exe+0x1475F2E,
  `lea eax,[rcx+r12]; cmp eax,ebp; mov [rdi+0x2F8],eax; cmovge r15d,1` - that is
  `TiredHp += getTiredDamage(hit); if (TiredHp >= getTiredThreshoud()) tired`, the health never consulted.
  `resetTired` (exe+0x7D660) is the whole of `mov dword [rdx+0x2F8],0`, so it is an accumulator from 0, and
  `getTiredThreshoud` (exe+0x7EB00) starts from `mov edi,0x7FFFFFFF` and reads
  `Em7100UserData.TiredInfo` (`TiredData`, a `List<TiredInfo>`) - **INT32_MAX is its "cannot be tired now"**.
  `TiredHp` is the only field of that name in all 79 479 types, so this is G2's alone. Its fight also has
  GUTS_MODE (`Em7100Think.onThinkStart`, `SetType_WasteWater`), so `make_lethal` leaves the health alone
  anyway - **nothing One hit kills wrote could ever have staggered him**. `game::stun_g2` tops the counter up
  to `INT32_MAX / 2` before the game's handler adds this hit's share: past any real threshold, still short of
  the sentinel, so a phase the game says cannot be staggered still cannot be. Readers found: that one compare
  and `resetTired`.
- **The plants' health is not what kills them** (One hit kills; `app.ropeway.enemy.em5000` - the Ivy and the
  Poison Ivy are one class, KindID em5000 = 7, the only plant family in the build). `Em5000Think` holds a second
  health, **`<FlameOnlyHitPoint>` +0x178** (`FlameOnlyHitPoint` {`<MaxHitPoint>` +0x0, `<CurrentHitPoint>` +0x4};
  `addDamage` exe+0x3973B0 is `cur = max(0, cur - |damage|)`), and `Em5000Think.onHitDamage` (virtual[44],
  exe+0x155BC80) is the whole of the two stages:
  `base.onHitDamage(info); if (IsLive && FlameOnlyHitPoint.CurrentHitPoint <= 0) forceDead();`
  `else if (HitPoint.CurrentHitPoint == 1) setupFakeDead(true);` - `checkDead` (exe+0x3F2540) is that same
  `IsLive && flame HP <= 0`. `forceDead` (exe+0x15391E0) is the burn-up (`breakWeakPartsAll`, the
  `<BurnupEffectID>`); `setupFakeDead(callAction)` (exe+0x1574060) is the knockdown: `requestAction(4)` unless
  the posture is already RESTRICT_DOWN, then **`set_GutsMode(false)`, `HitPoint.CurrentHitPoint =
  <DefaultHitPoint>`, `HitPoint.<NoDamage> = true`**; `revival` (exe+0x1569640, from `onApplyAction`) puts the
  health back to default and clears `<NoDamage>`. So **`HitPointController.<NoDamage>` is the down flag**, and
  `HitPointController.addDamage` (exe+0xA8BF50) opens `cmp byte [this+0x5D],0; jne ret` - a hit on a downed plant
  never moves the health. `checkDamageHit` (exe+0x1520300, the `HitController.CheckDamageHitHandler`;
  `HitManager.hitSetting` calls the attacker's `[+0x130]` then the victim's `[+0x138]` through
  `HitController.callbackEvent` and **returns false for the hit when either answers false**) is the router, not a
  filter: it answers false only for a repeat contact of a shell type outside `Em5000Think.DamageThroughShellTypes`
  that is not a Knife. It sends flame damage to the flame health (`DamageInfo.<Damage>` +0x7C, scaled by
  `Em5000ParamUserData.FlameOnlyHitPointParam.StandingDamageRate` +0x4 while the plant stands, full while it is
  down - that is why the game wants it knocked down first), weak-part damage to `Em5000WeakParts` (`Status`
  {`<PartsID>`, `<MaxHitPoint>` +0x4, `<CurrentHitPoint>` +0x8, `<IsReviveState>`, `<RevivalTime>`}, 24
  `WeakPartsID` glands; all broken → `breakWeakPerts` → `setupFakeDead`), and for `WeaponCategory.RocketLauncher`
  (7) takes the whole lot at once (`requestAction(5000)`, `breakWeakPartsAll`, flame health to 0). The plants
  have GUTS_MODE while standing (`onThinkAwake`: `mov r8b,1; call set_GutsMode`), so `make_lethal` never touched
  them - **One hit kills did nothing at all to a plant** until 2026-09-18. `game::wither_plant` empties the flame
  health, which is the test `onHitDamage` reaches first: the plant burns up on any hit that does damage, whatever
  hit it and wherever, the knockdown never reached. It also puts `HitPoint.CurrentHitPoint` at
  `DamageInfo.<Damage> + 1`, so the game's own `addDamage` lands it on exactly 1 - that first test is behind
  `IsLive`, and GUTS_MODE, what normally holds a plant off 0, is **off after its first knockdown**
  (`setupFakeDead` clears it and `revival` does not put it back), so a big hit could otherwise take the health to
  0 and get a plain `EnemyController.dead()` where the burn-up belongs. `forceDead` writes the health to 0 itself
  a moment later, so that 1 is never seen. No new hook and no game call: both are field writes in the One hit
  kills hook that was already there, and `addDamage` and `onHitDamage` run after it in the same handler
  (`EnemyController.HitController_OnHitDamage`: `get_IsLive`, save the HP, `if (!get_NoDamage) addDamage`, ...,
  `<Think>.onHitDamage` at exe+0x14C4E51, then `dead()` when it was live and is not). (The first build of this,
  the same day, was the game's two stages instead - a hit to drop the plant, a second to burn it - which the user
  did not want: any damage goes straight to the burn-up.)
- **Play time**: the clear time the results screen ranks is `GameClock.get_ActualRecordTime`
  (exe+0x9D5C30): `_GameElapsedTime - (_DemoSpendingTime + _PauseSpendingTime)`, 0 below that (the
  inventory's time counts). `GameClock.update` (virtual[15], exe+0xA1B280) adds the frame
  (`[Application+0x354]` x 1e6) to `_SystemElapsedTime` always, and to the game's elapsed time - then the
  pause, else the inventory, else the cutscene time - only while `_MeasureGameElapsedTime` (+0x0) is set
  (by `initializeInGame`; the pause flags by `PauseFlow.setup` and `GUIMaster.openPauseFor*`). **Neither the
  editor nor the freeze moves that clock any more** (2026-09-20): the enemies are on it, see the next entry.
  The time to record is held in the mod (`cheats.cpp`) and put into `_GameElapsedTime` only where the game
  reads it for the record - the save capturing the clock (Events) and the clear (the RESULT states) - then
  taken straight back out.
- **The enemies run on that same clock** - why Freeze play time stopped them standing still and stalled
  scripted set pieces. Read out of the code 2026-09-20 and **confirmed the same day from a live session**: the
  user's game had `Freeze play time ON` and `play time set to 0:00:00` a minute apart (14:26, both logs; the
  13:13 run that stood the enemies still has the identical pair at 13:18/13:19), and 2½ hours later the
  Sewers' Main Power Room set piece would not run - the same stall as 2026-09-17, which a restart had
  cleared. The editor's write makes
  `get_ActualPlayingTime` **exactly 0** (it sets `_GameElapsedTime = 0 + demo + pause`, and the getter clamps:
  `cmp rcx,rbp; jbe -> xor eax,eax`), and the freeze then stopped the clock there for good. `GameClock.get_ActualPlayingTime` (exe+0x9D59F0) is
  `_GameElapsedTime - (_DemoSpendingTime + _InventorySpendingTime + _PauseSpendingTime)` - the record time
  less the inventory's - and the enemy code asks it for **every** time it needs (found by scanning `.text` for
  `call rel32` to each of `GameClock`'s 64 method bodies, `RE2CabbyCodes.methods.bin` giving the RVAs and the
  method whose body contains each call site naming the caller): `EnemyController.canRestrictMove`
  (exe+0x151E9F0) ends `mov rsi,[this+0x198]; call get_ActualPlayingTime; cmp rsi,rax; sete cl` - the enemy's
  `SkipRestrictMoveFrame` (+0x148) **equal to the current playing time**, so a frozen clock leaves every enemy
  that was move-restricted in that instant restricted for good; `EnemyManager.FrameTimer.update`
  (exe+0x1BE0CD0) is `now = get_ActualPlayingTime(); timer.current = now; timer.running = now < timer.end`, so
  no enemy timer ever expires; `EmCommonBtAction_SkipRestrictMoveStart`,
  `EnemyController.get_/requestSkipRestrictMoveStart`, `SoundAppealManager.SoundAppealStatus`'s constructor and
  update (the noise enemies are drawn to) and the Ghost Survivors' `RogueEnemySpawnManager.updateDeadVanish` /
  `RogueEnemySpawnController.onDead` read it too. The timers are `EnemyManager`'s own fields (+0xC8..+0xF0):
  **`ThinkOffTimer`** (`get_ThinkOff`: the enemies do not think while it runs - `GimmickEndThinkOffFrame`, a
  gimmick's end, arms it), **`NoAttackTimer`**, **`AttackThroughTimer`** (`setAttackThroughFrame`, called by
  `EmCommonFsmAction_AttackControl.updateTimer`, G2's `Em7100FsmAction_AttackControl` and
  `EnemyAttackInitiativeManager`: attacks pass through while it runs) and three em4000 hold timers;
  `FrameTimer` is {`Run` +0x0, `DelayFrame` +0x8, `EndFrame` +0x10, `FrameCount` +0x18} and its `update` is
  `if (Run) { now = get_ActualPlayingTime(); FrameCount = now; Run = (now < EndFrame); }` - an **unsigned**
  compare, so a clock pinned at 0 leaves every one of them running for ever: enemies that never think, never
  attack, and a set piece waiting on one of them that never fires.
  **The editor has the same reach**: a play time set backwards leaves every FrameTimer running until the clock
  passes its old value.
  **The fix** (2026-09-20, hook approved that day): the clock always runs, and the held time goes in only where
  the game records it - `GameClock.saveGameSaveData` (**virtual[32]**, method 336933, the shape Save without
  counting already uses on `MainFlowManager`'s) for what a save writes, and the clear. `setClearedGame` needs no
  hook of its own: `ResultFlow.update` calls it with a **direct `call rel32`** (an entry hook would never fire),
  and it reads `get_ActualRecordTime` into `RecordManager.<CurrentClearTime>` (+0x0, u64) - so the tick holds
  the clock for the whole of the end-of-run states (`ENDING`, `STAFFROLL`, `WAIT_STAFFROLL`, `RESULT`,
  `RESULT_EXTRA`, `ROGUE_RESULT`) and writes `<CurrentClearTime>` back down should the clear beat it by a
  frame. The save captures the clock's save data **by reference** (`GameClock.saveGameSaveData`, exe+0xA02320,
  only stamps version and hash into `_GameSaveData` and returns it; `SaveDataManager.saveGameSaveData` walks a
  `Dictionary<UInt32, IGameSaveData>` and keeps the objects), so what the hook takes out of the clock goes back
  only once the manager is idle again - at the latest after 5 s, so the clock is never left behind.
- **That fix was half of one, and the other half was in the save files** (2026-09-21, Mr. X standing still in the
  sewers with Freeze play time *off*; `src/game.h`, `tests/test_clock.cpp`). A hold lowered `_GameElapsedTime`
  alone, so `get_ActualRecordTime` read the held time - and `get_ActualPlayingTime`, **the record time less
  `_InventorySpendingTime`**, went down with it, to `held - inventory`: **clamped at 0 and unable to move** until
  the game had been played for as long as the inventory had ever been open. The save captures the clock's save
  data under the hold, so the *file* holds that pair, and **every later load of it begins with the enemies
  frozen** - which is why switching the freeze off and setting the play time forward changed nothing (neither
  touches the live clock any more). Worse each time: `_InventorySpendingTime` is saved and restored and only ever
  grows, while every save with the freeze on knocks the elapsed time back to `held + demo + pause`.
  A clock in that state pins **every enemy from the moment it spawns**, not just the ones stamped in an instant:
  `canRestrictMove` is `SkipRestrictMoveFrame == get_ActualPlayingTime()`, and a fresh enemy's field is 0 - equal
  to a clock stuck at 0. The log said so at the first pause of both sessions, before a cheat was on: `enemies: 6
  active, 6 held still by canRestrictMove; their clock ... is 0.000 s ... it moved 0.000 s in the 214.4 s since
  the last dump` (2026-09-21 19:02 and 19:05; the same at 2026-09-20 22:57, and that run's save wrote `this save
  records 0:00:00`).
  **The whole fix**: a hold takes the same out of `_InventorySpendingTime` as out of `_GameElapsedTime` (clamped
  at what it holds) and gives both back - so the enemies' clock does not move at all under a hold, live or in the
  file, and a hold below their clock takes it no lower than the held time, which is the clock a game that far in
  has. (The 09-20 note that the two times "differ only by `_InventorySpendingTime`, which only ever grows, so
  holding one holds the other" was the mistake: the inventory's time is a field like any other, and nothing but
  `get_ActualPlayingTime` reads it.) And `game::repair_enemy_clock`, from the tick once a second while in game
  and never under a hold, heals what the older builds wrote: a clock with `elapsed < demo + inventory + pause` -
  a state **the game itself cannot write**, since it only ever adds to the three inside the elapsed time - has
  its inventory time brought down to `elapsed - (demo + pause)`, which leaves the recorded play time exactly as
  it is and gets the enemies moving from 0. The four times are logged on every pause (the `enemies:` line) and
  the repair logs what it did. The arithmetic is pure and tested (`hold_by`, `taken_from`, `repaired`,
  `enemies_stuck` in `src/game.h`; 16 807 clocks x every held time in `tests/test_clock.cpp`).
- **The extra modes record their clear time early - a state before the hold goes in** (2026-09-23; reported:
  "the time freeze cheat doesn't honor the ending saved times for ghost survivors missions"). The main game's
  clear is `ResultFlow.update` → `RecordManager.setClearedGame` in **RESULT**, one of the states the tick holds
  the time for - but The Ghost Survivors and The 4th Survivor / The Tofu Survivor do not wait for their result
  state. The FSM action **`app.ropeway.fsmv2.EndMeasureAndRecordGameElapsedTimeForExtra.start(ActionArg)`**
  [369564] (**virtual[8]**, exe+0xA0FE70; its only method, no subclasses, its code shared by nothing) is the last
  thing the run does: `GameClock._MeasureGameElapsedTime = 0` (the clock stops measuring, so nothing later moves
  it), then a jump table (exe+0xA10008, 7 entries) on the header's **`ScenarioType` +0xC** for values 4..10 -
  4 and 5 (the 4th Survivor, Tofu) → `RecordManager.backupGameElapsedTimeForExtra()` (exe+0xF6C380: the time
  from `get_ActualRecordTime`, `<IsNewRecordExtra>` cleared then set if it beat the system save's), 6 → nothing,
  **7..10 (the four Ghost Survivors missions) → `RogueRecordManager.backupRoguePlayData(LostType = value - 7,
  isCleared = true)`** (exe+0xCA5150: `get_ActualRecordTime` into rbp, the scenario's clear flag by
  `MainFlowManager.<CurrentLostDifficulty>` +0x74 - Normal or Training -, then `if (saved > now || saved == 0)
  { IsUpdateClearTime = 1; clear_times[type] = now; }`, the special conditions, the kill and bullet counts).
  Scenarios 0..3 (the main game) fall out of the table and the action does nothing but stop the clock.
  `backupRoguePlayData`'s other caller is `GameOverFlow.update` with `isCleared` false, which writes no time.
  Both backups are reached by a **direct `call rel32`**, so only the action's own vtable slot is hookable.
  **The timing, from the user's log**: the run ends `IN_GAME → RESET_TO_RESULT_ROGUE (33)`, and **1.3 s later**
  `33 → ROGUE_RESULT (30)`, where the tick's hold goes in (`freeze play time: the clear records the time held`;
  12:03:23.399/12:03:24.714 and 12:22:54.505/12:22:55.829, both 2026-09-23). So the record kept the clock's
  time while the screen showed the held one: `gui.RogueResultBehavior.open` (exe+0x16B4620, from
  `RogueResultFlow.update` - in ROGUE_RESULT, the hold in) reads `get_ActualRecordTime` itself for
  `TextClearTime`, and `PanelNewRecordTime` comes from the `IsUpdateClearTime` the early backup already decided.
  The rogue times live in `rogueSystemSaveData` (`clear_times` +0x18, u64 per `LostType`, read by
  `getRogueClearTime(LostType, LostDifficulty)`); the extra modes' in the system save.
  **The fix** (the hook approved that day): the hold goes in for that one call and comes straight back out
  (`events::on_extra_record`, `cheats::hold_playtime_for_extra_record` / `give_playtime_hold_back`) - so the
  record, the NEW RECORD panel and the results screen all read the held time. While a hook owns the hold nothing
  else may give it back (`g_play_for_hook`, checked inside `give_play_hold_back` under the lock): the tick's
  5 s cap must not put the clock right in the middle of the call the hold was taken for.
- **Save count**: a save is `SaveDataManager.saveGameSaveData` (exe+0x1785BE0, from its `update`): unless the
  slot is a system one it calls `MainFlowManager.addSaveTimes` (the header's `SaveTimes` +1, capped at 999)
  and, **in the same call**, captures every `IGameSaveData` (`saveGameSaveData`, interface slot 1) into what
  it writes - so a hold a frame later cannot keep the count out of the file (the first version's could
  not: every save wrote the held count + 1). `RecordManager.<CurrentSaveCount>` is not touched by a save;
  `setClearedGame` (exe+0x10098A0, from `ResultFlow.update`) copies it from the header at the end. The
  editor writes both; Save without counting puts `SaveTimes` back to the held count in
  `MainFlowManager.saveGameSaveData` (virtual[25]), between the increment and the capture (Events).
- **The records' counters** (Item box without counting, Recovery items without counting, the step count's editor and
  Freeze step count). `RecordManager.RecordDataList` (`List<RecordNode>`, one per `RecordId`, from `RecordListUserData`)
  gives each record a `ProgressType` (NONE 0, COUNT_UP 1, COUNT_DOWN 2, CLEARTIME 3, ITEMBOX 4, CURE 5, WALK 6,
  ADA_GUN 7) and a `ClearCount`. `addRecordCount(id, n)` (exe+0xF68E50) switches on the node's type (jump table
  exe+0xF6915C): COUNT_UP/COUNT_DOWN add to `systemSaveData.RecordProgressNumber[id]` (the lifetime counts), ITEMBOX to
  `gameSaveData.OpenItemBox`, CURE to `UseHealItem`, WALK to the pedometer (`set_NumberOfSteps` inlined:
  `PlayerManager.addPedometer(n)`), ADA_GUN to `NumberOfRecord053`; then `ReNetTelemetry.notifyAddRecordCount`, and
  `AchievementManager.unlockRecord(id)` for every id but 52 and 73-75. At the clear `setClearedGame` (exe+0x10098A0)
  calls `checkRecordProgress(72..75, 0)` (exe+0xF72AF0, the same kind of switch, table exe+0xF72F30): ITEMBOX clears its
  record when `OpenItemBox == 0`, CURE when `UseHealItem == 0`, WALK when `PlayerManager.<Pedometer> <= ClearCount`,
  CLEARTIME by the clear time - **the live fields, nothing else**. The record table (pak entry (868096078,
  3150707310), 91 `RecordSingleData`): RecordId 73 ITEMBOX, 74 CURE, 75 WALK with ClearCount **14000**, 72 CLEARTIME,
  52 ADA_GUN. Where the counts move - the only places: `GUIMaster.openInventoryItemBoxMode` (exe+0x1AD5240) ends with
  `addRecordCount(73, 1)` once the box has opened, and its only caller is the FSM action
  `fsmv2.SwitchItemToInventory.update` (virtual[9], exe+0xB0B050; on its first update, `_IsFirstUpdate`);
  `InventoryManager.useHealItem` (exe+0x18A8150) ends with `addRecordCount(74, 1)` once it healed (an item whose
  `getItemEffectiveValue` is not above 0 returns before it), called only by `useItem` ← `useStock` ←
  `NewInventorySlotBehavior.executeCommand` ← `updateCommandControl` ← `update` (virtual[15], exe+0x1D98E90), direct
  calls all; a step: `via.motion.script.FootEffectController.lateUpdate` → `callFootEffect` → `call [vtable+0x98]`
  (virtual[19] `onLand`, exe+0xE8D5A9) → `PlayerFootEffectController.onLand` (exe+0x1C98A60; JointPartsType 0 only) →
  `addPedometer()` (exe+0x1C5B0C0, under checks of its survivor) → `PlayerManager.addPedometer(1)` (exe+0x1DFF3C0:
  `<Pedometer> += n`, then `ReNetTelemetry.notifyGeneralCount(11, n)`). No other `addRecordCount` caller passes 73 or
  74 (enemy kills 19-26, a zombie's dropped part 30, Ada's gun 52 from `Equipment.sendUseWeapon`, raccoons 55/88 -
  `fixRecordProgress` too -, files 57, items 61/63), and `addRecordCount`'s WALK case is reached by none of them.
  `get_OpenItemBox`/`get_UseHealItem` (no setters) have no direct callers; `get_NumberOfSteps` is the pedometer. Saves
  capture them (`RecordManager.saveGameSaveData` virtual[32], `PlayerManager.saveGameSaveData` virtual[25]) and a load
  puts them back. The mod: a switch going on (or a game starting with it on) sets the item box's or recovery items'
  count to 0, or takes the step count to hold, from the tick; the three hooks put the count back after the game's call
  (Events). The panel shows all three while the pause menu is up, and the step record's `ClearCount`.
- **Records and their rewards** (the records panel, `src/records.cpp`, 2026-09-19; read from the code, not yet
  seen in game - Open questions). **Main game** - `RecordManager`: `RecordDataList` +0x98
  (one `RecordNode` per `RecordId`, RECORD_001..091 = 0..90, built by `makeTable` from `UserData` +0x80; the table,
  pak entry (868096078, 3150707310), is 91 `RecordSingleData` {RecordId, title and condition Guids, RewardId_1..4 with
  "behind" flags, ProgressType, ClearCount, Type}) - `RecordNode` {`NameBehind` +0x0 (= `isLockedRecord`: "???" until
  cleared), `RewardList` +0x28, `ProgressType` +0x30, `ClearCount` +0x34, `Type` +0x38 (ALL 0, SCENARIO 1, SURVIVOR 2,
  HIDDEN 3)}; and `systemSaveData` +0x18 (`RecordManager.SystemSaveData`, the system save) {`RecordProgressNumber`
  +0x48 (int[91]), `IsClearedRecord` +0x50, `IsNewClearedRecord` +0x58 (bool[91]), `IsOpenedReward` +0x60,
  `IsNewReward` +0x68 (bool[133] by `RewardId`; the constructor sets every `IsNewReward`: it means "not looked at
  yet"), `IsOpenedPopUp_TofuCharacter` +0x79}. `RewardId`: INVALID 0, OPEN_MODE_00..09 1-10 (Hardcore - always open,
  `get_IsOpenedHardMode` is `mov al,1` -, Leon [2nd], Claire [2nd], S Completion Rank, The 4th Survivor, The Tofu
  Survivor, Konjac, Uiro-Mochi, Flan, Annin), COSTUME_00..08 11-19, WEAPON_00..04 20-24, CONCEPTART_00..29 25-54,
  FIGURE_00..77 55-132. 87 records are listed and 4 are HIDDEN (087 ATM-4 ∞, 088 Minigun ∞, 090 Konjac and Uiro-Mochi,
  091 all four tofu); only OPEN_MODE_06/07 come from two records (090, 091), and rewards 1, 15, 16, 19 and 123-132
  from none. Names: `Mes_Sys_Bouns_RecordNN` and the conditions in message file 0xB40600BF, rewards `Mes_Sys_Reward_*`
  (0x9FAC4820), costume rewards `Mes_Sys_Costume_RewardsNN`, weapons `WEAPON_NAME_WP*`.
  - **Earning one**: `clearRecord(id)` (exe+0xF764D0; ~70 callers - the game's events, `setClearedGame`,
    `checkRecordProgress`) only writes `RecordProgressNumber[id] = ClearCount`, then telemetry and
    `AchievementManager.unlockRecord(id, true)`; `addRecordCount` adds to it and calls `unlockRecord(id, false)`.
    `checkClearedRecord(id)` (exe+0xF70640, table exe+0xF70910) by ProgressType: NONE and COUNT_UP `ClearCount <=
    progress`, COUNT_DOWN `0 < progress <= ClearCount`, the rest `progress == ClearCount`. Only
    `checkClearedRecords(callback)` (exe+0xF70930) sets the flags: `openRecord(id)` for every record not
    `IsClearedRecord` whose check holds, then RECORD_001 ("Raccoon City Native") once every record of the ALL list
    (`isEqualRecordType`: all but HIDDEN) but itself is cleared, then the "records cleared" pop-up
    (`GUIMaster.openOpenedRecords`) when `ClearedRecordList` +0x28 is not empty, else the callback. Its callers:
    `RecordBehavior.mainStateFinished` (each time the records screen opens), `TitleFlow.update`, the game over and
    result flows. `openRecord(id)` (exe+0xFFD240, its only caller) sets `IsClearedRecord[id]` and
    `IsNewClearedRecord[id]`, adds id to `ClearedRecordList` (RECORD_001 inserted first), and for each reward of the
    node sets `IsOpenedReward[r]` (`openReward` exe+0x327C10 is that write alone, never called); a COSTUME reward also
    gets `setNewRewardCostume` (allocates `CostumeData` for `CostumeManager.addNewCostumeData`: the costume screen's
    NEW), OPEN_MODE_06..09 `IsOpenedPopUp_TofuCharacter = 1` (a one-shot title pop-up; `isNeedTofuCharacterPopUp`
    reads and clears it). **Nothing in the game ever un-clears a record or closes a reward**, and the check re-opens
    any record left un-cleared whose progress still holds.
  - **Achievements**: `AchievementDefine.Record2AchievementID` (static; `.cctor` exe+0x171A600) maps 41 records to the
    41 non-platinum achievements. `unlockRecord(id, immediate)` (exe+0xB04210) unlocks when `immediate` or
    `checkClearedRecord(id)` - from the **progress**, never `IsClearedRecord`; `requestClearTrophyAgain`
    (exe+0x1004AC0) re-unlocks every mapped record whose progress holds, and is called only from
    `SaveDataManager.update` after a successful cross-save transfer. So a record cleared by its flags alone unlocks
    no achievement; one whose progress is put at its goal unlocks it the next time the game counts toward it.
  - **Who reads the rewards**, all through `isOpenedReward(r, offset)` (exe+0xFE8610: `IsOpenedReward[r+offset]`, or
    true for anything but OPEN_MODE_01-03 while `RecordManager.<Naomi>` +0x11 is set - `registerUnlockAllBonus`, no
    direct caller or delegate, probably the all-rewards DLC): the title menus as they build (`MenuMainBehavior.initList`
    5/6, `MenuStoryBehavior`/`MenuScenarioBehavior.initList` 2/3, `TitleL/RBehavior` 5-10), `ClearStateBehavior` (4),
    `CostumeManager` (the costume lists, `changeAllClassic`), the figure and concept-art viewers, and the infinite
    weapons: `ItemLockerManager.CheckExtraWeapon` (exe+0x1976EB0; from its `loadGameSaveData` and
    `MainFlowManager.setupNewGame`, main scenarios only) puts each `AddItemData_Omake` weapon whose reward is open
    into the item box once and takes back the ones whose reward is closed, and `InventoryManager.deleteInvalidWeapon`
    (exe+0x18326C0, from its `loadGameSaveData`) removes a closed reward's weapon from every inventory. WEAPON_00 is
    WP4510 (Combat Knife), 01 WP7000 (Samurai Edge (Original Model)), 02 WP2200 (LE 5), 03 WP8400 (ATM-4 ∞), 04 WP8700
    (Minigun ∞). So a weapon reward switched in the middle of a game reaches the item box and the bag at the next load.
  - **The Ghost Survivors** - `RogueRecordManager`: `RogueRecordDataList` +0x38 (`RogueRecordNode` {`RogueReward` +0x0,
    `RecordType` +0x4 (CLEAR_NORMAL 0, CLEAR_TRAINING 1, SPECIAL_CONDITION 2, COUNT_UP 3), `LostType` +0x8 (A-D),
    `ClearCount` +0xC}; table pak entry (3812877124, 2438038100): 15 records, one accessory ACCESSORY_00..14 each), and
    `rogueSystemSaveData` +0x10 {`IsClearedRogueGame_Normal` +0x8, `_Training` +0x10 (bool[4] by LostType),
    `RoguePlayCount` +0x38, `IsSpecialClearedRogue` +0x40, `RogueRaccoonFigureCount` +0x50, `IsClearedRogueRecord`
    +0x60, `IsNewClearedRogueRecord` +0x68, `IsOpenedRogueReward` +0x70, `IsNewRogueReward` +0x78}. `openRogueRecord`
    (exe+0xD0E400) sets those four flags and adds to `<OpenedRogueRecordList>` +0x0; `checkClearedRogueRecord_All`
    (exe+0xCAC890; `RogueRecordBehavior.mainStateFinished`, the rogue result and game over) opens every record not
    cleared whose `checkClearedRogueRecord_Simple` (exe+0xCACB10) holds - worked out from the save data, there is no
    progress number: CLEAR_NORMAL `Normal[LostType]`, CLEAR_TRAINING `Training[LostType] || Normal[LostType]`, SPECIAL
    `IsSpecialClearedRogue[LostType]`, COUNT_UP records 0 and 5 `RoguePlayCount >= ClearCount`, 14
    `RogueRaccoonFigureCount >= ClearCount`. The same clear flags unlock No Way Out (`get_IsOpenedScenarioD`
    exe+0x1CDA10: A, B and C each cleared, normal or training). Rogue records 12 and 14 have achievements
    (`RogueRecord2AchievementID`, re-unlocked only after a cross-save, like the main ones).
  - **The screens**: `gui.RecordBehavior` (the title's Bonus menu, `BonusBehavior.toRecord`; the pause menu,
    `PauseBehavior.toRecord`) and `gui.RogueRecordBehavior` (`BonusBehavior.toRecordRogue`,
    `PauseBehavior.toLModeRecord`, `RogueGUIMaster.openRogueRecord`). `open` activates the GUI object
    (`RopewayGuiBehaviorRoot.enableObject` → `GameObjectExtension.requestActive`); `mainStateFinished` (a delegate
    made in `doAwake`) runs the clear check when the opening animation ends and deactivates the object when the
    closing one does; `EnableInput_` +0x24 is set by `<mainStateFinished>b__50_0` (after the check's pop-up) and
    cleared by `close`. So `update` (virtual[15]; exe+0x11DE4A0 and exe+0x15836A0, neither folded, no subclasses)
    should run only while a records screen is up. The rows re-read the manager when the list scrolls (`updateList`)
    and the detail pane when the cursor moves (`selectionChangedEvent`).
  - **Saving**: the records are in the system save, the Ghost Survivors' in the rogue system save.
    `requestSaveSystemDataNoSaveIcon` (exe+0x3AAB10) is `if (!IsBusy) { SaveRequest = 1; SlotId = -1; }`
    (`requestSaveSystemData` also shows the save icon); `SaveDataManager.update` writes the system save for SlotId -1
    and follows it with the rogue one (SlotId 21) on its own, and asks for a system save after every game save
    (exe+0x179F192). Other requests: closing Options or Costumes, the results screens,
    `GimmickRaccoonFigure.doBreak`. `get_IsBusy` (exe+0x17631F0): any of the four request bools, or `saveLoadStep`
    +0x5C in 2..14 or at least 16.
  - **What is re-derived, so "off" must reset it** (the Ghost Survivors' writers): at the end of a run
    `RogueRecordManager.backupRoguePlayData(LostType, isCleared)` (exe+0xCA5150) sets the scenario's
    `Normal`/`Training` clear flag, the clear times and kill counts, and on a normal clear the special conditions -
    D: `UseHandGunBulletCount` (+0x1C, per run) at most `D_COURCE_SPECIAL_CONDITION_NUM` (static +0x8); B:
    `UnlockPrisonKeyFlg` (+0x18); C: `ScenarioCThroughFlg_Short/_Long` (+0x19/+0x1A) into `IsClearedTypeScenarioC[0/1]`,
    both needed - through `noticeSpecialCleared(t)` (sets `IsSpecialClearedRogue[t]`); A's (a grenade made) comes from
    `Inventory.combineSlotsCore`. Its raccoon count is `RogueRaccoonFigureCount += broken - count`, i.e. set to
    `GimmickRogueRaccoonFigureManager.getBrokenCount()` - a bool per raccoon in that manager's own rogue save
    (`_RogueSystemSaveData` +0x0 -> `RaccoonFigureDataList` +0x8); `addRogueRaccoonFigureCount` adds. The main game's
    raccoons alike: `GimmickRaccoonFigureManager._SystemSaveData.RaccoonFigureDataList` (system save; `getBroken` /
    `getBrokenCount` exe+0x5A94E0 / exe+0xF93190 are shared by both managers), `GimmickRaccoonFigure.doBreak`
    (exe+0x1D3A3C0: `clearRecord(14)` - RECORD_015 -, `addRecordCount(55, 1)`, `addRecordCount(88, 1)`) and
    `fixRecordProgress(id)` (exe+0xF915D0, only 55 and 88, from `setClearedGame`) = `addRecordCount(id, broken -
    progress)`: the two raccoon records' progress is the broken count after every clear. The other "count the
    things found" records (files and films through `UIFileManager.SystemSaveData.getFile`, weapons and parts
    through `ItemManager.setGetItemOnce`/`setGetWeaponOnce`, the dial locks' `setRecordCount` from
    `GimmickDialLockManager.SetUnlockRecord`) count a thing once, into system-save lists of their own. The locks'
    one is recounted at every lock opened: `SetUnlockRecord(id)` (exe+0x19A1130) clears RECORD_013 ("First
    Break-In"), sets `_SystemSaveData.LockRecordList[id]` (+0x0 -> +0x8, a bool per `RecordDialLock`, 8) and ends
    `mov r9d,edx; mov r8d,0x36; ...; jmp setRecordCount` - RECORD_055 ("Master of Unlocking") = the locks ever
    opened - so the panel unmarks the last lock opened like a raccoon. The files', film's, weapons' and parts'
    records only ever add a new find: switched off, they stay off (not earned back by play; the panel switches
    them on) and their lists are left alone.
  - **Accessories worn** (The Ghost Survivors): `RogueAccessoryManager.rogueSystemSaveData` +0x0 ->
    `AccessoryEquipSettings` +0x8, a `List<AccessoryData>` {`SurvivorType` +0x0, `AccessoryID` +0x4
    (`SurvivorDefine.Accessory`, Invalid -1 = none)}; `getCurrentAccessory` and `SurvivorCostumeChanger.onStart`
    never check the reward is open, so an accessory locked while worn stays worn (Cat Ears keeps infinite ammo).
    `convertRewardIdToAccessoryDefine` (exe+0x1097A10, a jump table): ACCESSORY_00..13 -> Accessory 3..16,
    ACCESSORY_14 (Cat Ears) -> 18. Costumes likewise are not re-checked when worn (`CostumeManager` reads the
    rewards only for its lists and `changeAllClassic`).
  - **The panel** (`src/records.cpp`, `src/records_rules.h`, the names in `src/records_text.h` from
    `tools/gen_records.py`): the two screens' `update` hooks stamp when a screen was last up and taking input
    (`EnableInput_` +0x24, read from the object the hook is handed); while one is, the panel draws that set's
    window instead of the cheats. From the tick, while its screen is up: **on** - the progress `clearRecord` leaves
    (a better one kept; `records_rules.h`), `IsClearedRecord`/`IsNewClearedRecord`, each reward
    `IsOpenedReward` + `IsNewReward` (and the tofu pop-up flag) - `openRecord`'s writes but the costume NEW data
    and the "records cleared" pop-up - then `unlockRecord(id, true)`; The Ghost Survivors: `openRogueRecord`'s four
    flags, the save given what earns it (a scenario cleared either way gets the *training* clear - a normal one
    would earn the no-training record too), `unlockRogueClearRecord`.
    **Off** - the flags cleared, each reward no other cleared record gives locked (only OPEN_MODE_06/07 are
    shared), the progress just short of the goal; the raccoon records unmark raccoons broken, and "Master of
    Unlocking" locks opened (the highest-numbered first), until the count is below the goal; The Ghost Survivors:
    the accessory locked and taken off whoever wears it, the play count just short, the special flag (and both
    sewer routes) cleared, one raccoon unmarked (the count taken short even if the list cannot be read), a no-training
    record's scenario kept cleared as a training clear - and "Mission Complete?", "Reunited", "Getting Over It"
    (CLEAR_TRAINING of A-C) **held** while No Way Out is open: `RogueRecordNode.RecordType` set to COUNT_UP, which
    `checkClearedRogueRecord_Simple` answers "not earned" for any record but the three it counts (its code: 0,
    5, 14), `getRogueRecordProgressRate` answers 0 for, and `RogueRecordBehavior.updateList` (exe+0x1589367:
    `cmp edx,2; ja`, `cmp edx,3; jne` - types 0-2 hide the gauge, 3 draws it, anything else skips both and can
    leave a reused row's gauge, which is why not an out-of-range value) draws with an empty gauge. The table is
    the manager's own, built once in `doStart` (exe+0x1DBC50), so the original types are kept per table (the
    list object) and the holds in `RE2CabbyCodes.records.txt`. The game checks The Ghost Survivors' records in
    more places than its screen: the title (`TitleFlow.<>c__DisplayClass39_0.<update>b__4`, exe+0x5475C0, after
    `checkClearedRecords`), the end of a run, and the screen as its opening animation ends - and that screen's
    `EnableInput_` is set only by `<mainStateFinished>b__45_0`, after the check. So the holds are set again at
    every main state change and on the Ghost Survivors screen's first frame (seen, not yet taking input),
    retried every frame at start-up until the table exists (at most 10 s); a held record found cleared (the
    game got there first) is switched off again, and a hold is let go when No Way Out is not open with the
    save the game has (another save, a wiped one: nothing to lock out, so play earns it again). Every change asks
    for the system save at once (`SaveRequest` 1, `SlotId` -1 while `get_IsBusy` would say no, polled 4 times a
    second, given up after a minute of busy saves; the game follows it with the rogue save). A record switched on
    calls the achievement unlock whatever the dictionaries say (the game's call does nothing for a record
    without one) once `AchievementManager.AchievementService` is there - `unlockRecord` returns without a word
    when it is not. What the code reads out of the game's code: the raccoon records from `fixRecordProgress`
    (`cmp edi,A; je; cmp edi,B; jne`), the lock record from `SetUnlockRecord`'s tail, the counted rogue records
    from `checkClearedRogueRecord_Simple` (`test r14d,r14d; je; cmp r14d,P; je; cmp r14d,Q; jne`; without them a
    counted record is not switched off), the accessory map through `switch_eval.h`; which records carry an
    achievement (only shown) from `AchievementDefine`'s two static dictionaries.
- **Difficulty** (the difficulty switch): `MainFlowManager.Difficulty` EASY 0 (Assisted), NORMAL 1 (Standard),
  HARD 2 (Hardcore). The game's difficulty is the header's `CurrentDifficulty` (+0x10); `get_/set_CurrentDifficulty`
  (exe+0x5F4860 / 0x5F53C0) are `[this+0x1A0]+0x20` and are inlined nearly everywhere (two direct callers), so
  the readers read it live. `MainFlowManager.setDifficulty(d)` (exe+0x1F2EEC0, method 99149; called by
  `delayLoadGameSaveData` - every load, with the loaded header's - and the new-game lambdas of `TitleFlow` and
  `WakeUpFlow`) writes the header's difficulty, unless **`<ForceEasyContinue>`** (+0x68) is above 0: then EASY,
  and at 1 also `GameRankSystem.resetRankPoint` (when the rank system is up) and the field to 2. The field is set
  to 1 by `GameOverBehavior.<update>b__46_0` (the game over screen's continue on Assisted) and cleared only by
  `MainFlowManager.resetGame`, so after that choice every load is Assisted until the title. Then four
  `GlobalUserDataManager.setFlag(murmur3(UTF-16 name), bool)` (exe+0x16C0680: the native `via.userdata` variable
  by hash, type 2 = bool, set when it differs): **DifficultyEasy** (== EASY), **DifficultyNormal**,
  **DifficultyHard**, **DifficultyNoHard** (!= HARD); hashes 0x072D7758, 0xDA0A402B, 0x0BBB2123, 0x8ABC3160. A null
  `GlobalUserDataManager._Instance` throws `NullReferenceException` after the header is written. On a HARD load
  `delayLoadGameSaveData` also runs `ActivityManager.resetAllProgressActivities`. The flags are named by GUID in
  the global variables file (pak entry (1648221929, 1357613919), `uvar`: 0x30-byte records {GUID, name offset,
  value offset, 0, type u32, hash u32}) and used in six files only, found by a GUID scan of every entry up to 8 MB:
  DifficultyEasy by the Ivy zombies' spawn and sleep gimmicks (two SCN: `ESC_IvyZombie_*_notSpawnEasy`,
  `AT_AfterSanpuIvySleep_Easy/_NotEasy`), `GP_RankControl_Easy2nd` and a G2 (em7100) behaviour tree's "(EASY)"
  `ConditionUserVariable`; DifficultyNormal and DifficultyHard by `ContinueTipsUserData`, DifficultyNoHard by
  `CommonTipsUserData` (the loading tips); DifficultyHard by the scene gimmick
  `B1_PlayGo_ForceCheckPoint_ForHardMode`. Other readers: `EnemyManager.<CurrentDifficulty>` (static +0x84) is a
  copy `EnemyManager.lateUpdate` (virtual[6], exe+0x1869280) refreshes from the header every frame (read by
  `movePlayerSafeRoom` and Mr. X's `Em6200ChaserController.checkAlwaysFindMode` / `update`);
  `GameRankSystem.addRankPointDirect` clamps the rank points to the header's difficulty's
  `GameRankParameterData.DifficultyParam{Easy,Normal,Hard}` {`DefPoint`, `MinPoint`, `MaxPoint`} at every change
  (`getDifficultyParamTbl` exe+0x17552A0) - the rates (`getRankPlayerDamageRate`, ...) go by `<GameRank>`, not
  the difficulty; the typewriters' check lambdas (Typewriters); Assisted's slow healing -
  `SurvivorManager.isEnabledAutoHeal` (exe+0x176D850: a player, not survivor type 3, EASY, scenario ≤ CLAIRE_B),
  asked by `SurvivorCondition.updateAutoHeal` and `resetAutoHealStartTimeOnHitDamage` - and its aim assist -
  `CameraSystem.get_IsAimAssist` (exe+0x1F057C0: EASY, or HUNK/TOFU, or L mode with `<CurrentLostDifficulty>`
  TRAINING, then an option asked of the manager at exe+0x9184E90); the ammo lying in an area,
  `ItemPositions.calcBulletCountWithDifficulty` (exe+0x1B74E60, from `createInitializeItem`: the count scaled by
  `GameMaster.DifficultySettingsUserData.SetItemBulletCountRate` for the header's difficulty, as the item is
  made); `RecordManager.setClearedGame(header.ScenarioType,
  header.CurrentDifficulty)` from `ResultFlow.update`, so a clear counts for the difficulty the header holds at
  the end. **The switch** (`cheats.cpp`): from the tick while the pause menu is up, `<ForceEasyContinue>` to 0
  when leaving Assisted (put back if the switch fails), `setDifficulty` through `call.cpp`, the header read back.
  Nothing is reloaded; what an area set up by the difficulty as it loaded stays until it loads again.
- **Ink ribbons**: `Item.ID` sm70_201 (32). On Hardcore the typewriter asks for one through the use-item flow
  (`OpenUseItem` starts `InteractManager.TaskUseItem`; picking the ribbon runs `InventoryManager.useStockForGimmick`,
  then `TaskUseItem.update` → `TriggerUseItem.onUsedBase`, see Use-item triggers) but its item data is `CanReuse`
  (all 22 ribbon `TriggerUseItem.ItemData` a byte scan finds in the scenes: Count 1, CanReuse 1), so nothing is
  taken there. **The save
  menu spends it**: `CallSaveMenu.start` (exe+0xE14420) makes `Action<int>` delegates of the typewriter's
  `GimmickTypeWriter.reduceInkRibbonNum` (entry global exe+0x91756A8) and `recoverInkRibbonNum` (exe+0x91756B0) and
  passes them to `GUIMaster.openSave(mode, fade_in, fade_out, decide, cancel, error)` as decide and error;
  `SaveBehavior.<>c__DisplayClass8_0.<execDecide>b__0` (exe+0x115EDA0, the confirm dialog's) invokes
  `CallbackOnDecide(get_Cursor())`, then `request()` → `SaveDataManager.requestSaveGameData`.
  `reduceInkRibbonNum` (exe+0x51FE00): on HARD, `CurrentInventory.reduceSlot(sm70_201, 1)`; `recoverInkRibbonNum`
  (exe+0x51F330): on HARD, `InventoryManager.addItemCount(sm70_201, 1)`. So whatever path reached the save menu - the
  straight save too (Typewriters) - a Hardcore save takes a ribbon the player carries (none carried: nothing; read
  from the code, not yet seen in game). Save without ink ribbons does not hook them: it only stops the typewriter
  asking for a ribbon.
- **Durability** (No durability loss): only knives wear. `EquipmentDefine.isMeleeWeapon(t)` is
  `getCategory(t) == WeaponCategory.Knife` (9); `getCategory` (exe+0x19E4BE0) is a jump table over WeaponType
  1..0xFC - `dec edx; cmp edx,N; ja; movsxd rax,edx; lea rdx,[image]; movzx eax,byte [rdx+rax+cases];
  mov ecx,[rdx+rax*4+targets]; add rcx,rdx; jmp rcx`, cases `xor eax,eax; ret` / `mov eax,imm; ret` - which the
  mod decodes at start-up: the knives are WP4500, WP4510, WP4520. A knife's durability is its slot's count;
  `Equipment.onHitMeleeAttack` → `useSubWeapon` → `Inventory.reduceSlot(Sub, n)` → `Slot.reduce`, then
  `removeSubSlot` in the same call when the slot is empty - so the knife is made full before the hit's code
  runs and again after it (Events) rather than a hit put back (the last hit would already have removed it).
  Other reducers: `Weapon.onHitAttack` (melee: `MeleeWeaponUserData.getReducePoint` → `reduceSlot`),
  `Equipment.defend` (a weapon used on a grab) → `use`, and `SurvivorCondition.useSubWeapon` (direct, from the
  FSM action `MonitorSupportItem.execLateUpdate`, not hooked). Full is the game's own figure:
  `Slot.get_MaxNumber` → `EquipmentManager._WeaponBulletUserdata` (`WeaponBulletUserData`) →
  `_LoadingPartsCombos[]` (`WeaponLoadingpartsCombination` {`_WeaponType`, `_LoadingPartsCombos[]` of
  `LoadingPartsCombination` {`_Priority`, `_Parts`, `_OverwriteNumber`, `_Infinity`, `_Number`}}): among the
  entries whose parts the weapon has and that overwrite the number, the lowest `_Priority` wins
  (`WeaponLoadingpartsCombination.getNumber` and its lambdas); `_Infinity` makes it unlimited. Items take their
  maximum from `ItemManager.getItemMultipleUseMax`; `Slot.RemainingVital` (`inventory.Vital` Fine/Caution/
  Danger/Dead) is the count against that maximum.
- **Typewriters** (`app.ropeway.gimmick.action.GimmickTypeWriter`) offer the save through triggers
  (`gimmick.action.Trigger`), and `getTriggerFuncCheckValid(trigger)` (exe+0x137E510) gives each a check
  by its `UniqueName`: `<getTriggerFuncCheckValid>b__8_0` = `CurrentDifficulty != HARD && checkEnableButton()`
  (exe+0x522940), `b__8_1` = `== HARD && checkEnableButton()` (exe+0x5235F0), `b__8_2` = `!checkEnableButton()`
  (enemies near, or `SaveDataManager.IsBusy`). `CurrentDifficulty` is
  `MainFlowManager.gameHeaderSaveData.CurrentDifficulty` (`Difficulty` EASY 0, NORMAL 1, HARD 2). The typewriter's
  own tree (`natives/stm/ObjectRoot/SetModel/sm4x_Gimmick/common/TypeWriter/sm_tpl_TypeWriter_gimmick.fsmv2.40`:
  states Idle, Use, Full, Normal, Set - camera sets and `StartPlayContainer`) plays a container whose tree
  (3952 bytes, pak entry (913106458, 2884566595)) has `KeySuccessStart` → `GimmickEventBegin`,
  `GimmickFsmStateSet`, `CallSaveMenu`, `GimmickEventEnd` (the not-HARD button: straight to the save menu) and
  `UseItemOpenStart` → `OpenUseItem` → `UseItemSuccessStart` → `InkRibbonNumCheck`
  (`FsmCondition_CompareValue_UINT` on local variables InkRibbonNum/Add/Space/MaxNum, IsEmptyInkRibbon) →
  `CallSaveMenu` or `EmptyMessage_1` (the HARD path; the ribbon is spent by the save menu, see Ink ribbons).
  No tree uses `ReduceInkRibbon`/`RemoveInkRibbon`; `GimmickTypeWriter.reduceInkRibbonNum` and
  `recoverInkRibbonNum` have no direct callers - `CallSaveMenu.start` makes the save menu's delegates of them;
  `removeInkRibbonNum` and `CallSaveMenu.reduceInkRibbonNum` are the empty stub. `CheckUsedItemSettings.Param.execute` compares
  `InventoryManager.<LastUseItemId>` with its `CheckItemList`; `ItemIdSettings.Param.hasItem` needs `Enable`
  and an inventory slot of the item.
- **Save without a ribbon on Hardcore** (Save without ink ribbons): a trigger keeps its checks as delegates in
  `_CheckValid` (`List<Trigger.FuncCheckTrigger>`, filled as an area loads by `Trigger.setup` → `setupProvider`
  (exe+0xD28460) → the owner's `ITriggerFuncProvider`s). `getTriggerFuncCheckValid` makes a trigger's check
  from the lambda's method entry (target: the typewriter): 'Check' `b__8_0` (global exe+0x9175D48),
  'UseInkRibbon' `b__8_1` (exe+0x9175D50), a third `b__8_2`; the lambdas' code is reached only through the
  code array. The mod hooks both entries at start-up (Events): with the cheat on and the difficulty HARD,
  'Check' runs `b__8_1` (the HARD check: straight to the save menu when the button is enabled) and
  'UseInkRibbon' runs `b__8_0` (answers no on HARD), the effect of exchanging the two delegates. Which lambda
  is which is read out of their code (`cmp ecx,HARD; je` / `jne`). A typewriter's triggers are `TriggerKey`s
  named **'Check'** and **'UseInkRibbon'** sharing an `_Owner`; `UniqueName` is a `System.String` {info,
  refcount, length +0x10, UTF-16 +0x14}. (Before the events the mod scanned the registered triggers -
  `InteractManager._TrgWks` plus `_LockObj`'s `Registereds`/`RegisteredKeys`/`RegisteredHasUpdates`, 300+ in the
  police station, every loaded area's - four times a second and exchanged the delegates; confirmed in game
  2026-09-16, replaced the same day.)
  The tree files' RSZ instance type ids are the TDB typedefs' `fqn` (e.g. `FixCameraSet` 0xE709FA9E); pak paths
  hash with murmur3 (seed 0xFFFFFFFF) of the UTF-16 lower/upper path, `natives/stm/...` in this build.
- **Use-item triggers - how a gimmick takes an item** (wooden boards at a window): `TriggerUseItem`
  (< `TriggerKey` < `TriggerArea` < `Trigger`) lists `Items` and `UselessItems` (`List<TriggerUseItem.ItemData>`
  {`FsmStateUsed` +0x0, `Stock` +0x8 (`InventoryManager.STOCK_TYPE` BLANK 0, ITEM 1, WEAPON 2, DEAD 3), `ItemId` +0xC,
  `WeaponId` +0x10, `WeaponParts` +0x14, `Count` +0x18, `CanReuse` +0x1C}) and keeps an `ItemWork` {`ItData` +0x0,
  `PrimItem` +0x8, `Index` +0x10} per entry. The interact task (`InteractManager._TaskUseItem` +0x60,
  `TaskUseItem.update` exe+0x1CC1F40, called directly from `InteractManager.update`, virtual[15]) runs a state
  machine: `onStart` → `registerUseMode` (exe+0x16B7BE0) makes, for each work, a closure
  `<>c__DisplayClass33_0` {`wk`} with the delegate `<registerUseMode>b__0` (entry global exe+0x9175E50; body
  exe+0x276AB0: `InteractManager._Instance._TaskUseItem.entryUsedWork(wk)`) - `_1`'s `b__1` (exe+0x9175E58,
  exe+0x27A350 → `entryUselessWork`) for the useless ones - and registers it with
  `InventoryManager.registerUseModeItem(wk.PrimItem, callback)`; `GUIMaster.openInventoryUseMode` opens the
  inventory; picking an item runs `InventoryManager.useStockForGimmick(slot)` (exe+0x18A9290), which finds the
  `GimmickItemList` entry of that item and invokes its callback inline (`call [entry+8]` per delegate entry). The
  task's next update takes the queued work (under a lock) and, inlined, `unregisterUseModeItem`, `setHasUsed`,
  `onUsedBase(wk)` (exe+0x16B3A90): `if (!ItData.CanReuse && ItData.Stock == ITEM)
  InventoryManager.reduceItem(ItData.ItemId, ItData.Count)` (exe+0x4C40F0 → `Inventory.reduceSlot(Item.ID, n)`
  exe+0xC03B70: `getSlots(id)` = `_Slots.FindAll(s => s._Stock.DefaultItem.ItemId == id)`, list order, each
  `Slot.reduce` until n is taken), then `FsmStateController.setState(FsmStateUsed)`. `onUsedBase` is the only
  reader of `CanReuse` among the trigger's and task's methods. Wooden boards (`sm70_202`, 33): the scenes' 23
  board `ItemData` (found by a byte scan of the RSZ data, not a full parse) all have Count 1 and CanReuse 0, ten of
  them right after the string 'UseItemFailureStart' - likely boards listed as useless somewhere, which
  `onUsedBase` takes all the same. The delegates `TriggerUseItem.onUsedItem`/`onUsedUseless` exist but the task
  inlines them. **Infinite wooden boards** hooks both callback entries (Events): with the cheat on, a boards
  `ItemData` is set `CanReuse` before the callback queues the work (remembered by address); with it off, a mark
  the mod made is taken off at that trigger's next use - only the item data the callback hands over is touched.
  Other ways items are taken, for a later cheat: the inventory's Use (`NewInventorySlotBehavior.update` →
  `updateCommandControl` → `executeCommand` → `InventoryManager.useStock` → `useItem` exe+0x18A86E0, all direct:
  no hookable call encloses it but the screen's per-frame update) applies `useHealItem`/`useKeyItem`/damage by
  `ItemElement.Type` (Heal 1, Attack 2, Key 3, Damage 4) and then, by `Disposable`, `Inventory.removeSlot(slot)`
  (SingleUse: `OnRemoveSlot`, then the stock's `setBlank` - same prim object) or `Slot.reduce(n)` (MultipleUse),
  nothing for Infinity; `isUsableItem` offers Use for Heal items, Damage items (HP > 1), the Hip Pouch (262) and
  items on `KeyItemParameterList` (`useKeyItem` returns 3 for ink ribbons, 1 for boards - the list's registering
  `gimmick.trigger.*` classes are empty, so it looks unused). Every reader of `ItemElement.Disposable`
  (`ItemManager.getItemDisposable` exe+0x1C792B0; no inlined table reads) compares with MultipleUse only, except
  `useItem` (SingleUse removes, Infinity keeps), `Slot.reduce`/`Slot.add` (≤1 alike), `Slot.get_IsEmpty`/`IsFull`
  (Infinity counts against the maximum; their item callers check MultipleUse first) and the debug
  `Slot.get_DisplayName` - so marking the Heal items
  Infinity would keep them on Use and still let combining (`Inventory.combineSlotsCore` → `Slot.remove`/`reduce`)
  take them. The item table: `ItemUserData.itemElements` (`ItemElementList`, one `ItemElement` per item), built into
  `<ItemElementTable>` by `ItemManager.makeTable` from `doAwake`; this build: Heal/SingleUse the spray and the
  green, blue and mixed herbs, Key/Infinity the red herb and gunpowders, Attack/MultipleUse the ammo, Key/
  MultipleUse the ink ribbon, wooden boards and Spare Key (sm73_133). FSM actions `ReduceInventoryItem` (5 trees -
  a safe box, a crank, a search-and-use one and two generic 'Inventory/Try/After' trees), `ReduceInventoryItemLastUsed` (none) and
  `RemoveInventoryItem` (the wristbands) take items too.
- **Slot numbers are the stocks' `Index`, not the list's order.** `Inventory.getSlot(i)` is
  `_Slots.Find(s => s._Stock.Index == i)` (predicate exe+0x2FCDC0; `Slot.get_Index` reads `_Stock.Index`),
  and the game's inventory screen moves an item by rewriting the two stocks' `Index` values
  (`Inventory.exchangeSlot`, exe+0xBA7800) - the list and the stocks' contents stay put. So after the
  player rearranges anything, list position k is not slot k. `game::read_bag` files each slot under its
  number (`game::file_slots`, `tests/test_slots.cpp`): the slots in play must be numbered 0..size-1 once
  each, the locked ones follow in list order; otherwise the list order is kept and the item box's game
  calls are refused. (Version 0.1.0's first item-box build used the list order: a Store after a
  rearrangement named the wrong slot to `removeStock` - it refused, as the slot it hit was empty, but it
  could have removed another item. Store now also checks the stock's `Index` before any call.)
- **Two-slot items** store nothing in their second slot. `InventoryManager.isFatStock(stock)` (exe+0x18657F0;
  `Slot.get_IsFatSlot`, `Inventory.isFatSlot`) asks every stock all three, whatever it holds:
  `FatItemList.Contains(ItemId) || (WeaponParts & 1) || FatWeaponList.Contains(WeaponId)` - the middle one is
  `isFatWeapon(WeaponType, WeaponParts)` (exe+0x506AD0, static) inlined, which opens `test r8b,1; je; mov al,1;
  ret` before it asks the list. So **a weapon grows to two slots when part A is fitted** (`WeaponParts` None 0,
  A 1, B 2, C 4, FullCustom 7): the Matilda went from parts 6 to 7 with its stock (seen in game 2026-09-16).
  `isDeadSlot(i)` (exe+0x508370; `Inventory.isDeadSlot`, `Slot.get_IsDeadSlot`, `StockItem.get_IsDead` alike) is
  `(i & 3) != 0 && i < _CurrentSlotSize && isFatStock(getSlot(i - 1)._Stock)`. `StockItem.isBlank` (virtual[5],
  exe+0x497730) is `DefaultItem.IsBlank && !IsDead`, so `Slot.get_Type` (exe+0xBC7AA0: Blank for a blank or
  force-blank stock, else Item, else Weapon, else Dead 3 when `IsDeadSlot`, else -1) never calls a dead slot
  Blank and the game's own fills (`addStock`, `Inventory.enableGetFatItem`: two Blank slots anywhere) skip it -
  but `setStock` does not check, and a dead slot holding an item is typed Item. Rows are 4 wide:
  `Inventory.isRightEdgeSlot` is `(i & 3) == 3` (the mod reads the mask out of that method's code, and the parts
  mask out of `isFatWeapon`'s; `game::fat_by`/`dead_slots`, `tests/test_slots.cpp`). Early builds of the mod took
  `IsForceBlank` for the second half - wrong; version 0.1.0 checked only the lists, and its Take put the Square
  Crank into the slot behind the Matilda with its stock (log 2026-09-16 21:39). The panel now shows an item in
  a dead slot as under its neighbour, with Store to get it out.
- **Countdowns**: `app.ropeway.gui.CountDownBehavior` (`<CurrentTimerFrame>` +0x28 - frames, `FRAME_COUNT`
  60 -, `<CurrentData>` +0x30, `<SavedFlag>` +0x38, `<SavedTime>` +0x48, `<IsTimeUp>` +0x4C) runs the
  self-destruct countdown itself, not just its display: `lateUpdate` (virtual[6], exe+0x207500) calls
  `updateCountDown` (exe+0x1F4A4F0, its only caller) and `dispCountDown` while `<CurrentData>` is set.
  `updateCountDown` returns early while the game is busy (a movie, a camera event, saving or loading, a
  grapple kill), closes the countdown when `CountDownSingleData.ClearFlag` is set (`checkCountDownClear`),
  takes the frame's time off the timer (exe+0x1F6C4E0: via.Application's frame delta, scaled), clamps it at
  0 with `<IsTimeUp>` set and `MainFlowManager.goGameOverWhite` / `goGameOverSimple` called (`WhiteOutEnd` /
  `DeadEnd`), and sets `GlobalUserDataManager.setFlag` for each `SecondsLeftFlagList` entry whose `Time` it
  passed this frame (the alarms). `CountDownUserData.DataList` holds the `CountDownSingleData`
  {`TimeLimit`, `StartFlag`, `ClearFlag`, `DeadEnd`, `CloseEnd`, `WhiteOutEnd`, `MinimumTimeList`,
  `SecondsLeftFlagList`}. `GUIMaster.RefCountDown` (+0x380) is only a `GameObjectRef` - no need: the vtable
  hook is handed the behaviour every frame. The Ghost Survivors' `RogueCountDownBehavior`
  (`<CurrentTimerFrame>` +0x28, `<CurrentCountType>` +0x2C: CountUp 0, CountDown 1) counts up in `lateUpdate`
  and down in `updateCountDown` (exe+0x1D9E0E0), which nothing calls directly and no delegate is made of -
  reflection, or unused. Freeze countdown timer hooks both (Events).
- **The Ghost Survivors' run timer is the clock itself** (Freeze play time, 2026-09-23; reported: "with freeze
  time active, I still see the active timer running in game"). The HUD timer of a Ghost Survivors run is the
  count-*up* side of that same `RogueCountDownBehavior`: `lateUpdate` (**virtual[6]**, method 248804,
  exe+0x1B6CE0 - no direct callers, the engine's component walk reaches it, its code shared by nothing) calls
  `updateCountUp` (exe+0x1D9E4B0), which reads **`GameClock.get_ActualRecordTime` itself** (exe+0x1D9E540),
  converts it to a float into `<CurrentTimerFrame>` and lets `dispCountUp` (exe+0x1D2C920) draw it into the
  minute/second/millisecond panels - all in the one call. So writing the field after `lateUpdate` would be
  undone by the next frame's read; the held time has to be in the clock **for the length of the call**, which
  is what the hook does (Events). The count-down side is untouched: it works off the frame's time, not the
  clock. **The other display of the clock is not hookable**: the pause menu's play time is
  `RecordManager.get_RecordHour` / `get_RecordMinute` / `get_RecordSecond` (exe+0x2CA950 / 0x2CB120 /
  0x2CBA40), read **once** by `gui.PauseBehavior.open(Action, Action<Result>, Mode)` [87354] (exe+0xEE5D70) -
  which `GUIMaster.openPause` reaches by a direct `call rel32`, so no entry or slot of the game's leads to it.
  Its other readers are `ClearStateBehavior.open` (the clear screen) and `RogueActivityLogManager.backupLogData`.
  So with the freeze on the pause menu still shows the clock's time while the mod's own panel beside it shows
  the held one; the user was told and chose to leave it (2026-09-23). Nothing else in the game draws the clock:
  `GameClock.getElapsedTime` has two callers, both `ClockTimer` (a gameplay timer, not a display), and
  `get_GameClockRecordTime` / `get_RecordTime_Sec` have none.
- **Stacks - how the game's item box stores** (Store): `NewInventorySlotBehavior.executeCommandItemBoxMode`
  (exe+0x1D435E0) walks the screen's box list (`<ItemBoxStocks>` +0xA8, a `List<StockItem>` it keeps packed,
  `packedItemBox`) in order. A non-blank entry `i` for which `isAbleToCombineSlotAndBox(slot, i)`
  (exe+0x129FF60) holds gets `exchangeItemSlotAndBox(slot, i, isSlotNum = moveNum > 0, isBoxNum = false,
  isItemBoxIn = true)`, which moves `min(moveNum, Slot.get_MaxNumber - entry count)` - the entry's count goes up,
  the slot's down (field writes; `removeStock` when the slot reaches 0) - and returns false while some remain,
  so the walk goes on; the first blank entry takes the rest as a copy (`StockItem.setStock`, its count set to
  the rest) and ends it. `isAbleToCombineSlotAndBox`: both stocks `ItemData.isItem` (not blank, `ItemId != 0`)
  with the same `ItemId` and `ItemManager.getItemDisposable(id)` (exe+0x1C792B0, the table below) ==
  `Disposable.MultipleUse`; or both `isCombinedWeapon` (`ItemData.isWeaponSupport` - WeaponId 46..48 or 63..81,
  the knives and WP6000-6900 - and `EquipmentDefine.getCategory != Knife`) with the same `WeaponId`.
  `Slot.get_MaxNumber` (exe+0xBC5BC0): an item's `ItemManager.getItemMultipleUseMax(ItemId)`, a weapon's
  `WeaponBulletUserData.getNumber`. The screen's `getItemMax` is the same (1 for anything else), `getMoveItemNum`
  is that maximum for a blank entry and maximum - count for a same-item one, `getMoveNumSlotToItemBox` sums it
  over the box, capped at the maximum.
- **`ItemManager.getItemMultipleUseMax`** (exe+0x1C79620, an instance method: the id in r8) is a compiled switch -
  `cmp r8d,0x30; jle`, `lea eax,[r8-0x35]; cmp eax,5; ja`, `add r8d,-0x119`, three jump tables of RVAs
  (`lea rdx,[image]; mov ecx,[rdx+rax*4+table]; add rcx,rdx; jmp rcx`), `mov eax,imm32; ret`. The mod runs it at
  start-up for every `Item.ID` below `MAX` (297) with `src/switch_eval.h`, a small x86-64 evaluator (registers,
  compares, jumps, jump-table reads; anything else - a call, a store - answers "unknown" and turns stacking off).
  `tests/test_switch.cpp` runs it on the file: all 1 026 ids against a separate Python emulation, and all 252
  `getCategory` answers against that method's table decode. Build 11636119: Handgun Ammo 60, Shotgun Shells 20,
  Submachine Gun Ammo 200, MAG Ammo 20, sm70_104/105 20, sm70_106 10, Acid and Flame Rounds 10, Needle Cartridges
  100, Fuel 400, Large-caliber Handgun Ammo 60, High-Powered Rounds 20, Ink Ribbon 9, Wooden Boards 9, Spare Key
  (sm73_133) 9, sm75_017-022 9-10, everything else 1 - and some weapon parts (High-Capacity Mag. (Matilda) 12,
  Reinforced Frame 5, ...) whose element is not MultipleUse, which is why the Disposable test matters.
- **`ItemManager.<ItemElementTable>`** (+0xA0): `Dictionary`2<Item.ID, item.ItemElement>` {`_buckets` +0,
  `_entries` +8, `_count` +0x10, `_freeList`, `_freeCount`, `_comparer`, `_syncRoot`}; `_entries` is an array of
  `Dictionary`2.Entry<Item.ID, item.ItemElement>`, a 0x18-byte value type {`hashCode` +0 (negative when removed),
  `next` +4, `key` +8, `value` +0x10}; `ItemElement` {`Type` +0, `Disposable` +4 (Infinity 0, SingleUse 1,
  MultipleUse 2), `EffectiveValue` +8, `IsRemovable` +0xC, `DetailInfo` +0x10, `WeaponCrackInfo` +0x18}.
  `<WeaponElementTable>` (+0xA8) is the weapons' (not read).
- **What the mod does with it** (`game::stack_max`, Store in `inventory.cpp`): items only - weapons move whole
  (the game would stack grenades by `WeaponBulletUserData`); every same-item entry with room is topped up before
  an empty one is used, because the saved list, unlike the screen's, can have blanks between items (a Take leaves
  one); what finds no room and no empty entry stays in the slot (a count write, as the game's own). Take still
  moves a stack whole (the game would stop at the slot's maximum).
- **Weapons in the inventory editor** (2026-09-20; the user reported the Flash Grenade missing from the item
  picker). **The grenades are weapons, not items**: `EquipmentDefine.WeaponType` WP6200 Hand Grenade (65) and
  WP6300 Flash Grenade (66), beside the knives WP4500/WP4510 - `gen_items.py`'s `kSubWeapons`, the weapons with
  no magazine - so the picker, which walked `items::kItems` alone, could never offer them. Nothing is missing
  from the item list itself: all 126 items the game's text names are listed, and the 165 it leaves out have no
  name (`#Rejected#` or absent) - development placeholders.
  - **How the game makes a weapon from nothing**: `ItemLockerManager.AddWeaponToStrage(AddItemData.ItemData)`
    (exe+0x1976780, the unlockable weapons' path from `CheckExtraWeapon`) news an `InventoryManager.ItemData` and
    an `ItemExData` and calls **`ItemData.setWeapon(weapon_id, parts, count, bullet_id, ex_data)`** [5866]
    (exe+0x188A6D0) with the table entry's {`type`, 0, `ammNum`, `ammType`}. `setWeapon` is `setBlank()`
    (virtual[4]) and then, unless `weapon_id` is -1, four writes into `DefaultItem` - `WeaponId` +0x4,
    `WeaponParts` +0x8, `Count` +0x10, `BulletId` +0xC - and `ExData.setExData`. So **a weapon in an ItemData is
    those five fields and nothing else**, and a blank box entry (whose ExData the game already made) written by
    `game::write_item` is byte for byte what `setWeapon` would leave. That is what the editor does: the weapon is
    built in an empty item box entry - the box is a `List<ItemData>`, data alone, and the game puts weapons there
    itself - and that entry is handed to `InventoryManager.setStock`, exactly as a Take from the box
    (`inventory.cpp`, `set_weapon`). Nothing leaves the slot until everything the change needs is there; what was
    in the slot goes out the way the box's Store does (`unequipSlot` + `putShortcutWeapon` for a weapon, then
    `removeStock`), and a failed `setStock` puts the old stock back through the same entry.
  - **The ammo a weapon records** (`BulletId`): `WeaponBulletUserData.LoadingPartsCombination` has, beside the
    number the durability code reads, **`_OverwriteKind` +0x3C and `_Kind` +0x40** (an `EquipmentDefine.Bullet`,
    `WeaponLoadingpartsCombination.getKind`), chosen exactly as the number is - lowest `_Priority` among the
    entries whose parts the weapon has. `EquipmentDefine.getItemID(Bullet)` (static, exe+0x19E5880) turns it into
    the `Item.ID`, and is a compiled switch the mod runs with `switch_eval.h` (`game::weapon_bullet_id`, cached
    per weapon and parts beside `weapon_full_count`). Its inverse `getBulletType(Item.ID)` (exe+0x19E4860) is a
    jump table over ids 15..29 of which **12 name a kind**; `tests/test_switch.cpp` round-trips every one through
    both. A weapon whose kind names no single item (two kinds at once) falls back to the lowest bit set; a weapon
    with no kind - a knife, a grenade - keeps `BulletId` 0, which is what an empty weapon holds.
    `ItemManager.getMainWeaponOtherBulletID` (exe+0x56F650) is the same two switches around
    `WeaponBulletUserData.getMainWeaponOtherBullet`, which is how the pairing was found;
    `item.WeaponCombinationList.getBullet` (the `WeaponPair` list, weapon -> ammo -> result) is the game's other
    reading of it, but nothing in the database holds that list, so it is not reachable by name.
  - **What the picker offers**: the 28 listed weapons, with no parts (`WeaponParts` 0, so only a weapon in
    `FatWeaponList` takes two slots - checked with the free-pair rule at the chosen slot) and the count starting at
    `game::weapon_full_count`, which the tick publishes in the view (the panel never reads the game). The infinite
    weapons are rewards: `ItemLockerManager.CheckExtraWeapon` and `InventoryManager.deleteInvalidWeapon` take one
    back out of the box and every inventory at the next load unless its reward is open (Records and their rewards),
    which the help text says.

### Save files and the Load Game screen (the save files, src/savefiles.cpp)
- **The files**: `Steam/userdata/<id>/883710/remote/win64_save/`, one per slot of the engine's save service
  (`via.storage.saveService.SaveSlot`: System -1, Auto 0, Slot 1.., SlotMax 1024): `data00-1.bin` the system save,
  `data000.bin` slot 0 (the auto-save), `data001Slot.bin`.. slots 1 and up, `data021Slot.bin` The Ghost Survivors'
  (slot 21). The name is built natively from "data", a zero pad ("00" below 10, "0" below 100 - the UTF-16 strings after
  "Slot" at exe+0x5A7BFE8), the number, "Slot" for slots 1 and up, ".bin" (the service's impl holds "data" +0x90, "Slot"
  +0x70, ".bin" +0xB0; set in its constructor exe+0x2BF0BE0 / exe+0x2BF41C0). `SaveDataManager.getSaveDataIndex(mode,
  offset)` / `getSaveListCount(mode)` (static, jump tables exe+0x175E5F0 / exe+0x175E888): SYSTEM -1 / 1, **SCENARIO 0 /
  21** (the main game's list: the auto-save and slots 1-20), FOURTH and TOFU 0 / 0, ROGUE 21 / 11, DEBUG and STGJMP1-4
  32 / 200.
- **Steam, not the disk**: the engine's Steam backend (vtable around exe+0x5A7C4C0) keeps the slots through
  `ISteamRemoteStorage` v014 (`SteamInternal_ContextInit` accessor record at exe+0x9086578, the interface at +0x10 of it
  at run time): GetFileSize (vtable +0x78) + FileReadAsync (+0x18) + FileReadAsyncComplete (+0x20) to read,
  FileWriteAsync (+0x10) to write, FileExists (+0x68), GetFileTimestamp (+0x80), FileDelete (+0x30) (exe+0x2BFD700, result
  53 `Failed_Steam_RemoveError` when it fails) - each with the wide path converted to UTF-8 (exe+0x1FC2730). A file put
  in the folder behind Steam's back is not one the game's FileExists is sure to see; the mod goes through the same API.
- **The container**: `DSSS` v2, then `SaveDataEncryptionType` 3 (BlowFish) twice; the rest Blowfish-CBC with a zero IV
  (exe+0x2BED4B0: `xor` with the previous ciphertext block) under the key byte array at impl +0x900 (4-56 bytes;
  `get_ValidSaveEncryptionKeyBlowFish`; no managed caller sets it). The first block decrypts to `DSSSDSSS` (the check at
  exe+0x2BEFE09, a second key tried when it does not). All five files begin with the same 0x10 bytes of ciphertext, and
  the three game saves share their first 0x48 bytes: one key for every slot (the account's), and nothing slot-specific
  that early. `GameSaveKeyData` (region, CERO, IsOverPlayGo) is the only key data the game adds. Whether a slot's number
  is inside the file anywhere was not settled offline - the first in-game copy tells (Open questions).
- **The detail table**: `via.storage.saveService.SaveService` is native (static methods only; its object is the global
  exe+0x9178CE0, read by every wrapper; the impl - the Steam backend - at +0x8). Its table of the slots is an array of
  `SaveFileDetail*` at impl +0x118 (count +0x120), one per slot that has a save; `findSaveFileDetailTbl(slot)`
  (exe+0x8781C0) returns the one whose Slot matches, **or null**. A `SaveFileDetail` has no fields in the database; its
  getters are one load each (tested on the file, `tests/test_savefiles.cpp`): Slot +0x10 (`get_EmptyData` is `Slot ==
  0x80000000`, `get_InvalidSlot`'s constant), Title +0x18, SubTitle +0x20 (the scenario), Detail +0x28 (the localised
  fields the game parses), LastUpdateTimeStamp +0x30 (.NET ticks, UTC: `getDayStringFromTicks` makes a DateTime of kind
  Utc), UseSize +0x40, FreeSize +0x48. `System.String` alike: length +0x10 (`get_Length`), UTF-16 from +0x14
  (`get_Chars`). Rebuilding it is the service's worker thread's (the loop around exe+0x2BF6DB0, woken by `SetEvent` of
  service +0x78; the rebuild exe+0x2BF3050): exe+0x2BF8F20(service, user) asks for one unless the table is fresh
  (`impl->vfunc[34]()` = impl +0x55c is the user, the generation read from a user object equals service +0x2A8, impl
  +0x54C < 10, impl +0x9, service +0x2AD clear), writing impl +0x55C, service +0x2A8 and **service +0x2AC = 1** (busy)
  before the event. `SaveService.updateSaveFileDetailTbl(user)` [299023] (static, exe+0x12CB40) is `service+0x2A8 = -1;
  jmp exe+0x2BF8F20`: the forced re-read. `get_SaveFileDetailUpdateRequest` is the busy test (+0x2AC, SaveState +0x10 !=
  IDLE, +0x2AD).
- **The game's list**: `SaveDataManager.getSaveFileDetailList(mode)` (exe+0x175E620) calls exe+0x2BF8F20 only when the
  current user (the global exe+0x91AC5C8) is not `<LastDetailUserIndex>` (+0x58) - so once per user, at boot -, returns
  **null while the table is busy**, else a new `List<SaveFileDetail>` of `findSaveFileDetailTbl` for each slot of the
  mode, nulls included. `getMinimumIndex(mode)` walks it for the first slot with a save.
- **Delete** - `requestRemoveGameData(mode, slot)` (exe+0x1784A40, no caller in the game, like `requestRemoveAllGameData`)
  is `if (!IsBusy) { RemoveRequest (+0x3A) = true; SlotId (+0x3C) = getSaveDataIndex(mode, slot); }`. `update`
  (exe+0x179E510, state table exe+0x17A193C) in REQUEST_WAIT - unless `MainFlowManager.<IsInResetForXB1>` /
  `<IsDoneResetForXB1>` (+0x0/+0x1, Xbox only) - takes SaveRequest, LoadRequest, RemoveRequest, RemoveAllRequest in that
  order; the remove: exe+0x2BF8F20(service, user) (a table re-read if stale), `<LastSaveLoadUserIndex>` = the user,
  exe+0x2BF6B20(service), the native `removeSingle(SlotId)` (exe+0x2BF8590: slots -128..1024; only from SaveState IDLE or
  DialogIDLE, else it returns false and nothing happens; sets RemoveStart 11 and wakes the worker), step REMOVE_DATA_WAIT
  4. That state (exe+0x179F808) waits for SaveState IDLE, then step REQUEST_WAIT and `RemoveRequest = false` - **it never
  looks at the result**. RemoveAllRequest goes slot by slot from `getMinimumIndex(SCENARIO)`.
- **The screen**: `gui.LoadBehavior` < `gui.SaveLoadBaseBehavior` (its only other subclass: `SaveBehavior`, the typewriter's
  save screen), opened by `GUIMaster.openLoad` from `TitleFlow.<update>b__39_33`, `PauseBehavior.<toLoad>b__103_0` and
  `GameOverBehavior.toLoad`. Fields (declared by the base): `<SaveFileDetailList>` +0x20, `<SaveFileDetailTextList>` +0x28
  (`List<List<String>>`), `<SaveFileScenarioList>` +0x30, `<ScrollOffset>` +0x38, `<KeepRequest>` +0x3C, `SaveModeValue`
  +0x40, `GuiList` +0x88 (`via.gui.ScrollList`), `IsLoaded` +0x90, `IsSaved` +0x91. `update` [194697] (virtual[15],
  exe+0xD35090, LoadBehavior has none of its own): **`if (SaveFileDetailList == null) { if (reloadSaveDetail()) {
  initList(); setList(0); } return; }`**, then no input while `SaveDataManager` loads (step LOAD_WAIT or LoadRequest:
  `IsLoaded = 1`) or saves (`IsSaved`), its step is an error (>= 16), or a general dialog is up; `KeepRequest` re-runs
  `request()` (the load) every frame until the manager takes it. `reloadSaveDetail` (exe+0xD124E0): the list from
  `getSaveFileDetailList(SaveModeValue)` (false while it is null), then for each detail
  `getStringFromDetailString` (scenario, difficulty, the save count, location, area, purpose, player name - parsed out of
  the localised Detail) and `getScenarioTypeFromDetailString`. `LoadBehavior.initList` (exe+0xDEB1B0) puts the cursor on
  `SaveDataManager.<LoadGameDataIndex>` and sets the scroll list up for `getSaveListCount`. `set_SaveFileDetailList`
  (exe+0x5EF70) is the VM's refcounted reference store, folded with 100+ setters.
- **What the mod does** (`src/savefiles.cpp`, the user's approval above): classes, fields and methods by name at
  start-up; everything out of code - the getters' layout, the list's layout (`getSaveDataIndex(SCENARIO, 0)` = 0 and
  `getSaveListCount(SCENARIO)` = 21, run by `switch_eval.h` with the mode in rdx and the offset in r8), the two refresh
  calls' code - read by the tick only, into a copy published at once, once a second until it reads (the first via.*
  method code the mod reads; its linking time is not known). The hook on LoadBehavior's vtable slot 15 (the base's
  code, so the typewriter's SaveBehavior, same code in its own table, is not hooked) stamps the screen as up (SCENARIO
  list, no load under way) and, twice a second or when the list object changes, reads the rows: row i is slot first +
  i, and a list of another length, an entry that is not a SaveFileDetail, or a detail whose own Slot is not its row's,
  makes the list **not consistent** - shown, but no change goes by it. Every request carries what the panel showed of
  its rows (a hash of the slot, the engine's strings and the time stamp) and is dropped if the rows moved on.
  No change is taken while a refresh is pending, or before the list has been read again since the last one.
  **Delete**, from the tick: the manager's requests read (all clear, step idle), then armed with **one
  compare-exchange** of the qword the four request flags and `SlotId` share (+0x38..+0x3F of its fields, 8-aligned -
  checked at run time, `mem::exchange_ptr`) from "no request" to "RemoveRequest, this slot", so a request of the
  game's own (a load picked in the same instant shares `SlotId`) cannot land in between; were the fields laid out
  otherwise, field by field instead - `SlotId`, read again, `RemoveRequest`, read again, withdrawn at once on any
  foreign request. When the manager has cleared it and gone idle, the list is read again; a request the game has not
  taken in 10 s is withdrawn, and at a minute it is disarmed whatever the step (never left to run later against
  another `SlotId`; the remove-wait state never reads `RemoveRequest`). The list read again is the truth for a
  delete, withdrawn or not. **Copy**: a thread of
  the mod's own lists Steam's files (`GetFileCount` / `GetFileNameAndSize`), checks every row the game shows as used
  has its file and no empty row has one (`savefiles_rules.h`), reads the source (`GetFileSize` + `FileRead`) - and the
  target if it has a save -, `FileWrite`s the target's name (same folder, the pattern above), reads it back and
  compares, and puts the target back as it was (or deletes it) if it does not match; the list is read again once the
  game's saves are idle (a load the player started meanwhile goes first). The refresh, from the hook after the game's
  update and only while no load is starting: `updateSaveFileDetailTbl(<LastDetailUserIndex>)` and
  `set_SaveFileDetailList(null)` (both code pointers checked first); the screen's own next updates read the table
  again (null while the worker re-reads) and rebuild the rows - its cursor back on the last save loaded. The result
  is checked on the target's row of the list read again (empty; the copy with the source's scenario and strings); a
  change that already failed keeps its error rather than a later "done".
### The game's text (tools/pak_msg.py, tools/gen_items.py)
`re_chunk_000.pak` (+ `.patch_001/002`, later ones override by hash) is KPKA v4: 16-byte header,
then 48-byte entries {murmur3 of the lower- and upper-case path, offset, compressed, size,
attributes (low nibble: 0 raw, 1 raw deflate, 2 zstd), checksum}; the table is not encrypted. Message
files are found by content (`GMSG` at +4), v17 and a newer 0x03010011 layout, both parsed by the same
reader: header counts at 0x10, data offset 0x20, then (for both seen here) an 8-byte field, language
table, attribute tables and one u64 per entry; an entry is {Guid, crc, hash, name, attributes, one
string offset per language}; the string pool from the data offset on is obfuscated with RE Engine's
rolling XOR (`key[i & 15] ^ previous ciphertext byte`). Item names are `ITEM_NAME_70_000` for
`Item.ID` `sm70_000`, weapons `WEAPON_NAME_WP0000`; cut content reads `#Rejected#`. The name an item
takes once examined is spelled three ways: `ITEM_NAME_73_CHANGED_133`, `ITEM_NAME_CHANGED_72_200`
and (sic) `ITEM_NAME_CAHNGED_77_004`. Even with those, some items read exactly the same (herbs
sm70_001-003 and sm70_051-053, both Power Panel Parts, both Boxed Electronic Parts, the wristbands
sm73_430-432 and 435-437, the Wooden and Tin Storage Boxes): `gen_items.py` appends the code to every
one after the first, since a picker row's label must be unique for the player and for ImGui (whose
ID-conflict check fires on equal labels in one list). 131 items and 28 weapons are offered in the
pickers; `WP8400/8600/8700` are the unlockable infinite versions.
- **The item box panel's banks are the game's own categories** (2026-09-19). The game files every item it
  shows under an `app.ropeway.gamemastering.Item.Category` (Weapon 0, **Custom** 1, Material 2, Bullet 3,
  Heal 4, Support 5, Key 6, Etc 7; its text names them `SYS_INVENTORY_CATEGORY_*` - Key Item, **Custom
  Part**, Resource, Ammo, Recovery Item, Sub-weapon, Main Weapon, ...). `ItemManager.ItemOrderUserDataList`
  (+0x78) holds one order user file per category -
  `natives/stm/SectionRoot/UserData/ItemList/Order/{Weapon,Support,Heal,Material,Bullet,Custom,KeyItem}Order.user.2`,
  each a `List<ItemOrderSingleData>` of `Item.ID` (the weapon two: `WeaponOrderSingleData`) in the order the
  screens show them - and `makeTable` turns them into `ItemOrder` (`Dictionary<Item.ID, Item.Category>`) and
  `WeaponOrder`. This build: Bullet 10, Custom 15, Heal 14, KeyItem 85, Material 4, Support 4, Weapon 24.
  `gen_items.py` reads the five item ones **by path** (`pak_msg.path_key`, the murmur3 pair) for each item's
  bank: Key and Custom both go under the panel's **Key items** (the player's choice, 2026-09-19), Heal and
  Bullet under Heals and Ammo, Material (the four gunpowders) under Other; the eight listed items no table
  names - the six maps, the Hip Pouch and the unused Laser Sight (JMB Hp3) - fall back to the id's family
  (`fallback_bank`), and two the game calls Key Item are filed under Other all the same (`kBankOverrides`:
  the **ink ribbons** and the **wooden boards**, stocked up on like the gunpowders rather than carried like a
  key - the player's call, 2026-09-19). So: 121 of the 131 listed items from the game's tables, 2 by hand, 8
  by family. Until then the banks were that family rule alone, which also filed every weapon part under Other
  (reported in game: the High-Capacity Mag. (JMB Hp3) and the Shoulder Stock (GM 79)).

**`pak_msg.py` used to miss 42 message files** (fixed 2026-09-19): `find_msgs` inflated only the first 64 KB of each
entry, and a zstd entry whose first block needs more yields nothing, so the file was skipped - among them
`Mes_Sys_Bouns_*` (the records' names and conditions), options, save/load and tutorial text. It now reads up to 256 KB
of such an entry (454 message files, ~4 s for the scan).

- **The entry table** (42 222 entries in the base archive; 331 and 471 in the two patches) is **sorted by
  the upper-case hash**, and the hashed path includes the platform folder (`natives/stm/...`) - so a path
  can be tested without any file list, by hashing it and looking for the pair (`tools/pak_msg.py`'s reader;
  the magic check is at exe+0x206C34E, `cmp dword [rbp+7], 'KPKA'`, and the loader reads count x 48 bytes).
  **The engine does not plain-binary-search it**: Fluffy Mod Manager's `invalidate` install zeroes the
  matched entries' hashes *in place*, which leaves holes in the sorted order - 49 of them here, enough that
  a textbook binary search would miss 74 entries nobody touched - and the game reads the archive fine. It
  must re-sort or index the table at load. (Chased 2026-09-17 as a suspect for a broken scripted event and
  disproven: the entries were re-zeroed and the event still ran.)
- **Textures**: every one in this build is `TEX\0` **version 34** (so `.tex.34` is the RT extension, not a
  pre-RT one), header 0x38 bytes then the payload; {width u16 +0x8, height +0xA, depth +0xC, `mipCount:4
  imageCount:12` u16 +0xE, DXGI format u32 +0x10 (98 BC7_UNORM, 99 BC7_UNORM_SRGB, 71 BC1, 95 BC6H), -1
  +0x14, **+0x1C** `(n << 7) | flag`}. That +0x1C field is derived from the texture's shape - 1024x1024 /
  1 mip / BC7_SRGB is always 0x880 (35 of 35 sampled) - and **is never 0 in this build** (0 of 997 sampled),
  so a zero there is the tell of a pre-RT mod: fixing it is a four-byte header patch, the payload unchanged.

### The game's frame (src/app.cpp, src/dispatch.cpp)
RE2's window thread (the WinMain thread) blocks in `GetMessageW`; the game updates in the engine's jobs
and presents from a render thread, so there is no pump to tick from. RE Engine's `via.Application`
runs each frame as a table of module entries (REFramework's `sdk::Application::Function`), and the mod
swaps two entries' function pointers - a pointer store, no code patch, no game call.

- **The table**: `add_function(Application*, object, group, priority, function pair*, name)` at
  exe+0x1F64580 is the only writer, found by its instruction bytes (`app.cpp`), which also give the layout:
  count `Application+0x3A4`, table `+0x800`, **0xD0-byte entries** {object +0x0, function +0x8, int32
  this-adjustment +0x10, name +0x18 (the `via.ModuleEntry` name), priority u16 +0x20, group u16 +0x22,
  ...}. Its single caller (exe+0x57B5F90, `register(object, priority, function pair)`) takes the name from
  a priority-keyed map, derives the group from the priority (≤72 → 0, ≤124 → 1, ≤142 → 2, ≤279 → 3,
  ≤323 → 4, above → 5) and loads the Application object from the global at exe+0x9178EA8
  (`mov rcx,[rip+X]` just before the call).
- **Order**: after registration the table is sorted in place by the u32 at +0x20 (group, then priority;
  comparator exe+0x1F653E0), and `{first, count}` per group is written at `Application+0x34800`. The mod
  hooks only once the count holds still across two polls and the table is in that order, and checks once
  a second that its entries did not move.
- **Calls**: every runner does `function(object + adjustment)` (one argument; e.g. exe+0x1F65CA1 for
  group 0). **Group 3 (priorities 143-279: `UpdateBehavior` 187, `LateUpdateBehavior` 214,
  `PrepareRendering` 222, `LockScene` 241, `WaitRendering` 242, `BeginRendering` 245, `EndRendering`
  268) runs as jobs**: one 0x1F8-byte job per entry whose wrapper (exe+0x1F65200) tail-calls the entry,
  scheduled on the thread pool at exe+0x9178EC0 as its prerequisites finish (exe+0x1F65140), or serially
  (exe+0x1F664F7) in another mode. So `UpdateBehavior` may run on any pool thread; `dispatch::frame`
  serialises the tick with a try-lock and never waits.

### Game calls (src/call.cpp) - the item box's Take and Store, the difficulty switch, the records' achievements
- **The game's own item box screen** (`app.ropeway.gui.NewInventorySlotBehavior.exchangeItemSlotAndBox`,
  exe+0x1D40200) moves an item between a slot and the box's `List<StockItem>` with: `isFatStock` +
  `checkSlotFarRight` (a two-slot item cannot start in the right column); for a main weapon going out
  `Inventory.unequipSlot(i)` then `InventoryManager.putShortcutWeapon(i)`; `InventoryManager.removeStock(i)`,
  `setStock(i, stock)`, and `StockItem.setStock(ItemData)` copies. `InventoryManager.addStock` picks the
  first slot whose `Slot.get_Type` is Blank (not two-slot aware), then `setStock`,
  `setDoneGetStockEffect`, `updateInventoryFlags`.
- **What they do**: `setStock(slot, stock)` (exe+0x1889EE0) requires `slot < _CurrentSlotSize` (not that it is
  free, nor that it is no dead slot), calls the stock's virtual `isBlank` (ItemData virtual[5], so a box `ItemData` is accepted), then
  `Inventory.setSlot` with the item or the weapon overload - `Slot.set` copies ids, count,
  `AdditionalItem` (`ItemData.setAdditional` copies values) and `ExData` (`ItemExData.setExData`
  copies) into the slot's own stock, then invokes `OnAddSlot` → `Equipment.onAddWeapon`: `CreatedWeapons`
  (a `HashSet<WeaponType>`) and, the first time a type is added, `EquipmentManager.loadWeapon` +
  `instantiateWeapon` (asynchronous prefab). `removeStock(slot)` → `Inventory.removeSlot(Slot, false)`:
  `OnRemoveSlot` → `Equipment.onRemoveWeapon`, and the slot gets a **new** blank `StockItem` object.
  `Inventory.unequipSlot(i)` → `unequipMainSlot`/`unequipSubSlot`, which act only when that slot's stock
  `Index` is the equipped slot's. `ItemData.setData(ItemData)` and `setBlank()` copy and clear by value.
  No lazy creation exists: `Equipment.changeWeapon` just takes `getWeapon(type)` (null for a weapon
  written into a slot), so field-written weapons would have no model until a load rebuilt the
  inventory.
- **Calling convention**: a managed method is `ret f(VMContext* ctx, this, args...)`; after every call
  the engine tests `[[ctx+0x50]+0x18]` (the pending exception) and returns early when set. Native code
  enters managed code with a **bridge** (e.g. exe+0x93C55A): `ctx = get_thread_context(vm, -1)`
  (exe+0x1F7D4C0); if `[ctx+0x78]` (reference count) is 0, `begin_global_frame(ctx)` (exe+0x1F777F0,
  increments `[vm+0x458]`); `++[ctx+0x78]`; a pending exception goes to `unhandled_exception(ctx)`
  (exe+0x1F7A210: takes and clears `[[ctx+0x50]+0x18]`, hands it to the VM); `++[ctx+0x7C]`; the
  calls; `--[ctx+0x7C]`, exception check, a local gc when the local frame grew; `--[ctx+0x78]` and at 0:
  exception check, `local_frame_gc(ctx)` (exe+0x1F790B0), `end_global_frame(ctx)` (exe+0x1F781F0). The mod
  keeps the reference count part (the 0x7C counter only gates an early local gc).
- **Found at run time**: `get_thread_context` - the call in all 143 `mov rcx,[vm]; mov edx,-1; call`;
  begin frame - `jne +8; mov rcx,rax; call X` right after 141 of them; the reference count - the disp8
  every caller of begin compares with 0 (343 of 343); the exception handler - its unique body `48 8B 41
  ?? 48 8B D1 4C 8B 40 ?? 48 C7 40 ?? 00 00 00 00 48 8B 49 ?? E9` (frame and exception offsets read from
  it); gc and end - `call handler; mov rcx,reg; call gc; mov rcx,reg; call end` (326 of 326). Methods by
  name and parameter types (`re::find_method`, tested offline): the item box's seven all or none, and
  `setDifficulty` on its own, so either works without the other. A call that leaves an exception is
  logged by class, handed to the handler, and turns all calls off for the session (Take and Store fall
  back to field writes and refuse weapons; the difficulty cannot be switched).
- **Order the mod uses**: Take - pick the slot (a free one, or for a two-slot item a free pair in one
  row; never a dead or force-blank slot), `setStock(slot, box entry)`, check the slot, `ItemData.setBlank`
  on the box entry (field writes if that fails; the slot is removed again if the box entry cannot be
  emptied), `updateInventoryFlags`. Store - the box's stacks of the same item topped up (count writes, see
  Stacks), then for the rest `setData(blank box entry, slot stock)`, checked, and given the rest's count; for a
  weapon `unequipSlot` + `putShortcutWeapon`; `removeStock(slot)` and check it (else the box entry is cleared
  and the stacks put back); `updateInventoryFlags`. With no empty entry, what did not fit stays in the slot.
  The difficulty - only with the header and `GlobalUserDataManager._Instance` there (its null throws), see
  Difficulty.
- **The save files' refresh** (not through call.cpp: made from the `LoadBehavior.update` hook, on the screen's own
  thread, with the context the game handed the hook - no bridge needed): `SaveService.updateSaveFileDetailTbl(UserIndex)`
  [299023] (static: `f(ctx, user)`; the user is `SaveDataManager.<LastDetailUserIndex>`, read by the tick, 0-15 only)
  and `SaveLoadBaseBehavior.set_SaveFileDetailList(List)` [194681] with null. A pending exception after either turns the
  refresh off for the session. **Steam's remote storage** is called too, but it is not the game: the original
  steam_api64.dll's flat `SteamAPI_SteamRemoteStorage_v014`, `SteamAPI_ISteamRemoteStorage_GetFileCount /
  GetFileNameAndSize / GetFileSize / FileRead / FileWrite / FileDelete` (the delete only to take back a copy that did not
  read back), from a thread the mod creates for the copy.
- **The records' achievements**: `AchievementManager.unlockRecord(RecordId, Boolean)` [356242] (exe+0xB04210, `this` the
  AchievementManager singleton, `immediate` true as `clearRecord` passes it: no progress check) and
  `unlockRogueClearRecord(RogueRecordId)` [356240] (exe+0xB042F0), both only for a record their dictionaries map, then
  `AchievementServiceController.unlock` (virtual[35]: queued, sent by its update). Found by name and parameter types
  on their own, like `setDifficulty`.

### How the game calls its code - and the events the cheats hook (src/events.cpp)
- **Start-up linking**: 38 629 per-class records (0x28 bytes, `.data` exe+0x6F378C8: type index, flags,
  counts, static vtable, method code array, a BSS global) are walked by the loader (exe+0x87F254 →
  exe+0x1F7AE40), which copies each class's vtable and fills its methods' database entries
  (`methods[member_method+i]+8`) from the code array (exe+0x6D0C4E0, exe+0x6DE1AB0, the run at exe+0x70B0CB0);
  3 484 records of 24 bytes (exe+0x6E94590, `{&global, method index, 0}`, walked by exe+0x87F328 through
  exe+0x1F7AE20) point a global at a method's database entry. Code is folded (exe+0x4FD20 is 32 229
  methods'): never tell methods apart by code address.
- **Direct calls** (`call rel32`, most calls) cannot be hooked without patching code.
- **Delegates**: `mov r9,[rip+global]`, then the new-delegate helper (exe+0x1F796B0) makes {info, refcount,
  count +0x10, entries of 0x18 bytes from +0x18: {target, code, method entry}}, **copying the code out of
  the method's database entry** (`[entry+8]`); a virtual method's delegate takes its override's entry
  (exe+0x1F5FC00). Invoking (stub exe+0x52290) calls `code(ctx, target, arg)` per entry; removing
  (exe+0x1FC6D90) matches target and method entry, never code. So an entry exchanged before the game makes
  its delegates - at discovery, seconds before the title screen - makes the game call the mod at that event,
  and unsubscribing still works. Reflection's invoke also calls a non-virtual method through its entry.
  `re::hook_method` exchanges an entry only while it holds code in `.text` (a compare-and-swap,
  `mem::exchange_ptr`); `service()` re-hooks one the runtime fills again. The entries are in re2.exe's
  `.data`, which **Wine reports as `PAGE_WRITECOPY` however often it has been written** (a copy-on-write view
  does too - `tests/test_mem.cpp`): a check for `PAGE_READWRITE` refuses them (the first event build did, and
  hooked only the vtables); `exchange_ptr` writes copy-on-write pages as they are and unprotects anything else
  for the exchange.
- **Virtual calls**: `mov rax,[obj]; mov rcx,[rax-0x10]; call [rcx+vt*8]` (`InventoryManager.setStock` →
  `ItemData.isBlank`, vt 5 = +0x28): the vtable pointer sits just before the REObjectInfo and the slot is the
  method's `vt_index`, read at every call - an exchanged slot takes effect at once for every instance.
  Vtables are in `.data` (`Equipment`'s exe+0x7365B50, 59 slots) and can be shared (StockItem's with
  `InventoryListSettings.Param.StockInfo`). Interface calls read `[info-(3+n)*8]` (`IGameSaveData` n 0,
  `ITriggerFuncProvider` n 3). Behaviour callbacks (`update`, `start`) go through the vtable
  (component walk exe+0x1F90130 → exe+0x1F67000). The classes hooked this way have no subclasses.
- **Native code calling managed methods**: the VM's invokers (exe+0x1F67000 for a method with no arguments; siblings
  exe+0x1F5D960, 0x1F77820, 0x1FB10E0, 0x209C0D0, 0x27870B0, 0x27BD2F0, 0x2BFFF30) take a method entry and read its
  impl flags: a virtual one is called through `[[obj]-0x10][vt]` (an interface's through its table), anything else
  through the entry's code. The behaviour tree resolves its callbacks by name as it starts (exe+0x214D070, through
  exe+0x1F7D7E0: `appendedUpdateStart`, `appendedUpdate`, `start`, `update`, `end(via.behaviortree.ActionArg)` of
  `via.behaviortree.Action`, kept at +0xCF0, +0xCF8, +0xD00, **+0xD08**, +0xD10 of its module object) - the base
  class's virtuals - so an FSM action's `update` should reach its own class's vtable slot, which is where
  `SwitchItemToInventory.update` is hooked (the first-call log line will tell; Open questions).
- **Convention** (checked in each hooked method's prologue): `ret f(ctx, this, args...)`; a struct return
  (`ReduceResult`, 0x18 bytes; `Nullable<ReduceResult>`) is a hidden pointer first: `f(ret*, ctx, this,
  ...)`. After an original a hook does nothing more when `[[ctx+0x50]+0x18]` holds an exception.

| event | hooked in | what the cheat's hook does |
| --- | --- | --- |
| `EnemyController.HitController_OnHitDamage(DamageInfo)` [113641], the enemy's `HitController.DamageHitHandler` (subscribed in `doStart`; it calls `HitPointController.addDamage`, then `dead()` at 0) | entry | One hit kills, before: HP down to `DamageInfo.<Damage>`, `<IsKill>` set - not for 0 damage, `<NoDamage>`, or `<ConditionFlagsField2>.Flag` (`BitFlag<EnemyDefine.ConditionStateBitFlag2>`) with STOP_DAMAGE or GUTS_MODE; NO_DEATH loses its HP too (Enemy HP and death). G2's `Em7100Think.TiredHp` is topped up as well, the only way a hit staggers it (G2's stagger is not its health). A plant (`Em5000Think`) takes neither: its flame-only health is emptied and its HP put where `addDamage` lands it on 1, so the plant burns up on this hit - The plants' health is not what kills them |
| `EnemyReactionController.onHitDamage(DamageInfo)` [73442] | entry | One hit kills: the same, when the reaction handles the hit first |
| `SurvivorCondition.checkHitDamage(DamageInfo)` [55994], CheckDamageHitHandler (before `HitManager.hitSetting` decides `IsKill`) | entry | God mode: a player back to full first |
| `PlayerCondition.onHitDamage(DamageInfo)` [38168] (virtual[41]; sets poison after the HP change) | entry | God mode: **the health the hit took given back**, and poison cured, after. `checkHitDamage` only decides the hit does not kill - the game still takes the health, and until 2026-09-18 nothing put it back, so God mode showed damage |
| `PlayerCondition.<doSurvivorStart>b__36_0(int)` [38180], the `Action`1<int>` of `SurvivorCondition.OnChangeHitPoint` +0xA0 | entry | God mode: back to full while alive, the handler given the new HP - **but the game has never once called this hook** (every session in both logs, while the two above fire on every hit), so the health is put back in `onHitDamage` instead; kept in case it ever is |
| `GimmickTypeWriter.<getTriggerFuncCheckValid>b__8_0` / `b__8_1(Trigger)` [161177/161178] | entries | Save without ink ribbons on HARD: each runs the other |
| `TriggerUseItem.<>c__DisplayClass33_0.<registerUseMode>b__0()` / `_1.<registerUseMode>b__1()` [380469/380471], the use-item callbacks the inventory calls for the item picked | entries | Infinite wooden boards: the work's boards `ItemData` marked `CanReuse` before it is queued (a mark taken off at the trigger's next use with the cheat off) |
| `Weapon.onHitAttack(DamageInfo)` [330839] | entry | No durability loss: the equipped knife (`Inventory._SubSlot`) full before and after |
| `Equipment.defend(EquipCategory, int)` / `(WeaponType, int)` [47860/47861] | entries | No durability loss: the same |
| `Equipment.executeFire(WeaponType, int)` virtual[33] → `use` → `useMainWeapon` → `reduceSlot` | vtable | Infinite ammo: `Inventory._MainSlot`'s stock read before, its count put back after |
| `Equipment.onHitMeleeAttack(DamageInfo, Melee.Attack, int)` virtual[56] → `useSubWeapon` | vtable | No durability loss: the knife full before and after |
| `MainFlowManager.saveGameSaveData()` virtual[25], `IGameSaveData` slot 1 | vtable + interface table | Save without counting: `SaveTimes` back to the held count before the capture |
| `GameClock.saveGameSaveData()` virtual[32], `IGameSaveData` slot 1 | vtable + interface table | Freeze play time: the held time put into `_GameElapsedTime` before the capture, and the same taken out of `_InventorySpendingTime` so the enemies' clock does not move with it - in the file as well as live (the tick puts both back once the save is written). The clock itself is never stopped - the enemies are on it |
| `fsmv2.EndMeasureAndRecordGameElapsedTimeForExtra.start(ActionArg)` [369564] virtual[8], the last thing an extra-mode run does - it stops the clock measuring and records the clear time while the state is still IN_GAME (The extra modes record their clear time early) | vtable | Freeze play time, The Ghost Survivors and The 4th Survivor / The Tofu Survivor: the held time put in for that one call and taken straight back out, so the mission's saved record time (and its NEW RECORD) is the held one, as the results screen's already was |
| `CountDownBehavior.lateUpdate()` virtual[6] → `updateCountDown` | vtable | Freeze countdown timer: `<CurrentTimerFrame>` put back after the frame (not when it ran out in it) |
| `RogueCountDownBehavior.lateUpdate()` [248804] virtual[6] → `updateCountUp` → `get_ActualRecordTime`, every frame The Ghost Survivors' HUD timer is drawn (The Ghost Survivors' run timer is the clock itself) | vtable | Freeze play time: the held time put in for the length of the call, so the timer the player watches stands at it while the clock underneath keeps running for the enemies. Silent (per frame); the count-down side of the same behaviour is Freeze countdown timer's, on its own hook, and works off the frame's time |
| `RogueCountDownBehavior.updateCountDown()` [248810] | entry | Freeze countdown timer: the same for the Ghost Survivors' |
| `fsmv2.SwitchItemToInventory.update(ActionArg)` [369956] virtual[9], the item box's FSM action → `GUIMaster.openInventoryItemBoxMode` → `addRecordCount(73, 1)` | vtable | Item box without counting: `OpenItemBox` back to 0 after |
| `gui.NewInventorySlotBehavior.update()` [96410] virtual[15], the inventory screen's frame → ... → `useHealItem` → `addRecordCount(74, 1)` | vtable | Recovery items without counting: `UseHealItem` back to 0 after - every frame the screen updates, while on (where the count is, is kept: `game::hold_counter`) |
| `PlayerFootEffectController.onLand(JointPartsType, JointSideType, vec3*)` [52305] virtual[19] → `addPedometer` | vtable | Freeze step count: `<Pedometer>` back to the held count after |
| `gui.RecordBehavior.update()` [196123] / `gui.RogueRecordBehavior.update()` [159620] virtual[15], a records screen's frame (only while its object is active: `open` → `RopewayGuiBehaviorRoot.enableObject` → `GameObjectExtension.requestActive`) | vtable | The records panel: the screen stamped as up, and as taking input (`EnableInput_`) - nothing else read |
| `gui.SaveLoadBaseBehavior.update()` [194697] virtual[15] in **`gui.LoadBehavior`'s** vtable, the Load Game screen's frame (the typewriter's `SaveBehavior` has the same code in its own table, not hooked) | vtable | The save files: after the game's update, the screen stamped as up (SCENARIO list, no load under way) and its rows read (twice a second, or when the list object changes); a change's refresh made here - `SaveService.updateSaveFileDetailTbl` and `set_SaveFileDetailList(null)`, with the hook's context |

- **Not covered** (gaps the user accepted): One hit kills - G2's cling (`Em7100ActionState_DMG_CLING.start` →
  `Em7100Think.addDamage`, direct), parts with their own handlers (em6000 pustules, em6300 obstacles, em8200
  bombs), HP the game writes inline; No durability loss - `SurvivorCondition.useSubWeapon`; God mode -
  `EmCommonFsmAction_PlayerKill` → `deadHitPoint` (an instant kill, as before).
- **The game's own infinite flag** (`LoadingPartsCombination._Infinity` +0x19, `_InfinityVariable` a Guid;
  `Slot.get_Infinity` → `WeaponBulletUserData.getInfinity`, true for a weapon with no entry): a one-time write
  would skip every reduce, but `get_Number` answers -1 when infinite, so `set_Number` would store -1 into every
  count written while it is on (item box calls, pickups - and saves). Not used.
- **Game state**: `_CurrentMainState` is written only by `set_MainStateValue` (exe+0x1F35930, ~80 direct
  callers): no event, so the tick reads it once a frame. The pause menu's close is a delegate
  (`PauseFlow.<setup>b__3_1`, record 97419) if an event is ever wanted.

### Overlay (src/overlay_d3d.cpp)
A dummy D3D11 device and swap chain (hidden `STATIC` window, flip-discard, then discard, hardware,
then WARP) give the swap chain class's vtable; Present (8), ResizeBuffers (13), Present1 (22) and
ResizeBuffers1 (39, only when `IDXGISwapChain3` shares the vtable) are replaced. Under Proton both
APIs' swap chains are DXVK's `DxgiSwapChain`. The first swap chain that presents a visible window is
the game's; its device tells the API. DX12 needs the game's command queue: a dummy D3D12 queue and
swap chain show where a swap chain keeps its queue pointer (directly, or one pointer deeper when DXVK
wraps vkd3d-proton's swap chain - REFramework's scan), and the game's is read from there, checked by
the queue class's vtable. The panel is drawn with its own command list and allocators per back
buffer, fenced on that queue. `D3DCompile` and `CreateDXGIFactory1` are defined by the mod and load
their DLLs on first use, so the proxy imports no graphics DLL.

### The keyboard (src/keyboard.cpp)
- **The game's input**: `via.hid` makes one DirectInput device per kind (exe+0x2462650: `CreateDevice` with
  `GUID_SysKeyboard` for type 2, `GUID_SysMouse` for 3, an enumerated pad's GUID otherwise) and sets each up
  (exe+0x24638E0: `SetDataFormat`, `SetCooperativeLevel`): pads 9 (`DISCL_EXCLUSIVE | DISCL_BACKGROUND`), the mouse
  0xA (`NONEXCLUSIVE | BACKGROUND`), the keyboard a value exe+0x2462A80 derives from the engine's
  `via.hid.HIDEntry.KeyboardCooperativeLevel` (a native enum, not in the database; applied only in a packaged build
  when `ApplyKeyboardCooperativeLevelOnlyPackage` is set, else 6): Background 0 → 0xA, Foreground 1 → 6
  (`NONEXCLUSIVE | FOREGROUND`), ForegroundNoWinKey 2 → 0x16, **ForegroundExclusive 3 → 5** (`EXCLUSIVE |
  FOREGROUND`). Its value comes from the game's data and was not found offline.
- **An exclusive keyboard sends the window no key messages** - Wine's DirectInput drops every key at its low-level
  hook when an acquired keyboard is exclusive (Windows' behaviour not checked). `tests/test_keyboard.cpp` shows it
  under Wine 11.17: an exclusive keyboard beside our window, keys sent with `SendInput` → the window procedure gets
  **0 `WM_KEYDOWN`, 0 `WM_CHAR`** (a non-exclusive one: 1 and 1); `GetAsyncKeyState` still sees the key held. In game
  (2026-09-19) the panel's number fields could not be typed into - clicks and +/- worked, nothing typed arrived -
  which is that: ImGui types from `WM_CHAR`, and no key message had ever been seen (the log's first-`WM_KEYDOWN`
  line did not exist yet).
- **The panel's own keyboard**: a DirectInput keyboard of the mod's, `DISCL_NONEXCLUSIVE | DISCL_BACKGROUND`, buffered
  (256), created by the render thread once the context is up (`DirectInput8Create` taken from the game's dinput8 at
  run time; `c_dfDIKeyboard` is static data in `libdinput8.a`, so the proxy imports no dinput8). Non-exclusive access
  is always granted and gets every key whatever the game's device does (Wine hands each key to every acquired
  keyboard before deciding to drop it). `pump()` drains it every frame under the ImGui lock: DIK → virtual key
  (`MapVirtualKeyEx` in the game window's layout; the numpad and the 0x80+ extended DIK codes by table) → ImGui key
  events and modifiers (`ImGuiMod_*` only: the Win32 backend's shift workaround would release a
  `ImGuiKey_LeftShift` it cannot see in `GetKeyState`), text by `ToUnicodeEx` (flag 4: dead keys leave the thread's
  state alone; Ctrl or Alt alone type nothing, Ctrl+Alt is AltGr). The toggle key comes from it too (only while the
  game has the focus). Then the window's key messages are not given to ImGui - one source of keys, never two;
  without the device (`Disable = keyboard`, or DirectInput failing) they are, as before.
- **The guard**: while a panel field has the keyboard (`io.WantTextInput`), the game's DirectInput keyboard is told
  no key went down, Escape excepted: `IDirectInputDevice8W::GetDeviceState` (vtable 9: those bytes zeroed) and
  `GetDeviceData` (10: presses taken out of the buffer read, releases kept) hooked in the panel's device's vtable -
  dinput8's, shared with the game's devices (Wine: one vtable for every device kind), so the hooks pass the panel's
  own device and anything `GetDeviceInfo` does not call a keyboard. Keys held when a field takes the keyboard, and
  keys pressed while it has it, stay hidden until released (the Enter that commits a value must not reach the pause
  menu when the field lets go); the device's buffer losing events resyncs from its immediate state.

## Seen in game (build 11636119, Proton Experimental under gamescope, DX12, 2026-09-16)
- The type database is loaded by the time the mod thread looks (VM+0x35D8, header flag 0); the VM
  global is exe+0x9178E98 and the static table VM+0x35A8 (79 479 entries), 0x30 before it as expected.
  Every class the mod uses is found. `fieldptr` is +0x50 for the conditions and the inventory.
- via.Application has 301 entries once sorted (UpdateBehavior entry 149, EndRendering 208); both ran on
  one pool thread for the whole session, at the frame rate (120/s), and the tick never moved.
- DX12 presents through vkd3d-proton behind DXVK's DXGI: the queue is at swap chain +0x8.
- `MainState` goes 0 → WAKE_UP 2 → TITLE 3 → LOAD_GAME_DATA 4 → IN_GAME_INITIALIZE 5 → IN_GAME 6,
  and PAUSE 9 is the pause menu.
- Event build, first run (17:20): all four vtable hooks went in at start-up - the runtime vtables are heap
  copies (`Equipment` 0x120E0CE0, `MainFlowManager` 0x11ECF8A0, `GameClock` 0x118E5A90; no class shares one) -
  and `GameClock.update` was called at once; every database entry was refused (`PAGE_WRITECOPY`, fixed the
  same day).
- Frame rate: with the first version's per-frame reads the game ran at ~30 fps in play (gamescope's
  frame timings; 120 at the title screen), one pool thread 99% busy in user time, the GPU at 37% and
  gamescope under 1% CPU - the reads' `VirtualQuery` (Rules). Replaced by guarded copies and events.
- An empty inventory slot is {item 0, weapon -1, parts 0, bullet 0, count 1}; the item box keeps all
  400 entries, blanks included; a weapon's `Count` is its loaded rounds (infinite ammo tops the
  Matilda up from 9 to 10 after each shot). Store worked; clicks reach the panel.

## Open questions
- Store stacking in game: ammo onto a box stack with room, onto a full one (a new stack), more than fits with
  the box full (the rest stays in the slot); the game's own item box shows the counts, and its own stores stack
  onto the same entries (log: `game: discovery ... stacks 1 (N items stack above 1)`, `inventory: stored ...`).
- Weapons through the item box's game calls: does a taken weapon get its model (equip it, fire it), does
  storing the weapon in your hands unequip it cleanly, and does a two-slot weapon land in the right two
  slots? (log: `call:` and `inventory:` lines) Storing the Matilda through them worked. With the parts rule
  (Two-slot items): the log's discovery line says `two-slot items 1 (and weapons with parts 0x1)`; `Trace = 1`
  marks the Matilda with its stock `[two slots]` and the slot to its right `[second half]`; a Take never uses that
  slot, and the Matilda with its stock comes out of the box into a free pair.
- Typing into the panel (The keyboard): the log says `keyboard: the panel reads the keyboard through its own
  DirectInput device ... kept from what is typed into the panel`, then `keyboard: the panel's DirectInput keyboard
  gets keys` at the first key; whether the window gets key messages at all shows as `overlay: the game's window gets
  key messages (WM_KEYDOWN)` / `(WM_CHAR)` - absent means the game's keyboard is exclusive. Do the number fields and
  the item filters take typing (digits, letters, Backspace, Enter, the arrows), does F7 still hide the panel, and does
  the pause menu stay put while typing (Enter, W/A/S/D, Space) - and when Enter commits a value?
- The event build's first run (log `events:` lines): every entry hooked at start-up (a global points at
  each); the vtable hooks found through `[info-0x10]` once their classes are up (Equipment in game), and the
  save's interface slot; each hook's first call. In game: a damaging hit kills (and the reaction looks
  right), God mode heals and cures poison, a shot keeps its round, a knife hit leaves the knife full, a
  Hardcore typewriter saves without asking for a ribbon (and asks for one with the cheat off; a carried ribbon is
  still spent by the save menu, see Ink ribbons), a save with Save
  without counting keeps the count in the file (load screen), Freeze countdown timer holds the self-destruct
  (a save before the lab's countdown - the alarms and the escape still work), Freeze play time holds the results screen's
  time (its first build; rebuilt 2026-09-20, see below) - and the frame rate in play is back at the cap.
- Infinite wooden boards in game (log `events: TriggerUseItem use-item callback ...` hooked and first called;
  `Trace = 1`: `infinite wooden boards: this use takes no board`): board a window with the cheat on - the window is
  boarded and the count stays (panel: `(N boards)`); with the cheat off the next window takes one. Does any window
  take boards without the use-item inventory (the callback never called)?
- One hit kills on Mr. X (`Trace = 1`: `one hit kills: enemy ... loses all N HP to this hit (enemy controller)` or
  `(its reaction)` - the latter's line is new, so which handler runs first is also still to be seen): the first
  damaging hit puts him on his knees and he gets up with full health; on a ladder, hits do their normal damage. A
  zombie hit while it climbs in through a window dies when it is through.
- One hit kills on the plants (`Trace = 1`: `one hit kills: plant ... burns up on this hit`): any hit that does
  damage, anywhere on it, should burn one up at once - a handgun round, a knife, no fire needed and no knocking
  it down first. Does the burn-up play out as the game's own does (it is `forceDead`, the same call fire leads
  to: `breakWeakPartsAll`, the health to 0, the `<BurnupEffectID>`)? Does it look right on a plant that is
  already down, and on one hanging (SET_HANG)? A plant grabbing the player is left alone (STOP_DAMAGE).
  `plants 1` in the discovery line says the class and the two fields were found.
- The difficulty switch in game (log: `difficulty 1` and `Difficulty EASY=0 NORMAL=1 HARD=2` in the discovery line,
  `call: MainFlowManager.setDifficulty exe+0x1F2EEC0`; on a switch `difficulty: Standard -> Hardcore`; `Trace = 1`
  adds `difficulty: ..., continue on Assisted held N` to the pause-menu dump): does the call return cleanly (no
  `ERROR: call:` line), does the next typewriter ask for a ribbon on Hardcore (and not on Standard), does a save
  made after the switch load as the new difficulty (the load screen names it), and do the loading tips follow?
  On Assisted, does the health recover by itself and the aim assist come on? After a game over's continue on
  Assisted, does a switch to Standard hold through the next load?
- The records' counters in game (log: `record counters 111` in the discovery line; `events: SwitchItemToInventory.update
  ... hooked` once an area with an item box has loaded, `NewInventorySlotBehavior.update` and
  `PlayerFootEffectController.onLand` likewise, and each one's `called by the game (first time)` - for the item box
  that line is also the proof that the behaviour tree reaches an action's `update` through the vtable; `game: the step
  record allows 14000 steps` when the panel first opens; `Trace = 1`: `records: item box opened N, recovery items used
  N, steps N` in the pause-menu dump, and `Item box without counting: the count goes back from 1 to 0` at each
  opening). With the switches on: does opening the item box leave the panel's count at 0, does a heal leave it at 0
  (the item still used up, the health still restored), do steps stop counting, and does a count set in the editor
  hold? Does a clear with them on award the three records? Does the frame rate stay at the cap with Recovery items
  without counting on (its hook runs every frame the inventory screen updates)?
- The records panel in game (log: `records: discovery (0 name(s) missing): main 1 (91 records, 133 rewards, raccoon
  records 55/88), Ghost Survivors 1 (15 records, counted records known 1), raccoons 1/1, accessories 1 (15 mapped, none =
  -1), save 1, screens 196123/159620`; `call: AchievementManager.unlockRecord exe+0xB04210, unlockRogueClearRecord
  exe+0xB042F0`; `events: RecordBehavior.update (the records panel) hooked` once the class is up, and `called by the
  game (first time)` - also the proof that update runs only while the screen is up: `records: the records screen is up
  (main game)` / `is closed` as it opens and closes, never while it is not open). Do both screens show the window,
  from the title's Bonus menu and from the pause menu, and hide it again as they close? Are the rewards named (no
  `?`)? Does a record switched on show as done in the game's list once its cursor moves, its rewards appear (a
  concept art in the gallery, a figure, a costume in the costume menu, 2nd-run modes in the story menu), and a Steam
  achievement pop (`records: "..." on (...) - its Steam achievement is unlocked`)? Off: does the game's check at the
  next records screen leave it off (it must - the progress is below its goal), and do its rewards go? `records:
  system save requested` then `records: saved` - and do the changes survive a restart? An infinite weapon switched on:
  in the item box after the next load; off: gone from box and inventory after the next load. The Ghost Survivors:
  "Mission Complete?" off with No Way Out open is held (`(held off)`, `RE2CabbyCodes.records.txt` written) and No Way
  Out stays on the Ghost Survivors menu; does the held record stay off across a restart and a finished run? Cat Ears
  off while worn: taken off, no infinite ammo next run. "Got 'Em" off: one Mr. Raccoon back in a Ghost Survivors
  map. Raccoon City Native off while every other record is on: the game gives it back at the next records screen
  (expected).

- **Freeze play time and the play-time editor, rebuilt** (2026-09-20; log: `events: GameClock.saveGameSaveData
  (Freeze play time) hooked` and its `called by the game (first time)` at the first save; on the switch
  `freeze play time: saves and the clear record H:MM:SS from now on`, on an edit `play time set to H:MM:SS - what
  saves and the clear record`, at each save `freeze play time: this save records H:MM:SS, not the clock's time`;
  `Trace = 1` adds `the clock has its N s back after the save`). **The point of it**: with the freeze on and the
  play time set to 0:00:00, do the enemies still move, do they attack, and does a scripted set piece still run -
  the Sewers' Main Power Room above all, and Mr. X? The pause dump's `enemies:` line (logged on every pause,
  without Trace) must show the clock **moving** and nobody held by `canRestrictMove`. Then: does a save made with
  the freeze on record the held time (the load screen's rows do not show it - the results screen and a loaded
  game's clock do), does the clock still read right after the save (no jump), and does the clear show the held
  time on the results screen with the rank it earns? An edit with the freeze off should keep counting from what
  it was set to; one set **forward** should work too (the hold puts time in as well as taking it out).
  **2026-09-21**: the first answer was no, and it was the saves - see "That fix was half of one" above. What to
  look for now, in this order. **The saves already written** (all of them, made under the old hold): load one and
  watch for `the enemies' clock was N s behind and could not move (a save an older build wrote): put right, the
  play time left as it is` in the log within a second of the game starting - then do the enemies move, does Mr. X
  chase, and does the Sewers' Main Power Room run? The pause dump's `enemies:` line now prints the four times
  (`elapsed ... cutscenes ... inventory ... pauses`): the elapsed time must be at least the other three added up,
  their clock must **move** between dumps, and `held still by canRestrictMove` must be 0 with enemies about.
  N should be about as long as that run has ever had the inventory open. **A new save under the freeze**: make
  one with the play time held at 0:00:00, load it, and the repair line must **not** appear (the hold no longer
  writes a stuck clock) - the enemies moving from the first moment, the results screen and the loaded game's
  clock still reading 0:00:00. **Live**: the save's hold is in for up to 5 s, and nothing should twitch as it
  passes (`Trace = 1`: `the clock has its N s back after the save`).
  **2026-09-23**: the main game was right and the extra modes' *saved* times were not - the run records them a
  state early (The extra modes record their clear time early), hooked the same day. To check, in The Ghost
  Survivors (log: `events: EndMeasureAndRecordGameElapsedTimeForExtra.start (Freeze play time, the extra modes)
  hooked in 1 slot(s)` at start-up, then `called by the game (first time)` at the **end of the first run**, and
  `freeze play time: this run records H:MM:SS, not the clock's time` there; `Trace = 1` adds `the clock has its
  N s back after the record`; the main state line now names the states -
  `IN_GAME -> RESET_TO_RESULT_ROGUE -> ROGUE_RESULT`): finish a mission with the freeze on and the time held at,
  say, 0:05:00 - the results screen should read 0:05:00 as before, and now **the mission's time on the Ghost
  Survivors menu** (and the record screen) should read it too, not the real one. Then finish it again slower
  than the held time: the record should stay at the held time and no NEW RECORD should show. Does a *faster*
  real run under a held time still take the held one (it must - the record is what the freeze holds)? The same
  for **The 4th Survivor** and **The Tofu Survivor** (their time goes to the system save; RESULT_EXTRA).
  A run **failed** (game over) must still record nothing. And with the freeze **off**, every one of these must
  record the real time exactly as before - the hook does nothing when nothing is held.
  **Also 2026-09-23, the run timer on screen** (log: `events: RogueCountDownBehavior.lateUpdate (Freeze play
  time, the Ghost Survivors' run timer) hooked in 1 slot(s)`, then `called by the game (first time)` on the
  first Ghost Survivors run): with the freeze on, does the HUD timer **stand still** at the held time (the
  milliseconds too) and start moving again the moment the freeze goes off, from the real time? Does an edited
  play time show on it at once and keep counting from there with the freeze off? Does the self-destruct
  countdown still behave (it is the same behaviour, the other mode - Freeze countdown timer's hook), and does
  the frame rate hold (this hook runs every frame the timer is drawn)? The pause menu's play time is
  **expected** to keep showing the real time - it is not hookable, and the panel beside it shows the held one.
- **Weapons in the inventory editor** in game (2026-09-20; log: `game: weapon N (parts 0x0) is full at M, loaded
  with item K` once per weapon the picker offers, the first time the panel opens; on a change `inventory: slot N:
  ... -> ...`). The panel's slot list now has a **Weapons** section under the items - is the Flash Grenade there,
  and does picking it and pressing Apply put grenades in the slot? Do they throw, and does the count go down as
  they are used? Then a gun (the W-870, say): does it come with its magazine full, does it fire and reload, is it
  in the weapon shortcut, and does the game's own inventory screen show it properly (the model, not an empty
  slot)? A knife: does it stab and wear down? Replacing what a slot holds - a weapon over an item, an item over a
  weapon, a weapon over another weapon, and **(empty)** over a weapon (the game's own remove: is the equipped one
  unequipped cleanly?). A two-slot weapon into a slot with no room to its right must be refused with a word, not
  done. Does a save keep all of it, and does a load bring it back? And the reward rule: an infinite weapon put in
  without its record should be gone from the inventory after the next load.
- The save files in game (log: `save files: discovery (0 name(s) missing): screen 1 (update [194697] virtual[15]), saves
  1, users 16, Steam remote storage 1`, then `save files: the game's code read: details 1 (slot +0x10, strings
  +0x18/+0x20/+0x28, stamp +0x30, size +0x40, empty = 0x80000000), strings 1 (+0x10/+0x14), set_SaveFileDetailList
  exe+0x5EF70, SaveService.updateSaveFileDetailTbl exe+0x12CB40` - at once, or after `the game's code is not linked yet`
  a second or more later: which says whether the engine's via.* methods have their code at start-up; `events:
  LoadBehavior.update (the save files) hooked`; at the Load Game screen `save files: the Load Game screen is up`, `the game's list has 21
  row(s)`, one `the game's list: slot N ...` line per save - its strings show the text list's order and the time stamp
  whether the ticks are right). Does the window come up beside the title's, the pause menu's and the game over screen's
  Load Game, and go when a load starts? **Delete**: `deleting (SaveDataManager.RemoveRequest)`, `the game's delete of slot
  N is done`, `the game's list is being read again`, `slot N was deleted` - does the game's row read No Data at once, and
  the save stay gone after a restart (and in Steam Cloud)? **Copy** into an empty slot and over a used one: `copying
  win64_save/... -> ...`, `the copy, as the game lists it` - does the copy load, as the original (location, inventory,
  play time, save count)? That settles whether a slot's number is anywhere inside its file. Does "Continue" on the title
  now pick the copy (its time stamp)? Does the game's cursor jump back as expected after the refresh, and does the list
  refresh take long (the worker reads every slot through Steam)?

## Tooling notes
- `tests/test_mem.cpp` tests the guarded copies (`src/mem.cpp`) - unmapped, reserved, no-access, read-only
  and guard pages, other handlers' exceptions, eight threads - and times a read.
- `tests/test_records.cpp` tests the records panel's rules (`src/records_rules.h`: switched on earns a record by the
  game's own check, switched off un-earns it, 7 680 Ghost Survivors switches from 256 saves never close No Way Out) and
  its generated text (header-only).
- `tests/test_savefiles.cpp` tests `src/savefiles_rules.h` (header-only): the slots' file names both ways, the game's
  rows mapped to slots, Steam's file list checked against them, the time stamp's ticks, and - given re2.exe - the
  getters' code SaveFileDetail's and System.String's layout is read from, at this build's method RVAs.
- `tests/test_clock.cpp` tests the clock's arithmetic (`src/game.h`: `hold_by`, `taken_from`, `enemies_stuck`,
  `repaired`, header-only) - the record time a hold leaves, the enemies' clock it must not move, the give-back,
  and the repair, over 16 807 clocks x every held time. It also keeps the 2026-09-21 bug written down as a case:
  `x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_clock.cpp -o tests/build/test_clock.exe`.
- `tests/test_slots.cpp` tests `game::file_slots`, `fat_by` and `dead_slots` (header-only):
  `x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_slots.cpp -o tests/build/test_slots.exe`.
- `tests/test_switch.cpp` tests `src/switch_eval.h` (header-only) on re2.exe's code read from disk, with this
  build's method RVAs written in (the database's code pointers are 0 on disk) - including the save files' list,
  `getSaveDataIndex` / `getSaveListCount` for all 11 SaveModes (two argument registers), and
  `EquipmentDefine.getItemID` against its inverse `getBulletType` (the weapons' ammo, 12 items both ways).
- `tests/test_keyboard.cpp` tests `src/keyboard.cpp` against Wine's DirectInput beside a "game" keyboard, exclusive or
  shared (the argument), keys sent with `SendInput` to a window of its own. It needs no display: the scratch prefix's
  graphics driver set to null (`wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f`) keeps the window
  off the desktop, and the wineserver still routes the keys through the low-level hooks, DirectInput and the
  foreground window's queue.
- The tests run under system Wine in a scratch prefix: `WINEPREFIX=<scratchpad>/wineprefix
  WINEDEBUG=-all wine ...` (the EGL warnings are Wine probing a headless GPU). `tests/test_load.cpp`
  loads the built proxy beside the real Steam DLL (renamed), calls through two thunks, reads the
  forwarded data export and unloads it.
- `tools/tdb_dump.py --dump` takes ~8 s; `--type` needs the name map (~1 s per run).
- **`RE2CabbyCodes.methods.bin`'s index is `tdb_dump.py`'s index + 1** (found 2026-09-20 by checking known
  RVAs - `setDifficulty` [99149] = exe+0x1F2EEC0, `updateSaveFileDetailTbl` [299023] = exe+0x12CB40): read the
  RVA of method `i` at `12 + 4*(i+1)`, or every offline disassembly lands on the neighbouring method. The mod
  itself is unaffected - it looks every method up by name at run time.
- Python 3.14's `compression.zstd` reads the archives; no third-party modules anywhere.
- The ImGui backends are 1.92.5's (`imgui_impl_dx11/dx12/win32`), vendored unmodified.
