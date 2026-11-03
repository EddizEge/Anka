// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/maxwell_3d.h"

namespace {
using Tegra::Engines::WriteHistoryRequested;

TEST_CASE("EDEN_NX2_3D_HISTORY reads a leading 0 as off", "[video_core][maxwell_3d]") {
    REQUIRE(!WriteHistoryRequested(nullptr));
    REQUIRE(!WriteHistoryRequested("0"));
    REQUIRE(!WriteHistoryRequested("00"));
    REQUIRE(WriteHistoryRequested("1"));
    REQUIRE(WriteHistoryRequested(""));
}
} // namespace
