#pragma once

#include <cstddef>
#include <cstdint>

// Runs a compiled switch for one argument without calling it. Some of the game's
// rules are code, not data: `ItemManager.getItemMultipleUseMax(id)` is C#'s
// `switch (id) { case sm70_100: return 60; ... default: return 1; }`, compiled
// into compares, bounds checks, jump tables and `mov eax,imm32; ret`. This
// simulates such a method instruction by instruction for a given argument and
// gives back the constant it returns.
//
// Only what those switches are made of is known: moves and compares between
// registers and constants, lea, sign extension, register arithmetic, 32- and
// 64-bit conditional and unconditional jumps, a register jump, reads of jump
// tables, ret. Anything else - a call, a store, a read outside `in_data`, a jump
// outside `in_code`, a register the method never set, flags the last
// instruction did not leave comparable - makes the answer unknown (false),
// never a guess. Reads go through the caller, so the same code runs on the
// game's live image (game.cpp) and on re2.exe read from disk (tests/test_switch.cpp).
namespace re2cc::code {

// read(address, uint8_t* out, size_t n) -> bool
// in_code(address) -> bool: an instruction may be there
// in_data(address) -> bool: a table may be read there
// arg_reg: the register holding the argument (0 rax, 1 rcx, 2 rdx, ... 8 r8; a
// managed instance method's first argument is r8, after the context and `this`;
// a static method's is rdx, after the context). arg2_reg / arg2: a second
// argument, for a method of two (-1: none).
template <typename Read, typename InCode, typename InData>
bool eval_switch(uintptr_t fn, int arg_reg, uint64_t arg, Read&& read, InCode&& in_code, InData&& in_data,
                 int32_t* out, int arg2_reg = -1, uint64_t arg2 = 0) {
  uint64_t reg[16] = {};
  bool known[16] = {};
  if (arg_reg < 0 || arg_reg > 15 || arg2_reg > 15 || arg2_reg == arg_reg) return false;
  reg[arg_reg] = arg;
  known[arg_reg] = true;
  if (arg2_reg >= 0) {
    reg[arg2_reg] = arg2;
    known[arg2_reg] = true;
  }
  // The last compare as `a - b` at a width; `test a,b` and `xor` compare (a & b) or the result with 0.
  struct {
    bool known = false;
    uint64_t a = 0, b = 0;
    uint64_t mask = 0;
  } fl;
  auto sext = [](uint64_t v, uint64_t mask) -> int64_t {
    const uint64_t sign = (mask >> 1) + 1;
    v &= mask;
    return static_cast<int64_t>((v ^ sign) - sign);
  };
  auto rd = [&](uintptr_t at, size_t n, uint64_t* v) -> bool {
    uint8_t b[8] = {};
    for (size_t k = 0; k < n; ++k)
      if (!in_data(at + k)) return false;
    if (!read(at, b, n)) return false;
    *v = 0;
    for (size_t k = 0; k < n; ++k) *v |= static_cast<uint64_t>(b[k]) << (8 * k);
    return true;
  };

  uintptr_t pc = fn;
  for (int steps = 0; steps < 256; ++steps) {
    if (!in_code(pc) || !in_code(pc + 14)) return false;
    uint8_t b[15];
    if (!read(pc, b, sizeof(b))) return false;
    size_t i = 0;
    uint8_t rex = 0;
    if ((b[i] & 0xF0) == 0x40) rex = b[i++];
    const bool W = rex & 8;
    const int R = (rex & 4) ? 8 : 0, X = (rex & 2) ? 8 : 0, B = (rex & 1) ? 8 : 0;
    const uint64_t width = W ? ~0ull : 0xFFFFFFFFull;
    auto imm32 = [&](size_t at) -> int32_t {
      return static_cast<int32_t>(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24));
    };
    auto set = [&](int r, uint64_t v) {
      reg[r] = v & width;
      known[r] = true;
    };
    // A memory operand (mod != 3): its address, all of its registers known.
    // Leaves i past the operand; rip-relative is counted from there (no immediate follows here).
    auto mem_addr = [&](uint8_t modrm, uint64_t* addr) -> bool {
      const int mod = modrm >> 6, rm = modrm & 7;
      int64_t disp = 0;
      uint64_t a = 0;
      bool rip = false;
      if (rm == 4) {
        const uint8_t sib = b[i++];
        const int scale = 1 << (sib >> 6), index = ((sib >> 3) & 7) | X, base = (sib & 7) | B;
        if (index != 4) {
          if (!known[index]) return false;
          a += reg[index] * static_cast<uint64_t>(scale);
        }
        if ((sib & 7) == 5 && mod == 0) {
          disp = imm32(i);
          i += 4;
        } else {
          if (!known[base]) return false;
          a += reg[base];
        }
      } else if (mod == 0 && rm == 5) {
        disp = imm32(i);
        i += 4;
        rip = true;
      } else {
        const int base = rm | B;
        if (!known[base]) return false;
        a += reg[base];
      }
      if (mod == 1) disp = static_cast<int8_t>(b[i++]);
      else if (mod == 2) {
        disp = imm32(i);
        i += 4;
      }
      *addr = (rip ? pc + i : a) + static_cast<uint64_t>(disp);
      return true;
    };
    auto jump = [&](int cc, int64_t rel, size_t len) -> bool {
      if (!fl.known) return false;
      const uint64_t a = fl.a & fl.mask, c = fl.b & fl.mask;
      const int64_t sa = sext(a, fl.mask), sc = sext(c, fl.mask);
      bool take;
      switch (cc) {
        case 0x2: take = a < c; break;
        case 0x3: take = a >= c; break;
        case 0x4: take = a == c; break;
        case 0x5: take = a != c; break;
        case 0x6: take = a <= c; break;
        case 0x7: take = a > c; break;
        case 0x8: take = sext(a - c, fl.mask) < 0; break;
        case 0x9: take = sext(a - c, fl.mask) >= 0; break;
        case 0xC: take = sa < sc; break;
        case 0xD: take = sa >= sc; break;
        case 0xE: take = sa <= sc; break;
        case 0xF: take = sa > sc; break;
        default: return false;  // overflow and parity: not what a switch tests
      }
      pc += len;
      if (take) pc += static_cast<uint64_t>(rel);
      return true;
    };
    const uint8_t op = b[i++];
    if (op == 0x90) {
      pc += i;
    } else if (op == 0xC3) {
      if (!known[0]) return false;
      *out = static_cast<int32_t>(reg[0]);
      return true;
    } else if (op >= 0xB8 && op <= 0xBF) {  // mov r32, imm32 / mov r64, imm64
      const int r = (op - 0xB8) | B;
      if (W) {
        uint64_t v = 0;
        for (int k = 7; k >= 0; --k) v = (v << 8) | b[i + k];
        set(r, v);
        i += 8;
      } else {
        set(r, static_cast<uint32_t>(imm32(i)));
        i += 4;
      }
      pc += i;
    } else if (op == 0x3D) {  // cmp eax, imm32
      if (!known[0]) return false;
      fl = {true, reg[0], static_cast<uint64_t>(static_cast<int64_t>(imm32(i))), width};
      pc += i + 4;
    } else if (op == 0x81 || op == 0x83) {  // add / sub / cmp r, imm
      const uint8_t modrm = b[i++];
      if (modrm >> 6 != 3) return false;
      const int r = (modrm & 7) | B, ext = (modrm >> 3) & 7;
      int64_t imm;
      if (op == 0x83) imm = static_cast<int8_t>(b[i++]);
      else {
        imm = imm32(i);
        i += 4;
      }
      if (!known[r]) return false;
      if (ext == 7) {
        fl = {true, reg[r], static_cast<uint64_t>(imm), width};
      } else if (ext == 5) {
        fl = {true, reg[r], static_cast<uint64_t>(imm), width};
        set(r, reg[r] - static_cast<uint64_t>(imm));
      } else if (ext == 0) {
        fl.known = false;
        set(r, reg[r] + static_cast<uint64_t>(imm));
      } else {
        return false;
      }
      pc += i;
    } else if (op == 0x39 || op == 0x3B || op == 0x85 || op == 0x31 || op == 0x33 || op == 0x01 || op == 0x03 ||
               op == 0x89 || op == 0x8B) {
      const uint8_t modrm = b[i++];
      const int g = ((modrm >> 3) & 7) | R;
      if (modrm >> 6 != 3) {
        if (op != 0x8B) return false;  // only a table read: mov r, [mem]
        uint64_t at = 0, v = 0;
        if (!mem_addr(modrm, &at) || !rd(at, W ? 8 : 4, &v)) return false;
        set(g, v);
        pc += i;
        continue;
      }
      const int e = (modrm & 7) | B;
      // The destination is r/m for 01/31/39/85/89, the register for 03/33/3B/8B.
      const bool to_reg = op == 0x03 || op == 0x33 || op == 0x3B || op == 0x8B;
      const int dst = to_reg ? g : e, src = to_reg ? e : g;
      if ((op == 0x31 || op == 0x33) && dst == src) {  // xor r, r
        set(dst, 0);
        fl = {true, 0, 0, width};
      } else if (!known[src] || (op != 0x89 && op != 0x8B && !known[dst])) {
        return false;
      } else if (op == 0x39 || op == 0x3B) {
        fl = {true, reg[dst], reg[src], width};
      } else if (op == 0x85) {
        fl = {true, reg[dst] & reg[src], 0, width};
      } else if (op == 0x31 || op == 0x33) {
        set(dst, reg[dst] ^ reg[src]);
        fl = {true, reg[dst], 0, width};
      } else if (op == 0x01 || op == 0x03) {
        set(dst, reg[dst] + reg[src]);
        fl.known = false;
      } else {
        set(dst, reg[src]);
      }
      pc += i;
    } else if (op == 0x8D) {  // lea r, [mem]
      const uint8_t modrm = b[i++];
      uint64_t at = 0;
      if (modrm >> 6 == 3 || !mem_addr(modrm, &at)) return false;
      set(((modrm >> 3) & 7) | R, at);
      pc += i;
    } else if (op == 0x63 && W) {  // movsxd r64, r/m32
      const uint8_t modrm = b[i++];
      const int src = (modrm & 7) | B;
      if (modrm >> 6 != 3 || !known[src]) return false;
      set(((modrm >> 3) & 7) | R, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(reg[src]))));
      pc += i;
    } else if (op == 0x98 && W) {  // cdqe
      if (!known[0]) return false;
      set(0, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(reg[0]))));
      pc += i;
    } else if (op == 0x0F && (b[i] == 0xB6 || b[i] == 0xB7)) {  // movzx r, byte/word [mem]
      const size_t n = b[i++] == 0xB6 ? 1 : 2;
      const uint8_t modrm = b[i++];
      uint64_t at = 0, v = 0;
      if (modrm >> 6 == 3 || !mem_addr(modrm, &at) || !rd(at, n, &v)) return false;
      set(((modrm >> 3) & 7) | R, v);
      pc += i;
    } else if (op == 0x0F && b[i] >= 0x80 && b[i] <= 0x8F) {  // jcc rel32
      const int cc = b[i++] & 0xF;
      if (!jump(cc, imm32(i), i + 4)) return false;
    } else if (op >= 0x70 && op <= 0x7F) {  // jcc rel8
      if (!jump(op & 0xF, static_cast<int8_t>(b[i]), i + 1)) return false;
    } else if (op == 0xEB) {
      pc += i + 1 + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(b[i])));
    } else if (op == 0xE9) {
      pc += i + 4 + static_cast<uint64_t>(static_cast<int64_t>(imm32(i)));
    } else if (op == 0xFF) {  // inc / dec / jmp r64
      const uint8_t modrm = b[i++];
      const int r = (modrm & 7) | B, ext = (modrm >> 3) & 7;
      if (modrm >> 6 != 3 || !known[r]) return false;
      if (ext == 4) {
        pc = reg[r];
      } else if (ext == 0 || ext == 1) {
        set(r, ext == 0 ? reg[r] + 1 : reg[r] - 1);
        fl.known = false;
        pc += i;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return false;
}

}  // namespace re2cc::code
