// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_dk_dyn_pace.h"

namespace {
using namespace Service::Nx2DkDynPace;
constexpr u64 Mkw = 0x0400C3F00006E000ULL;
} // Anonymous namespace

TEST_CASE("Nx2DkDynPace: only once the patch went in, only for Bananza", "[core]") {
    REQUIRE(ReportVrr(true, ProgramId));
    REQUIRE(!ReportVrr(false, ProgramId));
    REQUIRE(!ReportVrr(true, Mkw));
    REQUIRE(!ReportVrr(true, 0));
    // Bit 63 of the first u64, the second word's top bit (E-2).
    REQUIRE(VrrReply[0] == 0);
    REQUIRE(VrrReply[1] == 0x80000000U);
    REQUIRE(((u64{VrrReply[1]} << 32 | VrrReply[0]) >> 63) == 1);
}
