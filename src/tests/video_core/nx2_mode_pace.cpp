// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_mode_pace_logic.h"

namespace {
using namespace VideoCommon::Nx2ModePace;

constexpr Params DEFAULTS{};

double Period(u32 mode) {
    return mode == 1 ? 1000.0 / 60 : mode == 2 ? 2000.0 / 60 : 3000.0 / 60;
}

/// What n presents did: every output, the first start and the first end.
struct Fed {
    std::vector<Output> outs;
    int started_at{-1};
    int ended_at{-1};

    const Output& Last() const {
        return outs.back();
    }
    int Starts() const {
        int n = 0;
        for (const Output& o : outs) {
            n += o.started != Push::None ? 1 : 0;
        }
        return n;
    }
};

Fed Feed(Controller& ctl, u32 mode, double work, double busy, double dt, int n) {
    Fed fed;
    for (int i = 0; i < n; ++i) {
        fed.outs.push_back(ctl.Step({.mode = mode, .work_ms = work, .busy_ms = busy, .dt_ms = dt}));
        if (fed.started_at < 0 && fed.outs.back().started != Push::None) {
            fed.started_at = i;
        }
        if (fed.ended_at < 0 && fed.outs.back().ended != Push::None) {
            fed.ended_at = i;
        }
    }
    return fed;
}

Fed Feed(Controller& ctl, u32 mode, double work, double busy, int n) {
    return Feed(ctl, mode, work, busy, Period(mode), n);
}

/// Presents until a push starts (at most n); the output that started it.
Output UntilStart(Controller& ctl, u32 mode, double work, double busy, double dt, int n,
                  int* index = nullptr) {
    for (int i = 0; i < n; ++i) {
        const Output out = ctl.Step({.mode = mode, .work_ms = work, .busy_ms = busy, .dt_ms = dt});
        if (out.started != Push::None) {
            if (index) {
                *index = i;
            }
            return out;
        }
    }
    return {};
}
} // Anonymous namespace

TEST_CASE("Nx2ModePace: no push at 60 where the frame fits", "[video_core]") {
    Controller ctl;
    const Fed fed = Feed(ctl, 1, 8.0, 5.0, 600);
    REQUIRE(fed.Starts() == 0);
    for (const Output& o : fed.outs) {
        REQUIRE(o.min_period_ms == 0.0);
    }
}

TEST_CASE("Nx2ModePace: stuck at 60, a down push in two stages", "[video_core]") {
    Controller ctl;
    int start = -1;
    const Output first = UntilStart(ctl, 1, 26.0, 26.0, 26.0, 200, &start);
    // 0.5 s of quiet at 60, then 0.5 s of dwell at 26 ms a frame.
    REQUIRE(first.started == Push::Down);
    REQUIRE(!first.by_probe);
    REQUIRE(start >= 35);
    REQUIRE(start <= 45);
    REQUIRE(first.min_period_ms == DEFAULTS.hold_ms);
    REQUIRE(first.k_ms == 26.0);
    const Fed fed = Feed(ctl, 1, 26.0, 26.0, 26.0, 60);
    for (int i = 0; i < 29; ++i) {
        REQUIRE(fed.outs[i].min_period_ms == DEFAULTS.hold_ms);
    }
    for (int i = 29; i < 59; ++i) {
        REQUIRE(fed.outs[i].min_period_ms == DEFAULTS.hold_late_ms);
    }
    REQUIRE(fed.ended_at == 59);
    REQUIRE(fed.Last().ended == Push::Down);
    REQUIRE(fed.Last().result == Result::GaveUp);
    REQUIRE(fed.Last().frames == 60u); // both stages
    REQUIRE(fed.Last().min_period_ms == 0.0);
    REQUIRE(fed.Last().rest_s == DEFAULTS.down_rest_s);
    REQUIRE(fed.Last().tries == 1u);
    // No push during the 2 s of rest.
    const Fed rest = Feed(ctl, 1, 26.0, 26.0, 26.0, 76);
    REQUIRE(rest.Starts() == 0);
}

TEST_CASE("Nx2ModePace: failed down pushes back off, then stop until the game moves",
          "[video_core]") {
    Controller ctl;
    std::vector<double> rests;
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(UntilStart(ctl, 1, 26.0, 26.0, 26.0, 1000).started == Push::Down);
        const Fed fed = Feed(ctl, 1, 26.0, 26.0, 26.0, 60);
        REQUIRE(fed.Last().result == Result::GaveUp);
        rests.push_back(fed.Last().rest_s);
        REQUIRE(fed.Last().tries == static_cast<u32>(attempt + 1));
        REQUIRE(fed.Last().blocked == (attempt == 2));
    }
    REQUIRE(rests == std::vector<double>{2.0, 4.0, 8.0});
    REQUIRE(ctl.Blocked(Push::Down));
    // 5 minutes stuck: nothing.
    REQUIRE(Feed(ctl, 1, 26.0, 26.0, 26.0, 12000).Starts() == 0);
    // The game goes to 30 and back by itself: pushes again.
    Feed(ctl, 2, 26.0, 26.0, 20);
    REQUIRE(!ctl.Blocked(Push::Down));
    REQUIRE(UntilStart(ctl, 1, 26.0, 26.0, 26.0, 200).started == Push::Down);
}

TEST_CASE("Nx2ModePace: the push ends when the game goes to 30", "[video_core]") {
    Controller ctl;
    REQUIRE(UntilStart(ctl, 1, 26.0, 26.0, 26.0, 200).started == Push::Down);
    Feed(ctl, 1, 26.0, 26.0, 42.0, 7);
    // The present that brings the new mode is not held (it would be a late frame of 30).
    REQUIRE(ctl.HoldMs(1) == DEFAULTS.hold_ms);
    REQUIRE(ctl.HoldMs(2) == 0.0);
    const Output out = ctl.Step({.mode = 2, .work_ms = 26.0, .busy_ms = 26.0, .dt_ms = 42.0});
    REQUIRE(out.ended == Push::Down);
    REQUIRE(out.result == Result::Done);
    REQUIRE(out.frames == 7u);
    REQUIRE(out.mode_changed);
    REQUIRE(out.old_mode == 1u);
    REQUIRE(out.by == Push::Down);
    REQUIRE(out.change_k_ms == 26.0);
    REQUIRE(out.min_period_ms == 0.0);
    REQUIRE(ctl.HoldMs(2) == 0.0);
    REQUIRE(ctl.Tries(Push::Down) == 0u);
}

TEST_CASE("Nx2ModePace: a down push that lands at 20 is done20", "[video_core]") {
    Controller ctl;
    REQUIRE(UntilStart(ctl, 1, 26.0, 26.0, 26.0, 200).started == Push::Down);
    Feed(ctl, 1, 26.0, 26.0, 42.0, 3);
    const Output out = ctl.Step({.mode = 3, .work_ms = 26.0, .busy_ms = 26.0, .dt_ms = 42.0});
    REQUIRE(out.ended == Push::Down);
    REQUIRE(out.result == Result::Done20);
    REQUIRE(ResultName(out.result) == "done20");
    REQUIRE(ctl.Tries(Push::Down) == 0u);
    REQUIRE(ctl.HoldMs(3) == 0.0);
}

TEST_CASE("Nx2ModePace: no push at 60 where the game drops itself", "[video_core]") {
    Controller ctl;
    REQUIRE(Feed(ctl, 1, 40.0, 40.0, 40.0, 200).Starts() == 0);
}

TEST_CASE("Nx2ModePace: the frame cost is the larger median of work and busy", "[video_core]") {
    // O1's heavy 30: ~12 ms of work, ~22 ms busy (the split's back thread).
    Controller heavy;
    const Fed fed = Feed(heavy, 2, 8.0, 22.0, 400);
    REQUIRE(fed.Starts() == 0);
    REQUIRE(fed.Last().k_ms == 22.0);
    REQUIRE(fed.Last().work_med_ms == 8.0);
    REQUIRE(fed.Last().busy_med_ms == 22.0);
    Controller light;
    int start = -1;
    const Output out = UntilStart(light, 2, 8.0, 8.0, Period(2), 400, &start);
    REQUIRE(out.started == Push::Up);
    REQUIRE(out.min_period_ms == DEFAULTS.hold_ms);
    // 1 s of quiet, then 5 s of dwell.
    const double at_s = (start + 1) * Period(2) / 1000.0;
    REQUIRE(at_s >= 5.9);
    REQUIRE(at_s <= 6.2);
    // The up push holds only presents at 30.
    REQUIRE(light.HoldMs(2) == DEFAULTS.hold_ms);
    REQUIRE(light.HoldMs(1) == 0.0);
}

TEST_CASE("Nx2ModePace: stalls stay out of the window and end a push", "[video_core]") {
    Controller ctl;
    Feed(ctl, 1, 10.0, 10.0, 100);
    const Output stall = ctl.Step({.mode = 1, .work_ms = 150.0, .busy_ms = 10.0, .dt_ms = 150.0});
    REQUIRE(stall.k_ms == 10.0);
    REQUIRE(UntilStart(ctl, 1, 26.0, 26.0, 26.0, 400).started == Push::Down);
    const Output aborted =
        ctl.Step({.mode = 1, .work_ms = 150.0, .busy_ms = 26.0, .dt_ms = 150.0});
    REQUIRE(aborted.ended == Push::Down);
    REQUIRE(aborted.result == Result::Aborted);
    REQUIRE(aborted.min_period_ms == 0.0);
    // No new push for 2 s after a stall.
    int start = -1;
    REQUIRE(UntilStart(ctl, 1, 26.0, 26.0, 26.0, 400, &start).started == Push::Down);
    REQUIRE((start + 1) * 26.0 >= 2000.0);
}

TEST_CASE("Nx2ModePace: the median shrugs off one long frame", "[video_core]") {
    Controller ctl;
    Feed(ctl, 1, 10.0, 10.0, 100);
    const Output spike = ctl.Step({.mode = 1, .work_ms = 90.0, .busy_ms = 90.0, .dt_ms = 90.0});
    REQUIRE(spike.k_ms == 10.0);
    REQUIRE(Feed(ctl, 1, 10.0, 10.0, 100).Starts() == 0);
}

TEST_CASE("Nx2ModePace: failed up pushes back off, then stop", "[video_core]") {
    Controller ctl;
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 10000).started == Push::Up);
        const Fed fed = Feed(ctl, 2, 5.0, 5.0, 90);
        REQUIRE(fed.ended_at == 89);
        REQUIRE(fed.Last().result == Result::GaveUp);
        REQUIRE(fed.Last().rest_s == DEFAULTS.up_rest_s * (1 << attempt));
        REQUIRE(fed.Last().tries == static_cast<u32>(attempt + 1));
    }
    REQUIRE(ctl.Blocked(Push::Up));
    REQUIRE(Feed(ctl, 2, 5.0, 5.0, 9000).Starts() == 0); // 300 s
    Feed(ctl, 1, 5.0, 5.0, 30);
    Feed(ctl, 2, 5.0, 5.0, 1);
    REQUIRE(!ctl.Blocked(Push::Up));
    REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 400).started == Push::Up);
}

TEST_CASE("Nx2ModePace: only the game's own change lifts the up push's tries", "[video_core]") {
    Controller ctl;
    REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 400).started == Push::Up);
    Feed(ctl, 2, 5.0, 5.0, 4);
    // The up push sent the game to 20: a failure.
    const Output other = ctl.Step({.mode = 3, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 42.0});
    REQUIRE(other.ended == Push::Up);
    REQUIRE(other.result == Result::Other);
    REQUIRE(other.by == Push::Up);
    REQUIRE(ctl.Tries(Push::Up) == 1u);
    // 20 -> 30 by an up20 push: still one try down.
    REQUIRE(UntilStart(ctl, 3, 5.0, 5.0, Period(3), 400).started == Push::Up20);
    const Output done = ctl.Step({.mode = 2, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 56.0});
    REQUIRE(done.result == Result::Done);
    REQUIRE(done.by == Push::Up20);
    REQUIRE(ctl.Tries(Push::Up) == 1u);
    // The game goes to 60 by itself: lifted.
    const Output self = ctl.Step({.mode = 1, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 33.3});
    REQUIRE(self.by == Push::None);
    REQUIRE(ctl.Tries(Push::Up) == 0u);
}

TEST_CASE("Nx2ModePace: the up dwell doubles when 60 does not last", "[video_core]") {
    Controller ctl;
    REQUIRE(ctl.UpDwell() == DEFAULTS.up_dwell_s);
    double expected = DEFAULTS.up_dwell_s;
    for (int round = 0; round < 5; ++round) {
        REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 4000).started == Push::Up);
        REQUIRE(ctl.Step({.mode = 1, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 42.0}).result ==
                Result::Done);
        // 3 s later the frame costs 25 ms: a down push within 10 s of the up push.
        int start = -1;
        REQUIRE(UntilStart(ctl, 1, 25.0, 25.0, 25.0, 400, &start).started == Push::Down);
        REQUIRE(start * 25.0 < 10000.0);
        expected = (std::min)(expected * 2.0, DEFAULTS.up_dwell_max_s);
        REQUIRE(ctl.UpDwell() == expected);
        REQUIRE(ctl.Step({.mode = 2, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 42.0}).result ==
                Result::Done);
    }
    REQUIRE(ctl.UpDwell() == DEFAULTS.up_dwell_max_s);
    // A minute at 60: back to the base.
    REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 4000).started == Push::Up);
    ctl.Step({.mode = 1, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 42.0});
    Feed(ctl, 1, 5.0, 5.0, 3700);
    REQUIRE(ctl.UpDwell() == DEFAULTS.up_dwell_s);
}

TEST_CASE("Nx2ModePace: 20 to 30, climbing hold, then no up push for a while", "[video_core]") {
    Controller ctl;
    int start = -1;
    const Output out = UntilStart(ctl, 3, 20.0, 20.0, Period(3), 400, &start);
    REQUIRE(out.started == Push::Up20);
    REQUIRE(out.min_period_ms == DEFAULTS.hold20_ms);
    const double at_s = (start + 1) * Period(3) / 1000.0;
    REQUIRE(at_s >= 2.9);
    REQUIRE(at_s <= 3.2);
    const Fed fed = Feed(ctl, 3, 20.0, 20.0, 56.0, 30);
    REQUIRE(fed.outs[8].min_period_ms == 56.0);
    REQUIRE(fed.outs[9].min_period_ms == 62.0);
    REQUIRE(fed.outs[19].min_period_ms == 68.0);
    REQUIRE(fed.outs[29].min_period_ms == 68.0);
    const Output done = ctl.Step({.mode = 2, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 68.0});
    REQUIRE(done.result == Result::Done);
    // after_up20 (10 s) then the up dwell (5 s).
    REQUIRE(Feed(ctl, 2, 5.0, 5.0, 420).Starts() == 0);
    REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 300).started == Push::Up);
}

TEST_CASE("Nx2ModePace: 30 everywhere with _60=0", "[video_core]") {
    Params params;
    params.allow60 = false;
    Controller ctl{params};
    REQUIRE(Feed(ctl, 2, 8.0, 8.0, 18000).Starts() == 0);
}

TEST_CASE("Nx2ModePace: up push as one long frame", "[video_core]") {
    Params params;
    params.up_single_ms = 75.0;
    Controller ctl{params};
    const Output out = UntilStart(ctl, 2, 5.0, 5.0, Period(2), 400);
    REQUIRE(out.started == Push::Up);
    REQUIRE(out.min_period_ms == 75.0);
    REQUIRE(ctl.HoldMs(2) == 75.0);
    const Fed fed = Feed(ctl, 2, 5.0, 5.0, 90);
    REQUIRE(fed.outs[0].min_period_ms == 0.0);
    REQUIRE(fed.outs[0].pushing);
    REQUIRE(ctl.HoldMs(2) == 0.0);
    REQUIRE(fed.ended_at == 89);
    REQUIRE(fed.Last().result == Result::GaveUp);
}

TEST_CASE("Nx2ModePace: to20 only when switched on", "[video_core]") {
    const auto swing = [](Controller& ctl) {
        int starts = 0;
        Output last{};
        for (int k = 0; k < 2; ++k) {
            const Fed at30 = Feed(ctl, 2, 40.0, 40.0, 40.0, 10);
            const Fed at60 = Feed(ctl, 1, 40.0, 40.0, 40.0, 10);
            starts += at30.Starts() + at60.Starts();
        }
        const Fed back = Feed(ctl, 2, 40.0, 40.0, 40.0, 6);
        starts += back.Starts();
        return starts;
    };
    Controller off;
    REQUIRE(swing(off) == 0);
    Params params;
    params.to20 = true;
    Controller on{params};
    Feed(on, 2, 40.0, 40.0, 40.0, 10);
    Feed(on, 1, 40.0, 40.0, 40.0, 10);
    Feed(on, 2, 40.0, 40.0, 40.0, 10);
    Feed(on, 1, 40.0, 40.0, 40.0, 10);
    int start = -1;
    const Output out = UntilStart(on, 2, 40.0, 40.0, 40.0, 10, &start);
    REQUIRE(out.started == Push::To20);
    REQUIRE(start == 3); // from 4 frames, without the mode quiet time
    REQUIRE(out.min_period_ms == params.to20_ms);
    // The held present: 110 ms apart, 40 of work: not a stall.
    const Output second = on.Step({.mode = 2, .work_ms = 40.0, .busy_ms = 40.0, .dt_ms = 110.0});
    REQUIRE(second.ended == Push::None);
    REQUIRE(second.min_period_ms == params.to20_ms);
    const Output third = on.Step({.mode = 2, .work_ms = 40.0, .busy_ms = 40.0, .dt_ms = 110.0});
    REQUIRE(third.pushing);
    REQUIRE(third.min_period_ms == 0.0);
    const Output done = on.Step({.mode = 3, .work_ms = 40.0, .busy_ms = 40.0, .dt_ms = 50.0});
    REQUIRE(done.result == Result::Done);
    REQUIRE(done.rest_s == params.to20_rest_s);
    // 30 s of rest, swinging again: nothing.
    Feed(on, 2, 40.0, 40.0, 40.0, 10);
    REQUIRE(swing(on) == 0);
}

TEST_CASE("Nx2ModePace: probe", "[video_core]") {
    REQUIRE(ParseProbe("cycle:15")->cycle);
    REQUIRE(ParseProbe("cycle:15")->every_s == 15.0);
    REQUIRE(ParseProbe("down:10")->kind == Push::Down);
    REQUIRE(ParseProbe("up20:2.5")->every_s == 2.5);
    REQUIRE(!ParseProbe("down:x"));
    REQUIRE(!ParseProbe("left:5"));
    REQUIRE(!ParseProbe("down:0"));
    REQUIRE(!ParseProbe("down"));
    REQUIRE(!ParseProbe("down:"));
    // A .bat's trailing space (or a tab, a newline) around the value or its parts.
    REQUIRE(ParseProbe("cycle:10 ")->every_s == 10.0);
    REQUIRE(ParseProbe(" down : 10\t\r\n")->kind == Push::Down);
    REQUIRE(!ParseProbe("down:1 0"));
    REQUIRE(!ParseProbe("   "));
    Params params;
    params.auto_push = false;
    Controller ctl{params, ParseProbe("cycle:15")};
    // At 60 on a light frame (no automatic push would start): a down push 15 s in.
    int start = -1;
    const Output down = UntilStart(ctl, 1, 8.0, 8.0, Period(1), 2000, &start);
    REQUIRE(down.started == Push::Down);
    REQUIRE(down.by_probe);
    REQUIRE(std::isnan(down.k_ms) == false);
    const double at_s = (start + 1) * Period(1) / 1000.0;
    REQUIRE(at_s >= 14.9);
    REQUIRE(at_s <= 15.1);
    Feed(ctl, 1, 8.0, 8.0, 42.0, 5);
    REQUIRE(ctl.Step({.mode = 2, .work_ms = 8.0, .busy_ms = 8.0, .dt_ms = 42.0}).result ==
            Result::Done);
    // The next probe 15 s after the push ended (not after it started).
    const Output up = UntilStart(ctl, 2, 8.0, 8.0, Period(2), 2000, &start);
    REQUIRE(up.started == Push::Up);
    REQUIRE(up.by_probe);
    REQUIRE((start + 1) * Period(2) / 1000.0 >= 14.9);
    REQUIRE((start + 1) * Period(2) / 1000.0 <= 15.1);
    // Probe pushes leave the tries alone; "down:10" holds whatever K is.
    Controller heavy{params, ParseProbe("down:10")};
    REQUIRE(UntilStart(heavy, 1, 45.0, 45.0, 45.0, 400).started == Push::Down);
    REQUIRE(Feed(heavy, 1, 45.0, 45.0, 45.0, 60).Last().result == Result::GaveUp);
    REQUIRE(heavy.Tries(Push::Down) == 0u);
}

TEST_CASE("Nx2ModePace: a mode change empties the window", "[video_core]") {
    Controller ctl;
    Feed(ctl, 2, 20.0, 20.0, 100);
    const Fed fed = Feed(ctl, 1, 10.0, 10.0, 8);
    REQUIRE(fed.outs[0].mode_changed);
    REQUIRE(fed.outs[0].change_k_ms == 20.0);
    for (int i = 0; i < 7; ++i) {
        REQUIRE(std::isnan(fed.outs[i].k_ms));
    }
    REQUIRE(fed.outs[7].k_ms == 10.0);
}

TEST_CASE("Nx2ModePace: the first present only records the times", "[video_core]") {
    Meter meter;
    // The busy time since the start would be one huge sample.
    REQUIRE(!meter.Present(1, 1000.0, 1010.0, 1016.0, 50000.0));
    const auto m = meter.Present(1, 1017.0, 1030.0, 1033.0, 50012.0);
    REQUIRE(m.has_value());
    REQUIRE(m->input.dt_ms == 17.0);
    REQUIRE(m->input.work_ms == 14.0);
    REQUIRE(m->input.busy_ms == 12.0);
    REQUIRE(m->gap_ms == 17.0);
    REQUIRE(m->pace_ms == 3.0);
    // The busy total never goes back.
    REQUIRE(meter.Present(1, 1034.0, 1045.0, 1050.0, 50000.0)->input.busy_ms == 0.0);
}

TEST_CASE("Nx2ModePace: a present interval counts at most 1 s", "[video_core]") {
    Controller ctl;
    REQUIRE(UntilStart(ctl, 1, 26.0, 26.0, 26.0, 200).started == Push::Down);
    const double before = ctl.Clock();
    const Output out = ctl.Step({.mode = 1, .work_ms = 5000.0, .busy_ms = 26.0, .dt_ms = 5000.0});
    REQUIRE(std::abs(ctl.Clock() - before - 1.0) < 1e-9);
    REQUIRE(out.result == Result::Aborted);
    REQUIRE(ctl.Tries(Push::Down) == 0u);
    REQUIRE(ctl.Aborts(Push::Down) == 1u);
    ctl.Step({.mode = 1, .work_ms = 10.0, .busy_ms = 10.0, .dt_ms = -5.0});
    ctl.Step({.mode = 1, .work_ms = 10.0, .busy_ms = 10.0, .dt_ms = NAN});
    REQUIRE(std::abs(ctl.Clock() - before - 1.0) < 1e-9);
}

TEST_CASE("Nx2ModePace: unexpected modes start nothing", "[video_core]") {
    Params params;
    Controller ctl{params, ParseProbe("cycle:1")};
    REQUIRE(Feed(ctl, 4, 26.0, 26.0, 26.0, 400).Starts() == 0);
    REQUIRE(Feed(ctl, 0, 26.0, 26.0, 26.0, 400).Starts() == 0);
    REQUIRE(ctl.HoldMs(0) == 0.0);
    // A push the game leaves for an unknown mode: other.
    Controller down;
    REQUIRE(UntilStart(down, 1, 26.0, 26.0, 26.0, 200).started == Push::Down);
    REQUIRE(down.Step({.mode = 4, .work_ms = 26.0, .busy_ms = 26.0, .dt_ms = 42.0}).result ==
            Result::Other);
}

TEST_CASE("Nx2ModePace: loading frames with stalls", "[video_core]") {
    // 25 ms frames with a stall every 1.5 s: the stall quiet time keeps every push away.
    Controller often;
    int starts = 0;
    for (int k = 0; k < 40; ++k) {
        often.Step({.mode = 1, .work_ms = 150.0, .busy_ms = 25.0, .dt_ms = 150.0});
        starts += Feed(often, 1, 25.0, 25.0, 25.0, 60).Starts();
    }
    REQUIRE(starts == 0);
    // A stall every 3.5 s: pushes start and are aborted, with a rest that doubles each time
    // (2, 4, 8 ... 60 s), but no block: over 140 s only a few.
    Controller rare;
    starts = 0;
    for (int k = 0; k < 40; ++k) {
        starts += Feed(rare, 1, 25.0, 25.0, 25.0, 140).Starts();
        const Output out =
            rare.Step({.mode = 1, .work_ms = 150.0, .busy_ms = 25.0, .dt_ms = 150.0});
        if (out.ended != Push::None) {
            // (The first push, before any stall, gives up: one try.)
            REQUIRE(out.result == Result::Aborted);
            REQUIRE(out.tries <= 1u);
            REQUIRE(out.aborts == rare.Aborts(Push::Down));
            REQUIRE(!out.blocked);
        }
    }
    REQUIRE(starts >= 3);
    REQUIRE(starts <= 7);
    REQUIRE(!rare.Blocked(Push::Down));
    REQUIRE(rare.Aborts(Push::Down) + rare.Tries(Push::Down) == static_cast<u32>(starts));
}

TEST_CASE("Nx2ModePace: names for the log", "[video_core]") {
    REQUIRE(PushName(Push::Up20) == "up20");
    REQUIRE(ResultName(Result::GaveUp) == "gave up");
    const std::array<float, 4> even{4.0f, 1.0f, 3.0f, 2.0f};
    REQUIRE(Median(even) == 2.5);
    REQUIRE(std::isnan(Median({})));
}

TEST_CASE("Nx2ModePace: precise sleep", "[video_core]") {
    using namespace std::chrono;
    // Never before the target. How far after depends on the timer resolution (Eden raises it at
    // start; a test process may run at 15.6 ms), so the tight bound is checked with a spin longer
    // than the whole wait: the yield loop alone. Local only: a loaded machine can miss it.
    for (const u32 spin : {0U, 1500U, 20000U}) {
        const auto target = steady_clock::now() + milliseconds{5};
        const auto start = steady_clock::now();
        const nanoseconds spun = SleepUntil(target, spin);
        const auto woke = steady_clock::now();
        REQUIRE(woke >= target);
        // The time it yielded: none without a spin, never more than the whole wait.
        REQUIRE(spun >= nanoseconds{0});
        REQUIRE(spun <= woke - start);
        if (spin == 0U) {
            REQUIRE(spun == nanoseconds{0});
        }
        if (spin == 20000U) {
            REQUIRE(woke < target + milliseconds{2});
            REQUIRE(spun >= milliseconds{4}); // the spin is longer than the wait: all of it
        }
    }
}

TEST_CASE("Nx2ModePace: failed up20 pushes back off, then stop", "[video_core]") {
    Controller ctl;
    std::vector<double> rests;
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(UntilStart(ctl, 3, 20.0, 20.0, Period(3), 4000).started == Push::Up20);
        // The hold is for presents at 20 only.
        REQUIRE(ctl.HoldMs(3) == DEFAULTS.hold20_ms);
        REQUIRE(ctl.HoldMs(2) == 0.0);
        REQUIRE(ctl.HoldMs(1) == 0.0);
        const Fed fed = Feed(ctl, 3, 20.0, 20.0, 56.0, 40);
        REQUIRE(fed.ended_at == 39);
        REQUIRE(fed.Last().result == Result::GaveUp);
        REQUIRE(fed.Last().frames == 40u);
        rests.push_back(fed.Last().rest_s);
        REQUIRE(fed.Last().tries == static_cast<u32>(attempt + 1));
        REQUIRE(fed.Last().blocked == (attempt == 2));
    }
    REQUIRE(rests == std::vector<double>{5.0, 10.0, 20.0});
    REQUIRE(ctl.Blocked(Push::Up20));
    REQUIRE(Feed(ctl, 3, 20.0, 20.0, 6000).Starts() == 0); // 300 s
}

TEST_CASE("Nx2ModePace: a to20 push holds only presents at 30", "[video_core]") {
    Params params;
    params.to20 = true;
    Controller ctl{params};
    for (int k = 0; k < 2; ++k) {
        Feed(ctl, 2, 40.0, 40.0, 40.0, 10);
        Feed(ctl, 1, 40.0, 40.0, 40.0, 10);
    }
    REQUIRE(UntilStart(ctl, 2, 40.0, 40.0, 40.0, 10).started == Push::To20);
    REQUIRE(ctl.HoldMs(2) == params.to20_ms);
    REQUIRE(ctl.HoldMs(1) == 0.0);
    REQUIRE(ctl.HoldMs(3) == 0.0);
}

TEST_CASE("Nx2ModePace: 30 that does not last after up20 is a failure", "[video_core]") {
    Controller ctl;
    std::vector<double> rests;
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(UntilStart(ctl, 3, 20.0, 20.0, Period(3), 4000).started == Push::Up20);
        REQUIRE(ctl.Step({.mode = 2, .work_ms = 20.0, .busy_ms = 20.0, .dt_ms = 56.0}).result ==
                Result::Done);
        // 3 s later the game goes back to 20 by itself (or escapes to 60: the same).
        Feed(ctl, 2, 20.0, 20.0, 90);
        const u32 left_to = attempt == 1 ? 1U : 3U;
        const Output back =
            ctl.Step({.mode = left_to, .work_ms = 20.0, .busy_ms = 20.0, .dt_ms = 40.0});
        REQUIRE(back.mode_changed);
        REQUIRE(back.by == Push::None);
        REQUIRE(back.unverified == Push::Up20);
        REQUIRE(back.tries == static_cast<u32>(attempt + 1));
        REQUIRE(back.blocked == (attempt == 2));
        rests.push_back(back.rest_s);
        REQUIRE(ctl.Tries(Push::Up20) == static_cast<u32>(attempt + 1));
        if (attempt == 1) {
            // Escaped to 60: a down push takes it to 20 (done20, O1 c9), the up20 tries stay.
            REQUIRE(UntilStart(ctl, 1, 25.0, 25.0, 25.0, 400).started == Push::Down);
            const Output done20 =
                ctl.Step({.mode = 3, .work_ms = 25.0, .busy_ms = 25.0, .dt_ms = 42.0});
            REQUIRE(done20.result == Result::Done20);
            REQUIRE(done20.unverified == Push::None);
            REQUIRE(ctl.Tries(Push::Up20) == 2u);
        }
    }
    REQUIRE(rests == std::vector<double>{5.0, 10.0, 20.0});
    REQUIRE(ctl.Blocked(Push::Up20));
    REQUIRE(Feed(ctl, 3, 20.0, 20.0, 6000).Starts() == 0);
}

TEST_CASE("Nx2ModePace: 30 that lasts after up20 is no failure", "[video_core]") {
    Controller ctl;
    REQUIRE(UntilStart(ctl, 3, 20.0, 20.0, Period(3), 4000).started == Push::Up20);
    ctl.Step({.mode = 2, .work_ms = 20.0, .busy_ms = 20.0, .dt_ms = 56.0});
    // 11 s at 30, then the game goes to 20 by itself: an ordinary change.
    Feed(ctl, 2, 20.0, 20.0, 330);
    const Output back = ctl.Step({.mode = 3, .work_ms = 20.0, .busy_ms = 20.0, .dt_ms = 40.0});
    REQUIRE(back.unverified == Push::None);
    REQUIRE(ctl.Tries(Push::Up20) == 0u);
    // A probe's up20 is not on trial.
    Params params;
    params.auto_push = false;
    Controller probe{params, ParseProbe("up20:5")};
    REQUIRE(UntilStart(probe, 3, 20.0, 20.0, Period(3), 4000).by_probe);
    probe.Step({.mode = 2, .work_ms = 20.0, .busy_ms = 20.0, .dt_ms = 56.0});
    Feed(probe, 2, 20.0, 20.0, 30);
    REQUIRE(probe.Step({.mode = 3, .work_ms = 20.0, .busy_ms = 20.0, .dt_ms = 40.0}).unverified ==
            Push::None);
}

TEST_CASE("Nx2ModePace: the game dropping 60 after an up push doubles the dwell once",
          "[video_core]") {
    Controller ctl;
    REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 4000).started == Push::Up);
    REQUIRE(ctl.Step({.mode = 1, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 42.0}).result ==
            Result::Done);
    const double up_at = ctl.Clock();
    // The frame costs 25 ms: a down push within 10 s doubles the dwell...
    REQUIRE(UntilStart(ctl, 1, 25.0, 25.0, 25.0, 400).started == Push::Down);
    REQUIRE(ctl.UpDwell() == 2.0 * DEFAULTS.up_dwell_s);
    // ... and the push failing, then the game dropping to 30 by itself, still within 10 s of the
    // up push, does not double it again.
    Feed(ctl, 1, 25.0, 25.0, 25.0, 60);
    const Output drop = ctl.Step({.mode = 2, .work_ms = 25.0, .busy_ms = 25.0, .dt_ms = 40.0});
    REQUIRE(drop.by == Push::None);
    REQUIRE(ctl.Clock() - up_at < DEFAULTS.verify60_s);
    REQUIRE(ctl.UpDwell() == 2.0 * DEFAULTS.up_dwell_s);
    // Alone (no down push), the game's own drop doubles it.
    Controller self;
    REQUIRE(UntilStart(self, 2, 5.0, 5.0, Period(2), 4000).started == Push::Up);
    self.Step({.mode = 1, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 42.0});
    Feed(self, 1, 5.0, 5.0, 120);
    REQUIRE(self.Step({.mode = 2, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 40.0}).by == Push::None);
    REQUIRE(self.UpDwell() == 2.0 * DEFAULTS.up_dwell_s);
}

TEST_CASE("Nx2ModePace: the up dwell halves back at 30 too", "[video_core]") {
    Params params;
    params.auto_push = false;
    Controller ctl{params, ParseProbe("up:1")};
    // A probe's up push; 60 does not last: the dwell doubles.
    REQUIRE(UntilStart(ctl, 2, 5.0, 5.0, Period(2), 400).started == Push::Up);
    ctl.Step({.mode = 1, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 42.0});
    Feed(ctl, 1, 5.0, 5.0, 60);
    ctl.Step({.mode = 2, .work_ms = 5.0, .busy_ms = 5.0, .dt_ms = 40.0});
    const double raised_at = ctl.Clock();
    REQUIRE(ctl.UpDwell() == 2.0 * DEFAULTS.up_dwell_s);
    // Then at 30 (the probe's up pushes give up: the game stays): raised for 60 s, then halved.
    while (ctl.Clock() - raised_at < 59.0) {
        Feed(ctl, 2, 40.0, 40.0, 40.0, 25);
    }
    REQUIRE(ctl.UpDwell() == 2.0 * DEFAULTS.up_dwell_s);
    while (ctl.Clock() - raised_at < 61.0) {
        Feed(ctl, 2, 40.0, 40.0, 40.0, 25);
    }
    REQUIRE(ctl.UpDwell() == DEFAULTS.up_dwell_s);
}

TEST_CASE("Nx2ModePace: the probe's interval counts from the end of a push", "[video_core]") {
    Params params;
    params.auto_push = false;
    Controller ctl{params, ParseProbe("down:2")};
    REQUIRE(UntilStart(ctl, 1, 45.0, 45.0, 45.0, 400).started == Push::Down);
    const Fed fed = Feed(ctl, 1, 45.0, 45.0, 45.0, 60); // 2.7 s, longer than the interval
    REQUIRE(fed.Last().result == Result::GaveUp);
    int start = -1;
    REQUIRE(UntilStart(ctl, 1, 45.0, 45.0, 45.0, 400, &start).started == Push::Down);
    REQUIRE((start + 1) * 45.0 >= 2000.0);
}

namespace {
/// A toy Donkey Kong Bananza (the mode review's model): it changes its mode on late frames (34 ms
/// or more at 60 and 30, 58 at 20) counted up and down (7 at 60, 24 at 30, 7 at 20); after
/// 20 -> 30 it goes back to 20 when its first 25 frames at 30 have 8 late ones (S15 c9).
struct ToyGame {
    double k20{};
    double k30{};
    double k60{};
    u32 mode = 1;
    bool sixty_to_20 = false; ///< from 60 on two late frames, straight to 20 (O1 c9)
    double jitter = 0.0;
    bool follows = true; ///< false: it ignores late frames at 60 (a load)
    u32 late = 0;
    u32 since = 0;
    bool from20 = false;
    u32 seed = 1;

    double Cost() {
        const double k = mode == 3 ? k20 : mode == 2 ? k30 : k60;
        if (jitter == 0.0) {
            return k;
        }
        seed = seed * 1664525U + 1013904223U;
        return k + jitter * (static_cast<double>(seed >> 8) / (1 << 24) * 2.0 - 1.0);
    }

    /// A frame shown `dt` ms after the previous one; a mode change shows from the next present.
    void Frame(double dt) {
        const u32 old = mode;
        ++since;
        const auto count = [&](bool is_late) {
            late = is_late ? late + 1 : (late > 0 ? late - 1 : 0);
        };
        if (mode == 1) {
            count(follows && dt >= 34.0);
            if (late >= (sixty_to_20 ? 2U : 7U)) {
                mode = sixty_to_20 ? 3 : 2;
                from20 = false;
            }
        } else if (mode == 2) {
            count(dt >= 34.0);
            if (from20 && since <= 25 && late >= 8) {
                mode = 3;
            } else if (late >= 24) {
                mode = 1;
            }
        } else {
            count(dt >= 58.0);
            if (late >= 7) {
                mode = 2;
                from20 = true;
            }
        }
        if (mode != old) {
            late = 0;
            since = 0;
        }
    }
};

struct Played {
    int changes{};               ///< mode changes from `from_s` on
    double minutes{};            ///< from `from_s` on
    double speed{};              ///< game time / wall time, all of it
    double first_change_s{-1.0}; ///< from `from_s` on
};

Played Play(ToyGame& game, Controller& ctl, double secs, double from_s = 0.0) {
    Played p;
    double t = 0.0;
    double game_time = 0.0;
    while (t < secs * 1000.0) {
        const u32 mode = game.mode;
        const double k = game.Cost();
        const double dt = (std::max)({mode * 1000.0 / 60.0, k, ctl.HoldMs(mode)});
        game.Frame(dt);
        const Output out = ctl.Step({.mode = mode, .work_ms = k, .busy_ms = k, .dt_ms = dt});
        t += dt;
        game_time += mode * 1000.0 / 60.0;
        if (out.mode_changed && out.old_mode != 0 && t >= from_s * 1000.0) {
            ++p.changes;
            if (p.first_change_s < 0.0) {
                p.first_change_s = t / 1000.0 - from_s;
            }
        }
    }
    p.minutes = (secs - from_s) / 60.0;
    p.speed = game_time / t;
    return p;
}
} // Anonymous namespace

TEST_CASE("Nx2ModePace: toy game, at most 4 mode changes a minute", "[video_core]") {
    struct Pattern {
        const char* name;
        double k20, k30, k60;
        bool sixty_to_20;
        double jitter;
        double to30_ms;
        bool r3 = false;   ///< the specification's accepted risk R3 (see below)
        u32 only_from = 0; ///< the start mode (0: each); at 30 and 60 these costs swing alone
    };
    // 20 -> 30 that does not hold (30 costs more: the game goes back to 20 within its first 25
    // frames, S15 c9) with the old to30 of 28 and the new 25; 30 above 33 with 60 below 34 (O1
    // c7-c9 busy per mode: up20, the game escapes to 60, a down push lands at 20); a stuck 60 at
    // 26; a light place. R3: the same O1 costs where 60 drops to 30 (not 20): down pushes and the
    // game's escapes go round (the specification accepts it, T2 measures it; it is faster than
    // the game alone), so only its speed is checked.
    const std::array<Pattern, 11> patterns{{
        {"20 back from 30, K30 29.9", 26.0, 29.9, 34.4, false, 0.0, 28.0},
        {"20 back from 30, K30 33.8", 26.0, 33.8, 38.9, false, 0.0, 28.0},
        {"20 back from 30, K30 33.8, jitter", 26.0, 33.8, 38.9, false, 3.0, 28.0},
        {"20 back from 30, K30 36", 26.0, 36.0, 41.0, false, 2.0, 28.0, false, 3},
        {"20 back from 30, to30 25", 26.0, 36.0, 41.0, false, 2.0, 25.0, false, 3},
        {"O1 c7-c9", 22.8, 39.3, 29.1, true, 0.0, 25.0},
        {"O1 c7-c9, to30 28", 22.8, 39.3, 29.1, true, 0.0, 28.0},
        {"O1 c7-c9, jitter", 22.8, 39.3, 29.1, true, 3.0, 25.0},
        {"stuck 60 at 26", 26.0, 26.0, 26.0, false, 0.0, 25.0},
        {"light", 8.0, 8.0, 8.0, false, 2.0, 25.0},
        {"R3: O1 c7-c9, 60 drops to 30", 22.8, 39.3, 29.1, false, 3.0, 25.0, true},
    }};
    for (const Pattern& pattern : patterns) {
        for (const u32 start_mode : {1U, 2U, 3U}) {
            if (pattern.only_from != 0 && start_mode != pattern.only_from) {
                continue;
            }
            Params params;
            params.to30_ms = pattern.to30_ms;
            Controller ctl{params};
            ToyGame game{.k20 = pattern.k20,
                         .k30 = pattern.k30,
                         .k60 = pattern.k60,
                         .mode = start_mode,
                         .sixty_to_20 = pattern.sixty_to_20,
                         .jitter = pattern.jitter};
            const Played all = Play(game, ctl, 600.0);
            ToyGame again{.k20 = pattern.k20,
                          .k30 = pattern.k30,
                          .k60 = pattern.k60,
                          .mode = start_mode,
                          .sixty_to_20 = pattern.sixty_to_20,
                          .jitter = pattern.jitter};
            Controller ctl2{params};
            const Played settled = Play(again, ctl2, 600.0, 120.0);
            // The game alone (it changes its mode on its own late frames too).
            ToyGame alone{.k20 = pattern.k20,
                          .k30 = pattern.k30,
                          .k60 = pattern.k60,
                          .mode = start_mode,
                          .sixty_to_20 = pattern.sixty_to_20,
                          .jitter = pattern.jitter};
            Params off = params;
            off.auto_push = false;
            Controller none{off};
            const Played base = Play(alone, none, 600.0);
            std::printf("  toy %-34s from %u: %2d changes in 10 min (%2d after 2 min), speed "
                        "%.2f; alone %2d, %.2f\n",
                        pattern.name, start_mode, all.changes, settled.changes, all.speed,
                        base.changes, base.speed);
            if (pattern.r3) {
                REQUIRE(all.speed >= base.speed - 0.01);
                continue;
            }
            // At most 4 a minute, over the 10 minutes and after the first 2.
            REQUIRE(all.changes <= 40);
            REQUIRE(settled.changes <= 4.0 * settled.minutes);
        }
    }
}

TEST_CASE("Nx2ModePace: toy game, a stuck 60 after a load gets its down push", "[video_core]") {
    // 20 s of loading at 60: 25 ms frames and a 150 ms stall every 3 s, the game ignoring the
    // holds (it stays at 60); then 26 ms frames without stalls where it follows them. The aborted
    // pushes of the load must not keep the down push away (O1: 138 s stuck without a change).
    Controller ctl;
    double t = 0.0;
    int aborted = 0;
    while (t < 20000.0) {
        const double since = t;
        const Output stall =
            ctl.Step({.mode = 1, .work_ms = 150.0, .busy_ms = 25.0, .dt_ms = 150.0});
        aborted += stall.result == Result::Aborted ? 1 : 0;
        t += 150.0;
        while (t - since < 3000.0) {
            const double dt = (std::max)(25.0, ctl.HoldMs(1));
            const Output out = ctl.Step({.mode = 1, .work_ms = 25.0, .busy_ms = 25.0, .dt_ms = dt});
            aborted += out.result == Result::Aborted ? 1 : 0;
            t += dt;
        }
    }
    REQUIRE(aborted >= 3);
    REQUIRE(ctl.Aborts(Push::Down) == static_cast<u32>(aborted));
    REQUIRE(!ctl.Blocked(Push::Down));
    ToyGame game{.k20 = 26.0, .k30 = 26.0, .k60 = 26.0, .mode = 1};
    const Played stuck = Play(game, ctl, 120.0);
    std::printf("  toy load then stuck 60: %d aborted, first change %.1f s after the load\n",
                aborted, stuck.first_change_s);
    REQUIRE(stuck.first_change_s >= 0.0);
    REQUIRE(stuck.first_change_s <= 70.0);
    REQUIRE(game.mode == 2);
}
