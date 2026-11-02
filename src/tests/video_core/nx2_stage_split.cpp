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
