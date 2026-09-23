// Offline test of the save files' rules (src/savefiles_rules.h):
//
//   x86_64-w64-mingw32-g++ -std=c++20 -O2 -static tests/test_savefiles.cpp -o tests/build/test_savefiles.exe
//   wine tests/build/test_savefiles.exe ['Z:\...\re2.exe']
//
// The slots' file names (as this build's save folder has them: data000.bin, the
// auto-save; data001Slot.bin.., the slots; data00-1.bin and data021Slot.bin,
// the system and The Ghost Survivors' saves), the game's rows mapped to slots,
// Steam's file list checked against them - and, given re2.exe, the getters'
// code the mod reads SaveFileDetail's and System.String's layout out of, at
// this build's method RVAs (tools/tdb_dump.py --methods; the database's code
// pointers are 0 on disk).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "../src/savefiles_rules.h"

namespace {

using namespace re2cc::savefiles;

int g_fail = 0;

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

// re2.exe's bytes at an RVA (from the section holding it).
struct Image {
  std::vector<uint8_t> file;
  struct Sec {
    uint32_t va, rsize, raw;
  };
  std::vector<Sec> secs;
  uint32_t u32(size_t at) const {
    return at + 4 <= file.size() ? file[at] | file[at + 1] << 8 | file[at + 2] << 16 | static_cast<uint32_t>(file[at + 3]) << 24 : 0;
  }
  uint16_t u16(size_t at) const { return at + 2 <= file.size() ? static_cast<uint16_t>(file[at] | file[at + 1] << 8) : 0; }
  bool load(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    file.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!ok || file.size() < 0x400) return false;
    const uint32_t pe = u32(0x3C);
    const uint16_t n = u16(pe + 6), opt = u16(pe + 20);
    for (uint16_t i = 0; i < n; ++i) {
      const size_t o = pe + 24 + opt + 40 * static_cast<size_t>(i);
      secs.push_back({u32(o + 12), u32(o + 16), u32(o + 20)});
    }
    return true;
  }
  bool read(uint32_t rva, uint8_t* out, size_t n) const {
    for (const auto& s : secs)
      if (rva >= s.va && rva + n <= static_cast<uint64_t>(s.va) + s.rsize) {
        std::memcpy(out, &file[s.raw + (rva - s.va)], n);
        return true;
      }
    return false;
  }
};

}  // namespace

int main(int argc, char** argv) {
  char what[256];

  // --- the slots' names ---------------------------------------------------------------------------------------
  check(rules::file_name("win64_save/", 0) == "win64_save/data000.bin", "slot 0 (the auto-save) is data000.bin");
  check(rules::file_name("win64_save/", 1) == "win64_save/data001Slot.bin", "slot 1 is data001Slot.bin");
  check(rules::file_name("win64_save/", 20) == "win64_save/data020Slot.bin", "slot 20 is data020Slot.bin");
  check(rules::file_name("", 7) == "data007Slot.bin", "no folder: data007Slot.bin");
  check(rules::file_name("x/", -1).empty() && rules::file_name("x/", 1000).empty(), "no name for slot -1 or 1000");
  struct Named {
    const char* name;
    int slot;
    const char* dir;
  };
  const Named names[] = {
      {"win64_save/data000.bin", 0, "win64_save/"},     {"win64_save/data001Slot.bin", 1, "win64_save/"},
      {"win64_save/data002Slot.bin", 2, "win64_save/"}, {"win64_save/data021Slot.bin", 21, "win64_save/"},
      {"win64_save/data00-1.bin", -1, ""},              {"win64_save/data000Slot.bin", -1, ""},
      {"win64_save/data001.bin", -1, ""},               {"win64_save/data01Slot.bin", -1, ""},
      {"win64_save/data001Slot.bin.bak", -1, ""},       {"data005Slot.bin", 5, ""},
      {"./win64_save/data010Slot.bin", 10, "./win64_save/"}, {"win64_save/Data001Slot.bin", -1, ""},
      {"", -1, ""},
  };
  for (const Named& n : names) {
    std::string dir = "(unset)";
    const int s = rules::slot_of(n.name, &dir);
    std::snprintf(what, sizeof(what), "'%s' is slot %d (got %d, folder '%s')", n.name, n.slot, s,
                  s >= 0 ? dir.c_str() : "-");
    check(s == n.slot && (s < 0 || dir == n.dir), what);
  }
  check(rules::slot_of(nullptr) == -1, "no name is no slot");
  bool round_trip = true;
  for (int s = 0; s <= rules::kMaxSlot; ++s) {
    std::string dir;
    round_trip = round_trip && rules::slot_of(rules::file_name("win64_save/", s).c_str(), &dir) == s && dir == "win64_save/";
  }
  check(round_trip, "every slot 0-999: its name reads back as itself");

  // --- the game's rows ------------------------------------------------------------------------------------------
  {
    // Row i is slot i (the main game's list: slots 0-20): the auto-save and slots 1-2 used.
    bool used[21] = {};
    used[0] = used[1] = used[2] = true;

    // Steam's files beside them.
    auto files = std::make_unique<rules::SteamFiles>();
    files->add("win64_save/data00-1.bin", 15800);
    files->add("win64_save/data000.bin", 663536);
    files->add("win64_save/data001Slot.bin", 4939516);
    files->add("win64_save/data002Slot.bin", 1825420);
    files->add("win64_save/data021Slot.bin", 1120);
    check(files->dir == "win64_save/" && !files->mixed_dirs, "Steam's files: one folder, win64_save/");
    check(files->exists[0] && files->exists[1] && files->exists[2] && files->exists[21] && !files->exists[3],
          "Steam's files: slots 0, 1, 2 and 21");
    check(files->size[1] == 4939516 && files->name[2] == "win64_save/data002Slot.bin", "sizes and names kept");
    check(rules::first_mismatch(*files, 0, used, 21) == -1, "the game's 21 rows match Steam's files");
    bool more[21];
    std::memcpy(more, used, sizeof(more));
    more[5] = true;
    check(rules::first_mismatch(*files, 0, more, 21) == 5, "a row the game says is used, with no file: row 5");
    more[5] = false;
    more[1] = false;
    check(rules::first_mismatch(*files, 0, more, 21) == 1, "a file for a row the game says is empty: row 1");
    check(rules::first_mismatch(*files, 1, used, 21) == 2,
          "the rows one slot off: rows 0-1 land on slots 1-2 (which have files), row 2 on slot 3 does not fit");
    files->add("other/data003Slot.bin", 10);
    check(files->mixed_dirs, "a slot file in another folder: not the game's layout");
  }

  // --- the time stamp -----------------------------------------------------------------------------------------------
  // 2026-09-19 12:00:00 UTC: Unix 1789819200; .NET ticks (Utc) = (unix + 62135596800) * 10^7.
  const long long unix_t = 1789819200LL;
  const long long ticks = (unix_t + 62135596800LL) * 10000000LL;
  std::snprintf(what, sizeof(what), "ticks %lld are Unix %lld (got %lld)", ticks, unix_t, rules::unix_seconds(ticks));
  check(rules::unix_seconds(ticks) == unix_t, what);
  check(rules::unix_seconds(0) == -1 && rules::unix_seconds(-5) == -1, "no time stamp: -1");
  check(rules::unix_seconds(unix_t) == -1, "Unix seconds taken for ticks: no plausible date, -1");

  // --- the getters' code --------------------------------------------------------------------------------------------
  {
    const uint8_t slot[] = {0x8B, 0x42, 0x10, 0xC3};
    const uint8_t title[] = {0x48, 0x8B, 0x42, 0x18, 0xC3};
    const uint8_t far32[] = {0x48, 0x8B, 0x82, 0x00, 0x01, 0x00, 0x00, 0xC3};
    const uint8_t other[] = {0x48, 0x8B, 0x41, 0x18, 0xC3};  // [rcx+0x18]: not the object's
    const uint8_t invalid[] = {0xB8, 0x00, 0x00, 0x00, 0x80, 0xC3};
    const uint8_t chars[] = {0x48, 0x83, 0xEC, 0x28, 0x45, 0x85, 0xC0, 0x78, 0x13, 0x44, 0x3B, 0x42, 0x10, 0x7D,
                             0x0D, 0x49, 0x63, 0xC0, 0x0F, 0xB7, 0x44, 0x42, 0x14, 0x48, 0x83, 0xC4, 0x28, 0xC3};
    int32_t c = 0;
    check(rules::getter_offset(slot, sizeof(slot), false) == 0x10, "mov eax,[rdx+0x10]; ret: 0x10");
    check(rules::getter_offset(slot, sizeof(slot), true) == -1, "a 32-bit load is not a 64-bit getter");
    check(rules::getter_offset(title, sizeof(title), true) == 0x18, "mov rax,[rdx+0x18]; ret: 0x18");
    check(rules::getter_offset(title, sizeof(title), false) == -1, "a 64-bit load is not a 32-bit getter");
    check(rules::getter_offset(far32, sizeof(far32), true) == 0x100, "mov rax,[rdx+0x100]; ret: 0x100");
    check(rules::getter_offset(other, sizeof(other), true) == -1, "mov rax,[rcx+0x18]: not a getter of the object");
    check(rules::getter_constant(invalid, sizeof(invalid), &c) && c == INT32_MIN, "mov eax,0x80000000; ret: INT32_MIN");
    check(!rules::getter_constant(slot, sizeof(slot), &c), "a load is not a constant");
    check(rules::chars_offset(chars, sizeof(chars)) == 0x14, "String.get_Chars: characters at +0x14");
  }

  if (argc > 1) {
    Image img;
    if (!img.load(argv[1])) {
      std::printf("cannot read %s\n", argv[1]);
      return 2;
    }
    // Build 11636119's method RVAs (tools/tdb_dump.py --methods).
    struct Getter {
      const char* name;
      uint32_t rva;
      bool wide;
      int32_t want;
    };
    const Getter getters[] = {
        {"SaveFileDetail.get_Slot [88627]", 0x52610, false, 0x10},
        {"SaveFileDetail.get_Title [88621]", 0x52080, true, 0x18},
        {"SaveFileDetail.get_SubTitle [88623]", 0x7A5C0, true, 0x20},
        {"SaveFileDetail.get_Detail [88625]", 0x6A1A0, true, 0x28},
        {"SaveFileDetail.get_LastUpdateTimeStamp [88628]", 0x51040, true, 0x30},
        {"SaveFileDetail.get_UseSize [88630]", 0x64830, true, 0x40},
        {"String.get_Length [270886]", 0x52610, false, 0x10},
    };
    for (const Getter& g : getters) {
      uint8_t b[10] = {};
      const int32_t got = img.read(g.rva, b, sizeof(b)) ? rules::getter_offset(b, sizeof(b), g.wide) : -2;
      std::snprintf(what, sizeof(what), "%s (exe+0x%X) reads +0x%X (got %d)", g.name, g.rva, g.want, got);
      check(got == g.want, what);
    }
    uint8_t b[48] = {};
    int32_t c = 0;
    check(img.read(0xC0D20, b, 6) && rules::getter_constant(b, 6, &c) && c == INT32_MIN,
          "SaveFileDetail.get_InvalidSlot [88633] (exe+0xC0D20) is 0x80000000");
    check(img.read(0x2E5FA0, b, sizeof(b)) && rules::chars_offset(b, sizeof(b)) == 0x14,
          "String.get_Chars [270891] (exe+0x2E5FA0): characters at +0x14");
    // SaveFileDetail.get_EmptyData [88632]: `cmp dword [rdx+0x10],0x80000000` - the empty slot is the slot field
    // holding get_InvalidSlot's constant.
    const uint8_t empty[] = {0x81, 0x7A, 0x10, 0x00, 0x00, 0x00, 0x80};
    check(img.read(0xC0790, b, sizeof(empty)) && std::memcmp(b, empty, sizeof(empty)) == 0,
          "SaveFileDetail.get_EmptyData (exe+0xC0790) compares the slot (+0x10) with 0x80000000");
    // SaveService.updateSaveFileDetailTbl [299023]: the table's generation made stale (-1), then the refresh.
    const uint8_t refresh[] = {0x48, 0x8B, 0x0D};  // mov rcx,[rip+SaveService]
    const uint8_t stale[] = {0xC7, 0x81};             // mov dword [rcx+d32], ...
    check(img.read(0x12CB40, b, 20) && std::memcmp(b, refresh, 3) == 0 && std::memcmp(b + 7, stale, 2) == 0 &&
              b[13] == 0xFF && b[14] == 0xFF && b[15] == 0xFF && b[16] == 0xFF && b[17] == 0xE9,
          "SaveService.updateSaveFileDetailTbl (exe+0x12CB40): the generation set to -1, then a jump to the refresh");
  }

  std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "all passed", g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
