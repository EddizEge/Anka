// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_ab.h"

namespace {
using namespace Common::Nx2Ab;

// Feeds `count` presents, each `frame_us` after the one before; collects the finished blocks.
struct Feed {
    Blocks blocks;
    long long now{};
    std::vector<Block> done;
    u32 flips{};

    explicit Feed(Config config) : blocks{config} {}

    void Run(u32 count, long long frame_us) {
        for (u32 i = 0; i < count; ++i) {
            now += frame_us;
            const auto step = blocks.Present(now);
            if (step.flipped) {
                ++flips;
                REQUIRE(step.block.has_value());
                done.push_back(*step.block);
            } else {
                REQUIRE(!step.block.has_value());
            }
        }
    }
};

} // namespace

TEST_CASE("Nx2Ab: configuration", "[nx2_ab]") {
    CHECK(ParseConfig(nullptr, nullptr, nullptr).toggle == 0);
    CHECK(ParseConfig("", "10", "B").toggle == 0);
    CHECK(ParseConfig("0", "10", "B").toggle == 0);
    CHECK(ParseConfig("12x", nullptr, nullptr).toggle == 0);
    CHECK(ParseConfig("99999999999", nullptr, nullptr).toggle == 0);
    // Off: the start phase and discard are A and 0 whatever their variables say.
    CHECK(ParseConfig("0", "10", "B").start == Phase::A);

    const Config c = ParseConfig("600", nullptr, nullptr);
    CHECK(c.toggle == 600);
    CHECK(c.discard == DefaultDiscard);
    CHECK(c.start == Phase::A);

    CHECK(ParseConfig("600", "0", "B").discard == 0);
    CHECK(ParseConfig("600", "0", "B").start == Phase::B);
    CHECK(ParseConfig("600", "x", "b").discard == DefaultDiscard);
    CHECK(ParseConfig("600", "x", "b").start == Phase::B);
    CHECK(ParseConfig("600", nullptr, "A").start == Phase::A);
    // The discard stays below the toggle.
    CHECK(ParseConfig("50", "60", nullptr).discard == 49);
    CHECK(ParseConfig("1", nullptr, nullptr).discard == 0);
}

TEST_CASE("Nx2Ab: ab switch values", "[nx2_ab]") {
    CHECK(AbOnValue("ab") == 1U);
    CHECK(AbOnValue("ab:2") == 2U);
    CHECK(AbOnValue("ab:17") == 17U);
    CHECK(!AbOnValue("ab:0"));
    CHECK(!AbOnValue("ab:"));
    CHECK(!AbOnValue("ab:x"));
    CHECK(!AbOnValue("abc"));
    CHECK(!AbOnValue("1"));
    CHECK(!AbOnValue("a"));
    CHECK(!AbOnValue(""));
    CHECK(!AbOnValue(nullptr));

    const Switch fixed = Switch::Parse("1", 1);
    CHECK(!fixed.FollowsPhase());
    CHECK(fixed.At(Phase::A) == 1);
    CHECK(fixed.At(Phase::B) == 1);
    CHECK(Switch::Parse(nullptr, 0).At(Phase::B) == 0);
    CHECK(!Switch::Parse(nullptr, 0).EverOn());

    const Switch ab = Switch::Parse("ab", 0);
    CHECK(ab.FollowsPhase());
    CHECK(ab.EverOn());
    CHECK(ab.At(Phase::A) == 0);
    CHECK(ab.At(Phase::B) == 1);

    const Switch ab2 = Switch::Parse("ab:2", 0);
    CHECK(ab2.At(Phase::A) == 0);
    CHECK(ab2.At(Phase::B) == 2);
}

TEST_CASE("Nx2Ab: phase after flips", "[nx2_ab]") {
    CHECK(PhaseAfter(Phase::A, 0) == Phase::A);
    CHECK(PhaseAfter(Phase::A, 1) == Phase::B);
    CHECK(PhaseAfter(Phase::A, 2) == Phase::A);
    CHECK(PhaseAfter(Phase::B, 0) == Phase::B);
    CHECK(PhaseAfter(Phase::B, 3) == Phase::A);
    CHECK(PhaseName(Phase::A) == 'A');
    CHECK(PhaseName(Phase::B) == 'B');
}

TEST_CASE("Nx2Ab: off counts nothing", "[nx2_ab]") {
    Feed f{Config{}};
    f.Run(5000, 25'000);
    CHECK(f.done.empty());
    CHECK(f.blocks.Index() == 0);
}

TEST_CASE("Nx2Ab: blocks, discard and phases", "[nx2_ab]") {
    // 10 presents per phase, the first 3 after a flip discarded: 7 intervals a block.
    Feed f{ParseConfig("10", "3", nullptr)};
    // The run's first present is phase 0's present 0; its 10th present ends block 0.
    f.Run(1, 0);
    f.Run(9, 25'000);
    REQUIRE(f.done.empty());
    f.Run(1, 25'000); // present 10 of phase 0
    REQUIRE(f.done.size() == 1);
    const Block& b0 = f.done[0];
    CHECK(b0.index == 0);
    CHECK(b0.phase == Phase::A);
    CHECK(b0.presents == 7);
    CHECK(b0.us == 7 * 25'000);
    CHECK(b0.max_us == 25'000);
    CHECK(b0.over_100ms == 0);
    CHECK(b0.Fps() == 40.0);
    CHECK(f.blocks.CurrentPhase() == Phase::B);

    // Phase B: slow during the discard (the other phase's work draining), then 30 fps.
    f.Run(3, 200'000);
    f.Run(7, 33'333);
    REQUIRE(f.done.size() == 2);
    const Block& b1 = f.done[1];
    CHECK(b1.index == 1);
    CHECK(b1.phase == Phase::B);
    CHECK(b1.presents == 7);
    // Intervals 1..3 (up to present 3) are discarded; the block holds intervals 4..10.
    CHECK(b1.us == 7 * 33'333);
    CHECK(b1.over_100ms == 0);
    CHECK(f.blocks.CurrentPhase() == Phase::A);

    // A hitch inside a block is counted.
    f.Run(5, 25'000);
    f.Run(1, 150'000);
    f.Run(4, 25'000);
    REQUIRE(f.done.size() == 3);
    CHECK(f.done[2].phase == Phase::A);
    CHECK(f.done[2].presents == 7);
    CHECK(f.done[2].max_us == 150'000);
    CHECK(f.done[2].over_100ms == 1);
    CHECK(f.done[2].us == 6 * 25'000 + 150'000);
}

TEST_CASE("Nx2Ab: long run alternates and counts every present", "[nx2_ab]") {
    for (const char* start : {"A", "B"}) {
        const Config c = ParseConfig("600", "60", start);
        Feed f{c};
        const u32 phases = 31;
        f.Run(1, 0);
        f.Run(600 * phases + 599, 30'000);
        REQUIRE(f.done.size() == phases);
        REQUIRE(f.flips == phases);
        for (u32 i = 0; i < phases; ++i) {
            const Block& b = f.done[i];
            CHECK(b.index == i);
            CHECK(b.phase == PhaseAfter(c.start, i));
            CHECK(b.presents == 540);
            CHECK(b.us == 540LL * 30'000);
        }
        CHECK(f.done[0].phase == (start[0] == 'A' ? Phase::A : Phase::B));
    }
}

TEST_CASE("Nx2Ab: no discard, toggle 1", "[nx2_ab]") {
    Feed f{ParseConfig("1", "0", nullptr)};
    f.Run(1, 0);
    f.Run(3, 20'000);
    REQUIRE(f.done.size() == 3);
    for (const Block& b : f.done) {
        CHECK(b.presents == 1);
        CHECK(b.us == 20'000);
    }
    CHECK(f.done[0].phase == Phase::A);
    CHECK(f.done[1].phase == Phase::B);
    CHECK(f.done[2].phase == Phase::A);
}

TEST_CASE("Nx2Ab: line format", "[nx2_ab]") {
    Block b{.index = 7, .phase = Phase::B, .presents = 540, .us = 15'000'000, .max_us = 41'260,
            .over_100ms = 2};
    CHECK(FormatLine(b) ==
          "NX2_AB blok=7 faz=B sunum=540 sure_ms=15000.0 fps=36.00 max_ms=41.3 ust100=2");
    Block empty{};
    CHECK(FormatLine(empty) ==
          "NX2_AB blok=0 faz=A sunum=0 sure_ms=0.0 fps=0.00 max_ms=0.0 ust100=0");
}

TEST_CASE("Nx2Ab: latch holds the phase between its updates", "[nx2_ab]") {
    // The global counter: these tests run alone in their process section, but restore it.
    const u32 saved = g_flips.load();
    g_flips.store(0);
    Latch latch;
    CHECK(!latch.Update()); // the first take
    CHECK(latch.Get() == GetConfig().start);
    Flip();
    // Not taken yet: the latch still reports the old phase.
    CHECK(latch.Get() == GetConfig().start);
    CHECK(latch.Update());
    CHECK(latch.Get() == Other(GetConfig().start));
    CHECK(!latch.Update());
    // Two flips between updates: the same phase, no change reported.
    Flip();
    Flip();
    CHECK(!latch.Update());
    CHECK(latch.Get() == Other(GetConfig().start));

    const Switch ab = Switch::Parse("ab:3", 0);
    const Switch fixed = Switch::Parse("2", 2);
    CHECK(latch.Value(fixed) == 2);
    CHECK(latch.Value(ab) == ab.At(latch.Get()));
    g_flips.store(saved);
}
