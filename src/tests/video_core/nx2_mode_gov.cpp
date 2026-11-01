// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_mode_gov.h"

namespace {
using VideoCommon::Nx2ModeGov::Governor;
using VideoCommon::Nx2ModeGov::Output;
using VideoCommon::Nx2ModeGov::Params;
using VideoCommon::Nx2ModeGov::ScaledClock;

constexpr Params DEFAULTS{};
} // Anonymous namespace

TEST_CASE("Nx2ModeGov: holds the mode it is in", "[video_core]") {
    Governor governor;
    for (int i = 0; i < 120; ++i) {
        const Output out = governor.Step({.mode = 1, .work_ms = 12.0, .dt_ms = 16.7});
        REQUIRE(out.want == 1);
        REQUIRE(out.shown_ms == DEFAULTS.hold_ms);
    }
}

TEST_CASE("Nx2ModeGov: 60 to 30 with one spike, then holds", "[video_core]") {
    Governor governor;
    governor.Step({.mode = 1, .work_ms = 12.0, .dt_ms = 16.7});
    int changed_at = -1;
    Output out{};
    for (int i = 0; i < 80 && changed_at < 0; ++i) {
        out = governor.Step({.mode = 1, .work_ms = 25.0, .dt_ms = 25.0});
        if (out.want == 2) {
            changed_at = i;
        }
    }
    // ~18 frames until the average passes 19.5 ms, then 0.5 s of dwell at 25 ms a frame.
    REQUIRE(changed_at >= 20);
    REQUIRE(out.want_changed);
    REQUIRE(out.shown_ms == DEFAULTS.spike_ms);
    out = governor.Step({.mode = 1, .work_ms = 25.0, .dt_ms = 25.0});
    REQUIRE(out.shown_ms == DEFAULTS.down_ms);
    out = governor.Step({.mode = 2, .work_ms = 25.0, .dt_ms = 33.3});
    REQUIRE(out.want == 2);
    REQUIRE(out.shown_ms == DEFAULTS.hold_ms);
}

TEST_CASE("Nx2ModeGov: 30 to 60 only after the up dwell", "[video_core]") {
    Governor governor;
    Output out{};
    for (int i = 0; i < 50; ++i) {
        out = governor.Step({.mode = 2, .work_ms = 12.0, .dt_ms = 33.3});
        REQUIRE(out.want == 2);
        REQUIRE(out.shown_ms == DEFAULTS.hold_ms);
    }
    bool changed = false;
    for (int i = 0; i < 20 && !changed; ++i) {
        out = governor.Step({.mode = 2, .work_ms = 12.0, .dt_ms = 33.3});
        changed = out.want == 1;
    }
    REQUIRE(changed);
    REQUIRE(out.shown_ms == DEFAULTS.up_ms);
    out = governor.Step({.mode = 1, .work_ms = 12.0, .dt_ms = 16.7});
    REQUIRE(out.shown_ms == DEFAULTS.hold_ms);
}

TEST_CASE("Nx2ModeGov: a push that gets nowhere rests, then pushes again", "[video_core]") {
    Governor governor;
    Output out{};
    do {
        out = governor.Step({.mode = 2, .work_ms = 12.0, .dt_ms = 33.3});
    } while (out.want != 1);
    // The frame the wanted mode changed was the first of the push.
    for (u32 i = 1; i < DEFAULTS.up_frames; ++i) {
        out = governor.Step({.mode = 2, .work_ms = 12.0, .dt_ms = 33.3});
        REQUIRE(out.shown_ms == DEFAULTS.up_ms);
    }
    for (u32 i = 0; i < DEFAULTS.cooldown_frames; ++i) {
        out = governor.Step({.mode = 2, .work_ms = 12.0, .dt_ms = 33.3});
        REQUIRE(out.shown_ms == DEFAULTS.hold_ms);
    }
    out = governor.Step({.mode = 2, .work_ms = 12.0, .dt_ms = 33.3});
    REQUIRE(out.shown_ms == DEFAULTS.up_ms);
    REQUIRE(out.want == 1);
}

TEST_CASE("Nx2ModeGov: 20 when 30 can't be held", "[video_core]") {
    Governor governor;
    Output out{};
    bool changed = false;
    for (int i = 0; i < 20 && !changed; ++i) {
        out = governor.Step({.mode = 2, .work_ms = 40.0, .dt_ms = 40.0});
        changed = out.want == 3;
    }
    REQUIRE(changed);
    REQUIRE(out.shown_ms == DEFAULTS.spike_ms);
    // At 30 only the spike: down_ms there would send the game back to 60.
    out = governor.Step({.mode = 2, .work_ms = 40.0, .dt_ms = 40.0});
    REQUIRE(out.want == 3);
    REQUIRE(out.shown_ms == DEFAULTS.hold_ms);
    // Reaching 20 holds; dropping from 60 to 30 on the way spikes again at 30.
    out = governor.Step({.mode = 3, .work_ms = 40.0, .dt_ms = 50.0});
    REQUIRE(out.shown_ms == DEFAULTS.hold_ms);
    Governor from60;
    from60.Step({.mode = 1, .work_ms = 40.0, .dt_ms = 40.0});
    do {
        out = from60.Step({.mode = 1, .work_ms = 40.0, .dt_ms = 40.0});
    } while (out.want == 1);
    REQUIRE(out.want == 3);
    REQUIRE(out.shown_ms == DEFAULTS.spike_ms);
    REQUIRE(from60.Step({.mode = 1, .work_ms = 40.0, .dt_ms = 40.0}).shown_ms == DEFAULTS.down_ms);
    REQUIRE(from60.Step({.mode = 2, .work_ms = 40.0, .dt_ms = 40.0}).shown_ms == DEFAULTS.spike_ms);
    REQUIRE(from60.Step({.mode = 2, .work_ms = 40.0, .dt_ms = 40.0}).shown_ms == DEFAULTS.hold_ms);
}

TEST_CASE("Nx2ModeGov: between the bounds the wanted mode stays", "[video_core]") {
    Governor at60;
    Governor at30;
    for (int i = 0; i < 600; ++i) {
        REQUIRE(at60.Step({.mode = 1, .work_ms = 18.5, .dt_ms = 18.5}).want == 1);
        REQUIRE(at30.Step({.mode = 2, .work_ms = 18.5, .dt_ms = 33.3}).want == 2);
    }
}

TEST_CASE("Nx2ModeGov: the scaled clock counts busy ticks and never goes back", "[video_core]") {
    ScaledClock clock;
    REQUIRE(clock.Read(1000) == 1000);
    REQUIRE(clock.Rebase(1000, 2.0) == 1000);
    REQUIRE(clock.Read(1500) == 2000);
    REQUIRE(clock.Rebase(1500, 0.5) == 500);
    REQUIRE(clock.Read(1700) == 2100);
    REQUIRE(clock.Read(1600) == 2100);
    REQUIRE(clock.Rebase(2000, 1.0) == 500);
    REQUIRE(clock.Read(2000) == 2250);
}
