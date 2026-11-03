// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_front_diag.h"

using namespace VideoCommon::Nx2FrontDiag;

TEST_CASE("Nx2FrontDiag: sampling rate, also for periodic callers", "[video_core]") {
    Sampler sampler;
    u64 sampled = 0;
    constexpr u64 Calls = 1 << 20;
    for (u64 i = 0; i < Calls; ++i) {
        sampled += sampler.Next(3) ? 1 : 0;
    }
    REQUIRE(sampled > Calls / 8 * 9 / 10);
    REQUIRE(sampled < Calls / 8 * 11 / 10);
    // Eight macros called round robin: each gets about its share.
    Sampler round_robin;
    std::array<u64, 8> per{};
    for (u64 i = 0; i < Calls; ++i) {
        per[i % 8] += round_robin.Next(3) ? 1 : 0;
    }
    for (const u64 count : per) {
        REQUIRE(count > Calls / 64 * 8 / 10);
        REQUIRE(count < Calls / 64 * 12 / 10);
    }
    Sampler sixteenth;
    u64 sampled16 = 0;
    for (u64 i = 0; i < Calls; ++i) {
        sampled16 += sixteenth.Next(4) ? 1 : 0;
    }
    REQUIRE(sampled16 > Calls / 16 * 9 / 10);
    REQUIRE(sampled16 < Calls / 16 * 11 / 10);
}

TEST_CASE("Nx2FrontDiag: macro table", "[video_core]") {
    MacroTable table;
    auto& a = table.Find(0x120);
    a.calls = 100;
    a.sampled = 10;
    a.ticks = 1000; // 100 per call: 10,000
    auto& b = table.Find(0x340);
    b.calls = 10;
    b.sampled = 10;
    b.ticks = 50000; // 5,000 per call: 50,000
    REQUIRE(&table.Find(0x120) == &a);
    REQUIRE(&table.Find(0x340) == &b);
    REQUIRE(table.Used() == 2);
    REQUIRE(a.EstimatedTicks() == 10000.0);
    auto& c = table.Find(0x777);
    c.calls = 5; // Never sampled: no time, after the timed ones
    const auto top = table.Top(16);
    REQUIRE(top.size() == 3);
    REQUIRE(top[0]->start == 0x340);
    REQUIRE(top[1]->start == 0x120);
    REQUIRE(top[2]->start == 0x777);
    REQUIRE(table.Top(1).size() == 1);
    // Full: the rest share one entry.
    MacroTable full;
    for (u32 start = 0; start < MacroTable::Slots; ++start) {
        full.Find(start * 7).calls = 1;
    }
    auto& rest = full.Find(0xffff0);
    REQUIRE(rest.start == ~0U);
    REQUIRE(&full.Find(0xffff4) == &rest);
    for (u32 start = 0; start < MacroTable::Slots; ++start) {
        REQUIRE(full.Find(start * 7).start == start * 7);
    }
    full.Clear();
    REQUIRE(full.Used() == 0);
    REQUIRE(full.Top(4).empty());
}

TEST_CASE("Nx2FrontDiag: register writes by method", "[video_core]") {
    RegisterWrites writes;
    for (int i = 0; i < 10; ++i) {
        writes.Note(0x200, true, true);
    }
    writes.Note(0x200, false, false);
    for (int i = 0; i < 4; ++i) {
        writes.Note(0x8e4, true, false);
    }
    writes.Note(0x300, false, false);
    writes.Note(0xE00, true, true); // A macro method: not a register
    const auto top = writes.Top(16);
    REQUIRE(top.size() == 2);
    REQUIRE(top[0].method == 0x200);
    REQUIRE(top[0].total == 11);
    REQUIRE(top[0].same == 10);
    REQUIRE(top[0].skipped == 10);
    REQUIRE(top[1].method == 0x8e4);
    REQUIRE(top[1].same == 4);
    REQUIRE(top[1].skipped == 0);
    const auto totals = writes.Totals();
    REQUIRE(totals[0] == 16);
    REQUIRE(totals[1] == 14);
    REQUIRE(totals[2] == 10);
    REQUIRE(writes.Top(1).size() == 1);
    writes.Clear();
    REQUIRE(writes.Totals()[0] == 0);
}

TEST_CASE("Nx2FrontDiag: drain wait bands", "[video_core]") {
    DrainHistogram histogram;
    histogram.Note(false, 123456); // No wait: a call only
    histogram.Note(true, 1000);    // 1 us at 1 ns a tick
    histogram.Note(true, 5000);    // 4096..8191: ~6 us
    histogram.Note(true, 100'000); // ~98 us
    histogram.Note(true, 3'000'000);
    histogram.Note(true, 20'000'000);
    histogram.Note(true, 0);
    REQUIRE(histogram.Calls() == 7);
    const auto bands = histogram.ToBands(1.0);
    REQUIRE(bands[0] == 2); // 0 and 1 us
    REQUIRE(bands[1] == 1); // 6 us
    REQUIRE(bands[2] == 0);
    REQUIRE(bands[3] == 1); // 98 us
    REQUIRE(bands[4] == 0);
    REQUIRE(bands[5] == 0);
    REQUIRE(bands[6] == 1); // 3 ms
    REQUIRE(bands[7] == 1); // 20 ms
    // A faster counter: the same ticks are shorter.
    const auto quarter = histogram.ToBands(0.25);
    REQUIRE(quarter[0] == 3);
    REQUIRE(quarter[3] == 0);
    REQUIRE(quarter[2] == 1);
}
