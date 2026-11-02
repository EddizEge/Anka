// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_stage_split.h"

using namespace VideoCommon::Nx2Stages;
using VideoCommon::QueryPropertiesFlags;
using VideoCommon::QueryType;

TEST_CASE("Nx2StageSplit: a report a fence carries stays on B", "[video_core]") {
    constexpr auto none = QueryPropertiesFlags{};
    constexpr auto fence = QueryPropertiesFlags::IsAFence;
    constexpr auto timed = QueryPropertiesFlags::HasTimeout;
    for (const bool high : {false, true}) {
        // A fence report: CounterReport signals a fence (CommitCacheFlushes takes the buffer
        // cache mutex), whatever its type.
        REQUIRE(!QueryGoesToC(QueryType::Payload, fence, high));
        REQUIRE(!QueryGoesToC(QueryType::Payload, fence | timed, high));
        REQUIRE(!QueryGoesToC(QueryType::ZPassPixelCount64, fence, high));
        REQUIRE(!QueryGoesToC(QueryType::StreamingByteCount, fence | timed, high));
        // Counters without a fence go to C (their write waits for a later fence).
        REQUIRE(QueryGoesToC(QueryType::ZPassPixelCount64, none, high));
        REQUIRE(QueryGoesToC(QueryType::StreamingPrimitivesSucceeded, timed, high));
    }
    // A payload release: written at once on C, or kept for the next fence with high accuracy.
    REQUIRE(QueryGoesToC(QueryType::Payload, none, false));
    REQUIRE(QueryGoesToC(QueryType::Payload, timed, false));
    REQUIRE(!QueryGoesToC(QueryType::Payload, none, true));
    REQUIRE(!QueryGoesToC(QueryType::Payload, timed, true));
}

TEST_CASE("Nx2StageSplit: a report in a join notes the WaitForIdle flag", "[video_core]") {
    // T7c review finding 2: KeplerCompute's report runs its old body on A in a join; B's next
    // WaitForIdle must join (the query cache's sync takes the buffer cache mutex).
    REQUIRE(QueryNotesWfi(true, false));
    // C runs the half B sent: B noted it already, and the flag is B's.
    REQUIRE(!QueryNotesWfi(true, true));
    // Without cut c there is no flag.
    REQUIRE(!QueryNotesWfi(false, false));
    REQUIRE(!QueryNotesWfi(false, true));
}

TEST_CASE("Nx2StageSplit: a write over a report C has not run waits for it", "[video_core]") {
    // T7c review finding 3: B sends C a payload release to X (seq 5: before the next packet),
    // then writes X itself; C would write the release after B's bytes.
    InFlightRanges writes;
    constexpr u64 X = 0x10000;
    writes.Note(X, X + 16, 5, 3);
    REQUIRE(writes.Overlaps(X + 8, X + 12, 4));
    REQUIRE(writes.Overlaps(X - 4, X + 1, 4));
    REQUIRE(!writes.Overlaps(X + 16, X + 32, 4));
    REQUIRE(!writes.Overlaps(X - 16, X, 4));
    // C ran the packet after it: done.
    REQUIRE(!writes.Overlaps(X, X + 16, 5));
    REQUIRE(writes.Size() == 0);
}

TEST_CASE("Nx2StageSplit: in-flight ranges keep the last seq of a range", "[video_core]") {
    InFlightRanges pools;
    pools.Note(0x1000, 0x2000, 10, 0);
    pools.Note(0x8000, 0x9000, 10, 0);
    // The same two pools for the next draws: no new entry, their seq moves on.
    pools.Note(0x1000, 0x2000, 11, 0);
    pools.Note(0x8000, 0x9000, 12, 0);
    REQUIRE(pools.Size() == 2);
    REQUIRE(pools.Overlaps(0x1800, 0x1900, 10));
    REQUIRE(pools.Overlaps(0x1800, 0x1900, 0));
    REQUIRE(!pools.Overlaps(0x1800, 0x1900, 11));
    REQUIRE(pools.Overlaps(0x8800, 0x8900, 11));
    REQUIRE(pools.Any(11));
    REQUIRE(!pools.Any(12));
    pools.Note(0x1000, 0x2000, 13, 12);
    pools.Clear();
    REQUIRE(!pools.Any(0));
}

TEST_CASE("Nx2StageSplit: in-flight ranges stay bounded while C runs on", "[video_core]") {
    InFlightRanges ranges;
    // Alternating ranges, C a packet behind: the list is pruned with what C ran.
    for (u64 seq = 1; seq <= 1000; ++seq) {
        const u64 base = (seq % 40) * 0x1000;
        ranges.Note(base, base + 0x100, seq, seq - 1);
    }
    REQUIRE(ranges.Size() <= 64);
    // What C has not run stays: the last one.
    REQUIRE(ranges.Overlaps(0, 0x1000 * 40, 999));
    // C far behind: nothing it has not run is dropped.
    InFlightRanges behind;
    for (u64 seq = 1; seq <= 100; ++seq) {
        behind.Note(seq * 0x1000, seq * 0x1000 + 0x10, seq, 0);
    }
    REQUIRE(behind.Size() == 100);
    REQUIRE(behind.Overlaps(0x1000, 0x1001, 0));
    REQUIRE(behind.Overlaps(100 * 0x1000, 100 * 0x1000 + 1, 99));
}
