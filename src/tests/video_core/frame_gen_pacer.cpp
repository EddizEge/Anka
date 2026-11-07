// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/frame_gen_pacer.h"
#endif
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

// FrameGenPacer itself is built with the frame generation (ENABLE_LSFG, HAS_LSFG)
#ifdef HAS_LSFG

namespace {

using Vulkan::FrameGenPacer;
using Clock = FrameGenPacer::Clock;

/// Dynamic frame generation (target 120) in the global settings for one test, as they were after.
struct DynamicSettings {
    bool frame_gen = Settings::values.frame_gen.GetValue();
    u32 target_rate = Settings::values.frame_gen_target_rate.GetValue();
    u32 multiplier = Settings::values.frame_gen_multiplier.GetValue();

    explicit DynamicSettings(u32 target = 120, u32 fixed_multiplier = 2) {
        Settings::values.frame_gen.SetValue(true);
        Settings::values.frame_gen_target_rate.SetValue(target);
        Settings::values.frame_gen_multiplier.SetValue(fixed_multiplier);
    }
    ~DynamicSettings() {
        Settings::values.frame_gen.SetValue(frame_gen);
        Settings::values.frame_gen_target_rate.SetValue(target_rate);
        Settings::values.frame_gen_multiplier.SetValue(multiplier);
    }
};

/// The pacer in front of a FIFO swapchain: every present (real or generated) waits for its own
/// refresh, so a real frame takes at least (generations + 1) refreshes, and the game is held to
/// refresh / (generations + 1) when that is below its own rate.
struct Display {
    FrameGenPacer pacer;
    Clock::time_point now{};
    f32 refresh_hz;
    f32 game_rate;
    size_t generations{};

    Display(f32 refresh_hz_, f32 game_rate_) : refresh_hz{refresh_hz_}, game_rate{game_rate_} {}

    /// Runs `seconds` of real frames; returns the largest number of generated frames planned.
    size_t Run(f32 seconds) {
        size_t most = 0;
        const auto end = now + std::chrono::duration_cast<Clock::duration>(
                                   std::chrono::duration<f32>(seconds));
        while (now < end) {
            const f32 interval =
                std::max(1.0f / game_rate, static_cast<f32>(generations + 1) / refresh_hz);
            now += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f32>(interval));
            generations = pacer.Plan(MAX_GENERATIONS, 1.0f / refresh_hz, now).generations;
            most = std::max(most, generations);
        }
        return most;
    }

    /// The game's own frame rate now (vsync included).
    f32 GameRate() const {
        return std::min(game_rate, refresh_hz / static_cast<f32>(generations + 1));
    }
};

} // Anonymous namespace

TEST_CASE("FrameGenPacer: 170 Hz, game at 46 FPS climbs to 3x and stays", "[video_core]") {
    const DynamicSettings settings;
    Display display{170.0f, 46.0f};
    display.Run(20.0f);
    REQUIRE(display.pacer.Limit() == 2);
    REQUIRE(display.GameRate() == 46.0f);
}

TEST_CASE("FrameGenPacer: 60 Hz, game at 33 FPS stays at 1x", "[video_core]") {
    const DynamicSettings settings;
    Display display{60.0f, 33.0f};
    REQUIRE(display.Run(30.0f) == 0);
    REQUIRE(display.pacer.Limit() == 0);
}

TEST_CASE("FrameGenPacer: 60 Hz, game at 25 FPS takes 2x", "[video_core]") {
    const DynamicSettings settings;
    Display display{60.0f, 25.0f};
    display.Run(20.0f);
    REQUIRE(display.pacer.Limit() == 1);
    REQUIRE(display.GameRate() == 25.0f);
}

TEST_CASE("FrameGenPacer: the window moves from 170 Hz to 60 Hz, the level steps down",
          "[video_core]") {
    const DynamicSettings settings;
    Display display{170.0f, 46.0f};
    display.Run(20.0f);
    REQUIRE(display.pacer.Limit() == 2);
    // At 60 Hz 3x holds the game at 20; before the step down it stayed there
    display.refresh_hz = 60.0f;
    display.Run(10.0f);
    REQUIRE(display.pacer.Limit() == 0);
    REQUIRE(display.GameRate() == 46.0f);
    // And back: it climbs again
    display.refresh_hz = 170.0f;
    display.Run(20.0f);
    REQUIRE(display.pacer.Limit() == 2);
    REQUIRE(display.GameRate() == 46.0f);
}

TEST_CASE("FrameGenPacer: a refresh change the level still fits keeps it", "[video_core]") {
    const DynamicSettings settings;
    Display display{170.0f, 46.0f};
    display.Run(20.0f);
    REQUIRE(display.pacer.Limit() == 2);
    display.refresh_hz = 165.0f; // 46 * 3 = 138 still clearly below
    display.Run(1.0f);
    REQUIRE(display.pacer.Limit() == 2);
}

TEST_CASE("FrameGenPacer: fixed multiplier mode ignores the refresh", "[video_core]") {
    const DynamicSettings settings{0, 3};
    // 40 * 3 = 120 > 60: fixed 3x is what the user chose, vsync or not
    Display display{60.0f, 40.0f};
    REQUIRE(display.Run(5.0f) == 2);
    REQUIRE(display.pacer.Limit() == 2);
}

#endif // HAS_LSFG

TEST_CASE("FrameGenPacerRules: the level after a refresh change", "[video_core]") {
    REQUIRE(RefreshChanged(170.0f, 60.0f));
    REQUIRE(RefreshChanged(0.0f, 60.0f));
    REQUIRE(RefreshChanged(60.0f, 0.0f));
    REQUIRE(!RefreshChanged(60.0f, 60.0f));
    REQUIRE(!RefreshChanged(59.94f, 60.0f));
    // 46 * 3 = 138 does not fit 60: back to 1x
    REQUIRE(LimitAfterRefreshChange(46.0f, 2, 60.0f) == 0);
    // Already throttled by the new refresh (20 * 3 = 60): back to 1x as well
    REQUIRE(LimitAfterRefreshChange(20.0f, 2, 60.0f) == 0);
    // Clearly fits: kept
    REQUIRE(LimitAfterRefreshChange(46.0f, 2, 165.0f) == 2);
    REQUIRE(LimitAfterRefreshChange(25.0f, 1, 60.0f) == 1);
    // Unknown refresh: kept
    REQUIRE(LimitAfterRefreshChange(46.0f, 2, 0.0f) == 2);
}
