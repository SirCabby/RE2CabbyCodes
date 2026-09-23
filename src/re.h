#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

// RE Engine's own reflection, read directly: the type database (TDB v70) the
// game's managed runtime (via.clr) keeps every class, field and method in, and
// the VM's table of static field blocks. Everything the mod knows about the
// game's objects comes through here by name - classes by full name, fields by
// name up the class chain, singletons through their static `_Instance` field -
// so nothing in the mod is an absolute address or a hard-coded field offset.
//
// The TDB sits in re2.exe's .data, uninitialised on disk; the VM turns its
// array offsets into pointers as it starts (tools/tdb_dump.py reads the file's
// copy for reverse engineering). The VM itself is found the way REFramework
// finds it: the global that `mov rcx,[rip+X]; mov edx,-1; call` sequences load
// dozens of times over, whose object holds a pointer to the TDB; the static
// table sits 0x30 bytes before that pointer (TDB 70).
namespace re2cc::re {

// --- set-up (mod thread) ----------------------------------------------------------
bool init();        // false = not yet (the VM is not up); call again later
bool ready();
const char* status();
bool attach_tdb(uintptr_t tdb);  // tests: use an (initialised) database without a VM

uint32_t num_types();
uint32_t num_methods();
uintptr_t vm();  // the VM object (via.clr.VM), 0 until init() has found it

// --- types (an index into the database; 0 = none) -------------------------------
uint32_t find_type(const char* full_name);  // "app.ropeway.GameClock", "System.Collections.Generic.List`1<...>"
const std::string& full_name(uint32_t t);
uint32_t parent(uint32_t t);
bool is_value_type(uint32_t t);
bool derives(uint32_t t, uint32_t base);  // t is base or one of its subclasses
uint32_t value_size(uint32_t t);          // a value type's size in an array
uintptr_t managed_vt(uint32_t t);         // the REObjectInfo every instance of exactly t points at

// --- fields ------------------------------------------------------------------------
struct Field {
  uint32_t declaring = 0;  // the class that declares it
  uint32_t type = 0;       // the field's own type
  uint32_t offset = 0;     // from the object's field pointer, or into the static block
  uint16_t flags = 0;
  bool valid() const { return declaring != 0; }
  bool is_static() const { return (flags & 0x10) != 0; }
};
Field find_field(uint32_t t, const char* name);  // t and its parents
int enum_value(uint32_t enum_type, const char* name, int fallback);  // a literal of an enum type

// --- statics -----------------------------------------------------------------------
uintptr_t static_block(uint32_t t);    // 0 until the class's static constructor has run
uintptr_t static_addr(const Field& f); // where a static field lives (0 when not yet)
// The singleton behind `_Instance` in the class chain of t (RE2's
// RopewaySingletonBehaviorRoot`1<T>), or 0: re-read on every call.
uintptr_t singleton(uint32_t t);

// --- objects -------------------------------------------------------------------------
uint32_t type_of(uintptr_t obj);     // 0 unless obj is a managed object whose class the database knows
bool is_a(uintptr_t obj, uint32_t t);
int32_t fieldptr_offset(uintptr_t obj);  // where obj's managed fields start (-1 unknown); for the log
// The address of an instance field in obj, 0 unless obj is (a subclass of) the
// field's declaring class. Value-type fields of a struct passed by address:
// value_field_addr.
uintptr_t field_addr(uintptr_t obj, const Field& f);
inline uintptr_t value_field_addr(uintptr_t value, const Field& f) { return value ? value + f.offset : 0; }

// --- managed arrays and List<T> ----------------------------------------------------------
struct Array {
  uintptr_t obj = 0;
  int32_t count = 0;
  uintptr_t data = 0;      // element 0
  uint32_t elem_size = 8;  // 8 for references
};
bool read_array(uintptr_t arr, Array* out);
struct List {
  uintptr_t obj = 0;
  int32_t count = 0;   // mSize
  Array items;         // mItems (capacity >= count)
};
bool read_list(uintptr_t list, List* out);
uintptr_t list_ref(const List& l, int i);  // reference element i (0 when out of range or null)

// --- methods (reverse engineering and code-site discovery) -----------------------------
uintptr_t method_code(uint32_t t, const char* name, int nparams = -1);  // compiled code of a method of t
// Exactly one overload: the parameter types by full name, and whether it is
// static. 0 unless a method of t matches all of it. find_method gives its index
// in the database (the tests use it: the code only exists at run time).
uint32_t find_method(uint32_t t, const char* name, std::initializer_list<const char*> param_types, bool is_static = false);
uintptr_t method_code_typed(uint32_t t, const char* name, std::initializer_list<const char*> param_types,
                            bool is_static = false);
uintptr_t method_record(uint32_t t, const char* name, int nparams = -1);  // the method's 16-byte entry in the database
std::string method_at(uintptr_t record);  // "Declaring.Name" when `record` is a method entry of the database, else ""
uintptr_t method_record_at(uint32_t index);  // the entry of method `index` (find_method's), 0 when out of range
bool dump_methods(const char* path);  // every method's code as an RVA, for tools/tdb_dump.py --methods

// --- method hooks: the game calls the mod ------------------------------------------------------
// A method's entry holds its compiled code, and that is what the game copies
// into every delegate it creates for the method - C# event handlers and
// callbacks: the runtime's new-delegate helper stores the entry and its code
// into the delegate - and what reflection calls. Pointing the entry at a
// replacement before the game creates those delegates makes the game call the
// mod at exactly those moments. No code is patched, and direct calls between
// compiled methods are not affected. The replacement has the managed
// convention - ret f(VMContext* ctx, this, args...) - and forwards to the code
// hook_method returns (0: the entry holds no game code, or is not writable).
uintptr_t hook_method(uintptr_t record, uintptr_t replacement, unsigned long* protect = nullptr);  // protect: the page's, for the log
bool unhook_method(uintptr_t record, uintptr_t replacement, uintptr_t original);  // only if it still holds ours
int globals_pointing_at(uintptr_t record);  // the log: .data globals holding the entry's address (-1 unknown)

// --- virtual methods: vtable slots -----------------------------------------------------------------
// A virtual call reads the vtable pointer stored just before the object's
// REObjectInfo and calls its slot at the method's vt index (`mov r8,[info-0x10];
// call [r8+vt*8]`); an interface call reads a table further back
// (`[info-(3+n)*8]`). Both are read at every call, so exchanging a slot takes
// effect at once for every instance of the class - and of any class sharing the
// table, so a replacement checks the class of `this`.
int method_vt_index(uint32_t method_index);  // -1 unless virtual
uintptr_t method_code_at(uint32_t method_index);  // the code its database entry holds now (0: none)
uintptr_t vtable_of(uint32_t t);  // 0 until t has its REObjectInfo
// The slot of a virtual method in t's vtable - 0 unless it holds exactly `code`.
uintptr_t vtable_slot(uint32_t t, uint32_t method_index, uintptr_t code);
// Every slot among t's first `tables` interface tables (their first 64 entries)
// holding `code`; returns how many were found.
int interface_slots(uint32_t t, uintptr_t code, uintptr_t* out, int max, int tables = 8);
bool swap_slot(uintptr_t slot, uintptr_t expected, uintptr_t replacement);  // only while it holds `expected`
int classes_sharing_vtable(uint32_t t);  // the log: other classes calling through t's vtable (-1 unknown)

}  // namespace re2cc::re
