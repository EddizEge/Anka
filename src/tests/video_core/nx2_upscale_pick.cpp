// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <span>
#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/nx2_upscale_pick.h"

namespace {
using namespace Tegra::Engines::Nx2UpscalePick;

// Constant bank 0 of Bananza's 1920x1080 upscaler, as dumped with its QMD (grid 60x34, block 128).
constexpr std::array<u32, 16> BANK0{0x80, 0x3c,  0x22, 0x1, 0,          0,    0x77f, 0x437,
                                    0,    0,     0xb3f, 0x653, 0x0ce71380, 0x22, 0x5ac0, 0};
constexpr u32 RGBA16F = 0x03;
constexpr u32 RG16F = 0x0c;
constexpr u32 R11G11B10F = 0x21;
constexpr u32 Z32 = 0x2f;
constexpr std::array<u32, 4> PREFERENCE{RGBA16F, R11G11B10F, 0x09, 0x08};
} // Anonymous namespace

TEST_CASE("Nx2UpscalePick: sizes from constant bank 0", "[video_core]") {
    const auto sizes = SizesFromBank0(BANK0, 128, 60, 34);
    REQUIRE(sizes.has_value());
    REQUIRE(*sizes == Sizes{1920, 1080, 2880, 1620});
    REQUIRE(!SizesFromBank0(BANK0, 128, 60, 33));
    REQUIRE(!SizesFromBank0(std::span{BANK0}.first(8), 128, 60, 34));
    // 2095x1179 is 0.05% off 2880x1620's aspect ratio.
    std::array<u32, 12> other{0x80, 0x42, 0x32, 1, 0, 0, 0x82e, 0x49a, 0, 0, 0xb3f, 0x653};
    REQUIRE(SizesFromBank0(other, 128, 66, 50) == Sizes{2095, 1179, 2880, 1620});
    other[10] = 0xfff;
    REQUIRE(!SizesFromBank0(other, 128, 66, 50));
}

TEST_CASE("Nx2UpscalePick: the colour input and every output of its format", "[video_core]") {
    const Sizes sizes{1920, 1080, 2880, 1620};
    const std::array<Candidate, 6> candidates{{
        {.slot = 3, .offset = 0x10, .index = 5, .address = 0x1000, .width = 1920, .height = 1080,
         .format = Z32, .usable = false},
        {.slot = 3, .offset = 0x14, .index = 6, .address = 0x2000, .width = 1920, .height = 1080,
         .format = RG16F, .usable = false},
        {.slot = 3, .offset = 0x18, .index = 7, .address = 0x3000, .width = 1920, .height = 1080,
         .format = R11G11B10F, .usable = true},
        {.slot = 3, .offset = 0x1c, .index = 8, .address = 0x4000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
        {.slot = 3, .offset = 0x20, .index = 9, .address = 0x5000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
        {.slot = 5, .offset = 0x40, .index = 8, .address = 0x4000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
    }};
    const Pick pick = Choose(sizes, candidates, PREFERENCE);
    REQUIRE(pick.input == 2u);
    REQUIRE(pick.output_count == 2u);
    REQUIRE(pick.outputs[0] == 3u);
    REQUIRE(pick.outputs[1] == 4u);
    REQUIRE(pick.output_total == 2u);
    REQUIRE(!pick.Ambiguous());
    const Pick only = Choose(sizes, candidates, PREFERENCE, std::nullopt, Location{3, 0x20});
    REQUIRE(only.output_count == 1u);
    REQUIRE(only.outputs[0] == 4u);
    REQUIRE(!Choose(sizes, candidates, PREFERENCE, Location{3, 0x10}).input);
}

TEST_CASE("Nx2UpscalePick: too many outputs of the input's format", "[video_core]") {
    const Sizes sizes{1920, 1080, 2880, 1620};
    std::array<Candidate, 5> candidates{};
    candidates[0] = {.slot = 4, .offset = 0, .index = 1, .address = 0x1000, .width = 1920,
                     .height = 1080, .format = RGBA16F, .usable = true};
    for (u32 i = 1; i < candidates.size(); ++i) {
        candidates[i] = {.slot = 4, .offset = i * 4, .index = 1 + i, .address = 0x1000 + i * 0x1000,
                         .width = 2880, .height = 1620, .format = RGBA16F, .usable = true};
    }
    const Pick pick = Choose(sizes, candidates, PREFERENCE);
    REQUIRE(pick.input == 0u);
    REQUIRE(pick.output_count == 3u);
    REQUIRE(pick.output_total == 4u);
    REQUIRE(pick.Ambiguous());
    const Pick pinned = Choose(sizes, candidates, PREFERENCE, std::nullopt, Location{4, 8});
    REQUIRE(pinned.output_count == 1u);
    REQUIRE(pinned.outputs[0] == 2u);
    REQUIRE(!pinned.Ambiguous());
}

TEST_CASE("Nx2UpscalePick: bank:offset", "[video_core]") {
    REQUIRE(ParseLocation("3:1a0") == Location{3, 0x1a0});
    REQUIRE(ParseLocation("0:FC") == Location{0, 0xfc});
    REQUIRE(!ParseLocation(nullptr));
    REQUIRE(!ParseLocation("8:10"));
    REQUIRE(!ParseLocation("3:"));
    REQUIRE(!ParseLocation("3:1g"));
}
