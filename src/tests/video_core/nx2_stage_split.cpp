// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

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

namespace {
GeometrySlot Stream(u8 slot, u64 buffer) {
    return {.slot = slot, .stride = 16, .buffer = buffer, .offset = 0x40, .size = 0x1000};
}
} // namespace

TEST_CASE("Nx2StageSplit: B sends every vertex stream after binds C did not see", "[video_core]") {
    // The stages began, a join's old setup, a light attempt that fell back.
    REQUIRE(GeometryForced(true, 7, 7));
    // Once per command buffer of C's (the use marks, CanReorderUpload).
    REQUIRE(GeometryForced(false, 8, 7));
    // Else only the streams B's replica has dirty.
    REQUIRE(!GeometryForced(false, 7, 7));
}

TEST_CASE("Nx2StageSplit: the geometry mirror binds again what the command buffer lost",
          "[video_core]") {
    GeometryMirror mirror;
    // Packet 1 binds streams 0 and 1 (both live) and an index buffer, replayed at tick 5.
    const GeometrySlot first[] = {Stream(0, 0xa0), Stream(1, 0xa1),
                                  {.slot = GeometryMirror::IndexSlot, .buffer = 0xb0}};
    REQUIRE(mirror.Apply(first, 0b11, 5, false) == 0b11);
    REQUIRE(mirror.Known() == 0b11);
    REQUIRE(!mirror.RebindIndex(5));
    // A submission (tick 6) dropped everything: the live streams and the index bind again.
    REQUIRE(mirror.Rebind(0b11, 0b11) == 0b11);
    REQUIRE(mirror.Missing(0b11, 0b11) == 0);
    REQUIRE(mirror.RebindIndex(6));
    REQUIRE(mirror.Vertex(1).buffer == 0xa1);
    REQUIRE(mirror.Vertex(1).stride == 16);
    REQUIRE(mirror.Index().buffer == 0xb0);
    // Only the dirty ones: a stream whose bind is still in the command buffer stays.
    REQUIRE(mirror.Rebind(0b11, 0b10) == 0b10);

    // Packet 2 binds stream 1 again only (stream 0 kept, B's flag clean): the mirror keeps 0.
    const GeometrySlot second[] = {Stream(1, 0xc1)};
    REQUIRE(mirror.Apply(second, 0b11, 6, false) == 0b10);
    REQUIRE(mirror.Vertex(0).buffer == 0xa0);
    REQUIRE(mirror.Vertex(1).buffer == 0xc1);
    // Its index buffer is its own: none in a packet without one.
    REQUIRE(!mirror.RebindIndex(7));
}

TEST_CASE("Nx2StageSplit: the geometry mirror drops the streams B stopped using",
          "[video_core]") {
    // T3 R2 geometri-2: a stream B no longer binds may name a buffer since freed.
    GeometryMirror mirror;
    const GeometrySlot both[] = {Stream(0, 0xa0), Stream(3, 0xa3)};
    mirror.Apply(both, 0b1001, 1, false);
    // Stream 3 disabled (vertex_live): never bound again from the mirror.
    mirror.Apply({}, 0b0001, 1, false);
    REQUIRE(mirror.Known() == 0b0001);
    REQUIRE(mirror.Rebind(0b1001, 0b1001) == 0b0001);
    // Live but unknown (not expected): left alone, reported.
    REQUIRE(mirror.Missing(0b1001, 0b1001) == 0b1000);
    // Live again: B's flag was dirty (disabled streams are never bound), so it binds it first.
    const GeometrySlot again[] = {Stream(3, 0xd3)};
    mirror.Apply(again, 0b1001, 2, false);
    REQUIRE(mirror.Rebind(0b1001, 0b1001) == 0b1001);
    REQUIRE(mirror.Vertex(3).buffer == 0xd3);
    // Transform feedback slots (Joined draws only) are not the mirror's.
    const GeometrySlot feedback[] = {{.slot = 33, .buffer = 0xe0}};
    REQUIRE(mirror.Apply(feedback, 0b1001, 3, false) == 0);
    mirror.Clear();
    REQUIRE(mirror.Known() == 0);
    REQUIRE(!mirror.RebindIndex(3));
}

TEST_CASE("Nx2StageSplit: the live streams ride in the draw's head", "[video_core]") {
    StageDrawWriter writer;
    writer.Begin();
    writer.Head().vertex_live = 0x8000'0005;
    writer.AddGeometry(Stream(2, 0xa2));
    std::vector<u64> words(writer.Words());
    writer.WriteTo(words.data());
    const StageDrawReader reader{words.data(), static_cast<u32>(words.size())};
    REQUIRE(reader.Head().vertex_live == 0x8000'0005);
    REQUIRE(reader.Geometry().size() == 1);
    REQUIRE(reader.Geometry()[0].buffer == 0xa2);
}

TEST_CASE("Nx2StageSplit: binds B replayed in a join bind again before the draw",
          "[video_core]") {
    // T3 R2 birakma-1: the record owner hook replayed the packet's binds, a submission may have
    // followed; C does not know which command buffer they went to.
    GeometryMirror mirror;
    const GeometrySlot slots[] = {Stream(2, 0xa2), {.slot = GeometryMirror::IndexSlot, .buffer = 0xb0}};
    // The streams to bind again are returned (the caller sets their flags dirty).
    REQUIRE(mirror.Apply(slots, 0b100, 9, true) == 0b100);
    REQUIRE(mirror.Rebind(0b100, 0b100) == 0b100);
    // The index buffer binds again even at the same tick.
    REQUIRE(mirror.RebindIndex(9));
}
