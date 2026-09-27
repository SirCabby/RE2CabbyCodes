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

// --- the database's layouts (TDB 70 and TDB 66) ------------------------------------------
// The current Steam build (11636119, ray tracing) has TDB 70; the dx11_non-rt
// beta (11055033, the game before its 2022 update) has TDB 66. Same magic, same
// idea - classes, fields, methods, a string pool and a byte pool, the array
// pointers offsets from the header in the exe's copy, rewritten to addresses by
// the VM as it starts - but different records:
//  - TDB 70 (REFramework's tdb70::TDB, RETypeDefVersion69): counts at 0x0C..,
//    18 array pointers from 0x58. Names, counts and flags live in separate "impl"
//    records the typedefs, fields and methods point at; 18-bit type indices.
//  - TDB 66 (tdb66::TDB, RETypeDefVersion66): counts at 0x0C.., 13 array
//    pointers from 0x48 (modules, types, methods, fields, properties, events, -,
//    params, init data, ?, strings, bytes, intern strings). No impl records:
//    every record carries its own name and counts; 16-bit type indices. The init
//    data is the array at 0x88, counted at 0x28 (REFramework's struct names the
//    two neighbours the other way round; the literals only read right this way).
// Layouts confirmed on both exes: every array ends where the next begins, every
// typedef names itself, and fields, methods, enum literals and generic names
// read back as tools/tdb_dump.py reads them (tests/test_re.cpp).
constexpr uint32_t kMagic = 0x00424454;  // "TDB\0"
constexpr uint32_t kVersionRT = 70;      // build 11636119
constexpr uint32_t kVersionDX11 = 66;    // build 11055033, the dx11_non-rt beta

bool supported_version(uint32_t v) { return v == kVersionRT || v == kVersionDX11; }

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

// RETypeDefVersion66 (0x78): its name, counts and sizes on the typedef itself.
struct TypeDef66 {
  uint64_t a;  // index:16 ?:16 parent:16 declaring:16
  uint32_t fqn, crc;
  uint64_t pad10;
  uint32_t name, ns, flags;
  uint8_t system, pad25, object_type, pad27;
  uint32_t ctor, field_size, size, pad34;
  uint32_t methods;  // num:12 first:19
  uint32_t fields;   // num:12 first:19
  uint32_t props;    // num:12 first:19
  uint32_t events, interfaces, pad4c;
  int32_t generics;
  uint32_t vt;
  uint64_t pad58;
  uintptr_t unk60;
  uintptr_t clr_type;
  uintptr_t managed_vt;
};
static_assert(sizeof(TypeDef66) == 0x78, "TypeDef66");

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
// tdb66::REField (0x14).
struct Field66 {
  uint64_t a;  // declaring:16 type:16 ?:32
  uint32_t name;
  uint16_t flags, init;
  uint32_t offset;
};
// tdb66::REMethodDefinition (0x20): the code pointer at +0x18, set by the VM.
struct Method66 {
  uint64_t a;  // declaring:16 vtable_index:16 num_params:8 ?:8 return:16
  uint16_t pad8;
  int16_t invoke;
  uint32_t name;
  uint16_t flags, impl_flags;
  uint32_t params;  // byte pool: num_params tdb66::REMethodParamDef {type:16 flags:16 name:31}
  uintptr_t code;
};
#pragma pack(pop)
static_assert(sizeof(TypeImpl) == 0x30, "TypeImpl");
static_assert(sizeof(MethodImpl) == 12, "MethodImpl");
static_assert(sizeof(FieldImpl) == 12, "FieldImpl");
static_assert(sizeof(ParamDef) == 12, "ParamDef");
static_assert(sizeof(Field66) == 0x14, "Field66");
static_assert(sizeof(Method66) == 0x20, "Method66");

struct Db {
  uintptr_t base = 0;
  uint32_t version = 0;
  uint32_t n_types = 0, n_methods = 0, n_fields = 0, n_type_impl = 0, n_method_impl = 0, n_field_impl = 0;
  uint32_t n_params = 0, n_init = 0, n_strings = 0, n_bytes = 0;
  uintptr_t types = 0, types_impl = 0, methods = 0, methods_impl = 0, fields = 0, fields_impl = 0;
  uintptr_t params = 0, init_data = 0, strings = 0, bytes = 0;
  // The records' sizes, and where a method keeps its code (what delegates copy).
  uint32_t type_size = sizeof(TypeDef), method_size = 16, field_size = 8, code_at = 8;
  bool v66() const { return version == kVersionDX11; }
} g;

uintptr_t g_vm_slot = 0;
uintptr_t g_static_elems = 0;  // uint8_t** - one static block per type index
uint32_t g_static_size = 0;
std::atomic<bool> g_ready{false};  // set last, on the mod thread; the tick reads it
std::atomic<bool> g_unsupported{false};  // re2.exe has no TDB the mod reads: init() never will succeed
char g_status[320] = "not started";

std::vector<std::string> g_names;      // full names, built once
std::unordered_map<std::string, uint32_t> g_by_name;
// The two caches the game tick fills as it meets classes: where an object's
// fields start (per REObjectInfo), and a List<T>'s two fields (per class).
std::unordered_map<uintptr_t, int32_t> g_fieldptr;
std::unordered_map<uint32_t, std::pair<Field, Field>> g_list_fields;
CRITICAL_SECTION g_cache_cs;
bool g_cache_cs_ready = false;

const char* str(uint32_t off) { return off < g.n_strings ? reinterpret_cast<const char*>(g.strings + off) : ""; }

// --- one view of a typedef, a field and a method, whichever the layout --------------------
uintptr_t td_at(uint32_t i) { return g.types + static_cast<uintptr_t>(i) * g.type_size; }
const TypeDef* td(uint32_t i) { return reinterpret_cast<const TypeDef*>(td_at(i)); }
const TypeDef66* td66(uint32_t i) { return reinterpret_cast<const TypeDef66*>(td_at(i)); }
const TypeImpl* timpl(uint32_t i) {
  return i < g.n_type_impl ? reinterpret_cast<const TypeImpl*>(g.types_impl + static_cast<uintptr_t>(i) * sizeof(TypeImpl)) : nullptr;
}
const TypeImpl* td_impl(uint32_t i) { return timpl(static_cast<uint32_t>((td(i)->b >> 36) & 0x3FFFF)); }

uint32_t td_index(uint32_t i) {
  return g.v66() ? static_cast<uint32_t>(td66(i)->a & 0xFFFF) : static_cast<uint32_t>(td(i)->a & 0x3FFFF);
}
uint32_t td_parent(uint32_t i) {
  return g.v66() ? static_cast<uint32_t>((td66(i)->a >> 32) & 0xFFFF) : static_cast<uint32_t>((td(i)->a >> 18) & 0x3FFFF);
}
uint32_t td_declaring(uint32_t i) {
  return g.v66() ? static_cast<uint32_t>((td66(i)->a >> 48) & 0xFFFF) : static_cast<uint32_t>((td(i)->a >> 36) & 0x3FFFF);
}
uint32_t td_objtype(uint32_t i) {  // via.clr's object type: 1 class, 3 string, 5 value type
  return g.v66() ? td66(i)->object_type : static_cast<uint32_t>((td(i)->a >> 61) & 7);
}
int32_t td_generics(uint32_t i) { return g.v66() ? td66(i)->generics : td(i)->generics; }
uintptr_t td_managed_vt(uint32_t i) { return g.v66() ? td66(i)->managed_vt : td(i)->managed_vt; }
const char* td_name(uint32_t i) {
  if (g.v66()) return str(td66(i)->name);
  const TypeImpl* ti = td_impl(i);
  return ti ? str(static_cast<uint32_t>(ti->name)) : "";
}
const char* td_namespace(uint32_t i) {
  if (g.v66()) return str(td66(i)->ns);
  const TypeImpl* ti = td_impl(i);
  return ti ? str(static_cast<uint32_t>(ti->ns)) : "";
}
uint32_t td_field_size(uint32_t i) {  // the fields' size: a value type's in an array
  if (g.v66()) return td66(i)->field_size;
  const TypeImpl* ti = td_impl(i);
  return ti ? static_cast<uint32_t>(ti->field_size) : 0;
}
uint32_t first_field_of(uint32_t t) {
  return g.v66() ? (td66(t)->fields >> 12) & 0x7FFFF : td(t)->member_field;
}
uint32_t num_fields_of(uint32_t t) {
  if (g.v66()) return td66(t)->fields & 0xFFF;
  const TypeImpl* ti = td_impl(t);
  return ti && td(t)->member_field ? static_cast<uint32_t>(ti->num_fields) : 0;
}
uint32_t first_method_of(uint32_t t) {
  return g.v66() ? (td66(t)->methods >> 12) & 0x7FFFF : td(t)->member_method;
}
uint32_t num_methods_of(uint32_t t) {
  if (g.v66()) return td66(t)->methods & 0xFFF;
  const TypeImpl* ti = td_impl(t);
  return ti && td(t)->member_method ? ti->num_methods : 0;
}

struct FieldView {
  bool ok = false;
  uint32_t declaring = 0, type = 0, offset = 0, init = 0;
  uint16_t flags = 0;
  const char* name = "";
};
FieldView field_view(uint32_t i) {
  FieldView v;
  if (i >= g.n_fields) return v;
  if (g.v66()) {
    const auto* f = reinterpret_cast<const Field66*>(g.fields + static_cast<uintptr_t>(i) * sizeof(Field66));
    v.ok = true;
    v.declaring = static_cast<uint32_t>(f->a & 0xFFFF);
    v.type = static_cast<uint32_t>((f->a >> 16) & 0xFFFF);
    v.offset = f->offset;
    v.init = f->init;
    v.flags = f->flags;
    v.name = str(f->name);
    return v;
  }
  const uint64_t raw = *reinterpret_cast<const uint64_t*>(g.fields + static_cast<uintptr_t>(i) * 8);
  const uint32_t impl = static_cast<uint32_t>((raw >> 18) & 0xFFFFF);
  if (impl >= g.n_field_impl) return v;
  const auto* fi = reinterpret_cast<const FieldImpl*>(g.fields_impl + static_cast<uintptr_t>(impl) * sizeof(FieldImpl));
  v.ok = true;
  v.declaring = static_cast<uint32_t>(raw & 0x3FFFF);
  v.type = fi->w1 & 0x3FFFF;
  v.offset = static_cast<uint32_t>(raw >> 38);
  v.init = (fi->w1 >> 18) | ((fi->w2 >> 30) << 14);  // the init data index, split across the impl's two words
  v.flags = fi->flags;
  v.name = str(fi->w2 & 0x3FFFFFFF);
  return v;
}

struct MethodView {
  bool ok = false;
  uint32_t declaring = 0, nparams = 0, params = 0;  // params: the byte pool offset of the parameter list
  uint16_t flags = 0;
  int16_t vt_index = -1;
  const char* name = "";
};
uintptr_t method_at_index(uint32_t i) { return g.methods + static_cast<uintptr_t>(i) * g.method_size; }
MethodView method_view(uint32_t i) {
  MethodView v;
  if (i >= g.n_methods) return v;
  if (g.v66()) {
    const auto* m = reinterpret_cast<const Method66*>(method_at_index(i));
    v.ok = true;
    v.declaring = static_cast<uint32_t>(m->a & 0xFFFF);
    v.vt_index = static_cast<int16_t>((m->a >> 16) & 0xFFFF);
    v.nparams = static_cast<uint32_t>((m->a >> 32) & 0xFF);
    v.params = m->params;
    v.flags = m->flags;
    v.name = str(m->name);
    return v;
  }
  const uint64_t raw = *reinterpret_cast<const uint64_t*>(method_at_index(i));
  const uint32_t impl = static_cast<uint32_t>((raw >> 18) & 0xFFFFF);
  if (impl >= g.n_method_impl) return v;
  const auto* mi = reinterpret_cast<const MethodImpl*>(g.methods_impl + static_cast<uintptr_t>(impl) * sizeof(MethodImpl));
  v.ok = true;
  v.declaring = static_cast<uint32_t>(raw & 0x3FFFF);
  v.params = static_cast<uint32_t>(raw >> 38);
  // The byte pool's ParamList: {u16 count; u16 invoke id; u32 return; u32 params[count]}.
  v.nparams = v.params + 2 <= g.n_bytes ? mem::read<uint16_t>(g.bytes + v.params) : 0xFFFF;
  v.flags = mi->flags;
  v.vt_index = mi->vt_index;
  v.name = str(mi->name & 0x3FFFFFFF);
  return v;
}
// The class of parameter k of a method (0 when it cannot be read).
uint32_t param_type(const MethodView& m, uint32_t k) {
  if (k >= m.nparams) return 0;
  if (g.v66()) {
    if (m.params + (k + 1) * 8ull > g.n_bytes) return 0;
    return static_cast<uint32_t>(mem::read<uint64_t>(g.bytes + m.params + k * 8ull) & 0xFFFF);
  }
  if (m.params + 8 + (k + 1) * 4ull > g.n_bytes) return 0;
  const uint32_t p = mem::read<uint32_t>(g.bytes + m.params + 8 + k * 4ull);
  if (p >= g.n_params) return 0;
  return reinterpret_cast<const ParamDef*>(g.params + static_cast<uintptr_t>(p) * sizeof(ParamDef))->w2 & 0x3FFFF;
}
uintptr_t code_of(uint32_t i) { return i < g.n_methods ? mem::read<uintptr_t>(method_at_index(i) + g.code_at) : 0; }

// The header, read from a candidate address. The pointers are addresses once the
// VM has initialised it, offsets from the header in the exe's own copy.
bool read_header(uintptr_t base, Db* out) {
  if (!mem::readable(base, 0xE8)) return false;
  const auto u32 = [&](int o) { return mem::read<uint32_t>(base + o); };
  const auto u64 = [&](int o) { return mem::read<uint64_t>(base + o); };
  if (u32(0) != kMagic || !supported_version(u32(4))) return false;
  Db d;
  d.base = base;
  d.version = u32(4);
  d.n_types = u32(0x0C);
  d.n_methods = u32(0x10);
  d.n_fields = u32(0x14);
  if (d.v66()) {
    d.n_params = u32(0x24);
    d.n_init = u32(0x28);
    d.n_strings = u32(0x40);
    d.n_bytes = u32(0x44);
    d.types = u64(0x50);
    d.methods = u64(0x58);
    d.fields = u64(0x60);
    d.params = u64(0x80);
    d.init_data = u64(0x88);
    d.strings = u64(0x98);
    d.bytes = u64(0xA0);
    d.type_size = sizeof(TypeDef66);
    d.method_size = sizeof(Method66);
    d.field_size = sizeof(Field66);
    d.code_at = offsetof(Method66, code);
    if (d.n_types > 0xFFFF) return false;
  } else {
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
    if (d.n_types > 0x3FFFF) return false;
  }
  if (!d.n_types || !d.n_methods || !d.n_fields || !d.n_strings || !d.n_bytes) return false;
  *out = d;
  return true;
}

// The header's array pointers, as a version lays them out: none may still be an
// offset once the VM has loaded the database.
int array_pointers(uint32_t version, int* first) {
  *first = version == kVersionDX11 ? 0x48 : 0x58;
  return version == kVersionDX11 ? 13 : 18;
}

// Every array the mod reads must be where the header says, whole.
bool arrays_readable(const Db& d) {
  const bool common = mem::readable(d.types, static_cast<size_t>(d.n_types) * d.type_size) &&
                      mem::readable(d.methods, static_cast<size_t>(d.n_methods) * d.method_size) &&
                      mem::readable(d.fields, static_cast<size_t>(d.n_fields) * d.field_size) &&
                      mem::readable(d.init_data, static_cast<size_t>(d.n_init) * 4) &&
                      mem::readable(d.strings, d.n_strings) && mem::readable(d.bytes, d.n_bytes);
  if (d.v66()) return common;
  return common && mem::readable(d.types_impl, static_cast<size_t>(d.n_type_impl) * sizeof(TypeImpl)) &&
         mem::readable(d.methods_impl, static_cast<size_t>(d.n_method_impl) * sizeof(MethodImpl)) &&
         mem::readable(d.fields_impl, static_cast<size_t>(d.n_field_impl) * sizeof(FieldImpl)) &&
         mem::readable(d.params, static_cast<size_t>(d.n_params) * sizeof(ParamDef));
}

// Every typedef names itself: its index field is its own position. A database
// read with the wrong layout fails this at once.
bool self_check(char* why, size_t n) {
  const uint32_t probes[] = {1, 2, 3, 100, 1000, g.n_types / 2, g.n_types - 1};
  for (uint32_t i : probes) {
    if (i == 0 || i >= g.n_types) continue;
    const uint32_t idx = td_index(i);
    if (idx != i) {
      std::snprintf(why, n, "typedef %u says it is %u", i, idx);
      return false;
    }
    if (!g.v66() && !td_impl(i)) {
      std::snprintf(why, n, "typedef %u has impl %u of %u", i, static_cast<uint32_t>((td(i)->b >> 36) & 0x3FFFF),
                    g.n_type_impl);
      return false;
    }
    if (!*td_name(i)) {
      std::snprintf(why, n, "typedef %u has no name", i);
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
  const char* ns = td_namespace(i);
  const char* nm = td_name(i);
  std::string full;
  const uint32_t decl = td_declaring(i);
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
  // A generic instance: "List`1<app.ropeway.inventory.Slot>". Its argument list
  // in the byte pool: {definition:18 num:14; u32 types[num]} in TDB 70,
  // {definition:16 num:16; u16 types[num]} in TDB 66.
  const int32_t generics = td_generics(i);
  if (generics > 0 && static_cast<uint32_t>(generics) + 4 <= g.n_bytes) {
    const uintptr_t gl = g.bytes + static_cast<uint32_t>(generics);
    const uint32_t w = mem::read<uint32_t>(gl);
    const uint32_t def = g.v66() ? (w & 0xFFFF) : (w & 0x3FFFF), num = g.v66() ? (w >> 16) : (w >> 18);
    const uint32_t arg = g.v66() ? 2 : 4;
    if (def != i && num > 0 && num <= 16 && static_cast<uint32_t>(generics) + 4 + num * arg <= g.n_bytes) {
      full += '<';
      for (uint32_t k = 0; k < num; ++k) {
        if (k) full += ',';
        const uint32_t a = g.v66() ? mem::read<uint16_t>(gl + 4 + 2 * k) : mem::read<uint32_t>(gl + 4 + 4 * k);
        full += build_name(a, depth + 1);
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

// The database in the image, found by its header: magic, version 70 or 66.
uintptr_t find_tdb_in_image(const mem::Range& data) {
  const auto* p = reinterpret_cast<const uint8_t*>(data.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(data.end) - 0x100;
  for (const uint8_t* c = p; c < end;) {
    c = static_cast<const uint8_t*>(std::memchr(c, 'T', end - c));
    if (!c) break;
    if ((reinterpret_cast<uintptr_t>(c) & 7) == 0 && std::memcmp(c, "TDB\0", 4) == 0 &&
        supported_version(mem::read<uint32_t>(reinterpret_cast<uintptr_t>(c) + 4)))
      return reinterpret_cast<uintptr_t>(c);
    ++c;
  }
  return 0;
}

// The version of a database header of any other version in the image (0: none)
// - the message of a game the mod was not made for. RE Engine has used 49 to the
// 80s.
uint32_t other_tdb_version(const mem::Range& data) {
  const auto* p = reinterpret_cast<const uint8_t*>(data.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(data.end) - 0x100;
  for (const uint8_t* c = p; c < end;) {
    c = static_cast<const uint8_t*>(std::memchr(c, 'T', end - c));
    if (!c) break;
    if ((reinterpret_cast<uintptr_t>(c) & 7) == 0 && std::memcmp(c, "TDB\0", 4) == 0) {
      const uint32_t v = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(c) + 4);
      if (v >= 40 && v < 128) return v;
    }
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
  if (g_unsupported) return false;  // what re2.exe carries does not change
  HMODULE exe = GetModuleHandleA(nullptr);
  const mem::Range image = mem::module_range(exe);
  const mem::Range text = mem::section(exe, ".text");
  const mem::Range data = mem::section(exe, ".data");
  if (text.empty() || data.empty()) {
    std::snprintf(g_status, sizeof(g_status),
                  "re2.exe has no .text/.data section: not a version of the game the mod can read (it works with the "
                  "current Steam version, build 11636119, and the dx11_non-rt beta, build 11055033)");
    g_unsupported = true;
    return false;
  }
  static uintptr_t s_image_tdb = 0;
  if (!s_image_tdb) {
    s_image_tdb = find_tdb_in_image(data);
    if (!s_image_tdb) {
      const uint32_t other = other_tdb_version(data);
      if (other)
        std::snprintf(g_status, sizeof(g_status),
                      "re2.exe's type database is v%u, not v70 or v66: a version of the game the mod was not made for "
                      "(it works with the current Steam version, build 11636119, and the dx11_non-rt beta, build "
                      "11055033)",
                      other);
      else
        std::snprintf(g_status, sizeof(g_status),
                      "no type database in re2.exe's .data: not a version of the game the mod can read (it works with "
                      "the current Steam version, build 11636119, and the dx11_non-rt beta, build 11055033)");
      g_unsupported = true;
      return false;
    }
    logf("re: TDB v%u header in .data at exe+0x%llX (%s)", mem::read<uint32_t>(s_image_tdb + 4),
         static_cast<unsigned long long>(s_image_tdb - image.begin),
         mem::read<uint32_t>(s_image_tdb + 4) == kVersionDX11 ? "the dx11_non-rt build" : "the ray-tracing build");
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
  // other header of the same version it holds.
  const uint32_t version = mem::read<uint32_t>(s_image_tdb + 4);
  int tdb_off = -1;
  uintptr_t tdb = 0;
  for (int off = 0x30; off < 0x20000; off += 8) {
    const uintptr_t p = mem::read_ptr(vm + off);
    if (!p || !mem::readable(p, 8) || mem::read<uint32_t>(p) != kMagic || mem::read<uint32_t>(p + 4) != version) continue;
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
  int first_array = 0;
  const int arrays = array_pointers(version, &first_array);
  for (int i = 0; i < arrays; ++i) {
    const uint64_t p = mem::read<uint64_t>(tdb + first_array + 8 * static_cast<uintptr_t>(i));
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
      std::snprintf(g_status, sizeof(g_status), "the type database header does not read as v%u", version);
      return false;
    }
    if (!attach(d)) return false;  // status set
    logf("re: type database v%u at VM+0x%X loaded: %u types, %u methods, %u fields (header flag %u)", g.version, tdb_off,
         g.n_types, g.n_methods, g.n_fields, mem::read<uint32_t>(tdb + 8));
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
  // The loader fills the methods' code in after the arrays are rebased, and
  // discovery reads that code (switches, the typewriters' lambdas, the knives):
  // on Windows the mod got here first and found some of it still 0 - knives 0,
  // typewriters 0, no stacking (2026-09-25). So wait until the count of methods
  // with code is above 0 and has not moved since the last look (~300 ms).
  static uint32_t s_linked = 0;
  uint32_t linked = 0;
  for (uint32_t i = 0; i < g.n_methods; ++i) linked += code_of(i) != 0;
  if (!linked || linked != s_linked) {
    s_linked = linked;
    std::snprintf(g_status, sizeof(g_status), "waiting for the game's code to be linked (%u of %u methods so far)", linked,
                  g.n_methods);
    return false;
  }
  logf("re: VM %p, static table at VM+0x%X (%u entries); %u of %u methods linked", reinterpret_cast<void*>(vm), static_off,
       g_static_size, linked, g.n_methods);
  g_ready = true;
  std::snprintf(g_status, sizeof(g_status), "ready (%u types)", g.n_types);
  return true;
}

bool ready() { return g_ready; }
bool unsupported() { return g_unsupported; }
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

uint32_t parent(uint32_t t) { return t && t < g.n_types ? td_parent(t) : 0; }

bool is_value_type(uint32_t t) { return t && t < g.n_types && td_objtype(t) == 5; }

bool derives(uint32_t t, uint32_t base) {
  for (int depth = 0; t && t < g.n_types && depth < 64; ++depth, t = td_parent(t))
    if (t == base) return true;
  return false;
}

uint32_t value_size(uint32_t t) { return t && t < g.n_types ? td_field_size(t) : 0; }

uintptr_t managed_vt(uint32_t t) { return t && t < g.n_types ? td_managed_vt(t) : 0; }

uint32_t tdb_version() { return g.version; }

// --- fields ----------------------------------------------------------------------------------
Field find_field(uint32_t t, const char* name) {
  for (int depth = 0; g_ready && t && t < g.n_types && depth < 64; ++depth, t = td_parent(t)) {
    const uint32_t first = first_field_of(t), n = num_fields_of(t);
    for (uint32_t k = 0; k < n; ++k) {
      if (first + k >= g.n_fields) break;
      const FieldView fv = field_view(first + k);
      if (!fv.ok || std::strcmp(fv.name, name) != 0) continue;
      return Field{fv.declaring ? fv.declaring : t, fv.type, fv.offset, fv.flags};
    }
  }
  return Field{};
}

int enum_value(uint32_t enum_type, const char* name, int fallback) {
  const Field f = find_field(enum_type, name);
  if (!f.valid() || !(f.flags & 0x40)) return fallback;  // Literal
  // The literal's value: its init data index (TDB 70: split across the field
  // impl's two words; TDB 66: on the field), an int32 offset into the byte pool.
  const uint32_t first = first_field_of(f.declaring), n = num_fields_of(f.declaring);
  for (uint32_t k = 0; k < n; ++k) {
    const FieldView fv = field_view(first + k);
    if (!fv.ok || std::strcmp(fv.name, name) != 0) continue;
    const uint32_t index = fv.init;
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
  if (cls < g.types || cls >= g.types + static_cast<uintptr_t>(g.n_types) * g.type_size) return 0;
  if ((cls - g.types) % g.type_size) return 0;
  return static_cast<uint32_t>((cls - g.types) / g.type_size);
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
// System.Array (TDB 70 and 66): the managed header (0x10), the element class at
// +0x10, the count at +0x1C, the elements from +0x20.
bool read_array(uintptr_t arr, Array* out) {
  if (!arr || !mem::readable(arr, 0x20)) return false;
  const uintptr_t elem_cls = mem::read_ptr(arr + 0x10);
  const int32_t count = mem::read<int32_t>(arr + 0x1C);
  if (count < 0 || count > 0x100000) return false;
  uint32_t elem = 8;
  if (elem_cls >= g.types && elem_cls < g.types + static_cast<uintptr_t>(g.n_types) * g.type_size &&
      (elem_cls - g.types) % g.type_size == 0) {
    const uint32_t et = static_cast<uint32_t>((elem_cls - g.types) / g.type_size);
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
  const uint32_t first = first_method_of(t), n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const MethodView m = method_view(first + k);
    if (!m.ok || std::strcmp(m.name, name) != 0) continue;
    if (nparams >= 0 && m.nparams != static_cast<uint32_t>(nparams)) continue;
    return code_of(first + k);
  }
  return 0;
}

uint32_t find_method(uint32_t t, const char* name, std::initializer_list<const char*> param_types, bool is_static) {
  if (!g.base || !t || t >= g.n_types) return 0;
  const uint32_t first = first_method_of(t), n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const MethodView m = method_view(first + k);
    if (!m.ok || std::strcmp(m.name, name) != 0) continue;
    if (((m.flags & 0x10) != 0) != is_static) continue;
    if (m.nparams != param_types.size()) continue;
    bool same = true;
    uint32_t i = 0;
    for (const char* want : param_types) {
      const uint32_t type = param_type(m, i++);
      if (!type || type >= g.n_types || full_name(type) != want) {
        same = false;
        break;
      }
    }
    if (same) return first + k;
  }
  return 0;
}

uintptr_t method_code_typed(uint32_t t, const char* name, std::initializer_list<const char*> param_types, bool is_static) {
  const uint32_t m = g_ready ? find_method(t, name, param_types, is_static) : 0;
  return m ? code_of(m) : 0;
}

uintptr_t method_record(uint32_t t, const char* name, int nparams) {
  if (!g_ready || !t || t >= g.n_types) return 0;
  const uint32_t first = first_method_of(t), n = num_methods_of(t);
  for (uint32_t k = 0; k < n && first + k < g.n_methods; ++k) {
    const MethodView m = method_view(first + k);
    if (!m.ok || std::strcmp(m.name, name) != 0) continue;
    if (nparams >= 0 && m.nparams != static_cast<uint32_t>(nparams)) continue;
    return method_at_index(first + k);
  }
  return 0;
}

static bool is_method_record(uintptr_t record) {
  return g_ready && record >= g.methods && record < g.methods + static_cast<uintptr_t>(g.n_methods) * g.method_size &&
         (record - g.methods) % g.method_size == 0;
}

uintptr_t method_record_at(uint32_t index) { return g_ready && index < g.n_methods ? method_at_index(index) : 0; }

// A method's code pointer - the one delegates are made from - is the record's
// second qword in TDB 70 and its fourth in TDB 66.
uintptr_t hook_method(uintptr_t record, uintptr_t replacement, unsigned long* protect) {
  if (protect) *protect = 0;
  if (!is_method_record(record) || !replacement) return 0;
  const uintptr_t slot = record + g.code_at;
  const mem::Range text = mem::section(GetModuleHandleA(nullptr), ".text");
  uintptr_t code = 0;
  if (!mem::read_safe(slot, &code) || !text.contains(code)) return 0;
  // Only while the entry still holds the code just read. The database lives in
  // re2.exe's .data: copy-on-write pages, or protected ones made writable for it.
  return mem::exchange_ptr(slot, code, replacement, protect) ? code : 0;
}

bool unhook_method(uintptr_t record, uintptr_t replacement, uintptr_t original) {
  return is_method_record(record) && replacement && original &&
         mem::exchange_ptr(record + g.code_at, replacement, original);
}

int method_vt_index(uint32_t method_index) {
  if (!g_ready || method_index >= g.n_methods) return -1;
  const MethodView m = method_view(method_index);
  return m.ok && (m.flags & 0x40) && m.vt_index >= 0 ? m.vt_index : -1;
}

uintptr_t method_code_at(uint32_t method_index) {
  return g_ready && method_index < g.n_methods ? code_of(method_index) : 0;
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
    if (i != t && td_managed_vt(i) && mem::read_ptr(td_managed_vt(i) - 0x10) == table) ++n;
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
  const MethodView m = method_view(static_cast<uint32_t>((record - g.methods) / g.method_size));
  return (m.declaring < g.n_types ? full_name(m.declaring) : std::string("?")) + "." + (m.ok ? m.name : "?");
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
    const uintptr_t code = code_of(i);
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
