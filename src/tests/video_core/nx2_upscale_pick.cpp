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

TEST_CASE("Nx2UpscalePick: bank 0's size words are not handles", "[video_core]") {
    const Sizes sizes{1440, 810, 2880, 1620};
    // Word 10 (+0x28) is 0xb3f, the output width minus one: as a handle, texture 2879, a
    // 1440x810 buffer of the input's format, found before the real input at +0x60.
    const std::array<Candidate, 4> candidates{{
        {.slot = 0, .offset = 0x28, .index = 2879, .address = 0x1000, .width = 1440, .height = 810,
         .format = R11G11B10F, .usable = true},
        {.slot = 0, .offset = 0x60, .index = 40, .address = 0x2000, .width = 1440, .height = 810,
         .format = R11G11B10F, .usable = true},
        {.slot = 0, .offset = 0x38, .index = 41, .address = 0x3000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
        {.slot = 0, .offset = 0x2c, .index = 1619, .address = 0x4000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
    }};
    const Pick pick = Choose(sizes, candidates, PREFERENCE);
    REQUIRE(pick.input == 1u);
    REQUIRE(pick.input_matches == 1u);
    REQUIRE(pick.output_count == 1u);
    REQUIRE(pick.outputs[0] == 2u);
    REQUIRE(pick.output_total == 1u);
    REQUIRE(Learnable(pick, candidates) == Location{0, 0x60});
    // A word pinned by EDEN_NX2_UPSCALE_IN / _OUT is taken even in the header.
    const Pick pinned = Choose(sizes, candidates, PREFERENCE, Location{0, 0x28}, Location{0, 0x2c});
    REQUIRE(pinned.input == 0u);
    REQUIRE(pinned.output_count == 1u);
    REQUIRE(pinned.outputs[0] == 3u);
}

TEST_CASE("Nx2UpscalePick: the preferred input location", "[video_core]") {
    const Sizes sizes{1440, 810, 2880, 1620};
    std::array<Candidate, 3> candidates{{
        {.slot = 3, .offset = 0x10, .index = 1, .address = 0x1000, .width = 1440, .height = 810,
         .format = R11G11B10F, .usable = true},
        {.slot = 0, .offset = 0x60, .index = 2, .address = 0x2000, .width = 1440, .height = 810,
         .format = R11G11B10F, .usable = true},
        {.slot = 3, .offset = 0x20, .index = 3, .address = 0x3000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
    }};
    const Pick by_format = Choose(sizes, candidates, PREFERENCE);
    REQUIRE(by_format.input == 0u);
    REQUIRE(by_format.input_matches == 2u);
    REQUIRE(!Learnable(by_format, candidates));
    const Pick preferred =
        Choose(sizes, candidates, PREFERENCE, std::nullopt, std::nullopt, Location{0, 0x60});
    REQUIRE(preferred.input == 1u);
    REQUIRE(preferred.input_matches == 2u);
    REQUIRE(preferred.output_count == 1u);
    // It overrides the format order too.
    candidates[0].format = RGBA16F;
    candidates[2].format = RGBA16F;
    REQUIRE(Choose(sizes, candidates, PREFERENCE).input == 0u);
    REQUIRE(Choose(sizes, candidates, PREFERENCE, std::nullopt, std::nullopt, Location{0, 0x60})
                .input == 1u);
    candidates[0].format = R11G11B10F;
    candidates[2].format = R11G11B10F;
    // A preferred word naming an image the blit can't use: the format order.
    candidates[1].usable = false;
    const Pick unusable =
        Choose(sizes, candidates, PREFERENCE, std::nullopt, std::nullopt, Location{0, 0x60});
    REQUIRE(unusable.input == 0u);
    REQUIRE(unusable.input_matches == 1u);
    // Only bank 0 teaches: the only input at bank 3 is not learned.
    REQUIRE(unusable.output_count == 1u);
    REQUIRE(!Learnable(unusable, candidates));
    // Two words naming one image count once.
    candidates[1] = {.slot = 0, .offset = 0x64, .index = 1, .address = 0x1000, .width = 1440,
                     .height = 810, .format = R11G11B10F, .usable = true};
    REQUIRE(Choose(sizes, candidates, PREFERENCE).input_matches == 1u);
}

TEST_CASE("Nx2UpscalePick: a pinned input wins over the preferred one", "[video_core]") {
    // The caller passes no preference with EDEN_NX2_UPSCALE_IN; if both came, the pin decides.
    const Sizes sizes{1440, 810, 2880, 1620};
    const std::array<Candidate, 3> candidates{{
        {.slot = 3, .offset = 0x10, .index = 1, .address = 0x1000, .width = 1440, .height = 810,
         .format = R11G11B10F, .usable = true},
        {.slot = 0, .offset = 0x60, .index = 2, .address = 0x2000, .width = 1440, .height = 810,
         .format = R11G11B10F, .usable = true},
        {.slot = 3, .offset = 0x20, .index = 3, .address = 0x3000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
    }};
    const Pick pick =
        Choose(sizes, candidates, PREFERENCE, Location{3, 0x10}, std::nullopt, Location{0, 0x60});
    REQUIRE(pick.input == 0u);
    REQUIRE(pick.input_matches == 1u);
}

TEST_CASE("Nx2UpscalePick: nothing learned from the remembered words alone", "[video_core]") {
    // A full scan found two inputs and took bank 3 +0x10 by format; the next dispatch reads only
    // the remembered words (that one and the output): one input, but not one to teach (bank 3).
    // The caller teaches from full scans only, so the location learned is a full scan's.
    const Sizes sizes{1440, 810, 2880, 1620};
    const std::array<Candidate, 2> remembered{{
        {.slot = 3, .offset = 0x10, .index = 1, .address = 0x1000, .width = 1440, .height = 810,
         .format = R11G11B10F, .usable = true},
        {.slot = 3, .offset = 0x20, .index = 3, .address = 0x3000, .width = 2880, .height = 1620,
         .format = R11G11B10F, .usable = true},
    }};
    const Pick pick = Choose(sizes, remembered, PREFERENCE);
    REQUIRE(pick.input == 0u);
    REQUIRE(pick.input_matches == 1u);
    REQUIRE(!Learnable(pick, remembered));
}
