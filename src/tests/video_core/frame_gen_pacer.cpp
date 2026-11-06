// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present/frame_gen_pacer_rules.h"

namespace {

using namespace Vulkan::FrameGenPacerRules;

constexpr f32 DYNAMIC_TARGET = 120.0f;
constexpr size_t MAX_GENERATIONS = 3;

/// The generation level the pacer can climb to with the game at base_rate: it steps while the
/// output is below the (effective) target and CanStepUp allows the next level.
size_t ReachableLevel(f32 base_rate, f32 target_rate, f32 refresh_hz) {
    const f32 target = EffectiveTarget(target_rate, refresh_hz);
    size_t limit = 0;
    while (limit < MAX_GENERATIONS && base_rate * static_cast<f32>(limit + 1) < target &&
           CanStepUp(base_rate, limit, refresh_hz)) {
        ++limit;
    }
    return limit;
}

} // Anonymous namespace

TEST_CASE("FrameGenPacerRules: the refresh rate from the period", "[video_core]") {
    REQUIRE(RefreshHz(0.0f) == 0.0f);
    REQUIRE(RefreshHz(-1.0f) == 0.0f);
    REQUIRE(RefreshHz(1.0f / 60.0f) > 59.99f);
    REQUIRE(RefreshHz(1.0f / 60.0f) < 60.01f);
}

TEST_CASE("FrameGenPacerRules: the target is capped at a known refresh rate", "[video_core]") {
    REQUIRE(EffectiveTarget(DYNAMIC_TARGET, 60.0f) == 60.0f);
    REQUIRE(EffectiveTarget(DYNAMIC_TARGET, 170.0f) == DYNAMIC_TARGET);
    REQUIRE(EffectiveTarget(DYNAMIC_TARGET, 0.0f) == DYNAMIC_TARGET);
    // Fixed multiplier mode (target 0) stays target 0
    REQUIRE(EffectiveTarget(0.0f, 60.0f) == 0.0f);
}

TEST_CASE("FrameGenPacerRules: 60 Hz, game at 33 FPS stays at 1x", "[video_core]") {
    // 33 * 2 = 66 > 60: on 2x vsync would hold the game at 30
    REQUIRE(!CanStepUp(33.0f, 0, 60.0f));
    REQUIRE(ReachableLevel(33.0f, DYNAMIC_TARGET, 60.0f) == 0);
    // A game at exactly 30 can take 2x on 60 Hz, at 20 even 3x
    REQUIRE(CanStepUp(30.0f, 0, 60.0f));
    REQUIRE(ReachableLevel(30.0f, DYNAMIC_TARGET, 60.0f) == 1);
    REQUIRE(ReachableLevel(20.0f, DYNAMIC_TARGET, 60.0f) == 2);
}

TEST_CASE("FrameGenPacerRules: 170 Hz, game at 46 FPS goes to 2x and 3x, not 4x",
          "[video_core]") {
    REQUIRE(CanStepUp(46.0f, 0, 170.0f));  // 46 * 2 = 92
    REQUIRE(CanStepUp(46.0f, 1, 170.0f));  // 46 * 3 = 138
    REQUIRE(!CanStepUp(46.0f, 2, 170.0f)); // 46 * 4 = 184 > 170
    // Target 120: 3x (138) is already enough
    REQUIRE(ReachableLevel(46.0f, DYNAMIC_TARGET, 170.0f) == 2);
    // A target above the refresh is capped at 170 and the cap stops at 3x
    REQUIRE(ReachableLevel(46.0f, 240.0f, 170.0f) == 2);
}

TEST_CASE("FrameGenPacerRules: unknown refresh rate, no cap", "[video_core]") {
    REQUIRE(CanStepUp(46.0f, 2, 0.0f));
    REQUIRE(CanStepUp(100.0f, 10, 0.0f));
    REQUIRE(ReachableLevel(33.0f, DYNAMIC_TARGET, 0.0f) == 3);
    REQUIRE(ReachableLevel(46.0f, DYNAMIC_TARGET, 0.0f) == 2);
}

TEST_CASE("FrameGenPacerRules: a probe that lowers the game's frame rate is reverted",
          "[video_core]") {
    // The observed failure: 33 -> 30 FPS on 1x -> 2x at 60 Hz (91%)
    REQUIRE(!ProbeKeepsGameRate(33.0f, 30.0f));
    // FSR3's own cost: 46 -> 44 (96%) is kept
    REQUIRE(ProbeKeepsGameRate(46.0f, 44.0f));
    REQUIRE(ProbeKeepsGameRate(46.0f, 46.0f));
    REQUIRE(ProbeKeepsGameRate(46.0f, 50.0f));
    // The threshold itself
    REQUIRE(ProbeKeepsGameRate(100.0f, 92.1f));
    REQUIRE(!ProbeKeepsGameRate(100.0f, 91.9f));
    // Halving (vsync to the next divisor) is always reverted
    REQUIRE(!ProbeKeepsGameRate(60.0f, 30.0f));
}
