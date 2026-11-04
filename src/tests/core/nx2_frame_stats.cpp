// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_frame_stats.h"

TEST_CASE("Nx2FrameStats: p95 by nearest rank", "[core]") {
    REQUIRE(Service::Nx2FrameStats::P95({}) == 0);
    std::array<u32, 1> one{33'000};
    REQUIRE(Service::Nx2FrameStats::P95(one) == 33'000);
    // 600 frames 1..600 in reverse: the 570th smallest.
    std::vector<u32> frames;
    for (u32 i = 600; i >= 1; --i) {
        frames.push_back(i);
    }
    REQUIRE(Service::Nx2FrameStats::P95(frames) == 570);
    // 599 of them (the first block): ceil(0.95 * 599) = 570 too.
    frames.clear();
    for (u32 i = 1; i <= 599; ++i) {
        frames.push_back(i * 100);
    }
    REQUIRE(Service::Nx2FrameStats::P95(frames) == 57'000);
    // 20 frames: the 19th.
    std::vector<u32> twenty;
    for (u32 i = 1; i <= 20; ++i) {
        twenty.push_back(i);
    }
    REQUIRE(Service::Nx2FrameStats::P95(twenty) == 19);
    // A few long frames above the 95th percentile do not move it.
    std::vector<u32> hitches(600, 33'000);
    for (size_t i = 0; i < 25; ++i) {
        hitches[i * 20] = 150'000;
    }
    REQUIRE(Service::Nx2FrameStats::P95(hitches) == 33'000);
    std::vector<u32> more(600, 33'000);
    for (size_t i = 0; i < 31; ++i) {
        more[i * 19] = 150'000;
    }
    REQUIRE(Service::Nx2FrameStats::P95(more) == 150'000);
}
