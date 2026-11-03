// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing.h"
#include "video_core/macro_turing_jit.h"
#include "video_core/nx2_front_same.h"

// EDEN_NX2_JIT_SAME: the macro JIT's same value filter (nx2_front_same.h) against the JIT without
// it, the interpreter and a model of Maxwell3D::FrontWrite, on synthetic MME programs written by
// hand from the instruction fields (no game code).
namespace {

using Word = std::uint32_t;

enum Op : Word {
    Add = 0, AddC = 1, Sub = 2, SubB = 3, Mul = 4, MulH = 5, MulU = 6, Ext = 7, Clz = 8, Sll = 9,
    Srl = 10, Sra = 11, And = 12, Nand = 13, Or = 14, Xor = 15, Merge = 16, Slt = 17, Sltu = 18,
    Sle = 19, Sleu = 20, Seq = 21, State = 22, Loop = 23, Jal = 24, Blt = 25, Bltu = 26, Ble = 27,
    Bleu = 28, Beq = 29, DRead = 30, DWrite = 31,
};
// Sources: 0-23 registers, then these.
constexpr Word Zero = 24, ImmS = 25, ImmO = 26, Imm32 = 27, Load0 = 28, Load1 = 29;
// Output selectors.
constexpr Word OutAlu0 = 1, OutAlu1 = 2, OutLoad0 = 3, OutLoad1 = 4, OutImm0 = 5, OutImm1 = 6,
               OutImm0High = 8, OutImm1High = 9, OutImm32 = 10;

struct LaneFields {
    Word op = Add, dst = Zero, a = Zero, b = Zero, imm = 0;
};

struct Ins {
    bool end = false;
    Word pred_mode = 0, pred_reg = 0;
    std::array<LaneFields, 2> lane{};
    std::array<Word, 2> method{}, emit{};

    Ins& End() { end = true; return *this; }
    Ins& Pred(Word mode, Word reg) { pred_mode = mode; pred_reg = reg; return *this; }
    Ins& Alu(Word l, Word op, Word dst, Word a, Word b, Word imm = 0) {
        lane[l] = {op, dst, a, b, imm};
        return *this;
    }
    Ins& Imm(Word l, Word imm) { lane[l].imm = imm; return *this; }
    Ins& Method(Word l, Word selector) { method[l] = selector; return *this; }
    Ins& Emit(Word l, Word selector) { emit[l] = selector; return *this; }
};

constexpr Word M(Word method, Word increment) {
    return (increment << 12) | method;
}

constexpr Word BranchImm(int offset, bool when, bool cancel_end) {
    return (when ? 0x8000U : 0U) | (cancel_end ? 0x4000U : 0U) | (static_cast<Word>(offset) & 0x1fffU);
}

std::array<Word, 3> Encode(const Ins& ins) {
    std::array<Word, 3> bits{};
    const auto field = [&](Word offset, Word count, Word value) {
        for (Word bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1U) << ((offset + bit) % 32);
        }
    };
    field(0, 1, ins.end ? 1U : 0U);
    field(1, 4, ins.pred_mode);
    field(5, 5, ins.pred_reg);
    for (Word l = 0; l < 2; ++l) {
        const Word base = 10 + 36 * l;
        field(base, 5, ins.lane[l].op);
        field(base + 5, 5, ins.lane[l].dst);
        field(base + 10, 5, ins.lane[l].a);
        field(base + 15, 5, ins.lane[l].b);
        field(base + 20, 16, ins.lane[l].imm);
    }
    field(82, 3, ins.method[0]);
    field(85, 4, ins.emit[0]);
    field(89, 3, ins.method[1]);
    field(92, 4, ins.emit[1]);
    return {bits[2], bits[1], bits[0]};
}

/// How a program's parameters are drawn.
enum class Params {
    Values,   ///< Values from a small pool (same value writes are common)
    Mode,     ///< Positions 0 and 3: shadow RAM modes
    ModeLane, ///< Position 1: a shadow RAM mode
    Methods,  ///< Position 0: a method word (method | increment << 12)
    Dma,      ///< Positions 0 and 1: DMA read counts
};

struct Program {
    const char* name;
    std::vector<Ins> code;
    Word start{};
    Params params{Params::Values};
};

std::vector<Word> Assemble(const Program& program) {
    std::vector<Word> words;
    for (const Ins& ins : program.code) {
        const auto encoded = Encode(ins);
        words.insert(words.end(), encoded.begin(), encoded.end());
    }
    return words;
}

std::vector<Program> Programs() {
    std::vector<Program> p;
    const auto emits = [](Word method, Word increment, Word count) {
        std::vector<Ins> code;
        for (Word i = 0; i < count; ++i) {
            Ins ins;
            if (i == 0) {
                ins.Imm(0, M(method, increment)).Method(0, OutImm0);
            }
            ins.Emit(0, OutLoad0);
            if (i + 2 == count) {
                ins.End();
            }
            code.push_back(ins);
        }
        return code;
    };
    // Constant emits (0x5A5A) from `method` on: the bound programs.
    const auto constants = [](Word method, Word increment, Word count) {
        std::vector<Ins> code;
        for (Word i = 0; i < count; ++i) {
            Ins ins;
            if (i == 0) {
                ins.Imm(0, M(method, increment)).Method(0, OutImm0);
            }
            ins.Imm(1, 0x5A5A).Emit(1, OutImm1);
            if (i + 2 == count) {
                ins.End();
            }
            code.push_back(ins);
        }
        return code;
    };
    p.push_back({"inc1", emits(0x300, 1, 8)});
    p.push_back({"inc0", emits(0x310, 0, 6)});
    p.push_back({"inc15", emits(0x200, 15, 5)});
    p.push_back({"two lanes inc4",
                 {Ins{}.Imm(0, M(0x380, 4)).Method(0, OutImm0).Emit(0, OutLoad0).Emit(1, OutLoad1),
                  Ins{}.Emit(0, OutLoad0).Emit(1, OutLoad1),
                  Ins{}.Emit(0, OutLoad0).Emit(1, OutLoad1).End(), Ins{}.Emit(0, OutLoad0)}});
    p.push_back({"bound inc1", constants(0xDFC, 1, 7)});
    p.push_back({"bound inc15", constants(0xDF1, 15, 6)});
    p.push_back({"bound past 0xfff", constants(0xFF8, 15, 6)});
    p.push_back({"bound 0xdff inc0", constants(0xDFF, 0, 4)});
    p.push_back({"loop",
                 {Ins{}.Alu(0, And, 1, Load0, ImmS, 3).Imm(1, M(0x320, 1)).Method(1, OutImm1),
                  Ins{}.Alu(0, Loop, Zero, 1, Zero, 3), Ins{}.Emit(0, OutLoad0),
                  Ins{}.Emit(0, OutLoad0), Ins{}.End(), Ins{}}});
    p.push_back({"branch",
                 {Ins{}.Alu(0, Add, 1, Load0, Zero), Ins{}.Alu(0, Beq, Zero, 1, Zero, BranchImm(3, true, false)),
                  Ins{}.Imm(0, M(0x360, 1)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Alu(0, Jal, Zero, Zero, Zero, 3),
                  Ins{}.Imm(0, M(0x368, 1)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Emit(0, OutLoad0), Ins{}.Emit(0, OutLoad0).End(), Ins{}}});
    p.push_back({"branch back",
                 {Ins{}.Alu(0, And, 1, Load0, ImmS, 3), Ins{}.Imm(0, M(0x370, 1)).Method(0, OutImm0),
                  Ins{}.Alu(0, Beq, Zero, 1, Zero, BranchImm(4, true, false)),
                  Ins{}.Alu(0, Sub, 1, 1, ImmS, 1).Emit(0, OutAlu0),
                  Ins{}.Alu(0, Beq, Zero, Zero, Zero, BranchImm(-2, true, false)), Ins{}, Ins{}.End(),
                  Ins{}}});
    p.push_back({"end delay",
                 {Ins{}.Imm(0, M(0x390, 1)).Method(0, OutImm0).Emit(0, OutLoad0).End(),
                  Ins{}.Emit(0, OutLoad0), Ins{}.Emit(0, OutLoad0)}});
    p.push_back({"end cancel",
                 {Ins{}.Alu(0, Add, 1, Load0, Zero), Ins{}.Imm(0, M(0x398, 1)).Method(0, OutImm0),
                  Ins{}.Alu(0, Beq, Zero, 1, Zero, BranchImm(2, true, true)).End(),
                  Ins{}.Emit(0, OutLoad0), Ins{}.Emit(0, OutLoad0).End(), Ins{}.Emit(0, OutLoad0)}});
    p.push_back({"loop end at end",
                 {Ins{}.Alu(0, And, 1, Load0, ImmS, 3).Imm(1, M(0x3a0, 1)).Method(1, OutImm1),
                  Ins{}.Alu(0, Loop, Zero, 1, Zero, 3), Ins{}.Emit(0, OutLoad0),
                  Ins{}.Emit(0, OutLoad0).End(), Ins{}.Emit(0, OutLoad0), Ins{}.Emit(0, OutLoad0)}});
    p.push_back({"stop",
                 {Ins{}.Imm(0, M(0x3b0, 1)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Alu(0, Beq, Zero, Zero, Zero, BranchImm(-4096, true, false)),
                  Ins{}.Emit(0, OutLoad0), Ins{}.End(), Ins{}}});
    // 0x154-0x156 (source, data RAM index), then MME_DMA_READ_FIFOED with a count from the
    // parameters and a loop over the words it appended.
    p.push_back({"dma fifoed",
                 {Ins{}.Imm(0, M(0x154, 1)).Method(0, OutImm0).Imm(1, 0).Emit(1, OutImm1),
                  Ins{}.Imm(1, 0x1000).Emit(1, OutImm1), Ins{}.Imm(1, 0).Emit(1, OutImm1),
                  Ins{}.Imm(0, M(0x158, 0)).Method(0, OutImm0).Emit(0, OutLoad0).Alu(1, And, 1, Load0, ImmS, 7),
                  Ins{}.Alu(0, Loop, Zero, 1, Zero, 2).Imm(1, M(0x3c0, 1)).Method(1, OutImm1),
                  Ins{}.Emit(0, OutLoad0), Ins{}.End(), Ins{}},
                 0, Params::Dma});
    p.push_back({"dma ram",
                 {Ins{}.Imm(0, M(0x154, 1)).Method(0, OutImm0).Imm(1, 0).Emit(1, OutImm1),
                  Ins{}.Imm(1, 0x2000).Emit(1, OutImm1), Ins{}.Imm(1, 0x10).Emit(1, OutImm1),
                  Ins{}.Emit(0, OutLoad0),
                  Ins{}.Alu(0, DRead, 1, ImmS, Zero, 0x10).Imm(1, M(0x3d0, 1)).Method(1, OutImm1).Emit(1, OutAlu0),
                  Ins{}.Alu(0, DRead, 1, ImmS, Zero, 0x11).Emit(1, OutAlu0).End(),
                  Ins{}.Alu(0, DRead, 1, ImmS, Zero, 0x12).Emit(1, OutAlu0)},
                 0, Params::Dma});
    p.push_back({"macro target",
                 {Ins{}.Imm(0, M(0xE10, 1)).Method(0, OutImm0).Emit(0, OutLoad0).End(), Ins{}}});
    p.push_back({"mode switch",
                 {Ins{}.Imm(0, M(0x49, 0)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Imm(0, M(0x400, 1)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Emit(0, OutLoad0),
                  Ins{}.Imm(0, M(0x49, 0)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Imm(0, M(0x400, 1)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Emit(0, OutLoad0).End(), Ins{}.Emit(0, OutLoad0)},
                 0, Params::Mode});
    // The mode written by one lane, the next lane's write in the same instruction under it.
    p.push_back({"mode switch lanes",
                 {Ins{}.Imm(0, M(0x48, 1)).Method(0, OutImm0).Emit(0, OutLoad0).Emit(1, OutLoad1),
                  Ins{}.Emit(0, OutLoad0).Emit(1, OutLoad1),
                  Ins{}.Imm(0, M(0x47, 1)).Method(0, OutImm0).Emit(0, OutLoad0).Emit(1, OutLoad1).End(),
                  Ins{}.Emit(0, OutLoad0)},
                 0, Params::ModeLane});
    p.push_back({"mme read",
                 {Ins{}.Alu(0, State, 1, ImmS, Zero, 0x420).Alu(1, State, 2, ImmS, Zero, 0x421),
                  Ins{}.Alu(0, Add, Zero, 1, Zero).Alu(1, Add, Zero, 2, Zero).Imm(0, M(0x420, 1))
                      .Method(0, OutImm0).Emit(0, OutAlu0).Emit(1, OutAlu1).End(),
                  Ins{}}});
    p.push_back({"predicated",
                 {Ins{}.Alu(0, And, 1, Load0, ImmS, 1),
                  Ins{}.Pred(1, 1).Imm(0, M(0x430, 1)).Method(0, OutImm0).Emit(0, OutLoad0),
                  Ins{}.Pred(11, 1).Imm(0, M(0x438, 1)).Method(0, OutImm0).Emit(0, OutLoad0).Emit(1, OutLoad1),
                  Ins{}.Pred(4, 1).Imm(0, M(0x43c, 1)).Method(0, OutImm0).Emit(0, OutLoad0).End(),
                  Ins{}.Emit(0, OutLoad0)}});
    p.push_back({"dwrite dread",
                 {Ins{}.Alu(0, DWrite, Zero, ImmS, Load0, 0x20).Alu(1, DWrite, Zero, ImmS, Load1, 0x21),
                  Ins{}.Alu(0, DRead, 1, ImmS, Zero, 0x20).Alu(1, DRead, 2, ImmS, Zero, 0x21),
                  Ins{}.Alu(0, Add, Zero, 1, Zero).Alu(1, Add, Zero, 2, Zero).Imm(0, M(0x440, 1))
                      .Method(0, OutImm0).Emit(0, OutAlu0).Emit(1, OutAlu1).End(),
                  Ins{}}});
    p.push_back({"computed method",
                 {Ins{}.Alu(0, Add, 1, Load0, Zero).Method(0, OutAlu0), Ins{}.Emit(0, OutLoad0),
                  Ins{}.Emit(0, OutLoad0).End(), Ins{}.Emit(0, OutLoad0)},
                 0, Params::Methods});
    p.push_back({"imm emits",
                 {Ins{}.Imm(0, M(0x450, 1)).Method(0, OutImm0).Imm(1, 3).Emit(0, OutImm0High).Emit(1, OutImm32),
                  Ins{}.Imm(1, 0x2000).Emit(0, OutImm1High).End(), Ins{}.Imm(1, 7).Emit(1, OutImm1)}});
    p.push_back({"arith",
                 {Ins{}.Alu(0, Add, 1, Load0, Load1).Alu(1, AddC, 2, Load0, Load1),
                  Ins{}.Alu(0, Mul, 3, 1, 2).Alu(1, MulH, 4, Zero, Zero),
                  Ins{}.Alu(0, Clz, 5, 3, Zero).Alu(1, Sll, 6, 1, ImmS, 3),
                  Ins{}.Alu(0, Merge, 7, 1, 2, (4U << 10) | (8U << 5) | 2U).Alu(1, Slt, 8, 1, 2),
                  Ins{}.Alu(0, Add, Zero, 3, Zero).Alu(1, Add, Zero, 4, Zero).Imm(0, M(0x460, 1))
                      .Method(0, OutImm0).Emit(0, OutAlu0).Emit(1, OutAlu1),
                  Ins{}.Alu(0, Add, Zero, 5, Zero).Alu(1, Add, Zero, 6, Zero).Emit(0, OutAlu0).Emit(1, OutAlu1),
                  Ins{}.Alu(0, Add, Zero, 7, Zero).Alu(1, Add, Zero, 8, Zero).Emit(0, OutAlu0)
                      .Emit(1, OutAlu1).End(),
                  Ins{}}});
    p.push_back({"arith 2",
                 {Ins{}.Alu(0, Sub, 1, Load0, Load1).Alu(1, SubB, 2, Load0, Load1),
                  Ins{}.Alu(0, MulU, 3, 1, 2).Alu(1, Srl, 4, 1, ImmS, 5),
                  Ins{}.Alu(0, Sra, 5, 1, ImmS, 7).Alu(1, Nand, 6, 1, 2),
                  Ins{}.Alu(0, Or, 7, 1, 2).Alu(1, Xor, 8, 1, 2),
                  Ins{}.Alu(0, Sltu, 9, 1, 2).Alu(1, Sle, 10, 1, 2),
                  Ins{}.Alu(0, Sleu, 11, 1, 2).Alu(1, Seq, 12, 1, 2),
                  Ins{}.Alu(0, Ext, Zero, ImmS, Zero, 0x1000).Alu(1, And, 13, 1, ImmO, 0xff),
                  Ins{}.Alu(0, Add, Zero, 3, Zero).Alu(1, Add, Zero, 4, Zero).Imm(0, M(0x470, 1))
                      .Method(0, OutImm0).Emit(0, OutAlu0).Emit(1, OutAlu1),
                  Ins{}.Alu(0, Add, Zero, 5, Zero).Alu(1, Add, Zero, 6, Zero).Emit(0, OutAlu0).Emit(1, OutAlu1),
                  Ins{}.Alu(0, Add, Zero, 7, Zero).Alu(1, Add, Zero, 8, Zero).Emit(0, OutAlu0).Emit(1, OutAlu1),
                  Ins{}.Alu(0, Add, Zero, 9, Zero).Alu(1, Add, Zero, 10, Zero).Emit(0, OutAlu0).Emit(1, OutAlu1),
                  Ins{}.Alu(0, Add, Zero, 11, Zero).Alu(1, Add, Zero, 12, Zero).Emit(0, OutAlu0)
                      .Emit(1, OutAlu1).End(),
                  Ins{}.Alu(0, Add, Zero, 13, Zero).Emit(0, OutAlu0)}});
    p.push_back({"early operands",
                 {Ins{}.Alu(0, Add, 1, Load0, Zero).Alu(1, Add, 2, Load1, Zero),
                  Ins{}.Alu(0, Add, 1, 1, ImmS, 1).Alu(1, Add, 3, 1, 2),
                  Ins{}.Alu(0, Add, Zero, 1, Zero).Alu(1, Add, Zero, 3, Zero).Imm(0, M(0x478, 1))
                      .Method(0, OutImm0).Emit(0, OutAlu0).Emit(1, OutAlu1).End(),
                  Ins{}}});
    p.push_back({"start 2", {Ins{}.Emit(0, OutLoad0), Ins{}.End(), Ins{}.Imm(0, M(0x480, 1)).Method(0, OutImm0).Emit(0, OutLoad0).End(), Ins{}.Emit(0, OutLoad0)}, 2});
    // Errors.
    p.push_back({"error source", {Ins{}.Alu(0, Add, 1, 30, Zero).End(), Ins{}}});
    p.push_back({"error destination", {Ins{}.Alu(0, Add, 25, Zero, Zero).End(), Ins{}}});
    p.push_back({"error output", {Ins{}.Imm(0, 0x500).Method(0, 7).End(), Ins{}}});
    p.push_back({"error emit output", {Ins{}.Imm(0, M(0x500, 1)).Method(0, OutImm0).Emit(0, 12).End(), Ins{}}});
    p.push_back({"error loop", {Ins{}.Alu(0, Loop, Zero, Zero, Zero, 1).End(), Ins{}}});
    p.push_back({"error dread", {Ins{}.Alu(0, DRead, 1, ImmS, Zero, 0x7fff).End(), Ins{}}});
    p.push_back({"error dwrite", {Ins{}.Alu(0, DWrite, Zero, ImmS, Zero, 0x0c00).End(), Ins{}}});
    p.push_back({"error extended", {Ins{}.Alu(0, Ext, Zero, ImmS, Zero, 5).End(), Ins{}}});
    p.push_back({"error merge", {Ins{}.Alu(0, Merge, 1, Zero, Zero, 40U << 10).End(), Ins{}}});
    p.push_back({"error outside", {Ins{}.Alu(0, Jal, Zero, Zero, Zero, 10), Ins{}}});
    p.push_back({"error fall through", {Ins{}.Imm(0, M(0x488, 1)).Method(0, OutImm0).Emit(0, OutLoad0), Ins{}}});
    p.push_back({"error start", {Ins{}.End(), Ins{}}, 100});
    return p;
}

namespace Same = VideoCommon::Nx2FrontSame;
using Jit = Tegra::TuringMacroJit;

/// The compiled code's size and FNV-1a 64 digest, its jump table (the last 8 bytes per
/// instruction, absolute addresses) taken relative to the code's start.
std::pair<std::size_t, std::uint64_t> Digest(const Jit& jit, std::size_t instructions) {
    const auto code = jit.Code();
    const std::size_t table = code.size() - instructions * 8;
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&](std::uint8_t byte) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    };
    for (std::size_t i = 0; i < table; ++i) {
        mix(code[i]);
    }
    const auto base = reinterpret_cast<std::uintptr_t>(code.data());
    for (std::size_t i = 0; i < instructions; ++i) {
        std::uint64_t entry;
        std::memcpy(&entry, code.data() + table + i * 8, sizeof(entry));
        entry -= base;
        for (int b = 0; b < 8; ++b) {
            mix(static_cast<std::uint8_t>(entry >> (8 * b)));
        }
    }
    return {code.size(), hash};
}

struct Golden {
    const char* name;
    std::size_t size;
    std::uint64_t digest;
};

// The programs compiled by macro_turing_jit.h as it was before the filter (Windows x64, its
// argument registers; Xbyak 7.40.1).
constexpr std::array<Golden, 41> golden{{
    {"inc1", 1304, 0x71a49d4c2af11247ULL},
    {"inc0", 1040, 0x9b17f784e472020cULL},
    {"inc15", 912, 0x0e8be098d2c93075ULL},
    {"two lanes inc4", 976, 0x9e9911ae903541c3ULL},
    {"bound inc1", 952, 0xbd17e8dcccacacfaULL},
    {"bound inc15", 848, 0x391eed3146ce5375ULL},
    {"bound past 0xfff", 848, 0x96c6669792c31d6cULL},
    {"bound 0xdff inc0", 656, 0xef407885d18ef934ULL},
    {"loop", 904, 0x150329a7eada967eULL},
    {"branch", 1192, 0xee7ecc4bac35cb06ULL},
    {"branch back", 952, 0x53735cbbcda79bdeULL},
    {"end delay", 648, 0xede6fd07b491df91ULL},
    {"end cancel", 976, 0x0c8432049133db3dULL},
    {"loop end at end", 1048, 0xff9693ab4bdf61d0ULL},
    {"stop", 760, 0x5d12a595cfcc6267ULL},
    {"dma fifoed", 1152, 0x19155a6ca2ce0c07ULL},
    {"dma ram", 1040, 0x210b8eae0a17abaeULL},
    {"macro target", 456, 0xb4c04d73847fbd44ULL},
    {"mode switch", 1248, 0xba64d158f08cf1fbULL},
    {"mode switch lanes", 1000, 0x04e535579affcd1dULL},
    {"mme read", 560, 0x8576f303d8724b45ULL},
    {"predicated", 1152, 0x059cc7b827864395ULL},
    {"dwrite dread", 696, 0xa03f65fa92e2f184ULL},
    {"computed method", 752, 0x902825674cfb6439ULL},
    {"imm emits", 584, 0x09347e5b10d28b70ULL},
    {"arith", 1056, 0x0309480265d8b275ULL},
    {"arith 2", 1512, 0xfa852bbe88106a47ULL},
    {"early operands", 696, 0x5a39e553c23cb8f5ULL},
    {"start 2", 736, 0x81aee5780177268dULL},
    {"error source", 368, 0x607a26454309ade8ULL},
    {"error destination", 368, 0x66071a3d6b633fb9ULL},
    {"error output", 384, 0xdcfdf11ed7af299dULL},
    {"error emit output", 424, 0xb78ebb8eeb15ae14ULL},
    {"error loop", 400, 0x89866da15a60a004ULL},
    {"error dread", 376, 0xdd8c1afaa16c6309ULL},
    {"error dwrite", 376, 0xca578fafef9c0f16ULL},
    {"error extended", 360, 0x26b7281189ba5866ULL},
    {"error merge", 352, 0x979bf4e744416b76ULL},
    {"error outside", 376, 0x90e05069a132d428ULL},
    {"error fall through", 432, 0xcc45dfb283ffb084ULL},
    {"error start", 360, 0xe4fb1c422f8a78a8ULL},
}};

constexpr std::size_t PadRegs = 0x1200; ///< Registers and shadow RAM past the macro range
constexpr Word PadValue = 0x5A5A;       ///< Their value there, the bound programs' writes

bool Executable(Word method) {
    return method % 5 == 0 || method == 0x49 || method == Same::DmaReadFifoed;
}

std::bitset<0x10000> Mask() {
    std::bitset<0x10000> mask;
    for (Word method = 0; method < mask.size(); ++method) {
        mask[method] = Executable(method) || method >= Same::NumRegs;
    }
    return mask;
}

/// The filter's table followed by entries a filter reading past it would take.
std::vector<std::uint8_t> PaddedTable() {
    const auto table = Same::BuildTable(Mask());
    std::vector<std::uint8_t> padded(PadRegs, 1);
    std::copy(table.begin(), table.end(), padded.begin());
    return padded;
}

/// Maxwell3D::FrontWrite's early return and register keeping for macro writes, the emit
/// callback of FrontRunMacro around it, and MME DMA reads (deterministic words).
struct Front {
    std::vector<Word> regs;
    std::vector<Word> shadow;
    Word control{};
    bool skip_same{true};
    /// Writes to the macro range are kept (and recorded) instead of failing: the method grows
    /// past 0xfff.
    bool lenient{};
    Same::Filter* filter{};
    std::vector<std::pair<Word, Word>> sent;
    std::uint64_t dropped{};          ///< Early returns
    std::uint64_t dropped_in_scope{}; ///< ... of writes the filter looks at
    std::array<Word, 0xc00>* dram{};
    std::vector<Word>* fifo{};

    void Write(Word method, Word argument) {
        const bool in_scope = Same::InScope(filter->ctx, method);
        Word value = argument;
        bool kept = true;
        if (control == Same::Track || control == Same::TrackWithFilter) {
            kept = shadow[method] == argument;
            shadow[method] = argument;
        } else if (control == Same::Replay) {
            value = shadow[method];
        }
        if (!Executable(method)) {
            if (regs[method] == value && kept && skip_same) {
                ++dropped;
                dropped_in_scope += in_scope ? 1 : 0;
                return;
            }
            regs[method] = value;
        } else {
            regs[method] = value;
            if (method == 0x49) {
                control = argument;
                shadow[method] = argument;
                filter->Control(control, regs.data(), shadow.data());
            }
        }
        filter->sent = true;
        sent.emplace_back(method, argument);
    }

    void DmaReadWords(Word method, Word count) {
        const Word ram = regs[0x156];
        for (Word i = 0; i < (count & 7); ++i) {
            const Word word = regs[0x155] * 31 + i * 7 + 1;
            if (method == Same::DmaReadFifoed) {
                fifo->push_back(word);
            } else if (ram + i < dram->size()) {
                (*dram)[ram + i] = word;
            }
        }
    }

    bool Emit(Word method, Word value) {
        if (method >= Same::NumRegs) {
            if (!lenient) {
                return false;
            }
            sent.emplace_back(method, value);
            return true;
        }
        Write(method, value);
        if (method == Same::DmaRead || method == Same::DmaReadFifoed) {
            DmaReadWords(method, value);
        }
        return true;
    }

    Word Read(Word address) const {
        return address < Same::NumRegs ? regs[address] : 0U;
    }
};

struct Setup {
    std::vector<Word> regs;
    std::vector<Word> shadow;
    std::vector<Word> params;
    std::array<Word, 0xc00> dram{};
    Word control{};
    bool skip_same{true};
    bool lenient{};
    bool has_check{};
    bool diag_regs_on{};
    bool profile_on{};
    Word gap_methods{~0U};
};

struct Outcome {
    bool success{};
    std::size_t consumed{};
    std::string_view error;
    std::vector<std::pair<Word, Word>> sent;
    std::uint64_t state{}; ///< Digest of the registers, shadow RAM, mode, data RAM and FIFO
    std::uint64_t hits{};
    std::uint64_t dropped{};
    std::uint64_t dropped_in_scope{};
    std::uint64_t mismatches{};
    std::uint64_t missed{};
};

enum class Variant { Reference, Interpreter, Skip, Verify, Cpp };

struct Compiled {
    const Program* program;
    std::vector<Word> code;
    Jit off;
    Jit skip{Jit::SameFilter::Skip};
    Jit verify{Jit::SameFilter::Verify};
};

std::uint64_t StateDigest(const Front& front, const std::vector<Word>& fifo) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&](Word word) {
        hash ^= word;
        hash *= 0x100000001b3ULL;
    };
    for (const Word word : front.regs) {
        mix(word);
    }
    for (const Word word : front.shadow) {
        mix(word);
    }
    mix(front.control);
    for (const Word word : *front.dram) {
        mix(word);
    }
    mix(static_cast<Word>(fifo.size()));
    for (const Word word : fifo) {
        mix(word);
    }
    return hash;
}

Outcome Run(Compiled& compiled, const Setup& setup, const std::vector<std::uint8_t>& table,
            Variant variant) {
    Front front;
    front.regs = setup.regs;
    front.shadow = setup.shadow;
    front.control = setup.control;
    front.skip_same = setup.skip_same;
    front.lenient = setup.lenient;
    std::array<Word, 0xc00> dram = setup.dram;
    std::vector<Word> fifo = setup.params;
    fifo.shrink_to_fit();
    front.dram = &dram;
    front.fifo = &fifo;
    Same::Filter filter;
    filter.mode = variant == Variant::Verify ? 2 : variant == Variant::Cpp ? 3 : 1;
    filter.Begin(table.data(), setup.skip_same, setup.control, setup.has_check,
                 setup.diag_regs_on, setup.profile_on, setup.gap_methods, front.regs.data(),
                 front.shadow.data());
    front.filter = &filter;
    const auto read = [&front](Word address) { return front.Read(address); };
    const auto emit = [&front](Word method, Word value) { return front.Emit(method, value); };
    Outcome outcome;
    const Word start = compiled.program->start;
    std::span<Word> ram{dram};
    switch (variant) {
    case Variant::Reference: {
        const auto result = compiled.off.Execute(start, fifo, ram, read, emit);
        outcome.success = result.success;
        outcome.consumed = result.consumed;
        outcome.error = result.error;
        break;
    }
    case Variant::Interpreter: {
        Tegra::TuringMacroInterpreter interpreter;
        interpreter.active_dram = &dram;
        const auto result = interpreter.Execute(compiled.code, start, fifo, read, emit);
        outcome.success = result.success;
        outcome.consumed = result.consumed;
        outcome.error = result.error;
        break;
    }
    case Variant::Skip: {
        const auto result = compiled.skip.Execute(start, fifo, ram, read, emit, &filter.ctx);
        outcome.success = result.success;
        outcome.consumed = result.consumed;
        outcome.error = result.error;
        break;
    }
    case Variant::Verify: {
        // As FrontRunMacro: a write the JIT would drop goes through verify, the rest through
        // the emit, both to FrontWrite.
        const auto checked = [&](Word method, Word value) {
            const bool in_scope = Same::InScope(filter.ctx, method);
            filter.sent = false;
            const bool ok = front.Emit(method, value);
            if (in_scope && !filter.sent) {
                ++filter.missed;
            }
            return ok;
        };
        const auto verify = [&](Word method, Word value) {
            filter.sent = false;
            const bool ok = front.Emit(method, value);
            if (filter.sent) {
                static_cast<void>(filter.NoteMismatch());
            }
            return ok;
        };
        const auto result =
            compiled.verify.Execute(start, fifo, ram, read, checked, &filter.ctx, verify);
        outcome.success = result.success;
        outcome.consumed = result.consumed;
        outcome.error = result.error;
        break;
    }
    case Variant::Cpp: {
        const auto filtered = [&](Word method, Word value) {
            if (Same::Hit(filter.ctx, method, value)) {
                ++filter.ctx.hits;
                return true;
            }
            return front.Emit(method, value);
        };
        const auto result = compiled.off.Execute(start, fifo, ram, read, filtered);
        outcome.success = result.success;
        outcome.consumed = result.consumed;
        outcome.error = result.error;
        break;
    }
    }
    outcome.sent = std::move(front.sent);
    outcome.state = StateDigest(front, fifo);
    outcome.hits = filter.ctx.hits;
    outcome.dropped = front.dropped;
    outcome.dropped_in_scope = front.dropped_in_scope;
    outcome.mismatches = filter.mismatches;
    outcome.missed = filter.missed;
    return outcome;
}

Word SmallValue(std::mt19937& rng) {
    const Word pick = rng() % 16;
    return pick < 12 ? pick % 4 : pick < 14 ? PadValue : static_cast<Word>(rng());
}

Word Mode(std::mt19937& rng) {
    constexpr std::array<Word, 6> modes{0, 1, 2, 3, 2, 7};
    return modes[rng() % modes.size()];
}

std::vector<Word> MakeParams(std::mt19937& rng, Params kind) {
    std::vector<Word> params(rng() % 22);
    for (Word& value : params) {
        value = SmallValue(rng);
    }
    const auto set = [&](std::size_t index, Word value) {
        if (index < params.size()) {
            params[index] = value;
        }
    };
    switch (kind) {
    case Params::Values:
        break;
    case Params::Mode:
        set(0, Mode(rng));
        set(3, Mode(rng));
        break;
    case Params::ModeLane:
        set(1, Mode(rng));
        break;
    case Params::Methods: {
        constexpr std::array<Word, 9> methods{0x157,          0x158,          0x49,
                                              M(0x300, 1),    M(0xDFF, 1),    M(0xDFE, 15),
                                              M(0x157, 1),    M(0x156, 1),    M(0x48, 1)};
        set(0, rng() % 4 == 0 ? static_cast<Word>(rng() & 0xffff) : methods[rng() % methods.size()]);
        break;
    }
    case Params::Dma:
        set(0, rng() % 10);
        set(1, rng() % 10);
        break;
    }
    return params;
}

std::vector<std::unique_ptr<Compiled>> CompileAll(const std::vector<Program>& programs) {
    std::vector<std::unique_ptr<Compiled>> all;
    for (const Program& program : programs) {
        auto compiled = std::make_unique<Compiled>();
        compiled->program = &program;
        compiled->code = Assemble(program);
        compiled->off.Prepare(compiled->code, 1);
        compiled->skip.Prepare(compiled->code, 1);
        compiled->verify.Prepare(compiled->code, 1);
        all.push_back(std::move(compiled));
    }
    return all;
}

void RequireSame(const Outcome& a, const Outcome& b) {
    REQUIRE(a.success == b.success);
    REQUIRE(a.consumed == b.consumed);
    REQUIRE(a.error == b.error);
    REQUIRE(a.sent == b.sent);
    REQUIRE(a.state == b.state);
}

/// Per seed: random registers and shadow RAM (small values, so same value writes are common),
/// mode, parameters and arming; every program run five ways.
void RunSeeds(std::uint32_t first, std::uint32_t count, std::uint64_t& total_hits,
              std::uint64_t& total_dropped) {
    const auto programs = Programs();
    auto compiled = CompileAll(programs);
    const auto table = PaddedTable();
    for (std::uint32_t seed = first; seed < first + count; ++seed) {
        std::mt19937 rng{seed};
        Setup base;
        base.regs.assign(PadRegs, PadValue);
        base.shadow.assign(PadRegs, PadValue);
        for (Word method = 0; method < Same::NumRegs; ++method) {
            base.regs[method] = SmallValue(rng);
            base.shadow[method] = rng() % 4 == 0 ? SmallValue(rng) : base.regs[method];
        }
        for (Word& word : base.dram) {
            word = static_cast<Word>(rng() % 5);
        }
        for (auto& entry : compiled) {
            Setup setup = base;
            setup.params = MakeParams(rng, entry->program->params);
            setup.control = Mode(rng);
            setup.skip_same = rng() % 10 != 0;
            setup.lenient = rng() % 4 == 0;
            setup.has_check = rng() % 20 == 0;
            setup.diag_regs_on = rng() % 20 == 0;
            setup.profile_on = rng() % 2 == 0;
            setup.gap_methods = rng() % 10 == 0 ? rng() % 17 : ~0U;
            const Outcome reference = Run(*entry, setup, table, Variant::Reference);
            const Outcome interpreter = Run(*entry, setup, table, Variant::Interpreter);
            const Outcome skip = Run(*entry, setup, table, Variant::Skip);
            const Outcome verify = Run(*entry, setup, table, Variant::Verify);
            const Outcome cpp = Run(*entry, setup, table, Variant::Cpp);
            RequireSame(reference, interpreter);
            RequireSame(reference, skip);
            RequireSame(reference, verify);
            RequireSame(reference, cpp);
            // The filter drops exactly the early returns it looks at; none is left to FrontWrite.
            REQUIRE(skip.hits == reference.dropped_in_scope);
            REQUIRE(skip.dropped_in_scope == 0);
            REQUIRE(skip.hits + skip.dropped == reference.dropped);
            REQUIRE(cpp.hits == reference.dropped_in_scope);
            REQUIRE(cpp.dropped_in_scope == 0);
            REQUIRE(verify.hits == reference.dropped_in_scope);
            REQUIRE(verify.dropped == reference.dropped);
            REQUIRE(verify.mismatches == 0);
            REQUIRE(verify.missed == 0);
            total_hits += skip.hits;
            total_dropped += reference.dropped;
        }
    }
}

} // namespace

TEST_CASE("Macro JIT code is unchanged with the filter off", "[video_core][nx2_macro_jit]") {
    const auto programs = Programs();
    REQUIRE(programs.size() == golden.size());
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto code = Assemble(programs[i]);
        Jit off;
        off.Prepare(code, 1);
        Jit skip{Jit::SameFilter::Skip};
        skip.Prepare(code, 1);
        const auto [size, digest] = Digest(off, programs[i].code.size());
        REQUIRE(std::string_view{programs[i].name} == golden[i].name);
#ifdef _WIN32
        REQUIRE(size == golden[i].size);
        REQUIRE(digest == golden[i].digest);
#endif
        // The filter's code: its prolog loads rbp (a few bytes, maybe within the table's
        // alignment), and each emit checks first.
        const bool emits = std::ranges::any_of(programs[i].code, [](const Ins& ins) {
            return ins.emit[0] != 0 || ins.emit[1] != 0;
        });
        const auto [skip_size, skip_digest] = Digest(skip, programs[i].code.size());
        REQUIRE(skip_digest != digest);
        REQUIRE((skip_size >= size + 32) == emits);
    }
}

TEST_CASE("Macro JIT filter: hand checks", "[video_core][nx2_macro_jit]") {
    const auto programs = Programs();
    auto compiled = CompileAll(programs);
    const auto table = PaddedTable();
    const auto find = [&](std::string_view name) -> Compiled& {
        for (auto& entry : compiled) {
            if (name == entry->program->name) {
                return *entry;
            }
        }
        REQUIRE(false);
        return *compiled.front();
    };
    Setup setup;
    setup.regs.assign(PadRegs, PadValue);
    setup.shadow.assign(PadRegs, PadValue);
    for (Word method = 0; method < Same::NumRegs; ++method) {
        setup.regs[method] = setup.shadow[method] = 1;
    }
    // Tracked: writes of 1 to 0x300-0x307, held already: dropped but 0x302 and 0x307
    // (executable).
    setup.params.assign(8, 1);
    setup.control = Same::Track;
    auto skip = Run(find("inc1"), setup, table, Variant::Skip);
    REQUIRE(skip.success);
    REQUIRE(skip.hits == 6);
    REQUIRE(skip.sent == std::vector<std::pair<Word, Word>>{{0x302, 1}, {0x307, 1}});
    // Shadow RAM differs: a tracked write is sent (and keeps the shadow RAM); passthrough, and an
    // unknown mode, compare the register alone.
    setup.shadow[0x301] = 2;
    skip = Run(find("inc1"), setup, table, Variant::Skip);
    REQUIRE(skip.hits == 5);
    REQUIRE(skip.sent == std::vector<std::pair<Word, Word>>{{0x301, 1}, {0x302, 1}, {0x307, 1}});
    setup.control = Same::Passthrough;
    REQUIRE(Run(find("inc1"), setup, table, Variant::Skip).hits == 6);
    setup.control = 7;
    REQUIRE(Run(find("inc1"), setup, table, Variant::Skip).hits == 6);
    // Replay: nothing dropped.
    setup.control = Same::Replay;
    REQUIRE(Run(find("inc1"), setup, table, Variant::Skip).hits == 0);
    setup.shadow[0x301] = 1;
    // Disarmed calls drop nothing.
    setup.control = Same::Track;
    setup.has_check = true;
    REQUIRE(Run(find("inc1"), setup, table, Variant::Skip).hits == 0);
    setup.has_check = false;
    setup.skip_same = false;
    REQUIRE(Run(find("inc1"), setup, table, Variant::Skip).hits == 0);
    setup.skip_same = true;
    setup.profile_on = true;
    setup.gap_methods = 3;
    REQUIRE(Run(find("inc1"), setup, table, Variant::Skip).hits == 0);
    setup.profile_on = false;
    REQUIRE(Run(find("inc1"), setup, table, Variant::Skip).hits == 6);
    setup.gap_methods = ~0U;
    // Zero increment: the same register six times.
    setup.params.assign(6, 1);
    REQUIRE(Run(find("inc0"), setup, table, Variant::Skip).hits == 6);
    // The macro writes Replay, then Track: 0x400 is FrontWrite's under Replay, dropped again
    // under Track (the context follows the mode); 0x401 is executable.
    setup.params = {Same::Replay, 1, 1, Same::Track, 1, 1, 1};
    skip = Run(find("mode switch"), setup, table, Variant::Skip);
    REQUIRE(skip.success);
    REQUIRE(skip.consumed == 7);
    REQUIRE(skip.hits == 2);
    REQUIRE(skip.dropped == 1);
    REQUIRE(skip.sent == std::vector<std::pair<Word, Word>>{
                             {0x49, Same::Replay}, {0x401, 1}, {0x49, Same::Track}, {0x401, 1}});
    // Up to the macro range: 0xDFD-0xDFF dropped (their registers hold the value; 0xDFC is
    // executable), 0xE00 fails.
    for (Word method = 0xDF0; method < Same::NumRegs; ++method) {
        setup.regs[method] = setup.shadow[method] = PadValue;
    }
    setup.params.clear();
    skip = Run(find("bound inc1"), setup, table, Variant::Skip);
    REQUIRE(!skip.success);
    REQUIRE(skip.hits == 3);
    REQUIRE(skip.error == "unsupported emitted method");
    REQUIRE(skip.sent == std::vector<std::pair<Word, Word>>{{0xDFC, PadValue}});
    // Kept writes past the table take the call, past 0xfff too.
    setup.lenient = true;
    skip = Run(find("bound past 0xfff"), setup, table, Variant::Skip);
    REQUIRE(skip.success);
    REQUIRE(skip.hits == 0);
    REQUIRE(skip.sent.size() == 6);
    REQUIRE(skip.sent.back().first == 0xFF8 + 5 * 15);
    setup.lenient = false;
    // MME_DMA_READ_FIFOED grows the FIFO inside its emit, its same value write is not dropped:
    // 0x155 and 0x156 are (0x154 is executable), then seven words, the first a parameter.
    setup.params = {7, 7};
    setup.regs[0x154] = setup.shadow[0x154] = 0;
    setup.regs[0x155] = setup.shadow[0x155] = 0x1000;
    setup.regs[0x156] = setup.shadow[0x156] = 0;
    setup.regs[0x158] = setup.shadow[0x158] = 7;
    skip = Run(find("dma fifoed"), setup, table, Variant::Skip);
    REQUIRE(skip.success);
    REQUIRE(skip.hits == 2);
    REQUIRE(skip.consumed == 8);
    REQUIRE(skip.sent.size() == 2 + 7);
    REQUIRE(skip.sent.back() == std::pair<Word, Word>{0x3c6, 0x1000 * 31 + 5 * 7 + 1});
}

TEST_CASE("Macro JIT filter against the JIT, the interpreter and FrontWrite (seeds)",
          "[video_core][nx2_macro_jit]") {
    std::uint64_t hits = 0;
    std::uint64_t dropped = 0;
    RunSeeds(1, 10000, hits, dropped);
    std::printf("nx2_macro_jit: 10000 seeds x %zu programs, %llu writes dropped by the filter of "
                "%llu early returns\n",
                golden.size(), static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(dropped));
    // The seeds hit the filter often, and FrontWrite drops more (Replay, disarmed calls).
    REQUIRE(hits > 10000);
    REQUIRE(dropped > hits);
}

#endif
