// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"

using VideoCommon::UsageTracker;

TEST_CASE("UsageTracker marks the 64-byte units a range touches", "[video_core][usage_tracker]") {
    SECTION("a range under 64 bytes marks only its unit") {
        UsageTracker tracker{16384};
        tracker.Track(0, 8);
        REQUIRE(tracker.IsUsed(32, 8));
        REQUIRE_FALSE(tracker.IsUsed(128, 8));
        REQUIRE_FALSE(tracker.IsUsed(1000, 8));
    }
    SECTION("a partly used unit at the end is marked") {
        UsageTracker tracker{16384};
        tracker.Track(0, 100);
        REQUIRE(tracker.IsUsed(96, 4));
        REQUIRE_FALSE(tracker.IsUsed(128, 64));
    }
    SECTION("a range over a page boundary marks the partly used unit of its last page") {
        UsageTracker tracker{16384};
        tracker.Track(4000, 200);
        REQUIRE(tracker.IsUsed(4160, 8));
        REQUIRE_FALSE(tracker.IsUsed(4224, 8));
    }
    SECTION("a range ending on a page boundary marks nothing in the next page") {
        UsageTracker tracker{16384};
        tracker.Track(0, 4096);
        REQUIRE(tracker.IsUsed(4032, 64));
        REQUIRE_FALSE(tracker.IsUsed(4096 + 512, 64));
    }
    SECTION("a query inside one page does not see units before it") {
        UsageTracker tracker{16384};
        tracker.Track(0, 64);
        REQUIRE_FALSE(tracker.IsUsed(128, 64));
        REQUIRE(tracker.IsUsed(0, 192));
    }
}
