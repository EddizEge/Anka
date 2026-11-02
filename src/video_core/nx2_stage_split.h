// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings.h"
#include "video_core/query_cache/types.h"

// Back stages, cut c (T7): what B decides when the Vulkan rasterizer splits an event between B and
// C, kept apart from the rasterizer so it can be tested without a device.
namespace VideoCommon::Nx2Stages {

/// Whether B sends a query report's query cache half to C through ring 3. A report a fence
/// carries stays on B, the old body in a join (C parked): CounterReport signals a fence for it,
/// whose CommitCacheFlushes and query sync take the buffer cache mutex (C takes it only inside a
/// ParkB, K3), or, a payload release with high GPU accuracy, queues the write for the next fence
/// and keeps the release for PendingSemaphoreRelease. Every other report goes to C: those are the
/// releases that go to C (§7/19 counts them with this).
[[nodiscard]] constexpr bool QueryGoesToC(QueryType type, QueryPropertiesFlags flags,
                                          bool gpu_level_high) noexcept {
    if (True(flags & QueryPropertiesFlags::IsAFence)) {
        return false;
    }
    return !(type == QueryType::Payload && gpu_level_high);
}
[[nodiscard]] inline bool QueryGoesToC(QueryType type, QueryPropertiesFlags flags) {
    return QueryGoesToC(type, flags, Settings::IsGPULevelHigh());
}

} // namespace VideoCommon::Nx2Stages
