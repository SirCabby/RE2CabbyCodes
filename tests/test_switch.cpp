// Offline test of src/switch_eval.h - running a compiled switch for an argument -
// on the code in re2.exe read from disk (build 11636119; .text is plain on disk):
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_switch.cpp -o tests/build/test_switch.exe
//   wine tests/build/test_switch.exe 'Z:\...\re2.exe'
//
// The methods' code addresses only exist at run time (the database's code
// pointers are 0 on disk), so this build's are named here, from
// tools/tdb_dump.py --methods. The expected maxima were read out of the same
// code by a separate emulator (Python) and match the game's stacks (Handgun Ammo
// 60, Shotgun Shells 20, ...); the knives are cross-checked against the jump
// table decode game.cpp used before.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/switch_eval.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

constexpr uint64_t kBase = 0x140000000;  // the image's preferred base: RIP-relative leas give it back

struct Image {
  std::vector<uint8_t> file;
  struct Sec {
    char name[9];
    uint32_t va, vsize, raw, rsize;
  };
  std::vector<Sec> secs;
  uint32_t size_of_image = 0;

  uint32_t u32(size_t at) const { return at + 4 <= file.size() ? file[at] | file[at + 1] << 8 | file[at + 2] << 16 | static_cast<uint32_t>(file[at + 3]) << 24 : 0; }
  uint16_t u16(size_t at) const { return at + 2 <= file.size() ? static_cast<uint16_t>(file[at] | file[at + 1] << 8) : 0; }

  bool load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    file.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool read = std::fread(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!read || file.size() < 0x400) return false;
    const uint32_t pe = u32(0x3C);
    const uint16_t n = u16(pe + 6), opt = u16(pe + 20);
    size_of_image = u32(pe + 24 + 56);
    for (uint16_t i = 0; i < n; ++i) {
      const size_t o = pe + 24 + opt + 40 * static_cast<size_t>(i);
      Sec s{};
      std::memcpy(s.name, &file[o], 8);
      s.vsize = u32(o + 8);
      s.va = u32(o + 12);
      s.rsize = u32(o + 16);
      s.raw = u32(o + 20);
      secs.push_back(s);
    }
    return true;
  }
  const Sec* section(const char* name) const {
    for (const auto& s : secs)
      if (!std::strncmp(s.name, name, 8)) return &s;
    return nullptr;
  }
  // Bytes at a virtual address (kBase + RVA), from the section holding it.
  bool read(uint64_t va, uint8_t* out, size_t n) const {
    if (va < kBase) return false;
    const uint64_t rva = va - kBase;
    for (const auto& s : secs)
      if (rva >= s.va && rva + n <= static_cast<uint64_t>(s.va) + s.rsize) {
        std::memcpy(out, &file[s.raw + (rva - s.va)], n);
        return true;
      }
    return false;
  }
};

Image g_exe;

bool in_text(uint64_t va) {
  const Image::Sec* t = g_exe.section(".text");
  return t && va >= kBase + t->va && va < kBase + t->va + t->vsize;
}
bool in_image(uint64_t va) { return va >= kBase && va < kBase + g_exe.size_of_image; }

bool run(uint32_t rva, int arg_reg, int64_t arg, int32_t* out, bool tables = true) {
  return re2cc::code::eval_switch(
      kBase + rva, arg_reg, static_cast<uint64_t>(arg),
      [](uint64_t at, uint8_t* o, size_t n) { return g_exe.read(at, o, n); }, in_text,
      [tables](uint64_t at) { return tables && in_image(at); }, out);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_switch.exe <re2.exe>\n");
    return 2;
  }
  check(g_exe.load(argv[1]) && g_exe.section(".text"), "re2.exe read, .text found");
  if (!g_exe.section(".text")) return 1;
  char what[256];

  // ItemManager.getItemMultipleUseMax(Item.ID) - an instance method: the id is in r8.
  constexpr uint32_t kMultipleUseMax = 0x1C79620;
  struct Want {
    int id, max;
  };
  const Want want[] = {{15, 60}, {16, 20}, {17, 200}, {18, 20}, {19, 20}, {20, 20}, {21, 10}, {22, 10}, {23, 10},
                       {24, 100}, {25, 400}, {26, 60}, {27, 20}, {32, 9}, {33, 9}, {48, 12}, {53, 5}, {54, 13},
                       {56, 4}, {58, 26}, {112, 9}, {281, 10}, {282, 9}, {283, 9}, {284, 9}, {286, 9}};
  int wrong = 0, unknown = 0;
  for (int id = -2; id < 0x400; ++id) {
    int expect = 1;
    for (const auto& w : want)
      if (w.id == id) expect = w.max;
    int32_t got = 0;
    if (!run(kMultipleUseMax, 8, id, &got)) {
      if (unknown++ < 5) std::printf("      id %d: unknown\n", id);
    } else if (got != expect) {
      if (wrong++ < 5) std::printf("      id %d: %d, want %d\n", id, got, expect);
    }
  }
  std::snprintf(what, sizeof(what), "getItemMultipleUseMax for ids -2..1023: %d wrong, %d unknown", wrong, unknown);
  check(!wrong && !unknown, what);
  int32_t v = 0;
  check(run(kMultipleUseMax, 8, 15, &v) && v == 60, "Handgun Ammo (sm70_100) stacks to 60");
  check(run(kMultipleUseMax, 8, 25, &v) && v == 400, "Fuel (sm70_110) stacks to 400 - through the first jump table");
  check(run(kMultipleUseMax, 8, 0x3A, &v) && v == 26, "id 0x3A: 26 - through the second jump table");
  check(run(kMultipleUseMax, 8, 0x11E, &v) && v == 9, "id 0x11E: 9 - through the third jump table, after add r8d");
  check(!run(kMultipleUseMax, 8, 15, &v, false), "a jump table outside the readable range: unknown");
  check(!run(kMultipleUseMax, 1, 15, &v), "the argument in the wrong register (r8 never set): unknown");

  // EquipmentDefine.getCategory(WeaponType) - static: the type is in rdx. Every
  // answer against the table decode game.cpp does (`dec edx; cmp edx,N; ja;
  // movsxd rax,edx; lea rdx,[image]; movzx eax,byte [rdx+rax+cases]; mov ecx,
  // [rdx+rax*4+targets]; add rcx,rdx; jmp rcx`, cases `xor eax,eax; ret` or
  // `mov eax,imm32; ret`).
  constexpr uint32_t kGetCategory = 0x19E4BE0;
  uint8_t code[40] = {};
  const bool shape = g_exe.read(kBase + kGetCategory, code, sizeof(code)) && code[0] == 0xFF && code[1] == 0xCA &&
                     code[2] == 0x81 && code[3] == 0xFA && code[13] == 0x48 && code[14] == 0x8D && code[15] == 0x15;
  check(shape, "EquipmentDefine.getCategory has the jump table shape");
  if (shape) {
    const uint32_t last = code[4] | code[5] << 8 | code[6] << 16 | static_cast<uint32_t>(code[7]) << 24;
    const uint32_t cases = code[24] | code[25] << 8 | code[26] << 16 | static_cast<uint32_t>(code[27]) << 24;
    const uint32_t targets = code[31] | code[32] << 8 | code[33] << 16 | static_cast<uint32_t>(code[34]) << 24;
    int mismatches = 0, knives = 0, total = 0;
    for (uint32_t id = 1; id <= last + 1 && id < 512; ++id) {
      uint8_t c = 0, t[4] = {}, body[6] = {};
      if (!g_exe.read(kBase + cases + id - 1, &c, 1) || !g_exe.read(kBase + targets + c * 4u, t, 4)) break;
      const uint32_t target = t[0] | t[1] << 8 | t[2] << 16 | static_cast<uint32_t>(t[3]) << 24;
      g_exe.read(kBase + target, body, sizeof(body));
      int table = -2;
      if ((body[0] == 0x31 || body[0] == 0x33) && body[1] == 0xC0 && body[2] == 0xC3) table = 0;
      else if (body[0] == 0xB8 && body[5] == 0xC3) table = body[1] | body[2] << 8 | body[3] << 16 | body[4] << 24;
      int32_t got = -3;
      const bool ok = run(kGetCategory, 2, id, &got);
      ++total;
      if (!ok || got != table) {
        if (mismatches++ < 5) std::printf("      weapon %u: evaluated %d (%s), table %d\n", id, got, ok ? "known" : "unknown", table);
      }
      knives += ok && got == 9;
    }
    std::snprintf(what, sizeof(what), "getCategory for weapons 1..%u: %d of %d differ from the table decode", last + 1,
                  mismatches, total);
    check(total > 200 && !mismatches, what);
    check(run(kGetCategory, 2, 46, &v) && v == 9 && run(kGetCategory, 2, 47, &v) && v == 9 && run(kGetCategory, 2, 48, &v) &&
              v == 9 && knives == 3,
          "the knives are WP4500, WP4510, WP4520 (WeaponCategory.Knife 9), and no other");
    check(run(kGetCategory, 2, 0, &v) && run(kGetCategory, 2, 0x1000, &v), "out of range types: the default case, known");
  }

  // Not a switch: ItemManager.getItemDisposable saves registers and calls the
  // dictionary's lookup - a store, so unknown.
  check(!run(0x1C792B0, 8, 15, &v), "getItemDisposable (a store and a call): unknown");

  // EquipmentDefine.getItemID(Bullet) - static: the kind is in rdx. A chain of
  // compares with `mov eax,imm; ret` and three `lea eax,[rdx+imm]` cases, which
  // gives the ammo item of a weapon's kind (game::weapon_bullet_id). Its inverse,
  // getBulletType(Item.ID), is a jump table over ids 15..29 of which 12 name a
  // kind: each id's kind must lead back to that very id, which pins both
  // readings at once.
  {
    constexpr uint32_t kItemIdOfBullet = 0x19E5880, kBulletOfItemId = 0x19E4860;
    int pairs = 0, bad = 0;
    for (int id = 0; id < 0x400; ++id) {
      int32_t kind = 0, back = 0;
      if (!run(kBulletOfItemId, 2, id, &kind)) {
        ++bad;
        continue;
      }
      if (!kind) continue;  // not ammo
      ++pairs;
      if (!run(kItemIdOfBullet, 2, kind, &back) || back != id) {
        if (bad++ < 5) std::printf("      item %d: kind 0x%X leads back to %d\n", id, kind, back);
      }
    }
    std::snprintf(what, sizeof(what), "getItemID and getBulletType agree on all %d ammo items (%d wrong)", pairs, bad);
    check(pairs == 12 && !bad, what);
  }
  check(run(0x19E5880, 2, 1, &v) && v == 15, "Bullet 1 is Handgun Ammo (sm70_100)");
  check(run(0x19E5880, 2, 2, &v) && v == 16, "Bullet 2 is Shotgun Shells (sm70_101)");
  check(run(0x19E5880, 2, 0x10, &v) && v == 22, "Bullet 0x10 is Acid Rounds (sm70_107) - through a lea");
  check(run(0x19E5880, 2, 3, &v) && v == 0, "two kinds at once: no single ammo item, and the mod tries each bit");

  // The records panel (records.cpp). RogueAccessoryManager.convertRewardIdToAccessoryDefine(RogueRewardId) -
  // an instance method, the reward in r8: `mov edx,-1; cmp r8d,0x10; ja; ...jmp rcx`, cases `mov edx,imm;
  // mov eax,edx; ret`. Expected from the jump table read separately (Python): ACCESSORY_00..13 are
  // SurvivorDefine.Accessory 3..16, ACCESSORY_14 (Cat Ears) 18, MAX and INVALID none (-1).
  constexpr uint32_t kRewardToAccessory = 0x1097A10;
  {
    int bad = 0;
    for (int r = 0; r <= 16; ++r) {
      const int expect = r <= 13 ? r + 3 : r == 14 ? 18 : -1;
      int32_t got = -9;
      if (!run(kRewardToAccessory, 8, r, &got) || got != expect) {
        if (bad++ < 5) std::printf("      reward %d: %d, want %d\n", r, got, expect);
      }
    }
    check(!bad, "convertRewardIdToAccessoryDefine: rewards 0..16 map to the accessories the jump table says");
  }
  // The ids records.cpp reads out of code: RecordManager.fixRecordProgress's raccoon records
  // (`cmp edi,0x37; je; cmp edi,0x58; jne`) and checkClearedRogueRecord_Simple's COUNT_UP case
  // (`test r14d,r14d; je; cmp r14d,5; je; cmp r14d,0xE; jne`).
  auto find = [](uint32_t rva, uint32_t len, const int16_t* pat, size_t n) -> int64_t {
    std::vector<uint8_t> b(len + n);
    if (!g_exe.read(kBase + rva, b.data(), b.size())) return -1;
    for (uint32_t i = 0; i < len; ++i) {
      size_t k = 0;
      while (k < n && (pat[k] < 0 || b[i + k] == pat[k])) ++k;
      if (k == n) return i;
    }
    return -1;
  };
  {
    const int16_t pat[] = {0x83, 0xFF, -1, 0x74, -1, 0x83, 0xFF, -1, 0x0F, 0x85};
    uint8_t ids[8] = {};
    const int64_t at = find(0xF915D0, 0x40, pat, sizeof(pat) / sizeof(pat[0]));
    const bool ok = at >= 0 && g_exe.read(kBase + 0xF915D0 + at, ids, 8) && ids[2] == 0x37 && ids[7] == 0x58;
    check(ok, "fixRecordProgress names the raccoon records 55 and 88 (RECORD_056, RECORD_089)");
  }
  {
    const int16_t pat[] = {0x45, 0x85, 0xF6, 0x74, -1, 0x41, 0x83, 0xFE, -1, 0x74, -1, 0x41, 0x83, 0xFE, -1, 0x75};
    uint8_t ids[16] = {};
    const int64_t at = find(0xCACB10, 0x180, pat, sizeof(pat) / sizeof(pat[0]));
    const bool ok = at >= 0 && g_exe.read(kBase + 0xCACB10 + at, ids, 16) && ids[8] == 5 && ids[14] == 14;
    check(ok, "checkClearedRogueRecord_Simple counts plays for records 0 and 5, raccoons for 14");
  }
  {
    // GimmickDialLockManager.SetUnlockRecord's tail: `mov r9d,edx; mov r8d,ID; mov rdx,rcx` then setRecordCount.
    const int16_t pat[] = {0x44, 0x8B, 0xCA, 0x41, 0xB8, -1, -1, -1, -1, 0x48, 0x8B, 0xD1};
    uint8_t ids[12] = {};
    const int64_t at = find(0x19A1130, 0x200, pat, sizeof(pat) / sizeof(pat[0]));
    const bool ok = at >= 0 && g_exe.read(kBase + 0x19A1130 + at, ids, 12) && ids[5] == 0x36 && !ids[6] && !ids[7] && !ids[8];
    check(ok, "SetUnlockRecord recounts record 54 (RECORD_055, Master of Unlocking)");
  }

  // The save files' list (savefiles.cpp): SaveDataManager.getSaveDataIndex(SaveMode, Int32) and
  // getSaveListCount(SaveMode) - static, so the mode is in rdx and the offset in r8 - are
  // jump tables over the SaveMode. The main game's list (SCENARIO 1) is slots 0-20.
  {
    constexpr uint32_t kIndex = 0x175E5B0, kCount = 0x175E840;
    auto index = [](int mode, int offset, int32_t* out) {
      return re2cc::code::eval_switch(
          kBase + kIndex, 2, static_cast<uint64_t>(mode),
          [](uint64_t at, uint8_t* o, size_t n) { return g_exe.read(at, o, n); }, in_text, in_image, out, 8,
          static_cast<uint64_t>(offset));
    };
    // SYSTEM, SCENARIO, FOURTH, TOFU, ROGUE, TEMP, DEBUG, STGJMP1-4.
    const int base[11] = {-1, 0, 0, 0, 21, 0, 32, 32, 32, 32, 32}, count[11] = {1, 21, 0, 0, 11, 0, 200, 200, 200, 200, 200};
    int bad = 0;
    for (int m = 0; m < 11; ++m) {
      int32_t i0 = 0, i5 = 0, c = 0;
      if (!index(m, 0, &i0) || i0 != base[m] || !index(m, 5, &i5) || i5 != base[m] + 5 || !run(kCount, 2, m, &c) ||
          c != count[m]) {
        if (bad++ < 3) std::printf("      mode %d: index %d/%d, count %d\n", m, i0, i5, c);
      }
    }
    check(!bad, "getSaveDataIndex(mode, 0/5) and getSaveListCount(mode) for all 11 modes: SCENARIO slots 0-20");
    int32_t v = 0;
    check(!run(kIndex, 2, 1, &v), "getSaveDataIndex with the offset (r8) not given: unknown");
    check(!index(1, 0, &v) || !re2cc::code::eval_switch(
                                   kBase + kIndex, 2, 1, [](uint64_t at, uint8_t* o, size_t n) { return g_exe.read(at, o, n); },
                                   in_text, in_image, &v, 2, 0),
          "a second argument in the first's register: refused");
  }

  std::printf("%s\n", g_fail ? "FAILED" : "all passed");
  return g_fail ? 1 : 0;
}
