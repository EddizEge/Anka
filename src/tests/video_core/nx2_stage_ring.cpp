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

TEST_CASE("Nx2StageRing: sequence numbers compare across a wrap", "[video_core]") {
    REQUIRE(SeqAtLeast<u16>(5, 5));
    REQUIRE(SeqAtLeast<u16>(6, 5));
    REQUIRE(!SeqAtLeast<u16>(5, 6));
    REQUIRE(SeqAtLeast<u16>(2, 0xfffe));
    REQUIRE(!SeqAtLeast<u16>(0xfffe, 2));
    REQUIRE(SeqAtLeast<u32>(3, 0xffffff00u));
    REQUIRE(!SeqAtLeast<u32>(0xffffff00u, 3));
    // Half the range apart or more: the older one reads as later (K5).
    REQUIRE(!SeqAtLeast<u32>(0x80000000u, 0));
    REQUIRE(SeqAtLeast<u64>(u64{1} << 40, 7));
    static_assert(SeqAtLeast<u16>(1, 0xffff) && !SeqAtLeast<u16>(0xffff, 1));
}

// T1: order, wrap, publish and wake (two threads, 10^6 records), the join handshake, the =2
// lockstep with a join (park ack, S8), marks reaching consumed in order.
