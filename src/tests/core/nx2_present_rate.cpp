// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present_rate.h"

namespace {
using namespace Service::Nx2PresentRate;
} // Anonymous namespace

TEST_CASE("Nx2PresentRate: the rate in the swap interval word", "[core]") {
    // Bananza's 60/30/20 modes: n = 60, the pacing as before.
    REQUIRE(Rate(0x1003c) == 0);
    REQUIRE(Rate(0x2003c) == 0);
    REQUIRE(Rate(0x3003c) == 0);
    // Its VRR path: 41-59 Hz.
    REQUIRE(Rate(0x10029) == 41);
    REQUIRE(Rate(0x1002d) == 45);
    REQUIRE(Rate(0x1003b) == 59);
    REQUIRE(Rate(0x20032) == 50);
    // 30-240 only.
    REQUIRE(Rate(0x1001e) == 30);
    REQUIRE(Rate(0x1001d) == 0);
    REQUIRE(Rate(0x100f0) == 240);
    REQUIRE(Rate(0x100f1) == 0);
    REQUIRE(Rate(0) == 0);
    REQUIRE(Rate(0x10000) == 0);
    REQUIRE(Rate(0xffffffff) == 0);
}

TEST_CASE("Nx2PresentRate: vblank length", "[core]") {
    // 60 Hz at 100% as before (100'000'000'000 / 60 / 100).
    REQUIRE(VblankNs(0, 100) == 16'666'666);
    REQUIRE(VblankNs(Rate(0x2003c), 100) == 16'666'666);
    REQUIRE(VblankNs(45, 100) == 22'222'222);
    REQUIRE(VblankNs(Rate(0x10029), 100) == 24'390'243);
    REQUIRE(VblankNs(120, 100) == 8'333'333);
    // The speed limit scales it like the 60 Hz pacing.
    REQUIRE(VblankNs(45, 200) == 11'111'111);
    REQUIRE(VblankNs(0, 50) == 33'333'333);
}
