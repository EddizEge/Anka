// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_dk_dyn_pace.h"
#include "core/hle/service/nx2_osd_speed.h"

namespace {
using namespace Service::Nx2OsdSpeed;
using Service::Nx2DkDynPace::StepEstimate;
} // Anonymous namespace

TEST_CASE("Nx2OsdSpeed: the game time a present stepped", "[core]") {
    // DK-DYN: the frame, up to 2.5 vblanks (StepEstimate's cap).
    REQUIRE(GameUs(true, 0, 16'667) == 16'667);
    REQUIRE(GameUs(true, 3, 33'333) == 33'333); // the interval is not used
    REQUIRE(GameUs(true, 0, 41'667) == 41'667);
    REQUIRE(GameUs(true, 0, 50'000) == StepEstimate::CapUs);
    // Unpatched with the interval word known: the interval at 60 Hz, whatever the frame took.
    REQUIRE(GameUs(false, 1, 20'000) == 16'666);
    REQUIRE(GameUs(false, 2, 40'000) == 33'333);
    REQUIRE(GameUs(false, 3, 50'000) == 50'000);
    // No estimate.
    REQUIRE(GameUs(false, 0, 33'333) == 0);
    REQUIRE(GameUs(true, 0, 0) == 0);
    REQUIRE(GameUs(true, 0, -5) == 0);
}

TEST_CASE("Nx2OsdSpeed: the same speed as the 600 present line's estimate", "[core]") {
    // Frames of 16.7-60 ms: the OSD's totals give StepEstimate's Speed() exactly.
    StepEstimate estimate;
    Counter counter;
    long long frame = 16'667;
    for (int i = 0; i < 600; ++i) {
        frame = 16'667 + (frame * 7919 + i * 104'729) % 43'333;
        estimate.Add(frame);
        counter.Add(frame, GameUs(true, 0, frame));
    }
    const Totals totals = counter.Get();
    REQUIRE(static_cast<long long>(totals.frame_us) == estimate.frame_us);
    REQUIRE(static_cast<long long>(totals.game_us) == estimate.stepped_us);
    REQUIRE(estimate.capped > 0);
    REQUIRE(static_cast<double>(totals.game_us) / static_cast<double>(totals.frame_us) ==
            estimate.Speed());
}

TEST_CASE("Nx2OsdSpeed: speeds at steady frame rates", "[core]") {
    const auto speed = [](bool dk_dyn, u32 interval, long long frame) {
        Counter counter;
        for (int i = 0; i < 60; ++i) {
            counter.Add(frame, GameUs(dk_dyn, interval, frame));
        }
        const Totals t = counter.Get();
        return static_cast<double>(t.game_us) / static_cast<double>(t.frame_us);
    };
    REQUIRE(speed(true, 0, 28'571) == 1.0);               // 35 fps
    REQUIRE(speed(true, 0, 41'667) == 1.0);               // 24 fps, at the cap
    REQUIRE(speed(true, 0, 50'000) == 41'667.0 / 50'000); // 20 fps: 83%
    REQUIRE(speed(false, 2, 33'333) == 1.0);              // 30 mode at 30 fps
    const double slow = speed(false, 2, 40'000);          // 30 mode at 25 fps
    REQUIRE(slow > 0.8333 && slow < 0.8334);
}

TEST_CASE("Nx2OsdSpeed: nothing counted without an estimate", "[core]") {
    Counter counter;
    counter.Add(33'333, 0);
    counter.Add(0, 16'667);
    counter.Add(-1, 16'667);
    const Totals t = counter.Get();
    REQUIRE(t.frame_us == 0);
    REQUIRE(t.game_us == 0);
}
