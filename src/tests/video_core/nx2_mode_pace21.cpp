// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_mode_pace21_logic.h"

namespace {
using namespace VideoCommon::Nx2ModePace;

double Period(u32 mode) {
    return mode * 1000.0 / 60;
}

Output21 Step(Controller21& ctl, u32 mode, double cost, double dt) {
    return ctl.Step({.mode = mode, .work_ms = cost, .busy_ms = cost, .dt_ms = dt});
}

struct Fed {
    std::vector<Output21> outs;
    int started_at{-1};
    int ended_at{-1};

    int Starts() const {
        int n = 0;
        for (const Output21& o : outs) {
            n += o.started != Push21::None ? 1 : 0;
        }
        return n;
    }
};

/// n presents of one mode and cost, each `dt` apart (the mode's period by default).
Fed Feed(Controller21& ctl, u32 mode, double cost, int n, double dt = 0.0) {
    Fed fed;
    for (int i = 0; i < n; ++i) {
        fed.outs.push_back(Step(ctl, mode, cost, dt > 0.0 ? dt : Period(mode)));
        if (fed.started_at < 0 && fed.outs.back().started != Push21::None) {
            fed.started_at = i;
        }
        if (fed.ended_at < 0 && fed.outs.back().ended != Push21::None) {
            fed.ended_at = i;
        }
    }
    return fed;
}

/// Bananza as runs 73 and 79 showed it, from the frame the controller gives it (request to request
/// is the present's period here): at 60, three frames of 34-41.7 ms take it to 30 and three of 42 or
/// more to 20; at 30, three of 42 or more to 20; at 20, one of 36.6 or less to 30 at the next
/// present. Never up by itself, unless at 30 `escape30` frames of 34 ms or more in a row take it to
/// 60; `drop30` of them take it to 20 (0: off).
struct ToyGame {
    u32 mode{3};
    u32 drop30{};
    u32 escape30{};
    u32 late30{};
    u32 late20{};
    u32 slow30{};
    bool up_next{};

    void See(double frame) {
        if (up_next) {
            up_next = false;
            mode = 2;
            late30 = late20 = 0;
            return;
        }
        if (mode == 3) {
            up_next = frame <= 36.6;
            return;
        }
        late20 = frame >= 42.0 ? late20 + 1 : 0;
        late30 = mode == 1 && frame >= 34.0 && frame < 42.0 ? late30 + 1 : 0;
        slow30 = mode == 2 && frame >= 34.0 ? slow30 + 1 : 0;
        if (late20 >= 3) {
            mode = 3;
            late20 = late30 = slow30 = 0;
        } else if (late30 >= 3) {
            mode = 2;
            late20 = late30 = slow30 = 0;
        } else if (escape30 != 0 && slow30 >= escape30) {
            mode = 1;
            late20 = late30 = slow30 = 0;
        } else if (drop30 != 0 && slow30 >= drop30) {
            mode = 3;
            late20 = late30 = slow30 = 0;
        }
    }
};

struct Run {
    std::vector<u32> modes; ///< the game's mode at each present
    std::vector<double> at; ///< the present's time (s)
    std::vector<Output21> outs;
    int pushes{};
    int holds_over_41{}; ///< presents held 41.7 ms or more
    double seconds_at(u32 mode) const {
        double s = 0.0;
        for (const u32 m : modes) {
            s += m == mode ? Period(m) / 1000.0 : 0.0;
        }
        return s;
    }
    /// The game's mode changes from `from_s` on.
    int changes_from(double from_s) const {
        int n = 0;
        for (std::size_t i = 1; i < modes.size(); ++i) {
            n += at[i] >= from_s && modes[i] != modes[i - 1] ? 1 : 0;
        }
        return n;
    }
};

/// The controller driving the toy game for n presents of frame cost costs[mode - 1] (ms, busy and
/// work), or for `seconds` when n is 0.
Run Drive(Controller21& ctl, ToyGame& game, const std::array<double, 3>& costs, int n,
          double seconds = 0.0) {
    Run run;
    double release = 0.0;
    double clock = 0.0;
    for (int i = 0; n > 0 ? i < n : clock < seconds; ++i) {
        // As nx2_gsv.h: the hold for the present's mode, the release from the previous present.
        const u32 mode = game.mode;
        const double cost = costs[mode - 1];
        const double hold = ctl.HoldMs(mode);
        double frame = (std::max)(Period(mode), cost);
        if (hold > 0.0) {
            frame = (std::max)(frame, hold);
            run.holds_over_41 += hold >= 41.7 ? 1 : 0;
        }
        if (release > 0.0) {
            frame = (std::max)((std::min)(frame, release), cost);
        }
        const Output21 out = Step(ctl, mode, cost, frame);
        clock += frame / 1000.0;
        run.pushes += out.started != Push21::None ? 1 : 0;
        release = ctl.ReleaseMs(mode);
        run.modes.push_back(mode);
        run.at.push_back(clock);
        run.outs.push_back(out);
        game.See(frame);
    }
    return run;
}

Run Drive(Controller21& ctl, ToyGame& game, double cost, int n) {
    return Drive(ctl, game, {cost, cost, cost}, n);
}
} // Anonymous namespace

TEST_CASE("Nx2ModePace21: 30 stays where 30 fits", "[video_core]") {
    for (const double cost : {5.0, 12.0, 22.0, 28.0, 30.0, 31.0}) {
        Controller21 ctl;
        const Fed fed = Feed(ctl, 2, cost, 1800); // 60 s
        REQUIRE(fed.Starts() == 0);
        for (const Output21& o : fed.outs) {
            REQUIRE(o.hold_ms == 0.0);
            REQUIRE(o.release_ms == 0.0);
        }
        REQUIRE(fed.outs.back().why == Why::Fits30);
    }
}

TEST_CASE("Nx2ModePace21: to20 only after C stays above cap20 for its dwell", "[video_core]") {
    const Params21 p{};
    Controller21 ctl;
    // 1 s quiet after the first present's mode change, then 3 s of C 33 ms.
    const Fed fed = Feed(ctl, 2, 33.0, 400, 33.4);
    REQUIRE(fed.started_at >= 0);
    REQUIRE(fed.outs[static_cast<size_t>(fed.started_at)].started == Push21::To20);
    const double at_s = fed.started_at * 33.4 / 1000.0;
    REQUIRE(at_s >= p.mode_quiet_s + p.to20_dwell_s - 0.05);
    REQUIRE(at_s <= p.mode_quiet_s + p.to20_dwell_s + 0.4);
    REQUIRE(fed.outs[static_cast<size_t>(fed.started_at)].hold_ms == p.to20_hold_ms);
    REQUIRE(fed.ended_at - fed.started_at == static_cast<int>(p.to20_frames));
    REQUIRE(fed.outs[static_cast<size_t>(fed.ended_at) - 1].hold_ms == p.to20_hold_ms);
    REQUIRE(fed.outs[static_cast<size_t>(fed.ended_at)].result == Result::GaveUp);
    REQUIRE(fed.outs[static_cast<size_t>(fed.ended_at)].hold_ms == 0.0);
}

TEST_CASE("Nx2ModePace21: a short C dip restarts the to20 dwell", "[video_core]") {
    Controller21 ctl;
    Feed(ctl, 2, 33.0, 60, 33.4);         // 2 s
    const Fed dip = Feed(ctl, 2, 20.0, 9); // the median falls under cap20
    REQUIRE(dip.Starts() == 0);
    const Fed again = Feed(ctl, 2, 33.0, 60, 33.4); // 2 s: under the dwell from the dip
    REQUIRE(again.Starts() == 0);
}

TEST_CASE("Nx2ModePace21: rel20 where 30 fits, releasing rel_frames presents", "[video_core]") {
    const Params21 p{};
    Controller21 ctl;
    const Fed fed = Feed(ctl, 3, 12.0, 120);
    REQUIRE(fed.started_at >= 0);
    const Output21& start = fed.outs[static_cast<size_t>(fed.started_at)];
    REQUIRE(start.started == Push21::Rel20);
    REQUIRE(start.release_ms == p.rel_ms);
    REQUIRE(start.hold_ms == 0.0);
    const double at_s = fed.started_at * Period(3) / 1000.0;
    REQUIRE(at_s >= p.mode_quiet_s + p.rel_dwell_s - 0.06);
    REQUIRE(at_s <= p.mode_quiet_s + p.rel_dwell_s + 0.5);
    // The start and rel_frames - 1 more presents release; then rel_wait presents without.
    int released = 0;
    for (int i = fed.started_at; i < fed.ended_at; ++i) {
        released += fed.outs[static_cast<size_t>(i)].release_ms > 0.0 ? 1 : 0;
    }
    REQUIRE(released == static_cast<int>(p.rel_frames));
    REQUIRE(fed.ended_at - fed.started_at == static_cast<int>(p.rel_frames + p.rel_wait));
    const Output21& end = fed.outs[static_cast<size_t>(fed.ended_at)];
    REQUIRE(end.ended == Push21::Rel20);
    REQUIRE(end.result == Result::GaveUp);
    REQUIRE(end.fails == 1);
    REQUIRE(end.rest_s == p.rel_rest_s);
}

TEST_CASE("Nx2ModePace21: failed rel20 rests double, without a block", "[video_core]") {
    const Params21 p{};
    Controller21 ctl;
    const Fed fed = Feed(ctl, 3, 12.0, 20 * 60 * 5); // 5 minutes at 20 that never goes up
    std::vector<double> rests;
    for (const Output21& o : fed.outs) {
        if (o.ended == Push21::Rel20) {
            rests.push_back(o.rest_s);
        }
    }
    REQUIRE(rests.size() >= 6);
    REQUIRE(rests[0] == p.rel_rest_s);
    REQUIRE(rests[1] == 2 * p.rel_rest_s);
    REQUIRE(rests[2] == 4 * p.rel_rest_s);
    REQUIRE(rests.back() == p.rel_rest_max_s);
    REQUIRE(fed.outs.back().why != Why::Short30At20);
}

TEST_CASE("Nx2ModePace21: no rel20 where 30 does not fit", "[video_core]") {
    for (const double cost : {28.5, 31.0, 40.0}) {
        Controller21 ctl;
        const Fed fed = Feed(ctl, 3, cost, 1200);
        REQUIRE(fed.Starts() == 0);
        REQUIRE(fed.outs.back().why == Why::Short30At20);
    }
}

TEST_CASE("Nx2ModePace21: rel20 done; 20 again within verify30 fails it", "[video_core]") {
    const Params21 p{};
    Controller21 first;
    const Fed fed = Feed(first, 3, 12.0, 120);
    REQUIRE(fed.started_at >= 0);
    Controller21 ctl;
    Feed(ctl, 3, 12.0, fed.started_at + 1);
    REQUIRE(ctl.Pushing() == Push21::Rel20);
    REQUIRE(ctl.ReleaseMs(3) == p.rel_ms);
    Controller21 copy = ctl;
    // The game takes 30.
    const Output21 up = Step(ctl, 2, 12.0, 30.0);
    REQUIRE(up.ended == Push21::Rel20);
    REQUIRE(up.result == Result::Done);
    REQUIRE(up.mode_changed);
    REQUIRE(up.by == Push21::Rel20);
    REQUIRE(ctl.ReleaseMs(2) == 0.0);
    // 5 s later it drops to 20 by itself: rel20 failed after all.
    Feed(ctl, 2, 12.0, 150);
    const Output21 down = Step(ctl, 3, 12.0, 50.0);
    REQUIRE(down.unverified == Push21::Rel20);
    REQUIRE(down.fails == 1);
    REQUIRE(ctl.Rest(Push21::Rel20) > 0.0);
    // After verify30 the same drop is the game's alone.
    Step(copy, 2, 12.0, 30.0);
    Feed(copy, 2, 12.0, 330); // 11 s
    const Output21 late = Step(copy, 3, 12.0, 50.0);
    REQUIRE(late.unverified == Push21::None);
    REQUIRE(copy.Fails(Push21::Rel20) == 0);
}

TEST_CASE("Nx2ModePace21: rel20 done when the game goes up to 60", "[video_core]") {
    const Params21 p{};
    Controller21 first;
    const Fed fed = Feed(first, 3, 12.0, 120);
    REQUIRE(fed.started_at >= 0);
    Controller21 ctl;
    Feed(ctl, 3, 12.0, fed.started_at + 1);
    REQUIRE(ctl.Pushing() == Push21::Rel20);
    Controller21 copy = ctl;
    const Output21 up = Step(ctl, 1, 12.0, 30.0);
    REQUIRE(up.ended == Push21::Rel20);
    REQUIRE(up.result == Result::Done);
    REQUIRE(up.fails == 0);
    REQUIRE(ctl.Fails(Push21::Rel20) == 0);
    REQUIRE(ctl.Rest(Push21::Rel20) == 0.0);
    // That 60 drops to 20 within verify30: rel20 failed after all.
    Step(copy, 1, 12.0, 30.0);
    Feed(copy, 1, 12.0, 120); // 2 s
    const Output21 down = Step(copy, 3, 12.0, 50.0);
    REQUIRE(down.unverified == Push21::Rel20);
    REQUIRE(down.rest_s == p.rel_rest_s);
}

TEST_CASE("Nx2ModePace21: 60 stays where 60 fits, down to 30 inside its window", "[video_core]") {
    const Params21 p{};
    {
        Controller21 ctl;
        const Fed fed = Feed(ctl, 1, 14.0, 3600);
        REQUIRE(fed.Starts() == 0);
        REQUIRE(fed.outs.back().why == Why::Fits60);
    }
    Controller21 ctl;
    const Fed fed = Feed(ctl, 1, 20.0, 200, 20.0);
    REQUIRE(fed.started_at >= 0);
    const Output21& start = fed.outs[static_cast<size_t>(fed.started_at)];
    REQUIRE(start.started == Push21::Down);
    REQUIRE(!start.down_to20);
    REQUIRE(start.hold_ms == p.down_hold_ms);
    REQUIRE(start.hold_ms < 41.7);
    REQUIRE(start.hold_ms > 34.0);
    REQUIRE(fed.outs[static_cast<size_t>(fed.ended_at)].result == Result::GaveUp);
    REQUIRE(fed.ended_at - fed.started_at == static_cast<int>(p.down_frames));
    REQUIRE(fed.outs[static_cast<size_t>(fed.ended_at)].rest_s == p.down_rest_s);
    // Done at 30, done20 at 20: both reset the rests.
    for (const u32 to : {2U, 3U}) {
        Controller21 again;
        Feed(again, 1, 20.0, fed.started_at + 1, 20.0);
        REQUIRE(again.Pushing() == Push21::Down);
        REQUIRE(again.HoldMs(1) == p.down_hold_ms);
        REQUIRE(again.HoldMs(2) == 0.0);
        const Output21 end = Step(again, to, 20.0, 37.0);
        REQUIRE(end.result == (to == 2 ? Result::Done : Result::Done20));
        REQUIRE(end.hold_ms == 0.0);
        REQUIRE(again.Rest(Push21::Down) == 0.0);
        REQUIRE(again.Pushing() == Push21::None);
    }
}

TEST_CASE("Nx2ModePace21: no down where the game drops by itself", "[video_core]") {
    Controller21 ctl;
    const Fed fed = Feed(ctl, 1, 36.0, 300, 36.0);
    REQUIRE(fed.Starts() == 0);
    REQUIRE(fed.outs.back().why == Why::Self60);
}

TEST_CASE("Nx2ModePace21: down takes 60 to 20 only after a full 30", "[video_core]") {
    const Params21 p{};
    // The game escapes a full 30 (C 32) to 60: the down push holds to20_hold_ms.
    Controller21 ctl;
    Feed(ctl, 2, 32.0, 40, 33.4); // escapes before to20's dwell
    const Fed fed = Feed(ctl, 1, 25.0, 200, 25.0);
    REQUIRE(fed.started_at >= 0);
    REQUIRE(fed.outs[static_cast<size_t>(fed.started_at)].down_to20);
    REQUIRE(fed.outs[static_cast<size_t>(fed.started_at)].hold_ms == p.to20_hold_ms);
    // A 30 at C 25 (fits): 30.
    Controller21 fits;
    Feed(fits, 2, 25.0, 40);
    const Fed fed2 = Feed(fits, 1, 25.0, 200, 25.0);
    REQUIRE(fed2.started_at >= 0);
    REQUIRE(!fed2.outs[static_cast<size_t>(fed2.started_at)].down_to20);
    // Two escapes from a 30 at C 29.5 (between cap30 and cap20) within escape_s: 20.
    Controller21 two;
    Feed(two, 2, 29.5, 40);
    const Fed first = Feed(two, 1, 25.0, 200, 25.0);
    REQUIRE(first.started_at >= 0);
    REQUIRE(!first.outs[static_cast<size_t>(first.started_at)].down_to20);
    Step(two, 2, 25.0, 37.0); // the down push's 30
    Feed(two, 2, 29.5, 40);
    const Fed second = Feed(two, 1, 25.0, 200, 25.0);
    REQUIRE(second.started_at >= 0);
    REQUIRE(second.outs[static_cast<size_t>(second.started_at)].down_to20);
}

TEST_CASE("Nx2ModePace21: a stall aborts a push and rests it", "[video_core]") {
    const Params21 p{};
    Controller21 ctl;
    const Fed fed = Feed(ctl, 3, 12.0, 120);
    REQUIRE(fed.started_at >= 0);
    Controller21 c2;
    Feed(c2, 3, 12.0, fed.started_at + 1);
    REQUIRE(c2.Pushing() == Push21::Rel20);
    const Output21 stall = Step(c2, 3, 150.0, 150.0);
    REQUIRE(stall.ended == Push21::Rel20);
    REQUIRE(stall.result == Result::Aborted);
    REQUIRE(stall.why == Why::Stall);
    REQUIRE(stall.release_ms == 0.0);
    REQUIRE(c2.ReleaseMs(3) == 0.0);
    REQUIRE(c2.Rest(Push21::Rel20) == p.rel_rest_s);
    // Nothing within stall_quiet_s.
    const Fed after = Feed(c2, 3, 12.0, 35);
    REQUIRE(after.Starts() == 0);
}

TEST_CASE("Nx2ModePace21: rel60 only when switched on", "[video_core]") {
    Controller21 off;
    REQUIRE(Feed(off, 2, 8.0, 1200).Starts() == 0);
    Params21 p{};
    p.up_rel = true;
    Controller21 on{p};
    const Fed fed = Feed(on, 2, 8.0, 600);
    REQUIRE(fed.started_at >= 0);
    REQUIRE(fed.outs[static_cast<size_t>(fed.started_at)].started == Push21::Rel60);
    REQUIRE(fed.outs[static_cast<size_t>(fed.started_at)].release_ms == p.up_rel_ms);
    const double at_s = fed.started_at * Period(2) / 1000.0;
    REQUIRE(at_s >= p.mode_quiet_s + p.up_dwell_s - 0.05);
}

TEST_CASE("Nx2ModePace21: probes", "[video_core]") {
    REQUIRE(!ParseProbe21("up20:10"));
    REQUIRE(!ParseProbe21("rel20"));
    REQUIRE(!ParseProbe21("rel20:0.5"));
    REQUIRE(ParseProbe21(" rel20:10 ")->kind == Push21::Rel20);
    REQUIRE(ParseProbe21("cycle:2.5")->cycle);
    REQUIRE(ParseProbe21("cycle:2.5")->every_s == 2.5);
    REQUIRE(ParseProbe21("to20:30")->kind == Push21::To20);
    Params21 p{};
    p.auto_push = false;
    Controller21 ctl{p, ParseProbe21("rel20:10")};
    // C 40 (30 does not fit): the probe pushes anyway, auto does not.
    const Fed fed = Feed(ctl, 3, 40.0, 20 * 25);
    REQUIRE(fed.Starts() >= 1);
    for (const Output21& o : fed.outs) {
        REQUIRE((o.started == Push21::None || o.by_probe));
        if (o.ended != Push21::None) {
            REQUIRE(o.fails == 0); // probes leave the rests alone
        }
    }
    REQUIRE(ctl.Rest(Push21::Rel20) == 0.0);
}

TEST_CASE("Nx2ModePace21: toy game: 20 goes back to 30 and stays", "[video_core]") {
    for (const double cost : {6.0, 14.0, 22.0, 27.0}) {
        Controller21 ctl;
        ToyGame game{.mode = 3};
        const Run run = Drive(ctl, game, cost, 20 * 120);
        REQUIRE(game.mode == 2);
        REQUIRE(run.seconds_at(3) < 6.0);
        REQUIRE(run.pushes == 1);
        REQUIRE(run.holds_over_41 == 0);
    }
}

TEST_CASE("Nx2ModePace21: toy game: 60 that does not fit goes to 30, not 20", "[video_core]") {
    for (const double cost : {17.0, 21.0, 26.0, 30.0}) {
        Controller21 ctl;
        ToyGame game{.mode = 1};
        const Run run = Drive(ctl, game, cost, 60 * 120);
        REQUIRE(game.mode == 2);
        REQUIRE(run.seconds_at(3) == 0.0);
        REQUIRE(run.holds_over_41 == 0);
    }
}

TEST_CASE("Nx2ModePace21: toy game: 20 only where 30 does not fit", "[video_core]") {
    {
        Controller21 ctl;
        ToyGame game{.mode = 2};
        const Run run = Drive(ctl, game, 34.0, 30 * 60);
        REQUIRE(game.mode == 3);
        REQUIRE(run.pushes == 1);
    }
    {
        // Between cap30 and cap20: neither pushes, whatever the mode.
        Controller21 ctl;
        ToyGame game{.mode = 2};
        const Run run = Drive(ctl, game, 29.5, 30 * 120);
        REQUIRE(game.mode == 2);
        REQUIRE(run.pushes == 0);
        Controller21 c20;
        ToyGame g20{.mode = 3};
        const Run r20 = Drive(c20, g20, 29.5, 20 * 120);
        REQUIRE(g20.mode == 3);
        REQUIRE(r20.pushes == 0);
    }
}

TEST_CASE("Nx2ModePace21: capacity changes: 30 -> 20 -> 30 without a round", "[video_core]") {
    Controller21 ctl;
    ToyGame game{.mode = 2};
    Drive(ctl, game, 12.0, 30 * 30);
    REQUIRE(game.mode == 2);
    Drive(ctl, game, 35.0, 30 * 30); // 30 s heavy: to 20
    REQUIRE(game.mode == 3);
    const Run heavy = Drive(ctl, game, 35.0, 20 * 60);
    REQUIRE(heavy.pushes == 0);
    REQUIRE(game.mode == 3);
    const Run light = Drive(ctl, game, 12.0, 20 * 60); // light again: back to 30
    REQUIRE(game.mode == 2);
    REQUIRE(light.pushes == 1);
}

TEST_CASE("Nx2ModePace21: to20 soon after rel20 fails rel20 (no 20 <-> 30 round)", "[video_core]") {
    const Params21 p{};
    Controller21 first;
    const Fed fed = Feed(first, 3, 12.0, 120);
    REQUIRE(fed.started_at >= 0);
    Controller21 ctl;
    Feed(ctl, 3, 12.0, fed.started_at + 1);
    REQUIRE(Step(ctl, 2, 12.0, 30.0).result == Result::Done);
    // 30 does not fit after all: to20 within verify30.
    const Fed heavy = Feed(ctl, 2, 35.0, 200, 35.0);
    REQUIRE(heavy.started_at >= 0);
    REQUIRE(heavy.outs[static_cast<size_t>(heavy.started_at)].started == Push21::To20);
    REQUIRE((heavy.started_at + 1) * 35.0 / 1000.0 < p.verify30_s);
    // Restart from the to20 start: the game takes 20 at once.
    Controller21 again;
    Feed(again, 3, 12.0, fed.started_at + 1);
    Step(again, 2, 12.0, 30.0);
    Feed(again, 2, 35.0, heavy.started_at + 1, 35.0);
    REQUIRE(again.Pushing() == Push21::To20);
    const Output21 down = Step(again, 3, 35.0, 42.0);
    REQUIRE(down.ended == Push21::To20);
    REQUIRE(down.result == Result::Done);
    REQUIRE(down.unverified == Push21::Rel20);
    REQUIRE(again.Rest(Push21::Rel20) == p.rel_rest_s);
    REQUIRE(again.Fails(Push21::To20) == 0);
}

TEST_CASE("Nx2ModePace21: a 20 the escapes chose spends them", "[video_core]") {
    Controller21 ctl;
    Feed(ctl, 2, 32.0, 40, 33.4);
    Controller21 probe = ctl;
    const Fed fed = Feed(probe, 1, 25.0, 200, 25.0);
    REQUIRE(fed.started_at >= 0);
    Feed(ctl, 1, 25.0, fed.started_at + 1, 25.0);
    REQUIRE(ctl.HoldMs(1) == Params21{}.to20_hold_ms);
    REQUIRE(Step(ctl, 3, 25.0, 42.0).result == Result::Done20);
    // The game goes to 60 from that 20 (say a load): the next down push is to 30.
    Feed(ctl, 3, 25.0, 20);
    const Fed next = Feed(ctl, 1, 25.0, 200, 25.0);
    REQUIRE(next.started_at >= 0);
    REQUIRE(!next.outs[static_cast<size_t>(next.started_at)].down_to20);
}

TEST_CASE("Nx2ModePace21: rel60 whose 60 does not last fails", "[video_core]") {
    Params21 p{};
    p.up_rel = true;
    Controller21 first{p};
    const Fed fed = Feed(first, 2, 8.0, 600);
    REQUIRE(fed.started_at >= 0);
    Controller21 ctl{p};
    Feed(ctl, 2, 8.0, fed.started_at + 1);
    REQUIRE(ctl.ReleaseMs(2) == p.up_rel_ms);
    REQUIRE(Step(ctl, 1, 8.0, 14.0).result == Result::Done);
    Feed(ctl, 1, 8.0, 180); // 3 s
    const Output21 back = Step(ctl, 2, 8.0, 33.3);
    REQUIRE(back.unverified == Push21::Rel60);
    REQUIRE(ctl.Rest(Push21::Rel60) == p.up_rest_s);
}

TEST_CASE("Nx2ModePace21: rel60's failures stay until its 60 lasts", "[video_core]") {
    Params21 p{};
    p.up_rel = true;
    Controller21 ctl{p};
    // A 30 that never goes up: the first rel60 gives up.
    Output21 end{};
    for (int i = 0; i < 30 * 60 && end.ended == Push21::None; ++i) {
        end = Step(ctl, 2, 8.0, Period(2));
    }
    REQUIRE(end.ended == Push21::Rel60);
    REQUIRE(end.result == Result::GaveUp);
    REQUIRE(ctl.Fails(Push21::Rel60) == 1);
    // The next one is done: its failure stays until its 60 lasted verify60.
    while (ctl.Pushing() != Push21::Rel60) {
        Step(ctl, 2, 8.0, Period(2));
    }
    REQUIRE(Step(ctl, 1, 8.0, p.up_rel_ms).result == Result::Done);
    REQUIRE(ctl.Fails(Push21::Rel60) == 1);
    REQUIRE(ctl.Rest(Push21::Rel60) == 0.0);
    Feed(ctl, 1, 8.0, 540); // 9 s
    REQUIRE(ctl.Fails(Push21::Rel60) == 1);
    Feed(ctl, 1, 8.0, 120); // 11 s
    REQUIRE(ctl.Fails(Push21::Rel60) == 0);
}

TEST_CASE("Nx2ModePace21: no push of a kind while it rests", "[video_core]") {
    const Params21 p{};
    Controller21 ctl;
    const Fed fed = Feed(ctl, 3, 12.0, 20 * 120);
    int last_end = -1;
    double last_rest = 0.0;
    int checked = 0;
    for (int i = 0; i < static_cast<int>(fed.outs.size()); ++i) {
        const Output21& o = fed.outs[static_cast<size_t>(i)];
        if (o.started != Push21::None && last_end >= 0) {
            REQUIRE((i - last_end) * Period(3) / 1000.0 >= last_rest + p.rel_dwell_s - 0.06);
            ++checked;
        }
        if (o.ended != Push21::None) {
            last_end = i;
            last_rest = o.rest_s;
        }
    }
    REQUIRE(checked >= 3);
}

TEST_CASE("Nx2ModePace21: nothing within stall_quiet of a stall", "[video_core]") {
    const Params21 p{};
    Controller21 ctl;
    Feed(ctl, 1, 20.0, 10, 20.0); // within 60's quiet time
    Step(ctl, 1, 150.0, 150.0);
    const Fed quiet = Feed(ctl, 1, 20.0, 90, 20.0); // 1.8 s
    REQUIRE(quiet.Starts() == 0);
    REQUIRE(quiet.outs.back().why == Why::Stall);
    const Fed after = Feed(ctl, 1, 20.0, 60, 20.0);
    REQUIRE(after.started_at >= 0);
    REQUIRE((90 + after.started_at + 1) * 20.0 / 1000.0 >= p.stall_quiet_s);
}

TEST_CASE("Nx2ModePace21: C is the larger of work and busy", "[video_core]") {
    for (const bool busy_high : {false, true}) {
        Controller21 ctl;
        Output21 out{};
        for (int i = 0; i < 20; ++i) {
            out = ctl.Step({.mode = 2,
                            .work_ms = busy_high ? 5.0 : 33.0,
                            .busy_ms = busy_high ? 33.0 : 5.0,
                            .dt_ms = 33.4});
        }
        REQUIRE(out.cap_ms == 33.0);
        Output21 started{};
        for (int i = 0; i < 200 && started.started == Push21::None; ++i) {
            started = ctl.Step({.mode = 2,
                                .work_ms = busy_high ? 5.0 : 33.0,
                                .busy_ms = busy_high ? 33.0 : 5.0,
                                .dt_ms = 33.4});
        }
        REQUIRE(started.started == Push21::To20);
    }
}

TEST_CASE("Nx2ModePace21: the window: 8 frames, emptied at a mode change", "[video_core]") {
    Params21 p{};
    p.mode_quiet60_s = 0.0;
    p.down_dwell_s = 0.0;
    // From the change, no C (no decision) before the 8th frame.
    Controller21 ctl{p};
    const Fed fed = Feed(ctl, 1, 20.0, 30, 20.0);
    REQUIRE(fed.started_at == static_cast<int>(Controller21::MinSamples) - 1);
    for (int i = 0; i < fed.started_at; ++i) {
        REQUIRE(std::isnan(fed.outs[static_cast<size_t>(i)].cap_ms));
        REQUIRE(fed.outs[static_cast<size_t>(i)].why == Why::NoWindow);
    }
    // 30's frames do not count at 60.
    Controller21 c2{p};
    Feed(c2, 2, 25.0, 300);
    const Fed at60 = Feed(c2, 1, 10.0, 600);
    REQUIRE(at60.Starts() == 0);
}

TEST_CASE("Nx2ModePace21: toy game: a 30 that does not last slows the 20 <-> 30 round",
          "[video_core]") {
    const Params21 p{};
    // 20 fits (C 20), 30 does not (C 36): the game leaves rel20's 30 for 20 by itself, by to20, or
    // by an escape to a 60 that does not fit (C 45) either.
    struct Case {
        const char* name;
        u32 drop30;
        u32 escape30;
    };
    for (const Case& c : {Case{"by itself", 60, 0}, Case{"to20", 0, 0}, Case{"via 60", 0, 45}}) {
        Controller21 ctl;
        ToyGame game{.mode = 3, .drop30 = c.drop30, .escape30 = c.escape30};
        const Run run = Drive(ctl, game, {45.0, 36.0, 20.0}, 0, 600.0); // 10 minutes
        std::vector<double> rests;
        for (const Output21& o : run.outs) {
            if (o.unverified == Push21::Rel20) {
                rests.push_back(o.rest_s);
            }
            REQUIRE(o.result != Result::GaveUp); // every rel20 takes 20 up
        }
        REQUIRE(rests.size() >= 6);
        REQUIRE(rests[0] == p.rel_rest_s);
        REQUIRE(rests[1] == 2 * p.rel_rest_s);
        REQUIRE(rests[2] == 4 * p.rel_rest_s);
        REQUIRE(rests.back() == p.rel_rest_max_s);
        // Once the rests are long, at most 2 rounds (2 or 3 changes each) a minute.
        REQUIRE(run.changes_from(300.0) <= 3 * 2 * 5);
        REQUIRE(run.seconds_at(3) > 400.0);
    }
}

TEST_CASE("Nx2ModePace21: rel20's failures stay until its 30 lasts", "[video_core]") {
    const Params21 p{};
    Controller21 ctl;
    ToyGame game{.mode = 3, .drop30 = 60};
    // Two rounds where 30 does not fit: two failures.
    Drive(ctl, game, {45.0, 36.0, 20.0}, 0, 14.0);
    REQUIRE(ctl.Fails(Push21::Rel20) == 2);
    REQUIRE(game.mode == 3);
    // Now 30 fits: the next rel20 is done, its rest goes, its failures stay ...
    Run run = Drive(ctl, game, {45.0, 20.0, 20.0}, 0, 7.5);
    REQUIRE(game.mode == 2);
    REQUIRE(run.pushes == 1);
    double done_at = -1.0;
    for (std::size_t i = 0; i < run.outs.size(); ++i) {
        if (run.outs[i].ended == Push21::Rel20) {
            REQUIRE(run.outs[i].result == Result::Done);
            done_at = run.at[i];
        }
    }
    REQUIRE(done_at >= 0.0);
    REQUIRE(ctl.Fails(Push21::Rel20) == 2);
    REQUIRE(ctl.Rest(Push21::Rel20) == 0.0);
    // ... until that 30 lasted verify30.
    run = Drive(ctl, game, {45.0, 20.0, 20.0}, 0, 12.5);
    REQUIRE(7.5 + run.at.back() - done_at > p.verify30_s);
    REQUIRE(game.mode == 2);
    REQUIRE(ctl.Fails(Push21::Rel20) == 0);
    // 30 is too heavy again: the game's own drop is no rel20 failure, and the next round's first
    // failure rests rel_rest_s, not 4 times that.
    run = Drive(ctl, game, {45.0, 36.0, 20.0}, 0, 12.0);
    REQUIRE(game.mode == 3);
    int failed = 0;
    for (const Output21& o : run.outs) {
        if (o.unverified == Push21::Rel20) {
            REQUIRE(o.fails == 1);
            REQUIRE(o.rest_s == p.rel_rest_s);
            ++failed;
        }
    }
    REQUIRE(failed == 1);
}
