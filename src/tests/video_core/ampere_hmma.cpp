// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <span>
#include <string_view>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LoweredProgram;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};
// HMMA.16816.F16 R44, R16, R48, RZ as title 4's upscaler has it (op 23c 102c723c 00000030 000008ff).
constexpr Instruction HMMA_16816{{0x102c723c, 0x00000030, 0x000008ff, 0x04aff000}};
// UMOV UR4, UR22 and UMOV UR4, UR21.
constexpr Instruction UMOV_UR22{{0x00047c82, 22, 0x08000000, 0}};
constexpr Instruction UMOV_UR21{{0x00047c82, 21, 0x08000000, 0}};

constexpr std::string_view OVERLAP = "native matrix multiply scratch overlaps R240-R245";

// The lowering reads EDEN_NX2_TENSOR once per process, so each mode needs its own run.
std::string_view TensorMode() {
    const char* mode = std::getenv("EDEN_NX2_TENSOR");
    return mode ? mode : "";
}

bool Emulates() {
    const auto mode = TensorMode();
    return mode != "off" && mode != "0" && mode != "stub";
}

Instruction WithDest(Instruction insn, u32 dest) {
    insn.words[0] = (insn.words[0] & ~0xff0000U) | (dest << 16);
    return insn;
}

// overlap: the program also uses R240-R245, so only EDEN_NX2_TENSOR=unguarded emulates it.
void CheckMode(const LoweredProgram& lowered, bool overlap, u32 hmma_offset) {
    const auto mode = TensorMode();
    if (mode == "off" || mode == "0") {
        REQUIRE(!static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native matrix multiply (EDEN_NX2_TENSOR=off)");
        REQUIRE(lowered.error_offset == hmma_offset);
    } else if (overlap && mode != "stub" && mode != "unguarded") {
        REQUIRE(!static_cast<bool>(lowered));
        REQUIRE(lowered.error == OVERLAP);
        REQUIRE(lowered.error_offset == hmma_offset);
    } else {
        REQUIRE(static_cast<bool>(lowered));
    }
    // KeplerCompute::ProcessNativeLaunch stands in with an upscale for these rejections.
    if (!lowered) REQUIRE(lowered.error.find("matrix multiply") != std::string_view::npos);
}

size_t Count(std::span<const u64> words, u64 mask, u64 value) {
    size_t count = 0;
    for (const u64 word : words) count += (word & mask) == value;
    return count;
}

void CheckExpansion(const LoweredProgram& lowered, u32 dest, size_t products, size_t rounds) {
    const auto& words = lowered.instructions;
    REQUIRE(words.back() == 0xe30000000007000fULL);
    // FFMA per product, F2F.F16.F32 (round_half) per accumulator and k group of eight.
    REQUIRE(Count(words, 0xff80000000000000ULL, 0x5980000000000000ULL) == products);
    REQUIRE(Count(words, 0xfff8000000000f00ULL, 0x5ca8000000000900ULL) == rounds);
    // D rows are packed from the f32 accumulators: dest = R241:R240, dest + 1 = R243:R242.
    const u64 pack = 0x5c18000000000000ULL | (u64{16} << 39) | (u64{7} << 16);
    REQUIRE(words[words.size() - 3] == (pack | dest | (u64{241} << 8) | (u64{240} << 20)));
    REQUIRE(words[words.size() - 2] == (pack | (dest + 1) | (u64{243} << 8) | (u64{242} << 20)));
    // Everything else writes only the lowering scratch R240-R254, never UR0-UR21 or R239.
    for (size_t i = 0; i + 3 < words.size(); ++i) {
        REQUIRE((words[i] & 0xff) >= 240);
        REQUIRE((words[i] & 0xff) != 255);
    }
}

TEST_CASE("Ampere HMMA.16816 emulates unless EDEN_NX2_TENSOR rejects it", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{HMMA_16816, EXIT};
    const auto lowered = LowerToMaxwell(code);
    CheckMode(lowered, false, 0);
    if (TensorMode() == "stub") {
        // D = C = RZ: two zero moves, then EXIT.
        REQUIRE(lowered.instructions.size() == 3);
        REQUIRE(lowered.instructions[0] == (0x0100000000000000ULL | (u64{7} << 16) | 44 | (u64{15} << 12)));
    }
    if (Emulates()) CheckExpansion(lowered, 44, 64, 8);
}

TEST_CASE("Ampere HMMA.1688 rounds once per eight products", "[video_core][ampere]") {
    Instruction hmma = HMMA_16816;
    hmma.words[2] = 0xff; // form 0: depth 8, one A pair and one B register
    const std::array<Instruction, 2> code{hmma, EXIT};
    const auto lowered = LowerToMaxwell(code);
    CheckMode(lowered, false, 0);
    if (Emulates()) CheckExpansion(lowered, 44, 32, 4);
}

TEST_CASE("Ampere HMMA is not emulated next to UR22-UR27", "[video_core][ampere]") {
    // UR22-UR27 live in R240-R245, the accumulators: the program keeps its rejection.
    const std::array<Instruction, 3> uses_ur22{UMOV_UR22, HMMA_16816, EXIT};
    CheckMode(LowerToMaxwell(uses_ur22), true, 16);
    // A use after the HMMA counts too: the program is judged when it finishes.
    const std::array<Instruction, 3> ur22_after{HMMA_16816, UMOV_UR22, EXIT};
    CheckMode(LowerToMaxwell(ur22_after), true, 0);
    const std::array<Instruction, 3> uses_ur21{UMOV_UR21, HMMA_16816, EXIT};
    CheckMode(LowerToMaxwell(uses_ur21), false, 16);
}

TEST_CASE("Ampere HMMA is not emulated next to native R240-R245", "[video_core][ampere]") {
    // Without a uniform bank, natives may use up to R245.
    const std::array<Instruction, 2> high{WithDest(HMMA_16816, 240), EXIT};
    CheckMode(LowerToMaxwell(high), true, 0);
    const std::array<Instruction, 2> low{WithDest(HMMA_16816, 236), EXIT};
    const auto lowered = LowerToMaxwell(low);
    CheckMode(lowered, false, 0);
    if (Emulates()) CheckExpansion(lowered, 236, 64, 8);
}
} // namespace
