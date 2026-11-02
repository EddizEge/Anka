// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cmath>
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
    REQUIRE(fed.Last().frames == 30u);
    REQUIRE(fed.Last().min_period_ms == 0.0);
    REQUIRE(fed.Last().rest_s == DEFAULTS.down_rest_s);
    REQUIRE(fed.Last().tries == 1u);
    // No push during the 2 s of rest.
    const Fed rest = Feed(ctl, 1, 26.0, 26.0, 26.0, 76);
    REQUIRE(rest.Starts() == 0);
}

TEST_CASE("Nx2ModePace: failed down pushes back off, then stop until the game moves", "[video_core]") {
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
    const Output up = UntilStart(ctl, 2, 8.0, 8.0, Period(2), 2000, &start);
    REQUIRE(up.started == Push::Up);
    REQUIRE(up.by_probe);
    REQUIRE((start + 1) * Period(2) / 1000.0 <= 15.0);
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
    REQUIRE(ctl.Tries(Push::Down) == 1u);
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
    // A stall every 3.5 s: pushes start and are aborted, three times, then no more.
    Controller rare;
    starts = 0;
    for (int k = 0; k < 40; ++k) {
        starts += Feed(rare, 1, 25.0, 25.0, 25.0, 140).Starts();
        rare.Step({.mode = 1, .work_ms = 150.0, .busy_ms = 25.0, .dt_ms = 150.0});
    }
    REQUIRE(starts >= 1);
    REQUIRE(starts <= 3);
    REQUIRE(rare.Blocked(Push::Down) == (starts == 3));
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
        SleepUntil(target, spin);
        const auto woke = steady_clock::now();
        REQUIRE(woke >= target);
        if (spin == 20000U) {
            REQUIRE(woke < target + milliseconds{2});
        }
    }
}
