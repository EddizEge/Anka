// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

// EDEN_NX2_PRESENT_RATE=1 (off by default; nx2_gsv.h): the vi2 229 swap interval word is
// (k << 16) | n, k vblanks a frame at n Hz. Bananza's 60/30/20 modes send n = 60; its VRR path
// (vi2 107 answered "supported") sends 41-59. Without the switch the pacing reads only k
// (EDEN_NX2_PRESENT_INTERVAL_WORD=12:16:4) and paces k vblanks of 60 Hz; with it, a rate of 30-240
// Hz other than 60 paces k vblanks of n Hz instead (Duck912 f8d38607b6, moceure/eden-nx2-android
// 809670e3a5 pace by both always). src/tests/core/nx2_present_rate.cpp.
namespace Service::Nx2PresentRate {

/// The refresh rate in the low half of a swap interval word: n when 30-240 and not 60, else 0
/// (the 60 Hz pacing as before).
constexpr u32 Rate(u32 word) {
    const u32 n = word & 0xffff;
    return n >= 30 && n <= 240 && n != 60 ? n : 0;
}

/// Nanoseconds of one vblank at `rate` Hz (0: 60) and the speed limit `speed_percent` (> 0).
constexpr long long VblankNs(u32 rate, u32 speed_percent) {
    return 100'000'000'000LL / (rate != 0 ? rate : 60) / speed_percent;
}

} // namespace Service::Nx2PresentRate
