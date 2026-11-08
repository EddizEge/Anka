// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/nx2_reinterpret_log.h"

using namespace Vulkan::Nx2ReinterpretLog;

TEST_CASE("Nx2ReinterpretLog: aspect names", "[video_core]") {
    REQUIRE(AspectName(AspectColor) == "COLOR");
    REQUIRE(AspectName(AspectDepth) == "DEPTH");
    REQUIRE(AspectName(AspectStencil) == "STENCIL");
    REQUIRE(AspectName(AspectDepth | AspectStencil) == "DEPTH|STENCIL");
    REQUIRE(AspectName(0) == "none");
    REQUIRE(AspectName(AspectColor | 0x40) == "COLOR|0x40");
    REQUIRE(AspectName(0x100) == "0x100");

    REQUIRE(IsDepthStencil(AspectDepth | AspectStencil));
    REQUIRE_FALSE(IsDepthStencil(AspectDepth));
    REQUIRE_FALSE(IsDepthStencil(AspectStencil));
    REQUIRE_FALSE(IsDepthStencil(AspectColor));
}

TEST_CASE("Nx2ReinterpretLog: a line for each of the first calls", "[video_core]") {
    Counter counter;
    for (u64 call = 1; call <= DetailedCalls; ++call) {
        REQUIRE(counter.Note(call % 2 == 0) == call);
    }
    for (int call = 0; call < 100; ++call) {
        REQUIRE(counter.Note(true) == 0);
    }
}

TEST_CASE("Nx2ReinterpretLog: a summary at most once a minute, when the count moved",
          "[video_core]") {
    Counter counter;
    // Nothing yet: never a line
    REQUIRE_FALSE(counter.SummaryDue(1000).has_value());
    REQUIRE_FALSE(counter.SummaryDue(1000 + SummarySeconds).has_value());

    // Calls, but the minute isn't over
    counter.Note(true);
    counter.Note(false);
    counter.Note(true);
    REQUIRE_FALSE(counter.SummaryDue(1000 + SummarySeconds + 30).has_value());
    // Over: the totals
    const auto first = counter.SummaryDue(1000 + 2 * SummarySeconds);
    REQUIRE(first.has_value());
    REQUIRE(first->total == 3);
    REQUIRE(first->depth_stencil == 2);

    // No calls since: no line, even minutes later
    REQUIRE_FALSE(counter.SummaryDue(1000 + 3 * SummarySeconds).has_value());
    REQUIRE_FALSE(counter.SummaryDue(1000 + 10 * SummarySeconds).has_value());

    // A call: a line, a minute after the last check
    counter.Note(false);
    REQUIRE_FALSE(counter.SummaryDue(1000 + 10 * SummarySeconds + 1).has_value());
    const auto second = counter.SummaryDue(1000 + 11 * SummarySeconds);
    REQUIRE(second.has_value());
    REQUIRE(second->total == 4);
    REQUIRE(second->depth_stencil == 2);
}

TEST_CASE("Nx2ReinterpretLog: the first minute starts at the first ask", "[video_core]") {
    Counter counter;
    counter.Note(true);
    // The first ask after startup only starts the clock
    REQUIRE_FALSE(counter.SummaryDue(5000).has_value());
    REQUIRE_FALSE(counter.SummaryDue(5000 + SummarySeconds - 1).has_value());
    const auto summary = counter.SummaryDue(5000 + SummarySeconds);
    REQUIRE(summary.has_value());
    REQUIRE(summary->total == 1);
    REQUIRE(summary->depth_stencil == 1);
}
