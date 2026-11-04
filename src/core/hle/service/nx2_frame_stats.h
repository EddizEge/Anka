// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>

#include "common/common_types.h"

namespace Service::Nx2FrameStats {

/// The 95th percentile (nearest rank) of the frame times, 0 with none; reorders `frames`.
inline u32 P95(std::span<u32> frames) {
    if (frames.empty()) {
        return 0;
    }
    const auto rank = frames.begin() + static_cast<std::ptrdiff_t>((frames.size() * 95 - 1) / 100);
    std::nth_element(frames.begin(), rank, frames.end());
    return *rank;
}

} // namespace Service::Nx2FrameStats
