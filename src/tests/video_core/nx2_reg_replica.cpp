// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/nx2_reg_replica.h"

namespace {
using namespace VideoCommon::Nx2RegReplica;
} // Anonymous namespace

TEST_CASE("Nx2RegReplica: a delta word keeps its fields apart", "[video_core]") {
    const u64 word = MakeDelta(0xdff, 0x89abcdef, true, false);
    REQUIRE(DeltaMethod(word) == 0xdff);
    REQUIRE(DeltaValue(word) == 0x89abcdef);
    REQUIRE((word & DeltaDirty) != 0);
    REQUIRE((word & DeltaChanged) == 0);
    REQUIRE((word & 0xfffc0000u) == 0);
    const u64 side = MakeSideHeader(SideConstBuffers | SideDirtyFlags, 77);
    REQUIRE(SideParts(side) == (SideConstBuffers | SideDirtyFlags));
    REQUIRE(SideWords(side) == 77);
}

// T2: a random register stream (SKIP_SAME on and off, A -> B -> A within one range, executable
// methods) through ProcessDirtyRegisters and through RegCapture + ApplyDelta gives the same
// registers, dirty flags and light changes; it fails with Dirty / Changed left out.
