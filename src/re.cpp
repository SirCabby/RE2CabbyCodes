#include "re.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "mem.h"

namespace re2cc::re {
namespace {

// --- the database's layout (TDB 70) --------------------------------------------------
// Header (REFramework's tdb70::TDB): counts at 0x0C.., array pointers from 0x58.
// In the exe's copy the pointers are offsets from the header; the VM rewrites
// them to addresses and sets `initialized` before anything managed runs.
constexpr uint32_t kMagic = 0x00424454;  // "TDB\0"
constexpr uint32_t kVersion = 70;

// RETypeDefVersion69 - the typedef every TDB 69/70 game uses (0x50 bytes).
struct TypeDef {
  uint64_t a;  // index:18 parent:18 declaring:18 underlying:7 object_type:3
  uint64_t b;  // array:18 element:18 impl:18 system:10
  uint32_t flags, size, fqn, crc, ctor, vt, member_method, member_field;
  uint32_t prop;  // num_member_prop:12 member_prop:18
  uint32_t member_event;
  int32_t interfaces, generics;
  uintptr_t clr_type;
  uintptr_t managed_vt;  // the REObjectInfo of its instances
};
static_assert(sizeof(TypeDef) == 0x50, "TypeDef");

#pragma pack(push, 4)
struct TypeImpl {
  int32_t name, ns, field_size, static_size;
  uint8_t module, rank;
  uint16_t num_methods;
  int32_t num_fields;
  int16_t iface;
  uint16_t num_native_vt, attrs, num_vt;
  uint64_t mark, cycle;
};
struct MethodImpl {
  uint16_t attrs;
  int16_t vt_index;
  uint16_t flags, impl_flags;
  uint32_t name;
};
struct FieldImpl {
  uint16_t attrs, flags;
  uint32_t w1;  // type:18 init_lo:14
  uint32_t w2;  // name:30 init_hi:2
};
struct ParamDef {
  uint16_t attrs, init;
  uint32_t w1;  // name:30 modifier:2
  uint32_t w2;  // type:18 flags:14
};
#pragma pack(pop)
static_assert(sizeof(TypeImpl) == 0x30, "TypeImpl");
static_assert(sizeof(MethodImpl) == 12, "MethodImpl");
static_assert(sizeof(FieldImpl) == 12, "FieldImpl");
static_assert(sizeof(ParamDef) == 12, "ParamDef");

struct Db {
  uintptr_t base = 0;
  uint32_t n_types = 0, n_methods = 0, n_fields = 0, n_type_impl = 0, n_method_impl = 0, n_field_impl = 0;
  uint32_t n_params = 0, n_init = 0, n_strings = 0, n_bytes = 0;
  uintptr_t types = 0, types_impl = 0, methods = 0, methods_impl = 0, fields = 0, fields_impl = 0;
  uintptr_t params = 0, init_data = 0, strings = 0, bytes = 0;
} g;

uintptr_t g_vm_slot = 0;
uintptr_t g_static_elems = 0;  // uint8_t** - one static block per type index
uint32_t g_static_size = 0;
std::atomic<bool> g_ready{false};  // set last, on the mod thread; the tick reads it
char g_status[200] = "not started";

std::vector<std::string> g_names;      // full names, built once
std::unordered_map<std::string, uint32_t> g_by_name;
// The two caches the game tick fills as it meets classes: where an object's
// fields start (per REObjectInfo), and a List<T>'s two fields (per class).
std::unordered_map<uintptr_t, int32_t> g_fieldptr;
std::unordered_map<uint32_t, std::pair<Field, Field>> g_list_fields;
CRITICAL_SECTION g_cache_cs;
bool g_cache_cs_ready = false;

const TypeDef* td(uint32_t i) { return reinterpret_cast<const TypeDef*>(g.types + static_cast<uintptr_t>(i) * sizeof(TypeDef)); }
uint32_t td_parent(const TypeDef* t) { return static_cast<uint32_t>((t->a >> 18) & 0x3FFFF); }
uint32_t td_declaring(const TypeDef* t) { return static_cast<uint32_t>((t->a >> 36) & 0x3FFFF); }
uint32_t td_objtype(const TypeDef* t) { return static_cast<uint32_t>((t->a >> 61) & 7); }
uint32_t td_impl(const TypeDef* t) { return static_cast<uint32_t>((t->b >> 36) & 0x3FFFF); }
const TypeImpl* timpl(uint32_t i) {
  return i < g.n_type_impl ? reinterpret_cast<const TypeImpl*>(g.types_impl + static_cast<uintptr_t>(i) * sizeof(TypeImpl)) : nullptr;
}
const char* str(uint32_t off) { return off < g.n_strings ? reinterpret_cast<const char*>(g.strings + off) : ""; }

struct RawField {
  uint32_t declaring, impl, offset;
};
RawField raw_field(uint32_t i) {
  const uint64_t v = *reinterpret_cast<const uint64_t*>(g.fields + static_cast<uintptr_t>(i) * 8);
  return RawField{static_cast<uint32_t>(v & 0x3FFFF), static_cast<uint32_t>((v >> 18) & 0xFFFFF),
                  static_cast<uint32_t>(v >> 38)};
}
const FieldImpl* fimpl(uint32_t i) {
  return i < g.n_field_impl ? reinterpret_cast<const FieldImpl*>(g.fields_impl + static_cast<uintptr_t>(i) * sizeof(FieldImpl)) : nullptr;
}
struct RawMethod {
  uint32_t declaring, impl, params;
  uintptr_t code;
};
RawMethod raw_method(uint32_t i) {
  const uintptr_t at = g.methods + static_cast<uintptr_t>(i) * 16;
  const uint64_t v = *reinterpret_cast<const uint64_t*>(at);
  return RawMethod{static_cast<uint32_t>(v & 0x3FFFF), static_cast<uint32_t>((v >> 18) & 0xFFFFF),
                   static_cast<uint32_t>(v >> 38), *reinterpret_cast<const uintptr_t*>(at + 8)};
}
const MethodImpl* mimpl(uint32_t i) {
  return i < g.n_method_impl ? reinterpret_cast<const MethodImpl*>(g.methods_impl + static_cast<uintptr_t>(i) * sizeof(MethodImpl)) : nullptr;
}

uint32_t num_fields_of(uint32_t t) {
  const TypeImpl* ti = timpl(td_impl(td(t)));
  return ti && td(t)->member_field ? static_cast<uint32_t>(ti->num_fields) : 0;
}
uint32_t num_methods_of(uint32_t t) {
  const TypeImpl* ti = timpl(td_impl(td(t)));
  return ti && td(t)->member_method ? ti->num_methods : 0;
}

// The header, read from a candidate address. `absolute`: the pointers are
// addresses (the VM has initialised it), not offsets from the header.
bool read_header(uintptr_t base, Db* out) {
  if (!mem::readable(base, 0xE8)) return false;
  const auto u32 = [&](int o) { return mem::read<uint32_t>(base + o); };
  const auto u64 = [&](int o) { return mem::read<uint64_t>(base + o); };
  if (u32(0) != kMagic || u32(4) != kVersion) return false;
  Db d;
  d.base = base;
  d.n_types = u32(0x0C);
  d.n_methods = u32(0x10);
  d.n_fields = u32(0x14);
  d.n_type_impl = u32(0x18);
  d.n_field_impl = u32(0x1C);
  d.n_method_impl = u32(0x20);
  d.n_params = u32(0x30);
  d.n_init = u32(0x38);
  d.n_strings = u32(0x50);
  d.n_bytes = u32(0x54);
  d.types = u64(0x60);
  d.types_impl = u64(0x68);
  d.methods = u64(0x70);
  d.methods_impl = u64(0x78);
  d.fields = u64(0x80);
  d.fields_impl = u64(0x88);
  d.params = u64(0xA8);
  d.init_data = u64(0xB8);
  d.strings = u64(0xD0);
  d.bytes = u64(0xD8);
  if (!d.n_types || d.n_types > 0x3FFFF || !d.n_methods || !d.n_fields || !d.n_strings || !d.n_bytes) return false;
  *out = d;
  return true;
}

// Every array the mod reads must be where the header says, whole.
bool arrays_readable(const Db& d) {
  return mem::readable(d.types, static_cast<size_t>(d.n_types) * sizeof(TypeDef)) &&
         mem::readable(d.types_impl, static_cast<size_t>(d.n_type_impl) * sizeof(TypeImpl)) &&
         mem::readable(d.methods, static_cast<size_t>(d.n_methods) * 16) &&
         mem::readable(d.methods_impl, static_cast<size_t>(d.n_method_impl) * sizeof(MethodImpl)) &&
         mem::readable(d.fields, static_cast<size_t>(d.n_fields) * 8) &&
         mem::readable(d.fields_impl, static_cast<size_t>(d.n_field_impl) * sizeof(FieldImpl)) &&
         mem::readable(d.params, static_cast<size_t>(d.n_params) * sizeof(ParamDef)) &&
         mem::readable(d.strings, d.n_strings) && mem::readable(d.bytes, d.n_bytes);
}

// Every typedef names itself: its index field is its own position. A database
// read with the wrong layout fails this at once.
bool self_check(char* why, size_t n) {
  const uint32_t probes[] = {1, 2, 3, 100, 1000, g.n_types / 2, g.n_types - 1};
  for (uint32_t i : probes) {
    if (i == 0 || i >= g.n_types) continue;
    const uint32_t idx = static_cast<uint32_t>(td(i)->a & 0x3FFFF);
    if (idx != i) {
      std::snprintf(why, n, "typedef %u says it is %u", i, idx);
      return false;
    }
    if (!timpl(td_impl(td(i)))) {
      std::snprintf(why, n, "typedef %u has impl %u of %u", i, td_impl(td(i)), g.n_type_impl);
      return false;
    }
  }
  return true;
}

const std::string& build_name(uint32_t i, int depth) {
  static const std::string kEmpty;
  if (i == 0 || i >= g.n_types) return kEmpty;
  std::string& slot = g_names[i];
  if (!slot.empty() || depth > 16) return slot;
  const TypeDef* t = td(i);
  const TypeImpl* ti = timpl(td_impl(t));
  if (!ti) return kEmpty;
  const char* ns = str(static_cast<uint32_t>(ti->ns));
  const char* nm = str(static_cast<uint32_t>(ti->name));
  std::string full;
  const uint32_t decl = td_declaring(t);
  if (decl && decl != i && decl < g.n_types) {
    full = build_name(decl, depth + 1);
    full += '.';
    full += nm;
  } else {
    if (*ns) {
      full = ns;
      full += '.';
    }
    full += nm;
  }
  // A generic instance: "List`1<app.ropeway.inventory.Slot>".
  if (t->generics > 0 && static_cast<uint32_t>(t->generics) + 4 <= g.n_bytes) {
    const uintptr_t gl = g.bytes + static_cast<uint32_t>(t->generics);
    const uint32_t w = mem::read<uint32_t>(gl);
    const uint32_t def = w & 0x3FFFF, num = w >> 18;
    if (def != i && num > 0 && num <= 16 && static_cast<uint32_t>(t->generics) + 4 + num * 4 <= g.n_bytes) {
      full += '<';
      for (uint32_t k = 0; k < num; ++k) {
        if (k) full += ',';
        full += build_name(mem::read<uint32_t>(gl + 4 + 4 * k), depth + 1);
      }
      full += '>';
    }
  }
  slot = full;
  return slot;
}

bool attach(const Db& d) {
  g = d;
  char why[160] = "";
  if (!arrays_readable(g)) {
    std::snprintf(g_status, sizeof(g_status), "type database arrays not readable");
    return false;
  }
  if (!self_check(why, sizeof(why))) {
    std::snprintf(g_status, sizeof(g_status), "type database failed its self-check: %s", why);
    return false;
  }
  g_names.assign(g.n_types, std::string());
  g_by_name.clear();
  g_by_name.reserve(g.n_types * 2);
  for (uint32_t i = 1; i < g.n_types; ++i) {
    const std::string& n = build_name(i, 0);
    if (!n.empty()) g_by_name.emplace(n, i);
  }
  if (!g_cache_cs_ready) {
    InitializeCriticalSection(&g_cache_cs);
    g_cache_cs_ready = true;
  }
  return true;
}

// --- finding the VM ---------------------------------------------------------------------
// `mov rcx,[rip+X]; mov edx,-1; call get_thread_context` - the VM global X is
// loaded this way all over the code; REFramework takes the first global seen
// more than ten times.
uintptr_t find_vm_slot(const mem::Range& text, const mem::Range& image) {
  std::unordered_map<uintptr_t, int> seen;
  const auto* p = reinterpret_cast<const uint8_t*>(text.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(text.end) - 12;
  for (const uint8_t* c = p + 7; c < end;) {
    c = static_cast<const uint8_t*>(std::memchr(c, 0xBA, end - c));
    if (!c) break;
    if (c[1] == 0xFF && c[2] == 0xFF && c[3] == 0xFF && c[4] == 0xFF && c[5] == 0xE8 && c[-7] == 0x48 &&
        c[-6] == 0x8B && c[-5] == 0x0D) {
      const uintptr_t insn = reinterpret_cast<uintptr_t>(c - 7);
      const uintptr_t slot = mem::rip_target(insn, 3, 7);
      if (image.contains(slot) && ++seen[slot] > 10) return slot;
    }
    ++c;
  }
  return 0;
}

// The database in the image, found by its header: magic, version 70.
uintptr_t find_tdb_in_image(const mem::Range& data) {
  const auto* p = reinterpret_cast<const uint8_t*>(data.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(data.end) - 0x100;
  for (const uint8_t* c = p; c < end;) {
    c = static_cast<const uint8_t*>(std::memchr(c, 'T', end - c));
    if (!c) break;
    if ((reinterpret_cast<uintptr_t>(c) & 7) == 0 && std::memcmp(c, "TDB\0", 4) == 0 &&
        mem::read<uint32_t>(reinterpret_cast<uintptr_t>(c) + 4) == kVersion)
      return reinterpret_cast<uintptr_t>(c);
    ++c;
  }
  return 0;
}

}  // namespace

// --- set-up -------------------------------------------------------------------------------
bool attach_tdb(uintptr_t tdb) {
  Db d;
  if (!read_header(tdb, &d)) return false;
  g_ready = attach(d);
  if (g_ready) std::snprintf(g_status, sizeof(g_status), "ready (%u types)", g.n_types);
  return g_ready;
}

bool init() {
  if (g_ready) return true;
  HMODULE exe = GetModuleHandleA(nullptr);
  const mem::Range image = mem::module_range(exe);
  const mem::Range text = mem::section(exe, ".text");
  const mem::Range data = mem::section(exe, ".data");
  if (text.empty() || data.empty()) {
    std::snprintf(g_status, sizeof(g_status), "re2.exe has no .text/.data section");
    return false;
  }
  static uintptr_t s_image_tdb = 0;
  if (!s_image_tdb) {
    s_image_tdb = find_tdb_in_image(data);
    if (!s_image_tdb) {
      std::snprintf(g_status, sizeof(g_status), "no TDB v70 header in re2.exe's .data");
      return false;
    }
    logf("re: TDB v70 header in .data at exe+0x%llX", static_cast<unsigned long long>(s_image_tdb - image.begin));
  }
  if (!g_vm_slot) {
    g_vm_slot = find_vm_slot(text, image);
    if (!g_vm_slot) {
      std::snprintf(g_status, sizeof(g_status), "the VM global was not found in the code");
      return false;
    }
    logf("re: the VM global is at exe+0x%llX", static_cast<unsigned long long>(g_vm_slot - image.begin));
  }
  const uintptr_t vm = mem::read_ptr(g_vm_slot);
  if (!vm) {
    std::snprintf(g_status, sizeof(g_status), "waiting for the VM to start");
    return false;
  }
  // The VM's pointer to its database - the image's own copy, preferred, or any
  // other v70 header it holds.
  int tdb_off = -1;
  uintptr_t tdb = 0;
  for (int off = 0x30; off < 0x20000; off += 8) {
    const uintptr_t p = mem::read_ptr(vm + off);
    if (!p || !mem::readable(p, 8) || mem::read<uint32_t>(p) != kMagic || mem::read<uint32_t>(p + 4) != kVersion) continue;
    if (tdb_off < 0 || p == s_image_tdb) {
      tdb_off = off;
      tdb = p;
    }
    if (p == s_image_tdb) break;
  }
  if (tdb_off < 0) {
    std::snprintf(g_status, sizeof(g_status), "waiting for the VM's type database");
    return false;
  }
  // The VM's loader (exe+0x1F7F870, its rebase exe+0x1F54590 in build 11636119)
  // turns every array offset in the header into an address, in place, and sets
  // no flag while it does - the header's `initialized` stays 0. So "loaded" is
  // read off the arrays themselves: none of them may still be an offset.
  for (int i = 0; i < 18; ++i) {
    const uint64_t p = mem::read<uint64_t>(tdb + 0x58 + 8 * static_cast<uintptr_t>(i));
    if (p && p < tdb) {
      std::snprintf(g_status, sizeof(g_status), "waiting for the VM to load the type database (array %d is still an offset)", i);
      return false;
    }
  }
  if (tdb != s_image_tdb)
    logf("re: the VM's type database is at %p, not the image's copy at exe+0x%llX", reinterpret_cast<void*>(tdb),
         static_cast<unsigned long long>(s_image_tdb - image.begin));
  if (g.base != tdb) {
    Db d;
    if (!read_header(tdb, &d)) {
      std::snprintf(g_status, sizeof(g_status), "the type database header does not read as v70");
      return false;
    }
    if (!attach(d)) return false;  // status set
    logf("re: type database at VM+0x%X loaded: %u types, %u methods, %u fields (header flag %u)", tdb_off, g.n_types,
         g.n_methods, g.n_fields, mem::read<uint32_t>(tdb + 8));
  }
  // The static table: {u8** elements; u32 size}, 0x30 bytes before the database
  // pointer in TDB 70 (REFramework). Failing that, REFramework's fix-up for later
  // versions: the first of two {pointer, count} pairs of the same count below it
  // (the static blocks, and the table of which of them are initialised).
  auto table_at = [&](int off, uintptr_t* elems, uint32_t* size) {
    if (off < 0) return false;
    const uintptr_t e = mem::read_ptr(vm + static_cast<uintptr_t>(off));
    uint32_t n = 0;
    if (!e || !mem::read_safe(vm + static_cast<uintptr_t>(off) + 8, &n) || n < 2000 || n > 10000000 ||
        !mem::readable(e, static_cast<size_t>(n) * 8))
      return false;
    *elems = e;
    *size = n;
    return true;
  };
  int static_off = tdb_off - 0x30;
  uintptr_t elems = 0, e2 = 0;
  uint32_t size = 0, n2 = 0;
  if (!table_at(static_off, &elems, &size)) {
    static_off = -1;
    for (int i = 8; i < 0x100 && static_off < 0; i += 8)
      if (table_at(tdb_off - i, &e2, &n2) && table_at(tdb_off - i - 0x10, &elems, &size) && size == n2)
        static_off = tdb_off - i - 0x10;
    if (static_off < 0) {
      std::snprintf(g_status, sizeof(g_status), "the VM's static table was not found near its type database");
      return false;
    }
    logf("re: the static table is not at VM+0x%X - found it at VM+0x%X", tdb_off - 0x30, static_off);
  }
  g_static_elems = elems;
  g_static_size = size;
  logf("re: VM %p, static table at VM+0x%X (%u entries)", reinterpret_cast<void*>(vm), static_off, g_static_size);
  g_ready = true;
  std::snprintf(g_status, sizeof(g_status), "ready (%u types)", g.n_types);
  return true;
}

bool ready() { return g_ready; }
const char* status() { return g_status; }
uint32_t num_types() { return g.n_types; }
uint32_t num_methods() { return g.n_methods; }
uintptr_t vm() { return g_vm_slot ? mem::read_ptr(g_vm_slot) : 0; }

// --- types ----------------------------------------------------------------------------------
uint32_t find_type(const char* full) {
  if (!g_ready || !full) return 0;
  auto it = g_by_name.find(full);
  return it == g_by_name.end() ? 0 : it->second;
}

const std::string& full_name(uint32_t t) {
  static const std::string kNone = "?";
  return t && t < g.n_types && !g_names.empty() ? g_names[t] : kNone;
}

uint32_t parent(uint32_t t) { return t && t < g.n_types ? td_parent(td(t)) : 0; }

bool is_value_type(uint32_t t) { return t && t < g.n_types && td_objtype(td(t)) == 5; }

bool derives(uint32_t t, uint32_t base) {
  for (int depth = 0; t && t < g.n_types && depth < 64; ++depth, t = td_parent(td(t)))
    if (t == base) return true;
  return false;
}

uint32_t value_size(uint32_t t) {
  if (!t || t >= g.n_types) return 0;
  const TypeImpl* ti = timpl(td_impl(td(t)));
  return ti ? static_cast<uint32_t>(ti->field_size) : 0;
}

uintptr_t managed_vt(uint32_t t) { return t && t < g.n_types ? td(t)->managed_vt : 0; }

// --- fields ----------------------------------------------------------------------------------
Field find_field(uint32_t t, const char* name) {
  for (int depth = 0; g_ready && t && t < g.n_types && depth < 64; ++depth, t = td_parent(td(t))) {
    const uint32_t first = td(t)->member_field, n = num_fields_of(t);
    for (uint32_t k = 0; k < n; ++k) {
      if (first + k >= g.n_fields) break;
      const RawField rf = raw_field(first + k);
      const FieldImpl* fi = fimpl(rf.impl);
      if (!fi || std::strcmp(str(fi->w2 & 0x3FFFFFFF), name) != 0) continue;
      return Field{rf.declaring ? rf.declaring : t, fi->w1 & 0x3FFFF, rf.offset, fi->flags};
    }
  }
  return Field{};
}

int enum_value(uint32_t enum_type, const char* name, int fallback) {
  const Field f = find_field(enum_type, name);
  if (!f.valid() || !(f.flags & 0x40)) return fallback;  // Literal
  // The literal's value: init data index split across the impl's two words.
  const uint32_t first = td(f.declaring)->member_field, n = num_fields_of(f.declaring);
  for (uint32_t k = 0; k < n; ++k) {
    const RawField rf = raw_field(first + k);
    const FieldImpl* fi = fimpl(rf.impl);
    if (!fi || std::strcmp(str(fi->w2 & 0x3FFFFFFF), name) != 0) continue;
    const uint32_t index = (fi->w1 >> 18) | ((fi->w2 >> 30) << 14);
    if (index >= g.n_init) return fallback;
    const int32_t off = mem::read<int32_t>(g.init_data + static_cast<uintptr_t>(index) * 4);
    if (off < 0 || static_cast<uint32_t>(off) + 4 > g.n_bytes) return fallback;
    return mem::read<int32_t>(g.bytes + static_cast<uint32_t>(off));
  }
  return fallback;
}

// --- statics -----------------------------------------------------------------------------------
uintptr_t static_block(uint32_t t) {
  if (!g_static_elems || t >= g_static_size) return 0;
  return mem::read_ptr(g_static_elems + static_cast<uintptr_t>(t) * 8);
}

uintptr_t static_addr(const Field& f) {
  if (!f.valid() || !f.is_static()) return 0;
  const uintptr_t block = static_block(f.declaring);
  return block ? block + f.offset : 0;
}

uintptr_t singleton(uint32_t t) {
  const Field f = find_field(t, "_Instance");
  const uintptr_t at = static_addr(f);
  const uintptr_t obj = at ? mem::read_ptr(at) : 0;
  return obj && is_a(obj, t) ? obj : 0;
}

// --- objects ---------------------------------------------------------------------------------------
uint32_t type_of(uintptr_t obj) {
  if (!g_ready) return 0;
  const uintptr_t info = mem::read_ptr(obj);
  const uintptr_t cls = info ? mem::read_ptr(info) : 0;
  if (cls < g.types || cls >= g.types + static_cast<uintptr_t>(g.n_types) * sizeof(TypeDef)) return 0;
  if ((cls - g.types) % sizeof(TypeDef)) return 0;
  return static_cast<uint32_t>((cls - g.types) / sizeof(TypeDef));
}

bool is_a(uintptr_t obj, uint32_t t) { return t && derives(type_of(obj), t); }

// Where an object's managed fields begin: the int32 just before its
// REObjectInfo (REFramework's fieldptr offset), cached per class.
static int32_t fieldptr(uintptr_t obj) {
  const uintptr_t info = mem::read_ptr(obj);
  if (!info) return -1;
  EnterCriticalSection(&g_cache_cs);
  auto it = g_fieldptr.find(info);
  int32_t v = it != g_fieldptr.end() ? it->second : -2;
  LeaveCriticalSection(&g_cache_cs);
  if (v != -2) return v;
  if (!mem::read_safe(info - 8, &v) || v < 0 || v > 0x1000 || (v & 7)) v = -1;
  EnterCriticalSection(&g_cache_cs);
  g_fieldptr[info] = v;
  LeaveCriticalSection(&g_cache_cs);
  return v;
}

int32_t fieldptr_offset(uintptr_t obj) { return type_of(obj) ? fieldptr(obj) : -1; }

uintptr_t field_addr(uintptr_t obj, const Field& f) {
  if (!obj || !f.valid() || f.is_static()) return 0;
  const uint32_t t = type_of(obj);
  if (!t || !derives(t, f.declaring)) return 0;
  const int32_t fp = fieldptr(obj);
  if (fp < 0) return 0;
  return obj + static_cast<uint32_t>(fp) + f.offset;
}

// --- arrays and lists ------------------------------------------------------------------------------
// System.Array in TDB 70: the managed header (0x10), the element class at
// +0x10, the count at +0x1C, the elements from +0x20.
bool read_array(uintptr_t arr, Array* out) {
  if (!arr || !mem::readable(arr, 0x20)) return false;
  const uintptr_t elem_cls = mem::read_ptr(arr + 0x10);
  const int32_t count = mem::read<int32_t>(arr + 0x1C);
  if (count < 0 || count > 0x100000) return false;
  uint32_t elem = 8;
  if (elem_cls >= g.types && elem_cls < g.types + static_cast<uintptr_t>(g.n_types) * sizeof(TypeDef) &&
      (elem_cls - g.types) % sizeof(TypeDef) == 0) {
    const uint32_t et = static_cast<uint32_t>((elem_cls - g.types) / sizeof(TypeDef));
    if (is_value_type(et)) elem = value_size(et);
  }
  if (!elem || elem > 0x1000 || !mem::readable(arr + 0x20, static_cast<size_t>(count) * elem)) return false;
  *out = Array{arr, count, arr + 0x20, elem};
  return true;
}

bool read_list(uintptr_t list, List* out) {
  const uint32_t t = type_of(list);
  if (!t) return false;
  // One List`1 layout for every instantiation, but the offsets are read per class.
  EnterCriticalSection(&g_cache_cs);
  auto it = g_list_fields.find(t);
  std::pair<Field, Field> ff;
  if (it == g_list_fields.end()) {
    ff = {find_field(t, "mItems"), find_field(t, "mSize")};
    g_list_fields.emplace(t, ff);
  } else {
    ff = it->second;
  }
  LeaveCriticalSection(&g_cache_cs);
  if (!ff.first.valid() || !ff.second.valid()) return false;
  const uintptr_t items_at = field_addr(list, ff.first), size_at = field_addr(list, ff.second);
  int32_t size = 0;
  if (!items_at || !size_at || !mem::read_safe(size_at, &size) || size < 0) return false;
  List l;
  l.obj = list;
  l.count = size;
  const uintptr_t arr = mem::read_ptr(items_at);
  if (size > 0 && (!read_array(arr, &l.items) || l.items.count < size)) return false;
  *out = l;
  return true;
}

uintptr_t list_ref(const List& l, int i) {
  if (i < 0 || i >= l.count || !l.items.data || l.items.elem_size != 8) return 0;
  return mem::read_ptr(l.items.data + static_cast<uintptr_t>(i) * 8);
}

// --- methods --------------------------------------------------------------------------------------
uintptr_t method_code(uint32_t t, const char* name, int nparams) {
  if (!g_ready || !t || t >= g.n_types) return 0;
  const uint32_t first = td(t)->member_method, n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const RawMethod m = raw_method(first + k);
    const MethodImpl* mi = mimpl(m.impl);
    if (!mi || std::strcmp(str(mi->name & 0x3FFFFFFF), name) != 0) continue;
    if (nparams >= 0) {
      if (m.params + 2 > g.n_bytes) continue;
      if (mem::read<uint16_t>(g.bytes + m.params) != nparams) continue;
    }
    return m.code;
  }
  return 0;
}

uint32_t find_method(uint32_t t, const char* name, std::initializer_list<const char*> param_types, bool is_static) {
  if (!g.base || !t || t >= g.n_types) return 0;
  const uint32_t first = td(t)->member_method, n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const RawMethod m = raw_method(first + k);
    const MethodImpl* mi = mimpl(m.impl);
    if (!mi || std::strcmp(str(mi->name & 0x3FFFFFFF), name) != 0) continue;
    if (((mi->flags & 0x10) != 0) != is_static) continue;
    // The byte pool's ParamList: {u16 count; u16 invoke id; u32 return; u32 params[count]}.
    const size_t np = param_types.size();
    if (m.params + 8 + np * 4 > g.n_bytes || mem::read<uint16_t>(g.bytes + m.params) != np) continue;
    bool same = true;
    size_t i = 0;
    for (const char* want : param_types) {
      const uint32_t p = mem::read<uint32_t>(g.bytes + m.params + 8 + i * 4);
      const ParamDef* pd =
          p < g.n_params ? reinterpret_cast<const ParamDef*>(g.params + static_cast<uintptr_t>(p) * sizeof(ParamDef)) : nullptr;
      const uint32_t type = pd ? (pd->w2 & 0x3FFFF) : 0;
      if (!type || type >= g.n_types || full_name(type) != want) {
        same = false;
        break;
      }
      ++i;
    }
    if (same) return first + k;
  }
  return 0;
}

uintptr_t method_code_typed(uint32_t t, const char* name, std::initializer_list<const char*> param_types, bool is_static) {
  const uint32_t m = g_ready ? find_method(t, name, param_types, is_static) : 0;
  return m ? raw_method(m).code : 0;
}

uintptr_t method_record(uint32_t t, const char* name, int nparams) {
  if (!g_ready || !t || t >= g.n_types) return 0;
  const uint32_t first = td(t)->member_method, n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const RawMethod m = raw_method(first + k);
    const MethodImpl* mi = mimpl(m.impl);
    if (!mi || std::strcmp(str(mi->name & 0x3FFFFFFF), name) != 0) continue;
    if (nparams >= 0 && (m.params + 2 > g.n_bytes || mem::read<uint16_t>(g.bytes + m.params) != nparams)) continue;
    return g.methods + static_cast<uintptr_t>(first + k) * 16;
  }
  return 0;
}

static bool is_method_record(uintptr_t record) {
  return g_ready && record >= g.methods && record < g.methods + static_cast<uintptr_t>(g.n_methods) * 16 &&
         (record - g.methods) % 16 == 0;
}

uintptr_t method_record_at(uint32_t index) {
  return g_ready && index < g.n_methods ? g.methods + static_cast<uintptr_t>(index) * 16 : 0;
}

uintptr_t hook_method(uintptr_t record, uintptr_t replacement, unsigned long* protect) {
  if (protect) *protect = 0;
  if (!is_method_record(record) || !replacement) return 0;
  const uintptr_t slot = record + 8;
  const mem::Range text = mem::section(GetModuleHandleA(nullptr), ".text");
  uintptr_t code = 0;
  if (!mem::read_safe(slot, &code) || !text.contains(code)) return 0;
  // Only while the entry still holds the code just read. The database lives in
  // re2.exe's .data: copy-on-write pages, or protected ones made writable for it.
  return mem::exchange_ptr(slot, code, replacement, protect) ? code : 0;
}

bool unhook_method(uintptr_t record, uintptr_t replacement, uintptr_t original) {
  return is_method_record(record) && replacement && original && mem::exchange_ptr(record + 8, replacement, original);
}

int method_vt_index(uint32_t method_index) {
  if (!g_ready || method_index >= g.n_methods) return -1;
  const MethodImpl* mi = mimpl(raw_method(method_index).impl);
  return mi && (mi->flags & 0x40) && mi->vt_index >= 0 ? mi->vt_index : -1;
}

uintptr_t method_code_at(uint32_t method_index) {
  return g_ready && method_index < g.n_methods ? raw_method(method_index).code : 0;
}

uintptr_t vtable_of(uint32_t t) {
  const uintptr_t info = managed_vt(t);
  return info ? mem::read_ptr(info - 0x10) : 0;
}

uintptr_t vtable_slot(uint32_t t, uint32_t method_index, uintptr_t code) {
  const int vt = method_vt_index(method_index);
  const uintptr_t table = vt >= 0 && code ? vtable_of(t) : 0;
  if (!table) return 0;
  const uintptr_t slot = table + static_cast<uintptr_t>(vt) * 8;
  return mem::read_ptr(slot) == code ? slot : 0;
}

int interface_slots(uint32_t t, uintptr_t code, uintptr_t* out, int max, int tables) {
  const uintptr_t info = managed_vt(t);
  if (!info || !code) return 0;
  int n = 0;
  for (int k = 0; k < tables; ++k) {
    const uintptr_t table = mem::read_ptr(info - static_cast<uintptr_t>(3 + k) * 8);
    if (!table || !mem::readable(table, 64 * 8)) continue;
    for (uintptr_t s = table; s < table + 64 * 8 && n < max; s += 8)
      if (mem::read_ptr(s) == code) out[n++] = s;
  }
  return n;
}

bool swap_slot(uintptr_t slot, uintptr_t expected, uintptr_t replacement) {
  return slot && expected && replacement && mem::exchange_ptr(slot, expected, replacement);
}

int classes_sharing_vtable(uint32_t t) {
  const uintptr_t table = vtable_of(t);
  if (!table) return -1;
  int n = 0;
  for (uint32_t i = 1; i < g.n_types; ++i)
    if (i != t && td(i)->managed_vt && mem::read_ptr(td(i)->managed_vt - 0x10) == table) ++n;
  return n;
}

int globals_pointing_at(uintptr_t record) {
  const mem::Range data = mem::section(GetModuleHandleA(nullptr), ".data");
  if (!record || data.empty() || !mem::readable(data.begin, data.size())) return -1;
  int n = 0;
  for (uintptr_t a = (data.begin + 7) & ~static_cast<uintptr_t>(7); a + 8 <= data.end; a += 8)
    if (*reinterpret_cast<const uintptr_t*>(a) == record) ++n;
  return n;
}

std::string method_at(uintptr_t record) {
  if (!is_method_record(record)) return {};
  const RawMethod m = raw_method(static_cast<uint32_t>((record - g.methods) / 16));
  const MethodImpl* mi = mimpl(m.impl);
  return (m.declaring < g.n_types ? full_name(m.declaring) : std::string("?")) + "." +
         (mi ? str(mi->name & 0x3FFFFFFF) : "?");
}

bool dump_methods(const char* path) {
  if (!g_ready) return false;
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  const mem::Range image = mem::module_range(GetModuleHandleA(nullptr));
  const uint32_t header[4] = {0x4D324552 /* "RE2M" */, 1, g.n_methods, 0};
  std::fwrite(header, sizeof(header), 1, f);
  uint32_t with_code = 0;
  std::vector<uint32_t> rvas(g.n_methods, 0);
  for (uint32_t i = 0; i < g.n_methods; ++i) {
    const uintptr_t code = raw_method(i).code;
    if (code && image.contains(code)) {
      rvas[i] = static_cast<uint32_t>(code - base);
      ++with_code;
    }
  }
  std::fwrite(rvas.data(), 4, rvas.size(), f);
  std::fclose(f);
  logf("re: wrote %s - %u of %u methods have code in the image", path, with_code, g.n_methods);
  return true;
}

}  // namespace re2cc::re
