// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_stage_ring.h"

namespace {
using namespace VideoCommon::Nx2Stages;
} // Anonymous namespace

TEST_CASE("Nx2StageRing: a record header keeps its fields apart", "[video_core]") {
    const u64 header = MakeHeader(RecordKind::Forward, 0xabcdef, 0x1234, 0xfedc, 0x5);
    REQUIRE(HeaderKind(header) == RecordKind::Forward);
    REQUIRE(HeaderFlags(header) == 0x5);
    REQUIRE(HeaderWords(header) == 0xabcdef);
    REQUIRE(HeaderDeltaWords(header) == 0x1234);
    REQUIRE(HeaderSeq(header) == 0xfedc);
    // Fields too wide for their bits do not reach the others.
    const u64 clipped = MakeHeader(RecordKind::Wrap, MaxRecordWords + 1, MaxDeltaWords + 1, 0);
    REQUIRE(HeaderKind(clipped) == RecordKind::Wrap);
    REQUIRE(HeaderWords(clipped) == 0);
    REQUIRE(HeaderDeltaWords(clipped) == 0);
    REQUIRE(HeaderSeq(clipped) == 0);
}

// T1: order, wrap, publish and wake (two threads, 10^6 records), the join handshake, the =2
// lockstep with a join (park ack, S8), marks reaching consumed in order.
