// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_stage_ring.h"

namespace {
using namespace VideoCommon::Nx2Stages;
} // Anonymous namespace

TEST_CASE("Nx2StageRing: a record header keeps its fields apart", "[video_core]") {
    const u64 header = MakeHeader(RecordKind::Forward, 0xabcd, 0xd5b, 0xfedc, 0x5);
    REQUIRE(HeaderKind(header) == RecordKind::Forward);
    REQUIRE(HeaderFlags(header) == 0x5);
    REQUIRE(HeaderWords(header) == 0xabcd);
    REQUIRE(HeaderDeltaWords(header) == 0xd5b);
    REQUIRE(HeaderSeq(header) == 0xfedc);
}

TEST_CASE("Nx2StageRing: the largest record fits its header and a quarter ring", "[video_core]") {
    // MakeHeader stops on a field that does not fit (K9); the largest one keeps every field.
    constexpr u64 largest =
        MakeHeader(RecordKind::CbData, MaxRecordWords, MaxDeltaWords, 0xffff, 0xf);
    REQUIRE(HeaderKind(largest) == RecordKind::CbData);
    REQUIRE(HeaderWords(largest) == MaxRecordWords);
    REQUIRE(HeaderDeltaWords(largest) == MaxDeltaWords);
    REQUIRE(HeaderSeq(largest) == 0xffff);
    REQUIRE(HeaderFlags(largest) == 0xf);
    static_assert(MaxRecordWords <= 0xffffff && MaxDeltaWords <= 0xffff);
    static_assert(size_t{MaxRecordWords} * 4 <= RingToCWords && RingToCWords <= RingToBWords);
    static_assert(MaxDeltaWords + MaxBodyWords == MaxRecordWords);
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
